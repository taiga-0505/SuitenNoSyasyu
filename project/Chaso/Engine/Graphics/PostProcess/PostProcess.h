#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include <cassert>
#include <d3d12.h>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include "Dx12/SRVManager/SRVManager.h"
#include "ScreenDropletSim.h"

class Dx12Core;
class PipelineManager;
class RenderTexture;
class GraphicsPipeline;

/// @enum PostEffectType
/// @brief ポストエフェクトの種類を定義する列挙型
enum class PostEffectType {
  None,       ///< そのまま転送（CopyImage）
  Grayscale,  ///< グレースケール
  Sepia,      ///< セピア調
  Vignette,   ///< ビネット（周辺減光）
  BoxFilter,  ///< ボックスフィルタ（平滑化）
  GaussianFilter, ///< ガウシアンフィルタ（ガウス分布による平滑化）
  DepthBasedOutline, ///< 深度ベースアウトライン
  RadialBlur, ///< ラジアルブラー
  Dissolve,   ///< ディゾルブ（ノイズマスクによる消失演出）
  RandomNoise,///< ランダムノイズ（時間経過によるプロシージャルノイズ）
  MaskOutline,///< マスクされたオブジェクトだけの輪郭（インタラクト対象の強調など）
  Ssao,       ///< スクリーンスペース AO（深度から接地感の陰を落とす。積む順は最初）
  Bloom,      ///< 明部のにじみ（1 パス近似）
  ColorGrade, ///< カラーグレーディング（露出・コントラスト・彩度・色温度）
  Fxaa,       ///< アンチエイリアス（積む順は最後）
  Underwater, ///< 水中エフェクト（UV 歪みと青み）
  Caustics,   ///< 水面から差す光の網目模様（深度からワールド座標を復元して投影）
  LightShaft, ///< 水中の降り注ぐ光（レイマーチ型のボリューメトリックライト）
  ScreenDroplets, ///< レンズ水滴（水上・水中の遷移や着水時にレンズへ付いて流れる水滴）
  BloodOverlay, ///< 被弾・瀕死のときに画面の周辺へ付く血（手続き型。テクスチャ不要）
  InkOverlay,   ///< タコの墨がレンズに貼り付いて視界を塞ぐ（手続き型。テクスチャ不要）
  VisionBleed,  ///< 意識が遠のく視界（周辺から水彩のように滲むぼかし・色のにじみと二重像。手続き型）
};

/// @class PostProcess
/// @brief ポストプロセス（RenderTexture の内容を画面に描画する）を管理するクラス
/// @details エフェクトの重ね掛け（マルチパス）に対応しており、ピンポンバッファを用いて複数のエフェクトを順次適用できる。
class PostProcess {
public:
  /// @brief コンストラクタ（GPU リソースは Initialize で作る）
  PostProcess() = default;
  /// @brief デストラクタ
  ~PostProcess() = default;

  /// @brief 各エフェクトのパイプラインを取得し、定数バッファを初期化する
  /// @param dxCore          DirectX12 コアシステムへのポインタ
  /// @param pipelineManager パイプライン管理クラスへのポインタ
  /// @param width           画面解像度（幅）
  /// @param height          画面解像度（高さ）
  void Initialize(Dx12Core *dxCore, PipelineManager *pipelineManager,
                  uint32_t width, uint32_t height);

  /// @brief 解像度の変更に追従する
  /// @param width  新しい幅
  /// @param height 新しい高さ
  /// @note キャッシュ済みの深度 SRV を破棄し、ピンポンテクスチャを作り直す。
  void Resize(uint32_t width, uint32_t height);

  /// @brief 内部時間を更新する（毎フレーム呼び出す）
  /// @param deltaTime 経過時間（秒）
  void UpdateTime(float deltaTime);

  /// @brief 描画実行（スタックされた全エフェクトを適用）
  /// @param cmdList       コマンドリスト
  /// @param renderTexture 描画ソースとなるレンダーテクスチャ（SRV 状態であること）
  /// @param dstTexture    最終出力先（nullptr ならバックバッファ）
  void Draw(ID3D12GraphicsCommandList *cmdList,
            const RenderTexture &renderTexture,
            RenderTexture *dstTexture = nullptr);

  /// @brief プロジェクション逆行列を設定する（DepthBasedOutline / Underwater / Caustics / LightShaft / SSAO で使用）
  /// @param projInv16 4x4 行列（float 16 個）
  void SetProjectionInverse(const float* projInv16);

  /// @brief ビュー逆行列（カメラのワールド行列）を設定する
  /// @details Caustics / LightShaft がビュー空間座標をワールド空間へ戻すために使用する。
  ///          RenderContext::SetCamera から毎フレーム供給される。
  /// @param viewInv16 4x4 行列（float 16 個）
  void SetViewInverse(const float* viewInv16);

  // ===========================
  // 単体エフェクト（後方互換）
  // ===========================

  /// @brief アウトラインの色を設定する
  /// @param color 色（RGBA の 4 要素）
  void SetOutlineColor(const float color[4]);

  /// @brief アウトラインの太さ（検出の重み）を設定する
  /// @param weight 検出の重み
  void SetOutlineWeight(float weight);

  /// @brief アウトラインのピクセル幅（太さ）を設定する
  /// @param thickness 太さ（ピクセル）
  void SetOutlineThickness(float thickness);

  /// @brief アウトラインの描画モードを設定する
  /// @param mode 0 = 両側、1 = 外側、2 = 内側
  void SetOutlineMode(int mode);

  // ===========================
  // Dissolve パラメータ
  // ===========================

  /// @brief Dissolve の閾値を設定する
  /// @param threshold 閾値（0.0〜1.0。1.0 で全面が抜ける）
  void SetDissolveThreshold(float threshold);

  /// @brief Dissolve の縁の発光色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetDissolveEdgeColor(float r, float g, float b);

  /// @brief Dissolve のベースカラー（抜けた部分の色）を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a 不透明度
  void SetDissolveBaseColor(float r, float g, float b, float a);

  /// @brief Dissolve の縁として光らせる幅を設定する
  /// @param range 縁の幅（閾値の差）
  void SetDissolveEdgeRange(float range);

  /// @brief Dissolve に使うノイズテクスチャを選ぶ
  /// @param index ノイズテクスチャのインデックス（テクスチャ数で剰余を取る）
  /// @note ノイズテクスチャが 1 枚も無い間は何もしない。
  void SetDissolveNoiseIndex(int index);

  /// @brief Resources/noise 内の PNG を読み込み、Dissolve のノイズテクスチャリストを初期化する
  /// @note 2 回目以降の呼び出しは何もしない。RC::Init の後に呼ぶこと。
  void InitDissolveNoiseTextures();

  // ===========================
  // RandomNoise パラメータ
  // ===========================

  /// @brief RandomNoise の強度を設定する
  /// @param intensity 強度（0.0〜1.0）
  void SetRandomNoiseIntensity(float intensity);

  /// @brief RandomNoise の色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetRandomNoiseColor(float r, float g, float b);

  // ===========================
  // MaskOutline パラメータ
  // ===========================

  /// @brief マスクアウトラインの色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a 合成の最大不透明度
  void SetMaskOutlineColor(float r, float g, float b, float a = 1.0f);

  /// @brief マスクアウトラインの太さを設定する
  /// @param thickness 太さ（ピクセル。1.0〜4.0）
  void SetMaskOutlineThickness(float thickness);

  /// @brief マスクアウトラインの強さを設定する
  /// @param strength 強さ（0.0〜1.0）
  void SetMaskOutlineStrength(float strength);

  /// @brief 輪郭を出さない矩形の最大数（HUD の領域）
  static constexpr int kMaxOutlineExclusions = 16;

  /// @brief 輪郭を出さない矩形（HUD の領域）をまとめて設定する
  /// @details `DepthBasedOutline`（画面全体の輪郭）と `MaskOutline`（対象の強調）の両方に
  ///          効く。どちらも 2D まで描き終えた最終画に掛かるポストエフェクトなので、
  ///          何もしないと HP バーやカウンターの上に輪郭が浮いてしまう。
  ///          UV への変換はここで行うので、呼び出し側は画面ピクセルのまま渡してよい。
  /// @param rectsPixels (minX, minY, maxX, maxY) を count 個並べた配列（画面ピクセル座標）
  /// @param count       矩形の数（0 で全解除。kMaxOutlineExclusions を超えた分は捨てる）
  void SetOutlineExclusions(const float *rectsPixels, int count);

  // ===========================
  // Bloom パラメータ
  // ===========================

  /// @brief Bloom で光として拾う輝度の閾値を設定する
  /// @param threshold 閾値（0.0〜1.0）
  void SetBloomThreshold(float threshold);

  /// @brief Bloom の加算強度を設定する
  /// @param intensity 強度（0.0 で無効）
  void SetBloomIntensity(float intensity);

  /// @brief Bloom のにじみの広がりを設定する
  /// @param radius 広がり（ピクセル。1.0〜16.0 目安）
  void SetBloomRadius(float radius);

  /// @brief Bloom の閾値付近の柔らかさを設定する
  /// @param knee 柔らかさ（0.0 でスパッと切る）
  void SetBloomKnee(float knee);

  // ===========================
  // SSAO パラメータ
  // ===========================

  /// @brief SSAO のサンプリング半径を設定する
  /// @param radius 半径（メートル）
  void SetSsaoRadius(float radius);

  /// @brief SSAO の効きの強さを設定する
  /// @param intensity 強さ（0.0 で無効）
  void SetSsaoIntensity(float intensity);

  /// @brief SSAO の自己遮蔽対策の下駄を設定する
  /// @param bias 下駄（メートル）
  void SetSsaoBias(float bias);

  /// @brief SSAO のコントラストを設定する
  /// @param power pow の指数
  void SetSsaoPower(float power);

  // ===========================
  // ColorGrade パラメータ
  // ===========================

  /// @brief 露出を設定する
  /// @param ev 露出（EV。0 で等倍、+1 で 2 倍）
  void SetGradeExposure(float ev);

  /// @brief 露出に一時的に足すオフセットを設定する
  /// @details シーンが決めた露出（SetGradeExposure）とは別に持ち、シェーダへは和を渡す。
  ///          水面の出入りのフラッシュのように、持ち主の違う短い演出が
  ///          シーン側の露出を上書きせずに重ねられるようにするためのもの。
  ///          ClearEffects で 0 に戻る。
  /// @param ev オフセット（EV）
  void SetGradeExposureOffset(float ev);

  /// @brief コントラストを設定する
  /// @param contrast コントラスト（1.0 で素通し）
  void SetGradeContrast(float contrast);

  /// @brief 彩度を設定する
  /// @param saturation 彩度（1.0 で素通し、0 で白黒）
  void SetGradeSaturation(float saturation);

  /// @brief 色温度を設定する
  /// @param temperature 色温度（-1.0 寒色〜+1.0 暖色）
  void SetGradeTemperature(float temperature);

  /// @brief 色偏りを設定する
  /// @param tint 色偏り（-1.0 緑〜+1.0 マゼンタ）
  void SetGradeTint(float tint);

  /// @brief 全体に掛けるカラーフィルタを設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetGradeColorFilter(float r, float g, float b);

  /// @brief グレーディングの適用率を設定する
  /// @param lerpFactor 適用率（0.0〜1.0）
  void SetGradeLerpFactor(float lerpFactor);

  /// @brief マスク RT の SRV を渡す（毎フレーム、ポストプロセスの Draw より前に呼ぶ）
  /// @details 渡されていないフレームは MaskOutline パスを素通し（コピー）にする。
  ///          解放済み・未初期化のディスクリプタを読ませないための安全弁。
  /// @param srv GPU ディスクリプタハンドル（ptr == 0 ならマスク未描画として扱う）
  void SetMaskSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { maskSrv_ = srv; }

  // ===========================
  // Underwater パラメータ
  // ===========================

  /// @brief Underwater の色味（ティントカラー）を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a 不透明度
  void SetUnderwaterTintColor(float r, float g, float b, float a);

  /// @brief Underwater の歪みの強さを設定する
  /// @param force 歪みの強さ（UV のずれ量）
  void SetUnderwaterDistortionForce(float force);

  // ===========================
  // Caustics パラメータ
  // ===========================

  /// @brief Caustics の発光色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetCausticsColor(float r, float g, float b);

  /// @brief Caustics 全体の強さを設定する
  /// @param intensity 強さ（0.0〜）
  void SetCausticsIntensity(float intensity);

  /// @brief 網目の密度を設定する
  /// @param scale ワールド 1 単位あたりのタイル数
  /// @note LightShaft にも同じ値を反映する（SyncLightShaftWithCaustics）。
  void SetCausticsScale(float scale);

  /// @brief 網目のうねる速度を設定する
  /// @param speed 速度
  /// @note LightShaft にも同じ値を反映する（SyncLightShaftWithCaustics）。
  void SetCausticsSpeed(float speed);

  /// @brief 水面のワールド Y 座標と減衰距離を設定する
  /// @param waterHeight  水面の高さ
  /// @param fadeDistance この距離だけ潜ると caustics が消えきる
  /// @note 水面の高さは LightShaft にも反映する（SyncLightShaftWithCaustics）。
  void SetCausticsWater(float waterHeight, float fadeDistance);

  /// @brief 網目のコントラストを設定する
  /// @param contrast コントラスト（大きいほど細くシャープになる）
  void SetCausticsContrast(float contrast);

  /// @brief 色収差量を設定する
  /// @param offset 色収差量（0.0 で無効）
  void SetCausticsChromaticOffset(float offset);

  /// @brief 上向き法線への偏りを設定する
  /// @param bias 1.0 = 上向きの面（床）にだけ落ちる、0.0 = 面の向きを無視
  void SetCausticsUpwardBias(float bias);

  /// @brief カメラからの距離減衰の範囲を設定する
  /// @param start 減衰の開始距離
  /// @param end   減衰の終了距離
  void SetCausticsDistanceFade(float start, float end);

  /// @brief 水中ブレンド率を設定する
  /// @param lerpFactor 0.0 = 適用しない〜1.0 = 完全適用
  /// @note Underwater の lerpFactor と揃えて使うことを想定している。
  void SetCausticsLerpFactor(float lerpFactor);

  // ===========================
  // LightShaft パラメータ
  // ===========================

  /// @brief 光柱の色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetLightShaftColor(float r, float g, float b);

  /// @brief 光柱全体の強さを設定する
  /// @param intensity 強さ
  void SetLightShaftIntensity(float intensity);

  /// @brief 深さによる指数減衰の強さを設定する
  /// @param density 減衰の強さ（大きいほど浅い層だけ光る）
  void SetLightShaftDensity(float density);

  /// @brief 光柱の断面のコントラストを設定する
  /// @param contrast コントラスト
  void SetLightShaftContrast(float contrast);

  /// @brief バンディング対策のディザ量を設定する
  /// @param strength ディザ量（0.0〜1.0）
  void SetLightShaftDitherStrength(float strength);

  /// @brief レイマーチの最大距離を設定する
  /// @param maxDistance 最大距離
  void SetLightShaftMaxDistance(float maxDistance);

  /// @brief レイマーチのステップ数を設定する
  /// @param sampleCount ステップ数（1 未満は 1 に丸める）
  /// @note 性能の主要因。FPS が落ちる場合はまずここを下げる。
  void SetLightShaftSampleCount(int sampleCount);

  /// @brief 水中ブレンド率を設定する
  /// @param lerpFactor 0.0 = 適用しない〜1.0 = 完全適用
  void SetLightShaftLerpFactor(float lerpFactor);

  /// @brief 光の進む向き（ワールド座標。DirectionalLight の direction と同じ向き）を設定する
  /// @details 光柱はこの向きに傾く。内部で正規化し、y が -0.2 より上なら -0.2 に補正して
  ///          必ず下向きの傾きを持たせる（長さがほぼ 0 なら真下にする）。
  /// @param x 向きの X 成分
  /// @param y 向きの Y 成分
  /// @param z 向きの Z 成分
  void SetLightShaftSunDirection(float x, float y, float z);

  /// @brief 光柱全体の強さを取得する
  /// @return 強さ
  float GetLightShaftIntensity() const { return lightShaftIntensity_; }
  /// @brief 深さによる指数減衰の強さを取得する
  /// @return 減衰の強さ
  float GetLightShaftDensity() const { return lightShaftDensity_; }
  /// @brief 光柱の断面のコントラストを取得する
  /// @return コントラスト
  float GetLightShaftContrast() const { return lightShaftContrast_; }
  /// @brief Caustics 全体の強さを取得する
  /// @return 強さ
  float GetCausticsIntensity() const { return causticsIntensity_; }

  /// @brief 模様の密度・速度・水面高さを Caustics と同期させる
  /// @details 光柱の断面と床の網目は同じパターンを共有しているため、
  ///          scale / speed / waterHeight がズレると位置が合わなくなる。
  ///          Caustics 側の現在値をそのまま LightShaft へコピーする。
  void SyncLightShaftWithCaustics();

  // ===========================
  // ScreenDroplets パラメータ
  // ===========================

  /// @brief レンズ水滴の強度を設定する
  /// @param intensity 0.0 = 消えきった状態〜1.0 = フル
  void SetScreenDropletsIntensity(float intensity);

  /// @brief 水滴が流れ落ちる速度を設定する
  /// @param speed 速度
  void SetScreenDropletsSpeed(float speed);

  /// @brief 水滴による背景 UV 屈折（歪み）の強さを設定する
  /// @param distortion 歪みの強さ
  void SetScreenDropletsDistortion(float distortion);

  /// @brief 水滴のサイズ・密度（グリッドスケール）を設定する
  /// @param scale グリッドスケール
  void SetScreenDropletsScale(float scale);

  /// @brief 走行風で水滴が画面の外側へ流される強さを設定する
  /// @param wind 0 で重力 / 浮力だけ、大きいほど外へ流れる（既定 0.6）
  void SetScreenDropletsWind(float wind);

  /// @brief レンズ水滴の演出を最初から再生する（AddEffect も行う）
  /// @details 浮上は水膜が中央から割れて引き、残った水滴が流れ出る。
  ///          潜水は水面のラインが下から上へ抜け、レンズに付いた気泡が昇っていく。
  ///          Trigger せずに AddEffect だけした場合は、Speed の符号（負で潜水）から自動で選ぶ。
  /// @param mode 浮上か潜水か
  void TriggerScreenDroplets(ScreenDropletSim::Mode mode);

  /// @brief 残っているレンズ水滴を急いで消す（シーン遷移の直前などに呼ぶ）
  /// @details 一斉には消さず、粒ごとに時間差を付けて seconds 秒以内に痩せて消えきるようにする。
  /// @param seconds 消えきるまでの秒数
  void DrainScreenDroplets(float seconds);

  // ===========================
  // InkOverlay パラメータ
  // ===========================

  /// @brief 同時に貼れる墨の最大数（HLSL 側 kMaxSplats と揃えること）
  static constexpr int kMaxInkSplats = 6;

  /// @struct InkSplat
  /// @brief 墨 1 個ぶんの状態（ホスト側で寿命を管理して毎フレーム流し込む）
  struct InkSplat {
    float centerX = 0.5f;  ///< 中心 UV の X
    float centerY = 0.5f;  ///< 中心 UV の Y
    float radius = 0.3f;   ///< 半径（画面の高さを 1 とした長さ）
    float strength = 1.0f; ///< 強さ（0〜1）。下げると縁から崩れて縮む
    float age = 0.0f;      ///< 貼り付いてからの秒数（垂れ筋が伸びる）
    float seed = 0.0f;     ///< 形の種（0〜1）
  };

  /// @brief 貼り付いている墨をまとめて設定する
  /// @param splats 墨の配列（nullptr なら全消去）
  /// @param count  墨の数（0 で全消去。kMaxInkSplats を超えた分は捨てる）
  void SetInkOverlaySplats(const InkSplat *splats, int count);

  /// @brief 画面全体の濁り（墨が水に溶けた感じ）を設定する
  /// @param murk 濁り（0.0〜1.0）
  void SetInkOverlayMurk(float murk);

  /// @brief 墨の色と、最も濃いところの不透明度を設定する
  /// @param r       赤
  /// @param g       緑
  /// @param b       青
  /// @param opacity 最も濃いところの不透明度（0.0〜1.0）
  void SetInkOverlayColor(float r, float g, float b, float opacity);

  /// @brief 貼り付いている墨の数を取得する
  /// @return 墨の数（0 ならエフェクトを外してよい）
  int GetInkOverlaySplatCount() const { return inkSplatCount_; }

  // ===========================
  // BloodOverlay パラメータ
  // ===========================

  /// @brief 被弾フラッシュの強さを設定する
  /// @details 被弾の瞬間に 1.0 付近を入れ、呼び出し側で短時間に 0 へ落とす。
  /// @param flash 強さ（0.0〜1.0）
  void SetBloodOverlayHitFlash(float flash);

  /// @brief 低 HP 持続（脈動）の強さを設定する
  /// @details HP が閾値を下回っている間だけ入れ続ける。心拍で自動的に脈動する。
  /// @param level 強さ（0.0〜1.0）
  void SetBloodOverlayLowHealth(float level);

  /// @brief 血の色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  void SetBloodOverlayColor(float r, float g, float b);

  /// @brief 最大時に画面の何割まで血が侵食するかを設定する
  /// @param coverage 侵食する割合（0.0〜1.0）
  void SetBloodOverlayCoverage(float coverage);

  /// @brief 飛沫の密度を設定する
  /// @param scale 密度（大きいほど細かい粒になる）
  void SetBloodOverlaySplatterScale(float scale);

  /// @brief 血の下の彩度をどれだけ落とすかを設定する
  /// @param desaturate 彩度低下量（0.0〜1.0）
  void SetBloodOverlayDesaturate(float desaturate);

  /// @brief 脈動の速さを設定する
  /// @param speed 速さ（1.0 で毎秒 1 拍）
  void SetBloodOverlayPulseSpeed(float speed);

  /// @brief 飛沫パターンの種を直接指定する
  /// @details 通常は `RerollBloodOverlaySplatter()` を使う。特定の形を再現したいとき用。
  /// @param seed 種（0.0〜1.0。範囲外は小数部だけ使う）
  /// @note シェーダー側のハッシュは値が大きいほど float の刻みが粗くなり、
  ///       パターンの種類が減る。だから種は必ず [0, 1) に折り返して持つ。
  void SetBloodOverlaySeed(float seed);

  /// @brief 飛沫パターンを次の種へ進める
  /// @details パターンは time に依存しないので、被弾のたびにこれを呼ばないと
  ///          毎回まったく同じ形の血が出る。種はここが唯一の持ち主。
  void RerollBloodOverlaySplatter();

  // ===========================
  // VisionBleed パラメータ
  // ===========================

  /// @brief 意識が遠のく視界の強さをまとめて設定する
  /// @details b0 のルート定数で渡すので CBuffer は持たない。毎フレーム呼んでよい。
  /// @param bleed 周辺からの滲み（0.0 でなし 〜 1.0 で中央まで滲む）
  /// @param ghost 二重像のずれ（UV。画面幅に対する割合。0 でなし。0.02 で 720p 時に約 26px）
  /// @param pigment 濡れた領域の色ムラの強さ（0.0〜1.0）
  void SetVisionBleed(float bleed, float ghost, float pigment);

  /// @brief ポストエフェクトを 1 つだけ設定する
  /// @details 既存のエフェクトスタックをクリアして、指定されたエフェクトのみを設定する。
  /// @param type 設定するエフェクトの種類（None でエフェクトなし）
  void SetEffect(PostEffectType type);

  /// @brief 現在適用されている先頭のエフェクトを取得する
  /// @return エフェクトの種類（スタックが空なら None）
  PostEffectType GetEffect() const;

  // ===========================
  // 重ね掛け（スタック操作）
  // ===========================

  /// @brief エフェクトを適用スタックの末尾に追加する
  /// @param type 追加するエフェクトの種類
  /// @note すでに同じ種類のエフェクトが含まれている場合、または None の場合は追加しない。
  void AddEffect(PostEffectType type);

  /// @brief エフェクトを適用スタックから除去する
  /// @param type 除去するエフェクトの種類
  void RemoveEffect(PostEffectType type);

  /// @brief 全てのエフェクトを解除する
  /// @note 一時的な露出オフセット（SetGradeExposureOffset）も 0 に戻す。
  void ClearEffects();

  /// @brief エフェクトの適用順を 1 つ入れ替える
  /// @details 適用順は結果に影響する。特に Underwater は UV を歪めるため、
  ///          Caustics や LightShaft より後ろに置かないと模様の位置がズレる。
  /// @param index 動かすエフェクトの現在の位置
  /// @param offset -1 で前へ、+1 で後ろへ
  /// @return 実際に入れ替わったら true（範囲外なら false）
  bool MoveEffect(size_t index, int offset);

  /// @brief 指定したエフェクトが現在有効かどうかを確認する
  /// @param type 確認するエフェクトの種類
  /// @return 有効なら true
  bool HasEffect(PostEffectType type) const;

  /// @brief 現在有効なエフェクトのリストを取得する
  /// @return 適用順に並んだエフェクトの配列
  const std::vector<PostEffectType> &GetEffects() const {
    return activeEffects_;
  }

  /// @brief ImGui を使用したデバッグ用 UI を表示する
  /// @param label ウィンドウまたはヘッダーのラベル名
  void DrawImGui(const char *label = "PostEffect");

private:
  /// @brief 1 パス分のフルスクリーン描画を実行する
  /// @param cmdList    コマンドリスト
  /// @param srcSRV     描画元テクスチャの SRV ハンドル
  /// @param pipeline   使用するグラフィックスパイプライン（無効なら描画せずログを出す）
  /// @param effectType エフェクトの種類（定数バッファ等の切り替え用）
  /// @note 描画先（RTV・ビューポート）は呼び出し側で設定しておく。
  void DrawSinglePass(ID3D12GraphicsCommandList *cmdList,
                      D3D12_GPU_DESCRIPTOR_HANDLE srcSRV,
                      GraphicsPipeline *pipeline,
                      PostEffectType effectType = PostEffectType::None);

  /// @brief エフェクト種別に対応するパイプラインを取得する
  /// @param type エフェクトの種類
  /// @return 対応するパイプラインへのポインタ（未登録なら nullptr）
  GraphicsPipeline *GetPipelineForEffect(PostEffectType type);

  /// @brief マルチパス用のピンポンテクスチャが必要な時に生成されることを保証する
  /// @note 2 回目以降の呼び出しは何もしない。
  void EnsurePingPongTextures();

private:
  Dx12Core *dxCore_ = nullptr;                 ///< DirectX12 コアシステム
  PipelineManager *pipelineManager_ = nullptr; ///< パイプライン管理クラス

  uint32_t width_ = 0;  ///< 画面解像度（幅）
  uint32_t height_ = 0; ///< 画面解像度（高さ）

  // 各エフェクト用パイプライン
  GraphicsPipeline *pipelineCopy_ = nullptr;              ///< そのままコピー（None・素通し用）
  GraphicsPipeline *pipelineGrayscale_ = nullptr;         ///< Grayscale 用
  GraphicsPipeline *pipelineSepia_ = nullptr;             ///< Sepia 用
  GraphicsPipeline *pipelineVignette_ = nullptr;          ///< Vignette 用
  GraphicsPipeline *pipelineBoxFilter_ = nullptr;         ///< BoxFilter 用
  GraphicsPipeline *pipelineGaussianFilter_ = nullptr;    ///< GaussianFilter 用
  GraphicsPipeline *pipelineDepthBasedOutline_ = nullptr; ///< DepthBasedOutline 用
  GraphicsPipeline *pipelineRadialBlur_ = nullptr;        ///< RadialBlur 用
  GraphicsPipeline *pipelineDissolve_ = nullptr;          ///< Dissolve 用
  GraphicsPipeline *pipelineRandom_ = nullptr;            ///< RandomNoise 用
  GraphicsPipeline *pipelineMaskOutline_ = nullptr;       ///< MaskOutline 用
  GraphicsPipeline *pipelineSsao_ = nullptr;              ///< Ssao 用
  GraphicsPipeline *pipelineBloom_ = nullptr;             ///< Bloom 用
  GraphicsPipeline *pipelineColorGrade_ = nullptr;        ///< ColorGrade 用
  GraphicsPipeline *pipelineFxaa_ = nullptr;              ///< Fxaa 用
  GraphicsPipeline *pipelineUnderwater_ = nullptr;        ///< Underwater 用
  GraphicsPipeline *pipelineCaustics_ = nullptr;          ///< Caustics 用
  GraphicsPipeline *pipelineLightShaft_ = nullptr;        ///< LightShaft 用
  GraphicsPipeline *pipelineScreenDroplets_ = nullptr;    ///< ScreenDroplets 用
  GraphicsPipeline *pipelineBloodOverlay_ = nullptr;      ///< BloodOverlay 用
  GraphicsPipeline *pipelineInkOverlay_ = nullptr;        ///< InkOverlay 用
  GraphicsPipeline *pipelineVisionBleed_ = nullptr;       ///< VisionBleed 用

  std::vector<PostEffectType> activeEffects_; ///< アクティブなエフェクトスタック（適用順）

  // CBuffer（DepthBasedOutline 等で使用）
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferMaterial_; ///< 旧方式の定数バッファ（現在のバインドは dynMaterial_ を使う）
  // NOTE: DepthBasedOutline.PS.hlsl の cbuffer Material と 1:1 で対応する。
  //       末尾の excludeCount 以降は「UI の上に輪郭を出さない」ための領域
  //       （SetOutlineExclusions が流し込む）。増減時は HLSL 側も必ず合わせること。
  /// @struct MaterialData
  /// @brief DepthBasedOutline 用の定数バッファ
  struct MaterialData {
    float projectionInverse[16]; ///< プロジェクション逆行列
    float outlineColor[4];       ///< 輪郭の色
    float outlineWeight;         ///< 検出の重み
    float outlineThickness;      ///< 太さ（ピクセル）
    int outlineMode;             ///< 0 = 両側、1 = 外側、2 = 内側
    float padding;               ///< 16 バイト境界合わせ
    int excludeCount;            ///< 除外矩形の数
    float padding2[3];           ///< 16 バイト境界合わせ
    float excludeRects[kMaxOutlineExclusions][4]; ///< (minU, minV, maxU, maxV)
  };
  MaterialData* mappedMaterial_ = nullptr; ///< 書き込み先（dynMaterial_ の CPU 側）
  RC::DynamicCB<MaterialData> dynMaterial_; ///< mappedMaterial_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  SRVManager::Handle depthSrv_{}; ///< 深度バッファの SRV（DepthBasedOutline / SSAO / Underwater / Caustics / LightShaft で共有。遅延作成）

  float outlineColor_[4] = {0.0f, 0.0f, 0.0f, 1.0f}; ///< 輪郭の色
  float outlineWeight_ = 1.0f;                       ///< 検出の重み
  float outlineThickness_ = 1.0f;                    ///< 太さ（ピクセル）
  int outlineMode_ = 0;                              ///< 0 = 両側、1 = 外側、2 = 内側

  // マルチパス用ピンポンテクスチャ
  std::unique_ptr<RenderTexture> pingPongA_; ///< ピンポン A
  std::unique_ptr<RenderTexture> pingPongB_; ///< ピンポン B
  bool pingPongInitialized_ = false;         ///< ピンポンテクスチャを作成済みか

  float grayscaleLerpFactor_ = 1.0f; ///< Grayscale のブレンド係数（1.0 = 完全白黒、0.0 = カラー）

  int boxFilterK_ = 1; ///< BoxFilter のカーネル半径（1 = 3x3、2 = 5x5、...）
  int gaussianFilterK_ = 1; ///< GaussianFilter のカーネル半径
  float gaussianSigma_ = 2.0f; ///< GaussianFilter の標準偏差

  // RadialBlur パラメータ
  struct {
    float x = 0.5f; ///< 中心 UV の X
    float y = 0.5f; ///< 中心 UV の Y
  } radialBlurCenter_; ///< ブラーの中心（UV）
  float radialBlurWidth_ = 0.01f; ///< ブラーの幅
  int radialBlurSamples_ = 10;    ///< サンプル数

  // Dissolve パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferDissolve_; ///< 旧方式の定数バッファ（現在のバインドは dynDissolve_ を使う）
  /// @struct DissolveData
  /// @brief Dissolve 用の定数バッファ
  struct DissolveData {
    float edgeColor[4] = {1.0f, 0.4f, 0.3f, 1.0f}; ///< 縁の発光色
    float baseColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; ///< 抜けた部分の背景色
    float threshold = 0.0f;                          ///< 閾値
    float edgeRange = 0.03f;                         ///< 縁の検出幅
    float padding[2] = {0.0f, 0.0f};                 ///< 16 バイト境界合わせ
  };
  DissolveData *mappedDissolve_ = nullptr; ///< 書き込み先（dynDissolve_ の CPU 側）
  RC::DynamicCB<DissolveData> dynDissolve_; ///< mappedDissolve_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float dissolveEdgeColor_[3] = {1.0f, 0.4f, 0.3f};       ///< 縁の発光色
  float dissolveBaseColor_[4] = {0.0f, 0.0f, 0.0f, 1.0f}; ///< 抜けた部分の色
  float dissolveThreshold_ = 0.0f;                        ///< 閾値
  float dissolveEdgeRange_ = 0.03f;                       ///< 縁の検出幅

  // ノイズテクスチャ管理
  /// @struct NoiseEntry
  /// @brief Dissolve 用ノイズテクスチャ 1 枚分の情報
  struct NoiseEntry {
    std::string path;                  ///< ファイルパス
    std::string name;                  ///< 表示名（拡張子を除いたファイル名）
    D3D12_GPU_DESCRIPTOR_HANDLE srv{}; ///< テクスチャの SRV
  };
  std::vector<NoiseEntry> dissolveNoiseTextures_; ///< 読み込んだノイズテクスチャ一覧
  int dissolveNoiseIndex_ = 0;                    ///< 使用中のノイズテクスチャ
  bool dissolveNoiseInitialized_ = false;         ///< InitDissolveNoiseTextures を実行済みか

  // RandomNoise パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferRandom_; ///< 旧方式の定数バッファ（現在のバインドは dynRandom_ を使う）
  /// @struct RandomNoiseData
  /// @brief RandomNoise 用の定数バッファ
  struct RandomNoiseData {
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< ノイズの色
    float time = 0.0f;                         ///< 経過時間
    float intensity = 1.0f;                    ///< 強度
    float padding[2] = {0.0f, 0.0f};           ///< 16 バイト境界合わせ
  };
  RandomNoiseData *mappedRandom_ = nullptr; ///< 書き込み先（dynRandom_ の CPU 側）
  RC::DynamicCB<RandomNoiseData> dynRandom_; ///< mappedRandom_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float randomTime_ = 0.0f;                   ///< 経過時間（各エフェクトの time の元。UpdateTime で進める）
  float randomIntensity_ = 1.0f;              ///< RandomNoise の強度
  float randomColor_[3] = {1.0f, 1.0f, 1.0f}; ///< RandomNoise の色

  // MaskOutline パラメータ
  // NOTE: DepthBasedOutline（画面全体の輪郭）とは別の CBuffer を持つ。
  //       共用にすると「全体は黒く細く／対象だけ白く太く」を同時に出せない。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferMaskOutline_; ///< 旧方式の定数バッファ（現在のバインドは dynMaskOutline_ を使う）
  // NOTE: HLSL の cbuffer は 16 バイト単位で詰まる。
  //       color(16) + thickness/strength/excludeCount/padding(16) の後に
  //       float4 の配列が並ぶ並びで MaskOutline.PS.hlsl と一致させている。
  /// @struct MaskOutlineData
  /// @brief MaskOutline 用の定数バッファ
  struct MaskOutlineData {
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 輪郭の色（a は合成の最大不透明度）
    float thickness = 2.0f;                    ///< 太さ（ピクセル）
    float strength = 1.0f;                     ///< 強さ
    int excludeCount = 0;                      ///< 除外矩形の数
    float padding = 0.0f;                      ///< 16 バイト境界合わせ
    float excludeRects[kMaxOutlineExclusions][4] = {}; ///< (minU, minV, maxU, maxV)
  };
  MaskOutlineData *mappedMaskOutline_ = nullptr; ///< 書き込み先（dynMaskOutline_ の CPU 側）
  RC::DynamicCB<MaskOutlineData> dynMaskOutline_; ///< mappedMaskOutline_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float maskOutlineColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 輪郭の色
  float maskOutlineThickness_ = 2.0f;                    ///< 太さ（ピクセル）
  float maskOutlineStrength_ = 1.0f;                     ///< 強さ
  D3D12_GPU_DESCRIPTOR_HANDLE maskSrv_{}; ///< マスク RT の SRV（RenderContext が毎フレーム渡す）

  // Bloom パラメータ（b0 のルート定数 4 つに収まるので CBuffer は持たない）
  float bloomThreshold_ = 0.75f; ///< 光として拾う輝度の閾値
  float bloomIntensity_ = 0.85f; ///< 加算強度
  float bloomRadius_ = 4.0f;     ///< にじみの広がり（ピクセル）
  float bloomKnee_ = 0.35f;      ///< 閾値付近の柔らかさ

  // VisionBleed パラメータ（b0 のルート定数 4 つ = bleed / ghost / pigment / time に収まるので CBuffer は持たない）
  float visionBleed_ = 0.0f; ///< 周辺からの滲み（0〜1）
  float visionGhost_ = 0.0f; ///< 二重像のずれ（UV）
  float visionPigment_ = 0.0f; ///< 濡れた領域の色ムラの強さ（0〜1）

  // SSAO パラメータ
  // NOTE: projectionInverse を含むので専用 CBuffer。行列は SetProjectionInverse が毎フレーム流す。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferSsao_; ///< 旧方式の定数バッファ（現在のバインドは dynSsao_ を使う）
  /// @struct SsaoData
  /// @brief SSAO 用の定数バッファ
  struct SsaoData {
    float projectionInverse[16] = {}; ///< プロジェクション逆行列
    float radius = 0.5f;              ///< サンプリング半径（メートル）
    float intensity = 0.6f;           ///< 効きの強さ
    float bias = 0.02f;               ///< 自己遮蔽対策の下駄（メートル）
    float power = 1.0f;               ///< コントラスト（pow の指数）
  };
  SsaoData *mappedSsao_ = nullptr; ///< 書き込み先（dynSsao_ の CPU 側）
  RC::DynamicCB<SsaoData> dynSsao_; ///< mappedSsao_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float ssaoRadius_ = 0.5f;    ///< サンプリング半径（メートル）
  float ssaoIntensity_ = 0.6f; ///< 効きの強さ
  float ssaoBias_ = 0.02f;     ///< 自己遮蔽対策の下駄（メートル）
  float ssaoPower_ = 1.0f;     ///< コントラスト（pow の指数）

  // ColorGrade パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferColorGrade_; ///< 旧方式の定数バッファ（現在のバインドは dynColorGrade_ を使う）
  /// @struct ColorGradeData
  /// @brief ColorGrade 用の定数バッファ
  struct ColorGradeData {
    float colorFilter[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< カラーフィルタ
    float exposure = 0.0f;                           ///< 露出（EV。オフセット込みの和）
    float contrast = 1.0f;                           ///< コントラスト
    float saturation = 1.0f;                         ///< 彩度
    float temperature = 0.0f;                        ///< 色温度
    float tint = 0.0f;                               ///< 色偏り
    float lerpFactor = 1.0f;                         ///< 適用率
    float padding[2] = {0.0f, 0.0f};                 ///< 16 バイト境界合わせ
  };
  ColorGradeData *mappedColorGrade_ = nullptr; ///< 書き込み先（dynColorGrade_ の CPU 側）
  RC::DynamicCB<ColorGradeData> dynColorGrade_; ///< mappedColorGrade_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float gradeColorFilter_[4] = {1.0f, 1.0f, 1.0f, 1.0f}; ///< カラーフィルタ
  float gradeExposure_ = 0.0f;       ///< シーンが決めた露出（EV）
  float gradeExposureOffset_ = 0.0f; ///< 一時的な露出の上乗せ（SetGradeExposureOffset）
  float gradeContrast_ = 1.0f;       ///< コントラスト
  float gradeSaturation_ = 1.0f;     ///< 彩度
  float gradeTemperature_ = 0.0f;    ///< 色温度
  float gradeTint_ = 0.0f;           ///< 色偏り
  float gradeLerpFactor_ = 1.0f;     ///< 適用率

  // Underwater パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferUnderwater_; ///< 旧方式の定数バッファ（現在のバインドは dynUnderwater_ を使う）
  /// @struct UnderwaterData
  /// @brief Underwater 用の定数バッファ
  struct UnderwaterData {
    float projectionInverse[16];                   ///< プロジェクション逆行列
    float tintColor[4] = {0.2f, 0.5f, 1.0f, 1.0f}; ///< 色味
    float fogColor[4] = {0.0f, 0.3f, 0.6f, 1.0f};  ///< フォグの色
    float time = 0.0f;                             ///< 経過時間
    float distortionForce = 0.004f;                ///< 歪みの強さ
    float fogStart = 10.0f;                        ///< フォグの開始距離
    float fogEnd = 150.0f;                         ///< フォグの終了距離
    float lerpFactor = 1.0f;                       ///< 水中ブレンド率
    float padding[3] = {0.0f, 0.0f, 0.0f};         ///< 16 バイト境界合わせ
  };
  UnderwaterData *mappedUnderwater_ = nullptr; ///< 書き込み先（dynUnderwater_ の CPU 側）
  RC::DynamicCB<UnderwaterData> dynUnderwater_; ///< mappedUnderwater_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float underwaterTintColor_[4] = {0.2f, 0.5f, 1.0f, 1.0f}; ///< 色味
  float underwaterFogColor_[4] = {0.0f, 0.3f, 0.6f, 1.0f};  ///< フォグの色
  float underwaterDistortionForce_ = 0.004f;                ///< 歪みの強さ
  float underwaterFogStart_ = 10.0f;                        ///< フォグの開始距離

  float underwaterFogEnd_ = 150.0f;   ///< フォグの終了距離
  float underwaterLerpFactor_ = 1.0f; ///< 水中ブレンド率（UpdateTime で CB へ送る）

  // Caustics パラメータ
  // NOTE: HLSL 側 cbuffer CausticsParams（b1）と 1:1 で対応する。
  //       全 48 float = 192 byte がちょうど 12 個の float4 行に収まるため
  //       末尾パディングは不要（メンバを増減する際は必ず再確認すること）。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferCaustics_; ///< 旧方式の定数バッファ（現在のバインドは dynCaustics_ を使う）
  /// @struct CausticsData
  /// @brief Caustics 用の定数バッファ
  struct CausticsData {
    float projectionInverse[16];               ///< プロジェクション逆行列
    float viewInverse[16];                     ///< ビュー逆行列
    float causticsColor[4] = {0.75f, 0.95f, 1.0f, 1.0f}; ///< 網目の発光色
    float time = 0.0f;                         ///< 経過時間
    float intensity = 1.5f;                    ///< 全体の強さ
    float scale = 0.05f;                       ///< 網目の密度
    float speed = 0.5f;                        ///< うねる速度
    float contrast = 1.0f;                     ///< 網目のコントラスト
    float chromaticOffset = 2.0f;              ///< 色収差量
    float waterHeight = 150.0f;                  ///< 水面のワールド Y
    float depthFadeDistance = 200.0f;           ///< 水面からの減衰距離
    float upwardBias = 0.5f;                  ///< 上向き法線への偏り
    float distanceFadeStart = 300.0f;           ///< 距離減衰の開始
    float distanceFadeEnd = 150.0f;            ///< 距離減衰の終了
    float lerpFactor = 1.0f;                   ///< 水中ブレンド率
  };
  CausticsData *mappedCaustics_ = nullptr; ///< 書き込み先（dynCaustics_ の CPU 側）
  RC::DynamicCB<CausticsData> dynCaustics_; ///< mappedCaustics_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float causticsColor_[3] = {0.75f, 0.95f, 1.0f}; ///< 網目の発光色
  float causticsIntensity_ = 1.5f;                ///< 全体の強さ
  float causticsScale_ = 0.05f;                   ///< 網目の密度（LightShaft と共有）
  float causticsSpeed_ = 0.5f;                    ///< うねる速度（LightShaft と共有）
  float causticsContrast_ = 1.0f;                 ///< 網目のコントラスト
  float causticsChromaticOffset_ = 2.0f;          ///< 色収差量
  float causticsWaterHeight_ = 150.0f;            ///< 水面のワールド Y（LightShaft と共有）
  float causticsDepthFadeDistance_ = 200.0f;      ///< 水面からの減衰距離
  float causticsUpwardBias_ = 0.5f;               ///< 上向き法線への偏り
  float causticsDistanceFadeStart_ = 300.0f;      ///< 距離減衰の開始
  float causticsDistanceFadeEnd_ = 150.0f;        ///< 距離減衰の終了
  float causticsLerpFactor_ = 1.0f;               ///< 水中ブレンド率（UpdateTime で CB へ送る）

  // LightShaft パラメータ
  // NOTE: HLSL 側 cbuffer LightShaftParams（b1）と 1:1 で対応する。
  //       全 52 float = 208 byte（13 個の float4 行）。sunDir は 13 行目に単独で入る。
  //       末尾の _padding を含めて数が合っているので、メンバを増減する際は
  //       必ずパディングを調整すること。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferLightShaft_; ///< 旧方式の定数バッファ（現在のバインドは dynLightShaft_ を使う）
  /// @struct LightShaftData
  /// @brief LightShaft 用の定数バッファ
  struct LightShaftData {
    float projectionInverse[16];                      ///< プロジェクション逆行列
    float viewInverse[16];                            ///< ビュー逆行列
    float shaftColor[4] = {0.80f, 0.94f, 1.0f, 1.0f}; ///< 光柱の色
    float time = 0.0f;                                ///< 経過時間
    float intensity = 1.0f;                           ///< 全体の強さ
    float scale = 0.05f;                              ///< 模様の密度（Caustics と同期）
    float speed = 0.5f;                              ///< うねる速度（Caustics と同期）
    float contrast = 3.0f;                            ///< 断面のコントラスト
    float waterHeight = 150.0f;                         ///< 水面のワールド Y（Caustics と同期）
    float density = 0.045f;                           ///< 深さによる指数減衰
    float maxDistance = 120.0f;                       ///< レイマーチの最大距離
    int sampleCount = 24;                             ///< ステップ数（性能の主要因）
    float ditherStrength = 1.0f;                      ///< バンディング対策
    float lerpFactor = 1.0f;                          ///< 水中ブレンド率
    float _padding = 0.0f;                            ///< 16 バイト境界合わせ
    float sunDir[4] = {-0.464f, -0.743f, 0.464f, 0.0f};   ///< 光の進む向き（ワールド、正規化済み、y < 0）
  };
  LightShaftData *mappedLightShaft_ = nullptr; ///< 書き込み先（dynLightShaft_ の CPU 側）
  RC::DynamicCB<LightShaftData> dynLightShaft_; ///< mappedLightShaft_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float lightShaftSunDir_[3] = {-0.464f, -0.743f, 0.464f}; ///< 光の進む向き（正規化済み）
  float lightShaftColor_[3] = {0.80f, 0.94f, 1.0f};        ///< 光柱の色
  float lightShaftIntensity_ = 1.0f;                       ///< 全体の強さ
  float lightShaftContrast_ = 3.0f;                        ///< 断面のコントラスト
  float lightShaftDensity_ = 0.045f;                       ///< 深さによる指数減衰
  float lightShaftMaxDistance_ = 120.0f;                   ///< レイマーチの最大距離
  int lightShaftSampleCount_ = 24;                         ///< ステップ数
  float lightShaftDitherStrength_ = 1.0f;                  ///< ディザ量
  float lightShaftLerpFactor_ = 1.0f;                      ///< 水中ブレンド率（UpdateTime で CB へ送る）

  // ScreenDroplets パラメータ
  // NOTE: HLSL 側 cbuffer ScreenDropletsParams（b1）と 1:1 で対応する。
  //       先頭 20 float（float4 × 5 行）＋ 水滴 96 粒 × float4 × 2 = 3152 byte。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferScreenDroplets_; ///< 旧方式の定数バッファ（現在のバインドは dynScreenDroplets_ を使う）
  /// @struct ScreenDropletsData
  /// @brief ScreenDroplets 用の定数バッファ
  struct ScreenDropletsData {
    float time = 0.0f;               ///< 経過時間
    float intensity = 1.0f;          ///< 強度
    float speed = 1.0f;              ///< 流れ落ちる速度
    float distortion = 0.05f;        ///< 屈折（歪み）の強さ
    float scale = 1.5f;              ///< 水滴の大きさ（1.5 で既定）
    float aspectRatio = 1.777f;      ///< アスペクト比（幅 / 高さ）
    float elapsed = 0.0f;            ///< 演出開始からの秒数
    float mode = 0.0f;               ///< 0: 浮上 1: 潜水
    float dropCount = 0.0f;          ///< dropA / dropB の有効数
    float sheetCenterX = 0.888f;     ///< 水膜が割れ始める点 X
    float sheetCenterY = 0.4f;       ///< 水膜が割れ始める点 Y
    float sheetDuration = 1.1f;      ///< 水膜が引ききる / 水面ラインが抜けきる秒数
    float sheetRadiusMax = 1.65f;    ///< 水膜の乾いた領域が届く距離
    float sheetGravityBias = 0.45f;  ///< 上ほど早く乾く度合い
    float invScreenHeight = 1.0f / 720.0f; ///< 1 / 画面の高さ（ピクセル）
    float padding0 = 0.0f;           ///< 16 バイト境界合わせ
    float drainStart = 1.0e3f;       ///< Drain を始めた時刻（elapsed と同じ基準。していなければ大きな値）
    float drainDuration = 1.0f;      ///< Drain で消えきるまでの秒数
    float padding1[2] = {0.0f, 0.0f}; ///< 16 バイト境界合わせ
    float dropA[ScreenDropletSim::kMaxDrops][4] = {}; ///< xy: 中心, z: 半径, w: 後ろ側の伸び率
    float dropB[ScreenDropletSim::kMaxDrops][4] = {}; ///< xy: 進行方向, z: 進行方向の伸び率, w: 直交方向の伸び率
  };
  static_assert(sizeof(ScreenDropletsData) == 80 + ScreenDropletSim::kMaxDrops * 32,
                "ScreenDropletsData は HLSL 側 cbuffer ScreenDropletsParams と同じ並びにすること");
  ScreenDropletsData *mappedScreenDroplets_ = nullptr; ///< 書き込み先（dynScreenDroplets_ の CPU 側）
  RC::DynamicCB<ScreenDropletsData> dynScreenDroplets_; ///< mappedScreenDroplets_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float screenDropletsIntensity_ = 1.0f;     ///< 強度（UpdateTime で CB へ送る）
  float screenDropletsSpeed_ = 1.0f;         ///< 流れ落ちる速度
  float screenDropletsDistortion_ = 0.05f;   ///< 屈折（歪み）の強さ
  float screenDropletsScale_ = 1.5f;         ///< グリッドスケール
  float screenDropletsAspectRatio_ = 1.777f; ///< 画面のアスペクト比（BloodOverlay / InkOverlay にも流す）
  float screenDropletsWind_ = 0.6f;          ///< 走行風で外へ流される強さ
  ScreenDropletSim screenDropletSim_;        ///< 水滴の付着・滑落・合体（UpdateTime で進める）

  /// @brief 水滴の挙動を進め、結果を ScreenDroplets の CB へ書く（UpdateTime から呼ぶ）
  /// @param deltaTime 経過時間（秒）
  void UpdateScreenDroplets(float deltaTime);
  /// @brief 今の設定値から水滴シミュレーションの調整値を作る
  ScreenDropletSim::Params MakeScreenDropletParams() const;

  // BloodOverlay パラメータ
  // NOTE: HLSL 側 cbuffer BloodOverlayParams（b1）と 1:1 で対応する。
  //       全 16 float = 64 byte がちょうど 4 個の float4 行に収まる。
  //       メンバを増減する際は必ず末尾パディングを調整すること。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferBloodOverlay_; ///< 旧方式の定数バッファ（現在のバインドは dynBloodOverlay_ を使う）
  /// @struct BloodOverlayData
  /// @brief BloodOverlay 用の定数バッファ
  struct BloodOverlayData {
    float time = 0.0f;                                  ///< 経過時間（脈動の位相）
    float hitFlash = 0.0f;                              ///< 被弾フラッシュ
    float lowHealth = 0.0f;                             ///< 低 HP 持続
    float pulseSpeed = 1.15f;                           ///< 脈動の速さ
    float bloodColor[4] = {0.62f, 0.03f, 0.04f, 1.0f};  ///< 血の色
    float coverage = 0.40f;                             ///< 侵食する割合
    float splatterScale = 1.6f;                         ///< 飛沫の密度
    float desaturate = 0.65f;                           ///< 彩度低下量
    float aspectRatio = 1.777f;                         ///< アスペクト比
    float seed = 0.0f;                                  ///< 飛沫パターンの種
    float padding[3] = {0.0f, 0.0f, 0.0f};              ///< 16 バイト境界合わせ
  };
  BloodOverlayData *mappedBloodOverlay_ = nullptr; ///< 書き込み先（dynBloodOverlay_ の CPU 側）
  RC::DynamicCB<BloodOverlayData> dynBloodOverlay_; ///< mappedBloodOverlay_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）

  // InkOverlay パラメータ
  // NOTE: HLSL 側 cbuffer InkOverlayParams（b1）と 1:1 で対応する。
  //       ヘッダ 8 float + splatA/B 各 kMaxInkSplats 個の float4 = 224 byte。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferInkOverlay_; ///< 旧方式の定数バッファ（現在のバインドは dynInkOverlay_ を使う）
  /// @struct InkOverlayData
  /// @brief InkOverlay 用の定数バッファ
  struct InkOverlayData {
    float time = 0.0f;                                ///< 経過時間（艶の揺らぎ用）
    float aspectRatio = 1.777f;                       ///< アスペクト比
    int splatCount = 0;                               ///< 貼り付いている墨の数
    float murk = 0.0f;                                ///< 画面全体の濁り
    float inkColor[4] = {0.03f, 0.02f, 0.05f, 0.96f}; ///< 墨の色（a は最も濃いところの不透明度）
    float splatA[kMaxInkSplats][4] = {};              ///< 墨ごとの (centerX, centerY, radius, strength)
    float splatB[kMaxInkSplats][4] = {};              ///< 墨ごとの (age, seed, 未使用, 未使用)
  };
  static_assert(sizeof(InkOverlayData) == 32 + 32 * kMaxInkSplats,
                "InkOverlayData must match cbuffer InkOverlayParams");
  InkOverlayData *mappedInkOverlay_ = nullptr; ///< 書き込み先（dynInkOverlay_ の CPU 側）
  RC::DynamicCB<InkOverlayData> dynInkOverlay_; ///< mappedInkOverlay_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  int inkSplatCount_ = 0;                            ///< 貼り付いている墨の数
  float inkMurk_ = 0.0f;                             ///< 画面全体の濁り
  float inkColor_[4] = {0.03f, 0.02f, 0.05f, 0.96f}; ///< 墨の色と最大不透明度
  // ImGui のテスト用（ゲーム側のスクリプトが毎フレーム上書きする場合がある）
  float inkDebugStrength_ = 1.0f; ///< テスト用の墨の強さ
  float inkDebugAge_ = 1.5f;      ///< テスト用の墨の経過秒数
  float inkDebugRadius_ = 0.30f;  ///< テスト用の墨の半径
  float inkDebugSeed_ = 0.0f;     ///< テスト用の墨の形の種
  float bloodHitFlash_ = 0.0f;                  ///< 被弾フラッシュ（0〜1）
  float bloodLowHealth_ = 0.0f;                 ///< 低 HP 持続（0〜1）
  float bloodPulseSpeed_ = 1.15f;               ///< 脈動の速さ
  float bloodColor_[3] = {0.62f, 0.03f, 0.04f}; ///< 血の色
  float bloodCoverage_ = 0.40f;                 ///< 侵食する割合（0〜1）
  float bloodSplatterScale_ = 1.6f;             ///< 飛沫の密度
  float bloodDesaturate_ = 0.65f;               ///< 彩度低下量（0〜1）
  float bloodSeed_ = 0.0f;                      ///< 飛沫パターンの種（[0, 1) に折り返して持つ）

public:
  /// @brief Underwater の水中ブレンド率を設定する（CB への反映は UpdateTime で行う）
  /// @param lf 0.0 = 適用しない〜1.0 = 完全適用
  void SetUnderwaterLerpFactor(float lf) { underwaterLerpFactor_ = lf; }
  /// @brief Underwater のフォグの色を設定する
  /// @param r 赤
  /// @param g 緑
  /// @param b 青
  /// @param a 不透明度
  void SetUnderwaterFogColor(float r, float g, float b, float a = 1.0f) {
      underwaterFogColor_[0] = r; underwaterFogColor_[1] = g;
      underwaterFogColor_[2] = b; underwaterFogColor_[3] = a;
      if (mappedUnderwater_) {
          mappedUnderwater_->fogColor[0] = r;
          mappedUnderwater_->fogColor[1] = g;
          mappedUnderwater_->fogColor[2] = b;
          mappedUnderwater_->fogColor[3] = a;
      }
  }
  /// @brief Underwater のフォグの距離範囲を設定する
  /// @param start フォグの開始距離
  /// @param end   フォグの終了距離
  void SetUnderwaterFogRange(float start, float end) {
      underwaterFogStart_ = start;
      underwaterFogEnd_ = end;
      if (mappedUnderwater_) {
          mappedUnderwater_->fogStart = start;
          mappedUnderwater_->fogEnd = end;
      }
  }

  /// @brief RadialBlur のブラー幅を設定する
  /// @param w ブラー幅
  void SetRadialBlurWidth(float w) { radialBlurWidth_ = w; }
  /// @brief RadialBlur の中心を設定する
  /// @param x 中心 UV の X
  /// @param y 中心 UV の Y
  void SetRadialBlurCenter(float x, float y) { radialBlurCenter_.x = x; radialBlurCenter_.y = y; }
  /// @brief RadialBlur のサンプル数を設定する
  /// @param samples サンプル数
  void SetRadialBlurSamples(int samples) { radialBlurSamples_ = samples; }
  /// @brief Grayscale のブレンド係数を設定する
  /// @param lf 1.0 = 完全白黒、0.0 = カラー
  void SetGrayscaleLerpFactor(float lf) { grayscaleLerpFactor_ = lf; }
};
