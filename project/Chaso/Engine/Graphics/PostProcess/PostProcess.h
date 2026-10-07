#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include <cassert>
#include <d3d12.h>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include "Dx12/SRVManager/SRVManager.h"

class Dx12Core;
class PipelineManager;
class RenderTexture;
class GraphicsPipeline;

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
  Ssao,       ///< スクリーンスペースAO（深度から接地感の陰を落とす。積む順は最初）
  Bloom,      ///< 明部のにじみ（1パス近似）
  ColorGrade, ///< カラーグレーディング（露出・コントラスト・彩度・色温度）
  Fxaa,       ///< アンチエイリアス（積む順は最後）
  Underwater, ///< 水中エフェクト（UV歪みと青み）
  Caustics,   ///< 水面から差す光の網目模様（深度からワールド座標を復元して投影）
  LightShaft, ///< 水中の降り注ぐ光（レイマーチ型 volumetric light shaft）
  ScreenDroplets, ///< レンズ水滴（水上・水中の遷移や着水時にレンズへ付いて流れる水滴）
  BloodOverlay, ///< 被弾・瀕死のときに画面の周辺へ付く血（手続き型。テクスチャ不要）
  InkOverlay,   ///< タコの墨がレンズに貼り付いて視界を塞ぐ（手続き型。テクスチャ不要）
};

/// @class PostProcess
/// @brief ポストプロセス（RenderTextureの内容を画面に描画する）を管理するクラス
/// @details エフェクトの重ね掛け（マルチパス）に対応しており、ピンポンバッファを用いて複数のエフェクトを順次適用できます。
class PostProcess {
public:
  PostProcess() = default;
  ~PostProcess() = default;

  /// @brief 初期化
  /// @param dxCore DirectX12コアシステムへのポインタ
  /// @param pipelineManager パイプライン管理クラスへのポインタ
  /// @param width 画面解像度（幅）
  /// @param height 画面解像度（高さ）
  /// @brief 初期化
  /// @param dxCore DirectX12コアシステムへのポインタ
  /// @param pipelineManager パイプライン管理クラスへのポインタ
  /// @param width 画面解像度（幅）
  /// @param height 画面解像度（高さ）
  void Initialize(Dx12Core *dxCore, PipelineManager *pipelineManager,
                  uint32_t width, uint32_t height);

  /// @brief 解像度のリサイズ対応
  void Resize(uint32_t width, uint32_t height);

  /// @brief 内部時間を更新する（毎フレーム呼び出す）
  /// @param deltaTime 経過時間
  void UpdateTime(float deltaTime);

  /// @brief 描画実行（スタックされた全エフェクトを適用）
  /// @param cmdList コマンドリスト
  /// @param renderTexture 描画ソースとなるレンダーテクスチャ
  void Draw(ID3D12GraphicsCommandList *cmdList,
            const RenderTexture &renderTexture,
            RenderTexture *dstTexture = nullptr);

  /// @brief プロジェクション逆行列を設定する (DepthBasedOutline等で使用)
  void SetProjectionInverse(const float* projInv16);

  /// @brief ビュー逆行列（カメラのワールド行列）を設定する
  /// @details Caustics がビュー空間座標をワールド空間へ戻すために使用する。
  ///          RenderContext::SetCamera から毎フレーム供給される。
  void SetViewInverse(const float* viewInv16);

  // ===========================
  // 単体エフェクト（後方互換）
  // ===========================

  /// @brief アウトラインの色を設定する
  void SetOutlineColor(const float color[4]);

  /// @brief アウトラインの太さ（検出の重み）を設定する
  void SetOutlineWeight(float weight);

  /// @brief アウトラインのピクセル幅（太さ）を設定する
  void SetOutlineThickness(float thickness);

  /// @brief アウトラインの描画モード（0:両側, 1:外側, 2:内側）を設定する
  void SetOutlineMode(int mode);

  // ===========================
  // Dissolve パラメータ
  // ===========================

  /// @brief Dissolve の閾値を設定する (0.0 ~ 1.0)
  void SetDissolveThreshold(float threshold);

  /// @brief Dissolve の Edge 色を設定する (RGB)
  void SetDissolveEdgeColor(float r, float g, float b);

  /// @brief Dissolve のベースカラー（抜けた部分の色）を設定する (RGBA)
  void SetDissolveBaseColor(float r, float g, float b, float a);

  /// @brief Dissolve の Edge 検出幅を設定する
  void SetDissolveEdgeRange(float range);

  /// @brief Dissolve のノイズテクスチャインデックスを設定する
  void SetDissolveNoiseIndex(int index);

  /// @brief Dissolve のノイズテクスチャリストを初期化する
  void InitDissolveNoiseTextures();

  // ===========================
  // RandomNoise パラメータ
  // ===========================

  /// @brief RandomNoise の強度を設定する (0.0 ~ 1.0)
  void SetRandomNoiseIntensity(float intensity);

  /// @brief RandomNoise の色を設定する (RGB)
  void SetRandomNoiseColor(float r, float g, float b);

  // ===========================
  // MaskOutline パラメータ
  // ===========================

  /// @brief マスクアウトラインの色を設定する (RGBA。a は合成の最大不透明度)
  void SetMaskOutlineColor(float r, float g, float b, float a = 1.0f);

  /// @brief マスクアウトラインの太さ（ピクセル）を設定する (1.0 ~ 4.0)
  void SetMaskOutlineThickness(float thickness);

  /// @brief マスクアウトラインの強さを設定する (0.0 ~ 1.0)
  void SetMaskOutlineStrength(float strength);

  /// @brief 輪郭を出さない矩形の最大数（HUD の領域）
  static constexpr int kMaxOutlineExclusions = 16;

  /// @brief 輪郭を出さない矩形（HUD の領域）をまとめて設定する
  /// @param rectsPixels (minX, minY, maxX, maxY) を count 個並べた配列。画面ピクセル座標
  /// @param count       矩形の数。0 で全解除。kMaxOutlineExclusions を超えた分は捨てる
  /// @details `DepthBasedOutline`（画面全体の輪郭）と `MaskOutline`（対象の強調）の両方に
  ///          効く。どちらも 2D まで描き終えた最終画に掛かるポストエフェクトなので、
  ///          何もしないと HP バーやカウンターの上に輪郭が浮いてしまう。
  ///          UV への変換はここで行うので、呼び出し側は画面ピクセルのまま渡してよい。
  void SetOutlineExclusions(const float *rectsPixels, int count);

  // ===========================
  // Bloom パラメータ
  // ===========================

  /// @brief Bloom で光として拾う輝度の閾値 (0.0 ~ 1.0)
  void SetBloomThreshold(float threshold);

  /// @brief Bloom の加算強度 (0.0 で無効)
  void SetBloomIntensity(float intensity);

  /// @brief Bloom のにじみの広がり（ピクセル。1.0 ~ 16.0 目安）
  void SetBloomRadius(float radius);

  /// @brief Bloom の閾値付近の柔らかさ (0.0 でスパッと切る)
  void SetBloomKnee(float knee);

  // ===========================
  // SSAO パラメータ
  // ===========================

  /// @brief SSAO のサンプリング半径（メートル）
  void SetSsaoRadius(float radius);

  /// @brief SSAO の効きの強さ (0.0 で無効)
  void SetSsaoIntensity(float intensity);

  /// @brief SSAO の自己遮蔽対策の下駄（メートル）
  void SetSsaoBias(float bias);

  /// @brief SSAO のコントラスト（pow の指数）
  void SetSsaoPower(float power);

  // ===========================
  // ColorGrade パラメータ
  // ===========================

  /// @brief 露出（EV。0 で等倍、+1 で 2 倍）
  void SetGradeExposure(float ev);

  /// @brief コントラスト (1.0 で素通し)
  void SetGradeContrast(float contrast);

  /// @brief 彩度 (1.0 で素通し、0 で白黒)
  void SetGradeSaturation(float saturation);

  /// @brief 色温度 (-1.0 寒色 ~ +1.0 暖色)
  void SetGradeTemperature(float temperature);

  /// @brief 色偏り (-1.0 緑 ~ +1.0 マゼンタ)
  void SetGradeTint(float tint);

  /// @brief 全体に掛けるカラーフィルタ (RGB)
  void SetGradeColorFilter(float r, float g, float b);

  /// @brief グレーディングの適用率 (0.0 ~ 1.0)
  void SetGradeLerpFactor(float lerpFactor);

  /// @brief マスクRTのSRVを渡す（毎フレーム、ポストプロセスの Draw より前に呼ぶ）
  /// @param srv GPUディスクリプタハンドル。ptr == 0 ならマスク未描画として扱う
  /// @details 渡されていないフレームは MaskOutline パスを素通し（コピー）にする。
  ///          解放済み・未初期化のディスクリプタを読ませないための安全弁。
  void SetMaskSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { maskSrv_ = srv; }

  // ===========================
  // Underwater パラメータ
  // ===========================

  /// @brief Underwater の Tint Color を設定する (RGBA)
  void SetUnderwaterTintColor(float r, float g, float b, float a);

  /// @brief Underwater の歪みの強さを設定する
  void SetUnderwaterDistortionForce(float force);

  // ===========================
  // Caustics パラメータ
  // ===========================

  /// @brief Caustics の発光色を設定する (RGB)
  void SetCausticsColor(float r, float g, float b);

  /// @brief Caustics 全体の強さを設定する (0.0 ~)
  void SetCausticsIntensity(float intensity);

  /// @brief 網目の密度（ワールド1単位あたりのタイル数）を設定する
  void SetCausticsScale(float scale);

  /// @brief 網目のうねる速度を設定する
  void SetCausticsSpeed(float speed);

  /// @brief 水面のワールドY座標と減衰距離を設定する
  /// @param waterHeight 水面の高さ
  /// @param fadeDistance この距離だけ潜ると caustics が消えきる
  void SetCausticsWater(float waterHeight, float fadeDistance);

  /// @brief 網目のコントラストを設定する（大きいほど細くシャープになる）
  void SetCausticsContrast(float contrast);

  /// @brief 色収差量を設定する (0.0 で無効)
  void SetCausticsChromaticOffset(float offset);

  /// @brief 上向き法線への偏りを設定する
  /// @param bias 1.0 = 上向きの面（床）にだけ落ちる / 0.0 = 面の向きを無視
  void SetCausticsUpwardBias(float bias);

  /// @brief カメラからの距離減衰の範囲を設定する
  void SetCausticsDistanceFade(float start, float end);

  /// @brief 水中ブレンド率を設定する (0.0:適用しない ~ 1.0:完全適用)
  /// @note Underwater の lerpFactor と揃えて使うことを想定している。
  void SetCausticsLerpFactor(float lerpFactor);

  // ===========================
  // LightShaft パラメータ
  // ===========================

  /// @brief 光柱の色を設定する (RGB)
  void SetLightShaftColor(float r, float g, float b);

  /// @brief 光柱全体の強さを設定する
  void SetLightShaftIntensity(float intensity);

  /// @brief 深さによる指数減衰の強さを設定する（大きいほど浅い層だけ光る）
  void SetLightShaftDensity(float density);

  /// @brief 光柱の断面のコントラストを設定する
  void SetLightShaftContrast(float contrast);

  /// @brief バンディング対策のディザ量を設定する (0.0 ~ 1.0)
  void SetLightShaftDitherStrength(float strength);

  /// @brief レイマーチの最大距離を設定する
  void SetLightShaftMaxDistance(float maxDistance);

  /// @brief レイマーチのステップ数を設定する
  /// @note 性能の主要因。FPSが落ちる場合はまずここを下げる。
  void SetLightShaftSampleCount(int sampleCount);

  /// @brief 水中ブレンド率を設定する (0.0:適用しない ~ 1.0:完全適用)
  void SetLightShaftLerpFactor(float lerpFactor);

  /// @brief 光の進む向き（ワールド座標。DirectionalLight の direction と同じ向き）を設定する
  /// @details 光柱はこの向きに傾く。内部で正規化し、y が 0 以上なら真下向きに補正する。
  void SetLightShaftSunDirection(float x, float y, float z);

  /// @brief 模様の密度・速度・水面高さを Caustics と同期させる
  /// @details 光柱の断面と床の網目は同じパターンを共有しているため、
  ///          scale / speed / waterHeight がズレると位置が合わなくなる。
  ///          Caustics 側の現在値をそのまま LightShaft へコピーする。
  void SyncLightShaftWithCaustics();

  // ===========================
  // ScreenDroplets パラメータ
  // ===========================

  /// @brief レンズ水滴の強度を設定する (0.0:消えきった状態 ~ 1.0:フル)
  void SetScreenDropletsIntensity(float intensity);

  /// @brief 水滴が流れ落ちる速度を設定する
  void SetScreenDropletsSpeed(float speed);

  /// @brief 水滴による背景UV屈折（歪み）の強さを設定する
  void SetScreenDropletsDistortion(float distortion);

  /// @brief 水滴のサイズ・密度（グリッドスケール）を設定する
  void SetScreenDropletsScale(float scale);

  // ===========================
  // InkOverlay パラメータ
  // ===========================

  /// @brief 同時に貼れる墨の最大数（HLSL 側 kMaxSplats と揃えること）
  static constexpr int kMaxInkSplats = 6;

  /// @brief 墨 1 個ぶんの状態（ホスト側で寿命を管理して毎フレーム流し込む）
  struct InkSplat {
    float centerX = 0.5f;  ///< 中心 UV
    float centerY = 0.5f;
    float radius = 0.3f;   ///< 半径（画面の高さを 1 とした長さ）
    float strength = 1.0f; ///< 強さ 0..1。下げると縁から崩れて縮む
    float age = 0.0f;      ///< 貼り付いてからの秒数（垂れ筋が伸びる）
    float seed = 0.0f;     ///< 形の種 0..1
  };

  /// @brief 貼り付いている墨をまとめて設定する（count=0 で全消去）
  void SetInkOverlaySplats(const InkSplat *splats, int count);

  /// @brief 画面全体の濁り（墨が水に溶けた感じ）0.0 ~ 1.0
  void SetInkOverlayMurk(float murk);

  /// @brief 墨の色と、最も濃いところの不透明度
  void SetInkOverlayColor(float r, float g, float b, float opacity);

  /// @brief 貼り付いている墨の数（0 ならエフェクトを外してよい）
  int GetInkOverlaySplatCount() const { return inkSplatCount_; }

  // ===========================
  // BloodOverlay パラメータ
  // ===========================

  /// @brief 被弾フラッシュの強さを設定する (0.0 ~ 1.0)
  /// @details 被弾の瞬間に 1.0 付近を入れ、呼び出し側で短時間に 0 へ落とす。
  void SetBloodOverlayHitFlash(float flash);

  /// @brief 低HP持続（脈動）の強さを設定する (0.0 ~ 1.0)
  /// @details HP が閾値を下回っている間だけ入れ続ける。心拍で自動的に脈動する。
  void SetBloodOverlayLowHealth(float level);

  /// @brief 血の色を設定する (RGB)
  void SetBloodOverlayColor(float r, float g, float b);

  /// @brief 最大時に画面の何割まで血が侵食するかを設定する (0.0 ~ 1.0)
  void SetBloodOverlayCoverage(float coverage);

  /// @brief 飛沫の密度を設定する（大きいほど細かい粒になる）
  void SetBloodOverlaySplatterScale(float scale);

  /// @brief 血の下の彩度をどれだけ落とすかを設定する (0.0 ~ 1.0)
  void SetBloodOverlayDesaturate(float desaturate);

  /// @brief 脈動の速さを設定する（1.0 で毎秒1拍）
  void SetBloodOverlayPulseSpeed(float speed);

  /// @brief 飛沫パターンの種を直接指定する (0.0 ~ 1.0。範囲外は小数部だけ使う)
  /// @details 通常は `RerollBloodOverlaySplatter()` を使う。特定の形を再現したいとき用。
  /// @note シェーダー側のハッシュは値が大きいほど float の刻みが粗くなり、
  ///       パターンの種類が減る。だから種は必ず [0, 1) に折り返して持つ。
  void SetBloodOverlaySeed(float seed);

  /// @brief 飛沫パターンを次の種へ進める
  /// @details パターンは time に依存しないので、被弾のたびにこれを呼ばないと
  ///          毎回まったく同じ形の血が出る。種はここが唯一の持ち主。
  void RerollBloodOverlaySplatter();

  /// @brief ポストエフェクトを1つだけ設定する
  /// @details 既存のエフェクトスタックをクリアして、指定されたエフェクトのみを設定します。
  /// @param type 設定するエフェクトの種類（None でエフェクトなし）
  void SetEffect(PostEffectType type);

  /// @brief 現在適用されている先頭のエフェクトを取得する
  /// @return エフェクトの種類（スタックが空なら None）
  PostEffectType GetEffect() const;

  // ===========================
  // 重ね掛け（スタック操作）
  // ===========================

  /// @brief エフェクトを適用スタックに追加する
  /// @note すでに同じ種類のエフェクトが含まれている場合は追加しません。
  /// @param type 追加するエフェクトの種類
  void AddEffect(PostEffectType type);

  /// @brief エフェクトを適用スタックから除去する
  /// @param type 除去するエフェクトの種類
  void RemoveEffect(PostEffectType type);

  /// @brief 全てのエフェクトを解除する
  void ClearEffects();

  /// @brief エフェクトの適用順を1つ入れ替える
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
  /// @brief 1パス分のフルスクリーン描画を実行する
  /// @param cmdList コマンドリスト
  /// @param srcSRV 描画元テクスチャの SRV ハンドル
  /// @param pipeline 使用するグラフィックスパイプライン
  /// @param effectType エフェクトの種類（定数バッファ等の切り替え用）
  void DrawSinglePass(ID3D12GraphicsCommandList *cmdList,
                      D3D12_GPU_DESCRIPTOR_HANDLE srcSRV,
                      GraphicsPipeline *pipeline,
                      PostEffectType effectType = PostEffectType::None);

  /// @brief エフェクト種別に対応するパイプラインを取得する
  /// @param type エフェクトの種類
  /// @return 対応するパイプラインへのポインタ（未登録なら nullptr）
  GraphicsPipeline *GetPipelineForEffect(PostEffectType type);

  /// @brief マルチパス用のピンポンテクスチャが必要な時に生成されることを保証する
  void EnsurePingPongTextures();

private:
  Dx12Core *dxCore_ = nullptr;
  PipelineManager *pipelineManager_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  // 各エフェクト用パイプライン
  GraphicsPipeline *pipelineCopy_ = nullptr;
  GraphicsPipeline *pipelineGrayscale_ = nullptr;
  GraphicsPipeline *pipelineSepia_ = nullptr;
  GraphicsPipeline *pipelineVignette_ = nullptr;
  GraphicsPipeline *pipelineBoxFilter_ = nullptr;
  GraphicsPipeline *pipelineGaussianFilter_ = nullptr;
  GraphicsPipeline *pipelineDepthBasedOutline_ = nullptr;
  GraphicsPipeline *pipelineRadialBlur_ = nullptr;
  GraphicsPipeline *pipelineDissolve_ = nullptr;
  GraphicsPipeline *pipelineRandom_ = nullptr;
  GraphicsPipeline *pipelineMaskOutline_ = nullptr;
  GraphicsPipeline *pipelineSsao_ = nullptr;
  GraphicsPipeline *pipelineBloom_ = nullptr;
  GraphicsPipeline *pipelineColorGrade_ = nullptr;
  GraphicsPipeline *pipelineFxaa_ = nullptr;
  GraphicsPipeline *pipelineUnderwater_ = nullptr;
  GraphicsPipeline *pipelineCaustics_ = nullptr;
  GraphicsPipeline *pipelineLightShaft_ = nullptr;
  GraphicsPipeline *pipelineScreenDroplets_ = nullptr;
  GraphicsPipeline *pipelineBloodOverlay_ = nullptr;
  GraphicsPipeline *pipelineInkOverlay_ = nullptr;

  std::vector<PostEffectType> activeEffects_; ///< アクティブなエフェクトスタック（適用順）

  // CBuffer (DepthBasedOutline等で使用)
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferMaterial_;
  // NOTE: DepthBasedOutline.PS.hlsl の cbuffer Material と 1:1 で対応する。
  //       末尾の excludeCount 以降は「UI の上に輪郭を出さない」ための領域
  //       （SetOutlineExclusions が流し込む）。増減時は HLSL 側も必ず合わせること。
  struct MaterialData {
    float projectionInverse[16];
    float outlineColor[4];
    float outlineWeight;
    float outlineThickness;
    int outlineMode;
    float padding;
    int excludeCount;
    float padding2[3];
    float excludeRects[kMaxOutlineExclusions][4]; ///< (minU, minV, maxU, maxV)
  };
  MaterialData* mappedMaterial_ = nullptr;
  RC::DynamicCB<MaterialData> dynMaterial_; ///< mappedMaterial_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  SRVManager::Handle depthSrv_{};

  float outlineColor_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float outlineWeight_ = 1.0f;
  float outlineThickness_ = 1.0f;
  int outlineMode_ = 0;

  // マルチパス用ピンポンテクスチャ
  std::unique_ptr<RenderTexture> pingPongA_;
  std::unique_ptr<RenderTexture> pingPongB_;
  bool pingPongInitialized_ = false;

  float grayscaleLerpFactor_ = 1.0f; ///< Grayscale のブレンド係数（1.0 = 完全白黒, 0.0 = カラー）

  int boxFilterK_ = 1; ///< BoxFilter のカーネル半径（1 = 3x3, 2 = 5x5, ...）
  int gaussianFilterK_ = 1; ///< GaussianFilter のカーネル半径
  float gaussianSigma_ = 2.0f; ///< GaussianFilter の標準偏差

  // RadialBlur パラメータ
  struct {
    float x = 0.5f;
    float y = 0.5f;
  } radialBlurCenter_;
  float radialBlurWidth_ = 0.01f;
  int radialBlurSamples_ = 10;

  // Dissolve パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferDissolve_;
  struct DissolveData {
    float edgeColor[4] = {1.0f, 0.4f, 0.3f, 1.0f}; // Edge発光色
    float baseColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // 抜けた部分の背景色
    float threshold = 0.0f;                          // 閾値
    float edgeRange = 0.03f;                         // Edge検出幅
    float padding[2] = {0.0f, 0.0f};
  };
  DissolveData *mappedDissolve_ = nullptr;
  RC::DynamicCB<DissolveData> dynDissolve_; ///< mappedDissolve_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float dissolveEdgeColor_[3] = {1.0f, 0.4f, 0.3f};
  float dissolveBaseColor_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float dissolveThreshold_ = 0.0f;
  float dissolveEdgeRange_ = 0.03f;

  // ノイズテクスチャ管理
  struct NoiseEntry {
    std::string path;
    std::string name;
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
  };
  std::vector<NoiseEntry> dissolveNoiseTextures_;
  int dissolveNoiseIndex_ = 0;
  bool dissolveNoiseInitialized_ = false;

  // RandomNoise パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferRandom_;
  struct RandomNoiseData {
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float time = 0.0f;
    float intensity = 1.0f;
    float padding[2] = {0.0f, 0.0f};
  };
  RandomNoiseData *mappedRandom_ = nullptr;
  RC::DynamicCB<RandomNoiseData> dynRandom_; ///< mappedRandom_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float randomTime_ = 0.0f;
  float randomIntensity_ = 1.0f;
  float randomColor_[3] = {1.0f, 1.0f, 1.0f};

  // MaskOutline パラメータ
  // NOTE: DepthBasedOutline（画面全体の輪郭）とは別の CBuffer を持つ。
  //       共用にすると「全体は黒く細く／対象だけ白く太く」を同時に出せない。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferMaskOutline_;
  // NOTE: HLSL の cbuffer は 16 バイト単位で詰まる。
  //       color(16) + thickness/strength/excludeCount/padding(16) の後に
  //       float4 の配列が並ぶ並びで MaskOutline.PS.hlsl と一致させている。
  struct MaskOutlineData {
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float thickness = 2.0f;
    float strength = 1.0f;
    int excludeCount = 0;
    float padding = 0.0f;
    float excludeRects[kMaxOutlineExclusions][4] = {}; ///< (minU, minV, maxU, maxV)
  };
  MaskOutlineData *mappedMaskOutline_ = nullptr;
  RC::DynamicCB<MaskOutlineData> dynMaskOutline_; ///< mappedMaskOutline_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float maskOutlineColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float maskOutlineThickness_ = 2.0f;
  float maskOutlineStrength_ = 1.0f;
  D3D12_GPU_DESCRIPTOR_HANDLE maskSrv_{}; ///< マスクRTのSRV（RenderContext が毎フレーム渡す）

  // Bloom パラメータ（b0 のルート定数 4 つに収まるので CBuffer は持たない）
  float bloomThreshold_ = 0.75f;
  float bloomIntensity_ = 0.85f;
  float bloomRadius_ = 4.0f;
  float bloomKnee_ = 0.35f;

  // SSAO パラメータ
  // NOTE: projectionInverse を含むので専用 CBuffer。行列は SetProjectionInverse が毎フレーム流す。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferSsao_;
  struct SsaoData {
    float projectionInverse[16] = {};
    float radius = 0.5f;
    float intensity = 0.6f;
    float bias = 0.02f;
    float power = 1.0f;
  };
  SsaoData *mappedSsao_ = nullptr;
  RC::DynamicCB<SsaoData> dynSsao_; ///< mappedSsao_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float ssaoRadius_ = 0.5f;
  float ssaoIntensity_ = 0.6f;
  float ssaoBias_ = 0.02f;
  float ssaoPower_ = 1.0f;

  // ColorGrade パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferColorGrade_;
  struct ColorGradeData {
    float colorFilter[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float exposure = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    float lerpFactor = 1.0f;
    float padding[2] = {0.0f, 0.0f};
  };
  ColorGradeData *mappedColorGrade_ = nullptr;
  RC::DynamicCB<ColorGradeData> dynColorGrade_; ///< mappedColorGrade_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float gradeColorFilter_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float gradeExposure_ = 0.0f;
  float gradeContrast_ = 1.0f;
  float gradeSaturation_ = 1.0f;
  float gradeTemperature_ = 0.0f;
  float gradeTint_ = 0.0f;
  float gradeLerpFactor_ = 1.0f;

  // Underwater パラメータ
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferUnderwater_;
  struct UnderwaterData {
    float projectionInverse[16];
    float tintColor[4] = {0.2f, 0.5f, 1.0f, 1.0f};
    float fogColor[4] = {0.0f, 0.3f, 0.6f, 1.0f};
    float time = 0.0f;
    float distortionForce = 0.004f;
    float fogStart = 10.0f;
    float fogEnd = 150.0f;
    float lerpFactor = 1.0f;
    float padding[3] = {0.0f, 0.0f, 0.0f};
  };
  UnderwaterData *mappedUnderwater_ = nullptr;
  RC::DynamicCB<UnderwaterData> dynUnderwater_; ///< mappedUnderwater_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float underwaterTintColor_[4] = {0.2f, 0.5f, 1.0f, 1.0f};
  float underwaterFogColor_[4] = {0.0f, 0.3f, 0.6f, 1.0f};
  float underwaterDistortionForce_ = 0.004f;
  float underwaterFogStart_ = 10.0f;

  float underwaterFogEnd_ = 150.0f;
  float underwaterLerpFactor_ = 1.0f;

  // Caustics パラメータ
  // NOTE: HLSL 側 cbuffer CausticsParams (b1) と 1:1 で対応する。
  //       全 48 float = 192 byte がちょうど 12 個の float4 行に収まるため
  //       末尾パディングは不要（メンバを増減する際は必ず再確認すること）。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferCaustics_;
  struct CausticsData {
    float projectionInverse[16];               // プロジェクション逆行列
    float viewInverse[16];                     // ビュー逆行列
    float causticsColor[4] = {0.75f, 0.95f, 1.0f, 1.0f}; // 網目の発光色
    float time = 0.0f;                         // 経過時間
    float intensity = 1.5f;                    // 全体の強さ
    float scale = 0.05f;                       // 網目の密度
    float speed = 0.5f;                        // うねる速度
    float contrast = 1.0f;                     // 網目のコントラスト
    float chromaticOffset = 2.0f;              // 色収差量
    float waterHeight = 150.0f;                  // 水面のワールドY
    float depthFadeDistance = 200.0f;           // 水面からの減衰距離
    float upwardBias = 0.5f;                  // 上向き法線への偏り
    float distanceFadeStart = 300.0f;           // 距離減衰の開始
    float distanceFadeEnd = 150.0f;            // 距離減衰の終了
    float lerpFactor = 1.0f;                   // 水中ブレンド率
  };
  CausticsData *mappedCaustics_ = nullptr;
  RC::DynamicCB<CausticsData> dynCaustics_; ///< mappedCaustics_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float causticsColor_[3] = {0.75f, 0.95f, 1.0f};
  float causticsIntensity_ = 1.5f;
  float causticsScale_ = 0.05f;
  float causticsSpeed_ = 0.5f;
  float causticsContrast_ = 1.0f;
  float causticsChromaticOffset_ = 2.0f;
  float causticsWaterHeight_ = 150.0f;
  float causticsDepthFadeDistance_ = 200.0f;
  float causticsUpwardBias_ = 0.5f;
  float causticsDistanceFadeStart_ = 300.0f;
  float causticsDistanceFadeEnd_ = 150.0f;
  float causticsLerpFactor_ = 1.0f;

  // LightShaft パラメータ
  // NOTE: HLSL 側 cbuffer LightShaftParams (b1) と 1:1 で対応する。
  //       全 52 float = 208 byte（13 個の float4 行）。sunDir は 13 行目に単独で入る。
  //       末尾の _padding を含めて数が合っているので、メンバを増減する際は
  //       必ずパディングを調整すること。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferLightShaft_;
  struct LightShaftData {
    float projectionInverse[16];                      // プロジェクション逆行列
    float viewInverse[16];                            // ビュー逆行列
    float shaftColor[4] = {0.80f, 0.94f, 1.0f, 1.0f}; // 光柱の色
    float time = 0.0f;                                // 経過時間
    float intensity = 1.0f;                           // 全体の強さ
    float scale = 0.05f;                              // 模様の密度（Caustics と同期）
    float speed = 0.5f;                              // うねる速度（Caustics と同期）
    float contrast = 3.0f;                            // 断面のコントラスト
    float waterHeight = 150.0f;                         // 水面のワールドY（Caustics と同期）
    float density = 0.045f;                           // 深さによる指数減衰
    float maxDistance = 120.0f;                       // レイマーチの最大距離
    int sampleCount = 24;                             // ステップ数（性能の主要因）
    float ditherStrength = 1.0f;                      // バンディング対策
    float lerpFactor = 1.0f;                          // 水中ブレンド率
    float _padding = 0.0f;
    float sunDir[4] = {-0.464f, -0.743f, 0.464f, 0.0f};   // 光の進む向き（ワールド、正規化済み、y<0）
  };
  LightShaftData *mappedLightShaft_ = nullptr;
  RC::DynamicCB<LightShaftData> dynLightShaft_; ///< mappedLightShaft_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float lightShaftSunDir_[3] = {-0.464f, -0.743f, 0.464f};
  float lightShaftColor_[3] = {0.80f, 0.94f, 1.0f};
  float lightShaftIntensity_ = 1.0f;
  float lightShaftContrast_ = 3.0f;
  float lightShaftDensity_ = 0.045f;
  float lightShaftMaxDistance_ = 120.0f;
  int lightShaftSampleCount_ = 24;
  float lightShaftDitherStrength_ = 1.0f;
  float lightShaftLerpFactor_ = 1.0f;

  // ScreenDroplets パラメータ
  // NOTE: HLSL 側 cbuffer ScreenDropletsParams (b1) と 1:1 で対応する。
  //       全 8 float = 32 byte が 2 個の float4 行に収まる。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferScreenDroplets_;
  struct ScreenDropletsData {
    float time = 0.0f;
    float intensity = 1.0f;
    float speed = 1.0f;
    float distortion = 0.05f;
    float scale = 1.5f;
    float aspectRatio = 1.777f;
    float padding[2] = {0.0f, 0.0f};
  };
  ScreenDropletsData *mappedScreenDroplets_ = nullptr;
  RC::DynamicCB<ScreenDropletsData> dynScreenDroplets_; ///< mappedScreenDroplets_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  float screenDropletsIntensity_ = 1.0f;
  float screenDropletsSpeed_ = 1.0f;
  float screenDropletsDistortion_ = 0.05f;
  float screenDropletsScale_ = 1.5f;
  float screenDropletsAspectRatio_ = 1.777f;

  // BloodOverlay パラメータ
  // NOTE: HLSL 側 cbuffer BloodOverlayParams (b1) と 1:1 で対応する。
  //       全 16 float = 64 byte がちょうど 4 個の float4 行に収まる。
  //       メンバを増減する際は必ず末尾パディングを調整すること。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferBloodOverlay_;
  struct BloodOverlayData {
    float time = 0.0f;                                  // 経過時間（脈動の位相）
    float hitFlash = 0.0f;                              // 被弾フラッシュ
    float lowHealth = 0.0f;                             // 低HP持続
    float pulseSpeed = 1.15f;                           // 脈動の速さ
    float bloodColor[4] = {0.62f, 0.03f, 0.04f, 1.0f};  // 血の色
    float coverage = 0.40f;                             // 侵食する割合
    float splatterScale = 1.6f;                         // 飛沫の密度
    float desaturate = 0.65f;                           // 彩度低下量
    float aspectRatio = 1.777f;                         // アスペクト比
    float seed = 0.0f;                                  // 飛沫パターンの種
    float padding[3] = {0.0f, 0.0f, 0.0f};
  };
  BloodOverlayData *mappedBloodOverlay_ = nullptr;
  RC::DynamicCB<BloodOverlayData> dynBloodOverlay_; ///< mappedBloodOverlay_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）

  // InkOverlay パラメータ
  // NOTE: HLSL 側 cbuffer InkOverlayParams (b1) と 1:1 で対応する。
  //       header 8 float + splatA/B 各 kMaxInkSplats 個の float4 = 224 byte。
  Microsoft::WRL::ComPtr<ID3D12Resource> cbufferInkOverlay_;
  struct InkOverlayData {
    float time = 0.0f;
    float aspectRatio = 1.777f;
    int splatCount = 0;
    float murk = 0.0f;
    float inkColor[4] = {0.03f, 0.02f, 0.05f, 0.96f};
    float splatA[kMaxInkSplats][4] = {};
    float splatB[kMaxInkSplats][4] = {};
  };
  static_assert(sizeof(InkOverlayData) == 32 + 32 * kMaxInkSplats,
                "InkOverlayData must match cbuffer InkOverlayParams");
  InkOverlayData *mappedInkOverlay_ = nullptr;
  RC::DynamicCB<InkOverlayData> dynInkOverlay_; ///< mappedInkOverlay_ の実体（CPU/GPU 並行のため毎フレームの領域へ送る）
  int inkSplatCount_ = 0;
  float inkMurk_ = 0.0f;
  float inkColor_[4] = {0.03f, 0.02f, 0.05f, 0.96f};
  // ImGui のテスト用（ゲーム側のスクリプトが毎フレーム上書きする場合がある）
  float inkDebugStrength_ = 1.0f;
  float inkDebugAge_ = 1.5f;
  float inkDebugRadius_ = 0.30f;
  float inkDebugSeed_ = 0.0f;
  float bloodHitFlash_ = 0.0f;
  float bloodLowHealth_ = 0.0f;
  float bloodPulseSpeed_ = 1.15f;
  float bloodColor_[3] = {0.62f, 0.03f, 0.04f};
  float bloodCoverage_ = 0.40f;
  float bloodSplatterScale_ = 1.6f;
  float bloodDesaturate_ = 0.65f;
  float bloodSeed_ = 0.0f;

public:
  void SetUnderwaterLerpFactor(float lf) { underwaterLerpFactor_ = lf; }
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
  void SetUnderwaterFogRange(float start, float end) {
      underwaterFogStart_ = start;
      underwaterFogEnd_ = end;
      if (mappedUnderwater_) {
          mappedUnderwater_->fogStart = start;
          mappedUnderwater_->fogEnd = end;
      }
  }

  void SetRadialBlurWidth(float w) { radialBlurWidth_ = w; }
  void SetRadialBlurCenter(float x, float y) { radialBlurCenter_.x = x; radialBlurCenter_.y = y; }
  void SetRadialBlurSamples(int samples) { radialBlurSamples_ = samples; }
  void SetGrayscaleLerpFactor(float lf) { grayscaleLerpFactor_ = lf; }
};
