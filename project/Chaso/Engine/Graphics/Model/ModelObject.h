#pragma once
#include "Math/Math.h"
#include "Math/MathTypes.h"
#include "ModelMesh.h"
#include "ModelResource.h"
#include "Animation.h"
#include "function/function.h"
#include "struct.h"
#include <array>
#include <d3d12.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <wrl/client.h>

class TextureManager; // 前方宣言

namespace RC {
class FrameResource; // 前方宣言
}

// ============================================================
// ModelObject
// - 1つのModelMesh(共有)を配置して描画する
// - ModelMeshが持つ Node/DrawItem を使って「Node行列を反映」して描画できる
// - Textureは「override（SetTexture）」があればそれを優先。
//   overrideが無ければ、materialIndexに応じてTextureManagerでロードして使う。
//
// 内部的に ModelResource（GPUリソース管理）に委譲している。
// このクラスは Transform / 可視性 / ImGui / ライティング設定 を保持するラッパー。
// ============================================================
/// @class ModelObject
/// @brief 共有モデルリソース（ModelMesh）を配置・管理するためのオブジェクトクラス
/// @details トランスフォーム（座標・回転・スケール）、可視性、ライティング設定、
/// およびテクスチャのオーバーライド機能を持ちます。
/// 描画ロジックの詳細は内部の ModelResource クラスに委譲されています。
class ModelObject {
public:
  /// @struct LightingConfig
  /// @brief オブジェクト固有のライティング設定を保持する構造体
  struct LightingConfig {
    LightingMode mode = HalfLambert;      ///< ライティングモード
    float color[3] = {1.0f, 1.0f, 1.0f}; ///< ライトカラー
    float dir[3] = {0.0f, -1.0f, 0.0f};  ///< ライト方向
    float intensity = 1.0f;              ///< ライト強度

    constexpr LightingConfig() = default;
    constexpr LightingConfig(LightingMode m) : mode(m) {}
    constexpr LightingConfig(int m)
        : mode(static_cast<LightingMode>(m < 0 ? 0 : (m > 3 ? 3 : m))) {}
  };

  ModelObject() = default;
  explicit ModelObject(const LightingConfig &cfg) : initialLighting_(cfg) {}
  ~ModelObject() = default;

  /// @brief オブジェクトを初期化する
  /// @param device D3D12 デバイス
  void Initialize(ID3D12Device *device);

  /// @brief 使用するモデルメッシュを設定する
  /// @param mesh 設定する ModelMesh の shared_ptr
  void SetMesh(const std::shared_ptr<ModelMesh> &mesh) {
    resource_.SetMesh(mesh);
  }

  /// @brief 設定されているモデルメッシュを取得する
  /// @return ModelMesh の shared_ptr
  const std::shared_ptr<ModelMesh> &GetMesh() const {
    return resource_.GetMesh();
  }

  /// @brief モデルにUV座標がない場合、球面投影UVを生成する
  void EnsureSphericalUVIfMissing();

  /// @brief モデル全体のテクスチャを1つのハンドルでオーバーライドする
  /// @param srvGPUHandle 使用するテクスチャの GPU ハンドル
  void SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    resource_.SetTexture(srvGPUHandle);
  }

  /// @brief テクスチャのオーバーライドを解除し、モデル本来のマテリアル設定に戻す
  void ResetTextureToMtl() { resource_.ResetTextureToMtl(); }

  /// @brief 法線マップのオーバーライドを設定する
  /// @param srvGPUHandle GPU ハンドル
  void SetNormalMap(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    resource_.SetNormalMap(srvGPUHandle);
  }

  /// @brief ラフネスマップのオーバーライドを設定する
  /// @param srvGPUHandle GPU ハンドル
  void SetRoughnessMap(D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle) {
    resource_.SetRoughnessMap(srvGPUHandle);
  }

  /// @brief ライティング設定を一括で適用する
  /// @param cfg ライティング設定構造体
  /// @return 自身への参照（メソッドチェーン用）
  ModelObject &SetLightingConfig(const LightingConfig &cfg) {
    initialLighting_ = cfg;
    ApplyLightingIfReady_();
    return *this;
  }

  /// @brief ライティング設定を個別のパラメータで指定する
  /// @param mode ライティングモード
  /// @param color ライトカラー
  /// @param dir ライト方向
  /// @param intensity ライト強度
  /// @return 自身への参照
  ModelObject &SetLightingConfig(LightingMode mode,
                                 const std::array<float, 3> &color,
                                 const std::array<float, 3> &dir,
                                 float intensity);

  /// @brief ライティングモードのみを更新する
  /// @param m 設定する LightingMode
  /// @return 自身への参照
  /// @note 呼び出し以降、このモデルはシーンの DirectionalLight の
  ///       LightingMode に追従しなくなる（個別オーバーライド扱い）。
  ///       追従に戻すには ClearLightingModeOverride() を呼ぶ。
  ModelObject &SetLightingMode(LightingMode m) {
    lightingModeOverride_ = static_cast<int>(m);
    // ライトの色・方向・強度は維持し、モードだけを差し替える
    LightingConfig cfg = initialLighting_;
    cfg.mode = m;
    return SetLightingConfig(cfg);
  }

  /// @brief ライティングモードの個別オーバーライドを解除し、
  ///        シーンの DirectionalLight のモードに追従させる
  /// @return 自身への参照
  ModelObject &ClearLightingModeOverride() {
    lightingModeOverride_ = -1;
    return *this;
  }

  /// @brief ライティングモードのオーバーライド値を取得する
  /// @return -1: DirectionalLight に追従 / 0以上: 固定された LightingMode
  int GetLightingModeOverride() const { return lightingModeOverride_; }

  /// @brief トランスフォーム情報を取得する（読み書き可能）
  /// @return Transform への参照
  Transform &T() { return transform_; }

  /// @brief マテリアル情報を取得する（読み書き可能）
  /// @return Material ポインタ
  Material *Mat() { return resource_.Mat(); }
  const Material *Mat() const { return resource_.Mat(); }

  /// @brief オブジェクトのベースカラーを設定する
  /// @param color 設定するカラーベクトル
  void SetColor(const RC::Vector4 &color);

  /// @brief ライト情報を取得する（読み書き可能）
  /// @return DirectionalLight ポインタ
  DirectionalLight *Light() { return resource_.Light(); }
  const DirectionalLight *Light() const { return resource_.Light(); }

  /// @brief 光沢度（Shininess）を設定する (0.0: 鏡面反射なし)
  /// @param shininess 光沢度
  void SetShininess(float shininess) {
    initialShininess_ = shininess;
    if (Material *mat = resource_.Mat()) {
      mat->shininess = shininess;
    }
  }

  /// @brief 光沢度（Shininess）を取得する
  /// @return 光沢度
  float GetShininess() const {
    if (const Material *mat = resource_.Mat()) {
      return mat->shininess;
    }
    return initialShininess_;
  }

  /// @brief 環境マップ映り込み係数を設定する（非同期ロード対応）
  /// @param coeff 係数 (0.0: なし, 1.0: 最大)
  void SetEnvironmentCoefficient(float coeff) {
    initialEnvCoeff_ = coeff;
    if (Material *mat = resource_.Mat()) {
      mat->environmentCoefficient = coeff;
    }
  }

  /// @brief 環境マップ映り込み係数を取得する
  /// @return 係数値
  float GetEnvironmentCoefficient() const { return initialEnvCoeff_; }

  /// @brief 外部のライトCBアドレスを指定してライト設定を共有する
  /// @param addr ライトCBの GPU 仮想アドレス（0 で自前ライトに戻る）
  void SetExternalLightCBAddress(D3D12_GPU_VIRTUAL_ADDRESS addr) {
    resource_.SetExternalLightCBAddress(addr);
  }

  /// @brief 外部ライトCBの GPU 仮想アドレスを取得する
  /// @return アドレス値
  D3D12_GPU_VIRTUAL_ADDRESS GetExternalLightCBAddress() const {
    return resource_.GetExternalLightCBAddress();
  }

  /// @brief オブジェクトの可視状態を設定する
  /// @param v true で表示
  void SetVisible(bool v) { visible_ = v; }

  /// @brief オブジェクトが可視状態か取得する
  /// @return 可視なら true
  bool Visible() const { return visible_; }

  /// @brief テクスチャマネージャを設定する（モデル読み込み時のテクスチャ解決に使用）
  /// @param tm テクスチャマネージャへのポインタ
  void SetTextureManager(TextureManager *tm) { resource_.SetTextureManager(tm); }

  /// @brief モデルのファイルパスを保存する
  /// @param path ファイルパス
  void SetFilePath(const std::string &path) { filePath_ = path; }

  /// @brief モデルのファイルパスを取得する
  /// @return ファイルパス文字列
  const std::string &GetFilePath() const { return filePath_; }

  /// @brief 準備完了フラグを設定する
  /// @param r 準備完了状態
  void SetReady(bool r) { resource_.SetReady(r); }

  /// @brief モデルデータの準備が完了しているか確認する
  /// @return 準備完了なら true
  bool IsReady() const { return resource_.IsReady(); }

  /// @brief 毎フレームの更新処理（行列計算等）
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  void Update(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj);

  /// @brief ワールド行列を指定してオブジェクトを描画する
  /// @param cmdList グラフィックスコマンドリスト
  /// @param world 描画に使用するワールド行列
  /// @param frame フレームリソース（一時メモリ用）
  /// @param worldOnly true なら World 行列だけを転送する（シャドウパス用。VS が World しか読まないため
  ///        WVP / 逆転置行列の計算を省いても結果は変わらない）
  void Draw(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &world,
            RC::FrameResource &frame, bool worldOnly = false);

  /// @brief 複数のインスタンス（Transformリスト）を一括描画する
  /// @param cmdList グラフィックスコマンドリスト
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param instances インスタンスごとの Transform 配列
  /// @param frame フレームリソース
  /// @param worldOnly true なら World 行列だけを転送する（シャドウパス用）
  void DrawBatch(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &view,
                 const RC::Matrix4x4 &proj,
                 const std::vector<Transform> &instances,
                 RC::FrameResource &frame, bool worldOnly = false);

  /// @brief インスタンス一括描画（単色オーバーライド付き）
  /// @param cmdList グラフィックスコマンドリスト
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  /// @param instances インスタンスごとの Transform 配列
  /// @param color 全インスタンスに適用するオーバーライドカラー
  /// @param frame フレームリソース
  /// @param worldOnly true なら World 行列だけを転送する（シャドウパス用）
  void DrawBatch(ID3D12GraphicsCommandList *cmdList, const RC::Matrix4x4 &view,
                 const RC::Matrix4x4 &proj,
                 const std::vector<Transform> &instances,
                 const RC::Vector4 &color,
                 RC::FrameResource &frame, bool worldOnly = false);

  /// @brief バッチ描画用のカーソルをリセットする
  void ResetBatchCursor() { resource_.ResetBatchCursor(); }

  /// @brief 内部の ModelResource インスタンスを取得する
  /// @return ModelResource への参照
  ModelResource &Resource() { return resource_; }
  const ModelResource &Resource() const { return resource_; }

private:
  /// @brief ライティング設定を GPU リソースに反映する（準備完了時のみ）
  void ApplyLightingIfReady_();

  /// @brief 現在のアニメーションのルートモーション担当Jointを（未判定なら）解決する
  /// @details スケルトン構築後に一度だけ判定し、結果を rootMotionJointName_ に保持する
  void ResolveRootMotionJoint_();

  /// @brief バインドポーズ（rest）のJoint平行移動量を控えておく
  /// @details CreateSkeleton 直後（アニメーション適用前）に呼ぶこと。
  ///          joint.transform はフレームごとにアニメーション値で上書きされるため、
  ///          後からでは rest 値を取得できない。
  void CaptureRestPose_();

private:
  ModelResource resource_; ///< 描画リソース・ロジック本体

  Transform transform_{{1, 1, 1}, {0, 0, 0}, {0, 0, 0}}; ///< オブジェクトの座標・回転・スケール
  bool visible_ = true; ///< 可視フラグ

  RC::Matrix4x4 cachedView_ = {}; ///< Update で受け取ったビュー行列のキャッシュ
  RC::Matrix4x4 cachedProj_ = {}; ///< Update で受け取ったプロジェクション行列のキャッシュ
  bool hasVP_ = false;           ///< ビュー・プロジェクション行列がセットされているか

  LightingConfig initialLighting_{}; ///< 初期化時または外部から設定されたライティング情報

  int lightingModeOverride_ = -1; ///< -1: DirectionalLight に追従 / 0以上: 固定 LightingMode

  float initialShininess_ = 32.0f; ///< 光沢度の初期設定値
  float initialEnvCoeff_ = 0.0f; ///< 環境マップ映り込み係数の初期設定値
  float lastEnvCoeff_ = 0.5f;    ///< ImGui用：環境マップトグル時の係数保存用

  std::string filePath_; ///< モデルのアセットパス

  // === アニメーション関連 ===
public:
  /// @brief 自身のファイルパスを使ってアニメーションをロード・アタッチする
  void AttachAnimation();

  /// @brief 指定したファイルからアニメーションをロード・アタッチする
  /// @param filePath アニメーションファイル(.gltf等)のパス
  void AttachAnimation(const std::string& filePath);

  /// @brief 指定したファイルからインデックス指定でアニメーションをロード・アタッチする
  /// @param filePath アニメーションファイル(.gltf/.glb等)のパス
  /// @param animIndex アニメーションインデックス（0始まり）
  void AttachAnimation(const std::string& filePath, int animIndex);

  /// @brief 指定したファイルからアニメーションをロードし、現在のアニメーションからクロスフェード（ブレンド）して切り替える
  /// @param filePath 新しいアニメーションファイル(.gltf等)のパス
  /// @param blendDuration 切り替えにかけるブレンド秒数 (例: 0.2f)
  void CrossfadeAnimation(const std::string& filePath, float blendDuration = 0.2f);

  /// @brief 指定したファイルからインデックス指定でアニメーションをロードし、クロスフェードで切り替える
  /// @param filePath アニメーションファイル(.gltf/.glb等)のパス
  /// @param animIndex アニメーションインデックス（0始まり）
  /// @param blendDuration 切り替えにかけるブレンド秒数 (例: 0.2f)
  void CrossfadeAnimation(const std::string& filePath, int animIndex, float blendDuration = 0.2f);

  /// @brief アニメーションを更新し、Skeletonに適用する
  /// @param dt 経過時間
  void UpdateAnimation(float dt);

  /// @brief 現在ロードしているアニメーションの再生時間（秒）を取得する
  /// @return 再生時間（秒）
  float GetAnimationDuration() const { return animation_.duration; }

  /// @brief スケルトンのデバッグ描画を行う
  /// @details 各Jointを球で、親子関係のあるJoint同士を線で描画する
  void DrawSkeleton();

  /// @brief Skeleton が有効かどうか
  bool HasSkeleton() const { return hasSkeleton_; }

  /// @brief スケルトンを取得する（読み取り専用）
  const Skeleton &GetSkeleton() const { return skeleton_; }

  /// @brief 指定Jointのスケルトン空間行列（モデルローカル）を取得する
  /// @param jointName Joint名（例: "R_Hand"）
  /// @param out 取得先。ワールドに変換するには Multiply(out, モデルのワールド行列) とする
  /// @return 見つかれば true
  /// @details 武器などをボーンに追従させる（ソケット）用途に使う
  bool TryGetJointMatrix(const std::string &jointName, RC::Matrix4x4 &out) const {
    if (!hasSkeleton_) return false;
    auto it = skeleton_.jointMap.find(jointName);
    if (it == skeleton_.jointMap.end()) return false;
    out = skeleton_.joints[it->second].skeletonSpaceMatrix;
    return true;
  }

  // === ワールド行列の上書き（ボーン追従などで使う） ===

  /// @brief 描画に使うワールド行列を直接指定する（Transform の TRS を無視する）
  /// @details ボーン追従のように「行列でしか表せない姿勢」を与えるための機能。
  ///          Transform 経由だとオイラー角へ分解する必要があり誤差やジンバルの問題が出るため、
  ///          行列をそのまま渡せるようにしている。
  void SetWorldOverride(const RC::Matrix4x4 &world) {
    worldOverride_ = world;
    hasWorldOverride_ = true;
  }

  /// @brief ワールド行列の上書きを解除し、Transform 基準の描画に戻す
  void ClearWorldOverride() { hasWorldOverride_ = false; }

  /// @brief ワールド行列が上書きされているか
  bool HasWorldOverride() const { return hasWorldOverride_; }

  /// @brief 上書き中のワールド行列
  const RC::Matrix4x4 &WorldOverride() const { return worldOverride_; }

  // === ワールド行列のキャッシュ（インスタンス描画用） ===

  /// @brief 描画に使うワールド行列（WorldOverride があればそれ、無ければ Transform から）
  /// @details Transform / WorldOverride が前回と同じなら再計算しない。
  ///          影パス × 灯数ぶん同じ物を描いても、行列の計算は 1 フレーム 1 回で済む。
  const RC::Matrix4x4 &CachedWorld();

  /// @brief CachedWorld の逆転置行列（法線変換用）。必要になったときに 1 回だけ計算する
  const RC::Matrix4x4 &CachedWorldInverseTranspose();

  /// @brief スキンデータが有効かどうか（ボーンウェイト付きモデルか）
  bool HasSkinData() const;

  /// @brief スキニング行列パレットを取得する
  const std::vector<RC::Matrix4x4> &GetSkinMatrices() const { return skinMatrices_; }

  /// @brief ルートモーション（アニメーションに焼き込まれた移動量）の除去を切り替える
  /// @param enable true でルートモーションを打ち消し、その場アニメーションとして再生する
  /// @details Walk/Run などのアニメーションにはキーフレーム自体に前進移動が
  ///          含まれている場合がある。スクリプト側で Transform を動かしていると
  ///          二重適用になり、見た目だけが先に進んでループの瞬間に Transform
  ///          位置へ戻る現象が起きるため、既定では除去する。
  void SetRemoveRootMotion(bool enable) {
    if (removeRootMotion_ == enable) return;
    removeRootMotion_ = enable;
    rootMotionJointName_.clear();
    rootMotionResolved_ = false; // 次回更新時に再判定させる
  }

  /// @brief ルートモーション除去が有効かどうか
  bool IsRemoveRootMotion() const { return removeRootMotion_; }

  /// @brief 現在のアニメーションでルートモーション担当と判定された Joint 名
  /// @return 該当なしなら空文字列
  const std::string &GetRootMotionJointName() const { return rootMotionJointName_; }

private:
  RC::Animation animation_;   ///< ロードしたアニメーションデータ（現在・切り替え後）
  float animationTime_ = 0.0f;///< アニメーション再生時間
  bool isAnimated_ = false;   ///< アニメーションが有効かどうか
  bool animationRequested_ = false; ///< AttachAnimationが呼ばれたか（遅延ロード用）

  // === アニメーション補間（クロスフェード）関連 ===
  RC::Animation prevAnimation_;       ///< 切り替え前のアニメーションデータ (ブレンド用A)
  float prevAnimationTime_ = 0.0f;    ///< 切り替え前のアニメーション再生時間
  float blendFactor_ = 1.0f;          ///< ブレンド割合 t (0.0=Aのみ, 1.0=B/完了)
  float blendDuration_ = 0.2f;        ///< クロスフェードの所要時間 (秒)

  Skeleton skeleton_;          ///< スケルトンデータ
  bool hasSkeleton_ = false;   ///< スケルトンが構築済みか

  // === ワールド行列の上書き ===
  RC::Matrix4x4 worldOverride_{};      ///< 上書き用ワールド行列
  bool hasWorldOverride_ = false;      ///< 上書きが有効か

  // === ワールド行列のキャッシュ ===
  struct WorldCache {
    Transform transform{};             ///< 計算に使った Transform
    RC::Matrix4x4 overrideMatrix{};    ///< 計算に使った WorldOverride
    bool usedOverride = false;         ///< WorldOverride から作ったか
    bool valid = false;                ///< world が有効か
    bool witValid = false;             ///< worldInvTranspose が有効か
    RC::Matrix4x4 world{};             ///< ワールド行列
    RC::Matrix4x4 worldInvTranspose{}; ///< 逆転置行列
  };
  WorldCache worldCache_{};

  // === ルートモーション除去関連 ===
  bool removeRootMotion_ = true;      ///< アニメーションに焼き込まれた移動量を打ち消すか
  bool rootMotionResolved_ = false;   ///< 現アニメのルートモーション担当Jointを判定済みか
  std::string rootMotionJointName_;      ///< 現アニメ(B)のルートモーション担当Joint名（無ければ空）
  std::string prevRootMotionJointName_;  ///< クロスフェード元(A)のルートモーション担当Joint名
  /// @brief バインドポーズ時の Joint 平行移動量（Joint名 → translate）
  /// @details クリップ先頭の移動量が rest からズレているモデル（例: 走りモーションが
  ///          前方に0.5m進んだ位置から始まる）で、モデル全体が当たり判定からズレるのを補正するのに使う
  std::map<std::string, RC::Vector3> restTranslations_;

  // === スキニング関連 ===
  std::vector<RC::Matrix4x4> skinMatrices_; ///< スキンクラスター行列パレット (T_i = IBP_i * SSM_i)
};

