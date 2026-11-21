#pragma once
#include <array>
#include <cstdint>

// =============================================================================
// ScreenDropletSim（レンズ水滴の挙動）
//
// ScreenDroplets ポストエフェクトが描く「レンズに付いた水」の状態を CPU で持つ。
// シェーダは毎フレーム「今どこに・どんな形の水があるか」を受け取って描くだけにし、
// 付着 → 滑落 → 合体 → 滴の取り残し → 画面外へ流れ出る、という時間変化はここで決める。
//
// 座標系はシェーダと共通で「縦 = 1、横 = アスペクト比」の正規化座標（y は下向き）。
// 半径などの長さもすべて画面の高さを 1 とした単位。
//
//   浮上（Surface）: レンズ全面を覆った水膜が中央から外へ割れて引いていき、
//                    取り残された水滴が重力と走行風で外へ流れ出る
//   潜水（Dive）   : 水面のラインが下から上へ抜けたあと、レンズに付いた気泡が
//                    浮力と走行風で揺れながら外へ抜けていく
// =============================================================================

/// @class ScreenDropletSim
/// @brief レンズ水滴・気泡の付着と流れを計算し、シェーダへ渡す形に書き出す
class ScreenDropletSim {
public:
  static constexpr int kMaxDrops = 96; ///< 同時に持てる水滴の数（HLSL 側 MAX_DROPS と一致させる）

  /// @enum Mode
  /// @brief どちら向きに水面を抜けたか
  enum class Mode : uint32_t {
    Surface = 0, ///< 浮上（水中 → 水上）。水膜と水滴
    Dive = 1,    ///< 潜水（水上 → 水中）。水面ラインと気泡
  };

  /// @struct Params
  /// @brief 毎フレーム外から与える調整値
  struct Params {
    float sizeScale = 1.0f; ///< 水滴の大きさの倍率（1 で既定）
    float flowSpeed = 1.0f; ///< 流れる速さの倍率（1 で既定）
    float wind = 0.6f;      ///< 走行風で画面の外側へ押し出す強さ（0 で重力 / 浮力だけ）
    float aspect = 1.777f;  ///< 画面のアスペクト比（幅 / 高さ）
  };

  /// @struct Sheet
  /// @brief 浮上時の水膜・潜水時の水面ラインの進み方（HLSL 側と同じ式で使う）
  struct Sheet {
    float centerX = 0.888f;   ///< 水膜が割れ始める点 X（正規化座標）
    float centerY = 0.40f;    ///< 水膜が割れ始める点 Y
    float duration = 1.1f;    ///< 水膜が引ききるまで / 水面ラインが抜けきるまでの秒数
    float radiusMax = 1.65f;  ///< 水膜の乾いた領域が最終的に届く距離
    float gravityBias = 0.45f; ///< 上ほど早く乾き、下ほど遅くまで水が残る度合い
  };

  /// @struct GpuDrop
  /// @brief シェーダへ渡す 1 粒ぶん（float4 × 2）
  struct GpuDrop {
    float a[4]; ///< xy: 中心, z: 半径, w: 後ろ側（尾）の伸び率
    float b[4]; ///< xy: 進行方向（単位ベクトル）, z: 進行方向の伸び率, w: 直交方向の伸び率
  };

  /// @brief 演出を最初から始める（今ある水滴はすべて捨てる）
  /// @param mode   浮上か潜水か
  /// @param params 大きさ・アスペクト比など
  void Trigger(Mode mode, const Params &params);

  /// @brief 時間を進める（毎フレーム呼ぶ）
  /// @param dt     経過時間（秒）
  /// @param params 大きさ・流れる速さ・走行風など
  void Update(float dt, const Params &params);

  /// @brief 残っている水を急いで消す（シーン遷移の直前などに呼ぶ）
  /// @details 一斉には消さず、粒ごとに時間差を付けて seconds 秒以内に痩せて消えきるようにする。
  ///          まだ水膜の中に埋もれている（出現前の）粒は捨てる。
  /// @param seconds 消えきるまでの秒数
  void Drain(float seconds);

  /// @brief 画面に出ている水滴をシェーダ用の形で書き出す
  /// @param out      書き出し先（kMaxDrops 個ぶん確保しておくこと）
  /// @param maxCount 書き出す上限
  /// @param fade     全体の強度（0〜1）。粒ごとに違う値で消えるので、下げても一斉には消えない
  /// @return 書き出した数
  int Export(GpuDrop *out, int maxCount, float fade) const;

  /// @brief すべての水滴を捨てて止める
  void Clear();

  /// @brief Trigger からの経過秒数
  float Elapsed() const { return elapsed_; }
  /// @brief 今の演出の種類
  Mode GetMode() const { return mode_; }
  /// @brief 水膜 / 水面ラインの進み方
  const Sheet &GetSheet() const { return sheet_; }
  /// @brief 演出中か（Trigger 後、Clear されていない）
  bool Active() const { return active_; }
  /// @brief Drain を始めた時刻（Trigger からの秒数。Drain していなければ大きな値）
  float DrainStart() const { return drainStart_; }
  /// @brief Drain で消えきるまでの秒数
  float DrainDuration() const { return drainDuration_; }

  /// @brief 水膜が「この点まで引いた」と判定される時刻（HLSL の ArrivalTime と同じ式）
  /// @param mode  浮上か潜水か
  /// @param sheet 水膜の進み方
  /// @param x     点 X（正規化座標）
  /// @param y     点 Y
  /// @return Trigger からの秒数（届かない場合は大きな値）
  static float ArrivalTime(Mode mode, const Sheet &sheet, float x, float y);

private:
  /// @struct Drop
  /// @brief 1 粒の状態
  struct Drop {
    float x = 0.0f, y = 0.0f;   ///< 中心
    float vx = 0.0f, vy = 0.0f; ///< 速度（画面の高さ / 秒）
    float dirX = 0.0f, dirY = 1.0f; ///< 形を伸ばす向き（止まっている間は最後の向きを保つ）
    float r = 0.0f;             ///< 半径（体積の 1/3 乗に比例）
    float stretch = 1.0f;       ///< 進行方向への伸び（速いほど縦長の涙型）
    float wobbleAmp = 0.0f;     ///< 形の揺れの振幅（合体・滑り出しで励起され、減衰する）
    float wobblePhase = 0.0f;   ///< 揺れの位相
    float shedDistance = 0.0f;  ///< 前回の滴の取り残しから進んだ距離
    float delay = 0.0f;         ///< 出現までの残り秒数（水膜が引いてから現れる）
    float age = 0.0f;           ///< 出現してからの秒数
    float seed = 0.0f;          ///< 個体差用の乱数（滴を取り残すたびに引き直す）
    float fadeSeed = 0.0f;      ///< 全体の強度が下がったときに消える順番（0〜1。生まれたときに固定）
    float life = 0.0f;          ///< 消え始めるまでの残り秒数（粒ごとにばらつかせ、一斉に消えないようにする）
    float vanish = 0.0f;        ///< 消えていく進み具合（0: まだ 〜 1: 消えきった）
    float vanishTime = 0.4f;    ///< 消え始めてから消えきるまでの秒数
    bool alive = false;         ///< 使用中か
    bool sliding = false;       ///< 滑っている最中か（滑り出しの瞬間に揺れを入れるため）
  };

  /// @brief 空きに 1 粒追加する（寿命は大きさと演出の種類から決める）
  /// @return 追加した粒（空きが無ければ nullptr）
  Drop *Spawn(float x, float y, float r, float delay);

  /// @brief この位置での流れの向き（重力 / 浮力 ＋ 外向きの走行風）
  void FlowDir(float x, float y, const Params &params, float &outX, float &outY) const;

  /// @brief 1 ステップぶん進める
  void Step(float dt, const Params &params);

  /// @brief 重なった粒を体積を保って 1 つにまとめる
  void Merge();

  /// @brief 0〜1 の一様乱数
  float Rand();

  std::array<Drop, kMaxDrops> drops_{}; ///< 水滴の置き場
  Sheet sheet_{};                       ///< 水膜の進み方
  Mode mode_ = Mode::Surface;           ///< 今の演出の種類
  float elapsed_ = 0.0f;                ///< Trigger からの経過秒数
  uint32_t rng_ = 0x9E3779B9u;          ///< 乱数の状態（xorshift32）
  uint32_t triggerCount_ = 0;           ///< Trigger の回数（毎回違う配置にするため）
  float sizeScale_ = 1.0f;              ///< Trigger 時の大きさの倍率（寿命の判定に使う）
  float drainStart_ = 1.0e3f;           ///< Drain を始めた時刻（していなければ大きな値）
  float drainDuration_ = 1.0f;          ///< Drain で消えきるまでの秒数
  bool active_ = false;                 ///< 演出中か
};
