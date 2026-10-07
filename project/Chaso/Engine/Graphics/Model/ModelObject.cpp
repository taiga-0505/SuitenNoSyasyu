#include "ModelObject.h"
#include "Common/Log/Log.h"
#include "Render/FrameResource.h"
#include "Texture/TextureManager/TextureManager.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace RC;
namespace fs = std::filesystem;

void ModelObject::Initialize(ID3D12Device *device) {
  resource_.Initialize(device);

  // 初期ライティングを反映
  ApplyLightingIfReady_();
}

void ModelObject::EnsureSphericalUVIfMissing() {
  if (resource_.GetMesh()) {
    resource_.GetMesh()->EnsureSphericalUVIfMissing();
  }
}

void ModelObject::SetColor(const Vector4 &color) {
  if (Material *mat = resource_.Mat()) {
    mat->color = color;
  }
}

void ModelObject::Update(const Matrix4x4 &view, const Matrix4x4 &proj) {
  cachedView_ = view;
  cachedProj_ = proj;
  hasVP_ = true;
}

const Matrix4x4 &ModelObject::CachedWorld() {
  auto sameMatrix = [](const Matrix4x4 &a, const Matrix4x4 &b) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (a.m[r][c] != b.m[r][c]) return false;
    return true;
  };
  auto sameVec = [](const Vector3 &a, const Vector3 &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  };

  WorldCache &c = worldCache_;
  if (hasWorldOverride_) {
    if (c.valid && c.usedOverride && sameMatrix(c.overrideMatrix, worldOverride_)) {
      return c.world;
    }
    c.overrideMatrix = worldOverride_;
    c.world = worldOverride_;
    c.usedOverride = true;
  } else {
    if (c.valid && !c.usedOverride && sameVec(c.transform.scale, transform_.scale) &&
        sameVec(c.transform.rotation, transform_.rotation) &&
        sameVec(c.transform.translation, transform_.translation)) {
      return c.world;
    }
    c.transform = transform_;
    c.world = MakeAffineMatrix(transform_.scale, transform_.rotation, transform_.translation);
    c.usedOverride = false;
  }
  c.valid = true;
  c.witValid = false;
  return c.world;
}

const Matrix4x4 &ModelObject::CachedWorldInverseTranspose() {
  const Matrix4x4 &w = CachedWorld();
  if (!worldCache_.witValid) {
    worldCache_.worldInvTranspose = Transpose(Inverse(w));
    worldCache_.witValid = true;
  }
  return worldCache_.worldInvTranspose;
}

void ModelObject::Draw(ID3D12GraphicsCommandList *cmdList,
                       const Matrix4x4 &world, FrameResource &frame,
                       bool worldOnly) {
  if (!visible_)
    return;

  // View/Proj が無いなら単位行列で描く（事故防止）
  const Matrix4x4 view = hasVP_ ? cachedView_ : MakeIdentity4x4();
  const Matrix4x4 proj = hasVP_ ? cachedProj_ : MakeIdentity4x4();

  // スキニングモデルの場合
  if (HasSkinData()) {
    // CS スキニングが有効かつ Dispatch 済みの場合
    if (resource_.HasCSSkinning() && resource_.IsSkinningDispatched()) {
      // CS スキニング済み頂点で通常描画（object3d パイプラインで描画）
      resource_.DrawSkinnedCS(cmdList, world, view, proj, frame, worldOnly);
    } else if (!resource_.HasCSSkinning()) {
      // フォールバック: 従来の VS スキニング
      resource_.DrawSkinned(cmdList, world, view, proj, skinMatrices_, frame);
    }
    // CS 有効だが未 Dispatch → スキップ（次フレームで描画）
  } else {
    resource_.Draw(cmdList, world, view, proj, frame, worldOnly);
  }
}

void ModelObject::DrawBatch(ID3D12GraphicsCommandList *cmdList,
                            const Matrix4x4 &view, const Matrix4x4 &proj,
                            const std::vector<Transform> &instances,
                            FrameResource &frame, bool worldOnly) {
  resource_.DrawBatch(cmdList, view, proj, instances, frame, worldOnly);
}

void ModelObject::DrawBatch(ID3D12GraphicsCommandList *cmdList,
                            const Matrix4x4 &view, const Matrix4x4 &proj,
                            const std::vector<Transform> &instances,
                            const RC::Vector4 &color,
                            FrameResource &frame, bool worldOnly) {
  resource_.DrawBatch(cmdList, view, proj, instances, color, frame, worldOnly);
}

ModelObject &ModelObject::SetLightingConfig(LightingMode mode,
                                            const std::array<float, 3> &color,
                                            const std::array<float, 3> &dir,
                                            float intensity) {
  initialLighting_.mode = mode;
  initialLighting_.color[0] = color[0];
  initialLighting_.color[1] = color[1];
  initialLighting_.color[2] = color[2];

  initialLighting_.dir[0] = dir[0];
  initialLighting_.dir[1] = dir[1];
  initialLighting_.dir[2] = dir[2];

  initialLighting_.intensity = intensity;

  ApplyLightingIfReady_();
  return *this;
}

void ModelObject::ApplyLightingIfReady_() {
  resource_.ApplyLighting(static_cast<int>(initialLighting_.mode),
                          initialLighting_.color, initialLighting_.dir,
                          initialLighting_.intensity);

  // 環境マップ係数を再適用（Initialize後のリセット対策）
  if (Material *mat = resource_.Mat()) {
    mat->environmentCoefficient = initialEnvCoeff_;
  }
}

// ============================================================================
// アニメーション・スケルトン関連
// ============================================================================

// ------------------------------------------------------------------
// CreateJoint: NodeからJointを再帰的に作成する
// ------------------------------------------------------------------
// 深さ優先探索で、必ず親のIndexが自身より若くなるようにJointsに登録する。
// これにより、配列先頭から順にUpdateすれば階層計算が正しく行える。
static int32_t CreateJoint(const Node& node,
                           const std::optional<int32_t>& parent,
                           std::vector<Joint>& joints) {
    Joint joint;
    joint.name = node.name;
    joint.localMatrix = node.localMatrix;
    joint.skeletonSpaceMatrix = MakeIdentity4x4();
    joint.transform = node.transform;
    joint.index = int32_t(joints.size()); // 現在登録されている数をIndexに
    joint.parent = parent;

    // まず自分をJoint列に追加（子より先に追加することでIndex順を保証）
    joints.push_back(joint);

    // 子Nodeに対して再帰
    for (const Node& child : node.children) {
        // 子Jointを作成し、そのIndexを取得
        int32_t childIndex = CreateJoint(child, joint.index, joints);
        // 自分（既にpush済み）のchildren配列にchildIndexを追加
        joints[joint.index].children.push_back(childIndex);
    }

    // 自身のIndexを返す
    return joint.index;
}

// ------------------------------------------------------------------
// CreateSkeleton: Node階層からSkeletonを構築する
// ------------------------------------------------------------------
static Skeleton CreateSkeleton(const Node& rootNode) {
    Skeleton skeleton;

    // RootNodeからJoint配列を構築（深さ優先探索）
    skeleton.root = CreateJoint(rootNode, {}, skeleton.joints);

    // 名前とIndexのマッピングを行い、アクセスしやすくする
    for (const Joint& joint : skeleton.joints) {
        skeleton.jointMap.emplace(joint.name, joint.index);
    }

    return skeleton;
}

// ------------------------------------------------------------------
// UpdateSkeleton: 全JointのlocalMatrixとskeletonSpaceMatrixを更新
// ------------------------------------------------------------------
// jointsは親が必ず先（若いIndex）に入っているため、先頭から順に処理すれば
// 親のskeletonSpaceMatrixは必ず計算済み。
static void UpdateSkeleton(Skeleton& skeleton) {
    for (Joint& joint : skeleton.joints) {
        // TransformからlocalMatrixを再構築
        joint.localMatrix = MakeAffineMatrix(
            joint.transform.scale,
            joint.transform.rotate,
            joint.transform.translate);

        // skeletonSpaceMatrixの計算
        if (joint.parent) {
            // 親がいれば親の行列を掛ける
            joint.skeletonSpaceMatrix = Multiply(
                joint.localMatrix,
                skeleton.joints[*joint.parent].skeletonSpaceMatrix);
        } else {
            // 親がいないのでlocalMatrixとskeletonSpaceMatrixは一致する
            joint.skeletonSpaceMatrix = joint.localMatrix;
        }
    }
}

// ------------------------------------------------------------------
// ルートモーション除去
// ------------------------------------------------------------------
// Walk / Run などのアニメーションには、キーフレーム自体に前進移動
// （ルートモーション）が焼き込まれている場合がある。スクリプト側で
// TransformComponent を動かして移動させる設計では、これが二重適用となり
//   ・メッシュだけがコライダー（Transform位置）より前に進んでいく
//   ・アニメーションがループした瞬間に Transform 位置までワープして戻る
// という不具合になる。
//
// ここでは「最終キー − 先頭キー」＝1ループでの純移動量を、経過時間に対して
// 線形に差し引くことでドリフト成分のみを打ち消す。この方式には次の利点がある。
//   ・純移動量が 0 のアニメーション（Idle / Attack 等）は一切影響を受けない
//   ・上下の揺れや溜めなどのオシレーションはそのまま残る
//   ・t=0 と t=duration で補正量が首尾一致するため、ループの継ぎ目が跳ねない
// なお、GLB を再エクスポートする必要はない（読み込み後の実行時補正）。

static constexpr float kRootMotionDriftThreshold = 0.02f; ///< これ以下の純移動量は誤差として無視する
static constexpr int32_t kRootMotionMaxDepth = 4;         ///< ルート系と見なす階層の深さ上限

/// @brief Jointのスケルトンルートからの階層の深さを求める
static int32_t GetJointDepth(const Skeleton& skeleton, const Joint& joint) {
    int32_t depth = 0;
    std::optional<int32_t> parent = joint.parent;
    while (parent && depth <= kRootMotionMaxDepth) {
        ++depth;
        parent = skeleton.joints[*parent].parent;
    }
    return depth;
}

/// @brief 指定Animationでルートモーションを担っているJoint名を探す
/// @return 該当Joint名。ルートモーションが無ければ空文字列
/// @details jointsは必ず親が先に並んでいるため、先頭から走査して最初に
///          条件を満たしたものが階層最上位のルートモーションJointとなる。
///          手足のボーンを誤って対象にしないよう、階層の深さで絞り込む。
static std::string FindRootMotionJointName(const Skeleton& skeleton,
                                           const RC::Animation& animation) {
    for (const Joint& joint : skeleton.joints) {
        if (GetJointDepth(skeleton, joint) > kRootMotionMaxDepth) continue;

        auto it = animation.nodeAnimations.find(joint.name);
        if (it == animation.nodeAnimations.end()) continue;

        const std::vector<RC::KeyframeVector3>& keys = it->second.translate;
        if (keys.size() < 2) continue;

        const RC::Vector3& head = keys.front().value;
        const RC::Vector3& tail = keys.back().value;
        if (std::abs(tail.x - head.x) > kRootMotionDriftThreshold ||
            std::abs(tail.y - head.y) > kRootMotionDriftThreshold ||
            std::abs(tail.z - head.z) > kRootMotionDriftThreshold) {
            return joint.name;
        }
    }
    return std::string();
}

/// @brief ルートモーション分の移動を打ち消した translation を返す
/// @param nodeAnim 対象JointのNodeAnimation
/// @param time 現在の再生時刻（秒）
/// @param raw 補正前の補間済み translation
/// @param restTranslate バインドポーズ（rest）での translation。nullptr なら開始オフセット補正を行わない
/// @details 打ち消す成分は2つある。
///          (1) クリップ内で進んだ分（線形ドリフト）: 先頭→末尾の差分を経過割合で按分して引く。
///          (2) クリップ先頭そのものが rest からズレている分（開始オフセット）:
///              例えば farmer.glb の走りモーションは Hip が既に約0.5m前進した位置から
///              始まっているため、(1) だけではモデル全体が常に約0.5m前へズレたまま再生される。
///              その状態では当たり判定（Transform位置）より前にモデルが描画され、
///              壁の手前で止まってもモデルだけが壁にめり込んで見える。
static RC::Vector3 RemoveRootMotionDrift(const RC::NodeAnimation& nodeAnim,
                                         float time,
                                         const RC::Vector3& raw,
                                         const RC::Vector3* restTranslate) {
    const std::vector<RC::KeyframeVector3>& keys = nodeAnim.translate;
    if (keys.size() < 2) return raw;

    const float startTime = keys.front().time;
    const float endTime = keys.back().time;
    const float span = endTime - startTime;
    if (span <= 1e-6f) return raw;

    // 経過割合（0.0〜1.0）にクランプ
    float ratio = (time - startTime) / span;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    const RC::Vector3& head = keys.front().value;
    const RC::Vector3& tail = keys.back().value;
    RC::Vector3 result{
        raw.x - (tail.x - head.x) * ratio,
        raw.y - (tail.y - head.y) * ratio,
        raw.z - (tail.z - head.z) * ratio
    };

    // (2) 開始オフセットの補正：クリップ先頭を rest の位置に揃える
    if (restTranslate) {
        result.x -= (head.x - restTranslate->x);
        result.y -= (head.y - restTranslate->y);
        result.z -= (head.z - restTranslate->z);
    }
    return result;
}

/// @brief rest translation を引く（無ければ nullptr）
static const RC::Vector3* FindRestTranslate(
    const std::map<std::string, RC::Vector3>* restTranslations,
    const std::string& jointName) {
    if (!restTranslations) return nullptr;
    auto it = restTranslations->find(jointName);
    return (it != restTranslations->end()) ? &it->second : nullptr;
}

// ------------------------------------------------------------------
// CaptureRestPose_: バインドポーズの translation を控える
// ------------------------------------------------------------------
// ApplyAnimation は joint.transform を毎フレーム上書きしてしまうため、
// CreateSkeleton 直後（＝まだ rest 値が入っている状態）で控えておく必要がある。
void ModelObject::CaptureRestPose_() {
    restTranslations_.clear();
    for (const Joint& joint : skeleton_.joints) {
        restTranslations_[joint.name] = joint.transform.translate;
    }
}

// ------------------------------------------------------------------
// ResolveRootMotionJoint_: ルートモーション担当Jointの遅延判定
// ------------------------------------------------------------------
void ModelObject::ResolveRootMotionJoint_() {
    if (rootMotionResolved_) return;
    if (!hasSkeleton_) return; // スケルトン構築後にのみ判定できる

    // 対象はスキン付きモデル（キャラクター）のみに限定する。
    // AnimatedCube のような単一ノードアニメーションのプロップは、
    // 移動そのものがアニメーションの意図なので触ってはいけない。
    const auto mesh = resource_.GetMesh();
    const bool skinned = (mesh && mesh->HasSkinData());

    rootMotionJointName_ = (removeRootMotion_ && skinned)
        ? FindRootMotionJointName(skeleton_, animation_)
        : std::string();
    rootMotionResolved_ = true;
}

// ------------------------------------------------------------------
// ApplyAnimation: SkeletonにAnimationを適用する
// ------------------------------------------------------------------
// 全Jointをループし、対象のJointにNodeAnimationがあれば
// キーフレーム補間して transform に書き込む。
// rootMotionJoint に一致するJointは、焼き込まれた移動量を差し引いて適用する。
static void ApplyAnimation(Skeleton& skeleton,
                           const RC::Animation& animation,
                           float animationTime,
                           const std::string& rootMotionJoint,
                           const std::map<std::string, RC::Vector3>* restTranslations) {
    for (Joint& joint : skeleton.joints) {
        // 対象のJointのAnimationがあれば、値の適用を行う
        // C++17の初期化付きif文
        if (auto it = animation.nodeAnimations.find(joint.name);
            it != animation.nodeAnimations.end()) {
            const RC::NodeAnimation& nodeAnim = it->second;

            RC::Vector3 translate =
                RC::CalculateValue(nodeAnim.translate, animationTime);
            if (!rootMotionJoint.empty() && joint.name == rootMotionJoint) {
                translate = RemoveRootMotionDrift(nodeAnim, animationTime, translate,
                                                  FindRestTranslate(restTranslations, joint.name));
            }

            joint.transform.translate = translate;
            joint.transform.rotate =
                RC::CalculateValue(nodeAnim.rotate, animationTime);
            joint.transform.scale =
                RC::CalculateValue(nodeAnim.scale, animationTime);
        }
    }
}

// ------------------------------------------------------------------
// ApplyAnimationBlend: 2つのAnimationを同時に進めて t でブレンド適用する (回転は球面線形補間)
// ------------------------------------------------------------------
static void ApplyAnimationBlend(Skeleton& skeleton,
                                const RC::Animation& animA, float timeA,
                                const RC::Animation& animB, float timeB,
                                float t,
                                const std::string& rootMotionJointA,
                                const std::string& rootMotionJointB,
                                const std::map<std::string, RC::Vector3>* restTranslations) {
    for (Joint& joint : skeleton.joints) {
        auto itA = animA.nodeAnimations.find(joint.name);
        auto itB = animB.nodeAnimations.find(joint.name);

        bool hasA = (itA != animA.nodeAnimations.end());
        bool hasB = (itB != animB.nodeAnimations.end());

        // ルートモーション除去の対象Jointか（A/Bで別々に判定する）
        const bool isRootA = (!rootMotionJointA.empty() && joint.name == rootMotionJointA);
        const bool isRootB = (!rootMotionJointB.empty() && joint.name == rootMotionJointB);
        const RC::Vector3* rest = FindRestTranslate(restTranslations, joint.name);

        if (hasA && hasB) {
            const RC::NodeAnimation& nodeAnimA = itA->second;
            const RC::NodeAnimation& nodeAnimB = itB->second;

            RC::Vector3 posA = RC::CalculateValue(nodeAnimA.translate, timeA);
            RC::Vector3 posB = RC::CalculateValue(nodeAnimB.translate, timeB);
            if (isRootA) posA = RemoveRootMotionDrift(nodeAnimA, timeA, posA, rest);
            if (isRootB) posB = RemoveRootMotionDrift(nodeAnimB, timeB, posB, rest);
            RC::Quaternion rotA = RC::CalculateValue(nodeAnimA.rotate, timeA);
            RC::Quaternion rotB = RC::CalculateValue(nodeAnimB.rotate, timeB);
            RC::Vector3 sclA = RC::CalculateValue(nodeAnimA.scale, timeA);
            RC::Vector3 sclB = RC::CalculateValue(nodeAnimB.scale, timeB);

            // 補間: tB + (1-t)A （回転は Slerp）
            joint.transform.translate = Lerp(posA, posB, t);
            joint.transform.rotate    = Slerp(rotA, rotB, t);
            joint.transform.scale     = Lerp(sclA, sclB, t);
        } else if (hasB) {
            // 次のアニメーションにしかキーが無い場合はBのみ適用
            const RC::NodeAnimation& nodeAnimB = itB->second;
            RC::Vector3 posB = RC::CalculateValue(nodeAnimB.translate, timeB);
            if (isRootB) posB = RemoveRootMotionDrift(nodeAnimB, timeB, posB, rest);
            joint.transform.translate = posB;
            joint.transform.rotate    = RC::CalculateValue(nodeAnimB.rotate, timeB);
            joint.transform.scale     = RC::CalculateValue(nodeAnimB.scale, timeB);
        } else if (hasA) {
            // 前のアニメーションにしかキーが無い場合はAのみ適用
            const RC::NodeAnimation& nodeAnimA = itA->second;
            RC::Vector3 posA = RC::CalculateValue(nodeAnimA.translate, timeA);
            if (isRootA) posA = RemoveRootMotionDrift(nodeAnimA, timeA, posA, rest);
            joint.transform.translate = posA;
            joint.transform.rotate    = RC::CalculateValue(nodeAnimA.rotate, timeA);
            joint.transform.scale     = RC::CalculateValue(nodeAnimA.scale, timeA);
        }
    }
}

// ------------------------------------------------------------------
// AttachAnimation
// ------------------------------------------------------------------

void ModelObject::AttachAnimation() {
    // filePath_がディレクトリの場合、ModelMeshが解決した実ファイルパスを使う
    std::string animPath = filePath_;
    if (resource_.GetMesh() && !resource_.GetMesh()->SourceFilePath().empty()) {
        animPath = resource_.GetMesh()->SourceFilePath();
    }
    AttachAnimation(animPath);
}

void ModelObject::AttachAnimation(const std::string& filePath) {
    AttachAnimation(filePath, 0);
}

void ModelObject::AttachAnimation(const std::string& filePath, int animIndex) {
    if (filePath.empty()) return;
    animationRequested_ = true;
    animation_ = RC::LoadAnimationFile(filePath, animIndex);
    isAnimated_ = (animation_.duration > 0.0f && !animation_.nodeAnimations.empty());
    animationTime_ = 0.0f;
    blendFactor_ = 1.0f; // 新しくアタッチされたらブレンドなし

    // ルートモーション担当Jointはアニメーションごとに異なるため再判定させる
    rootMotionResolved_ = false;
    rootMotionJointName_.clear();
    prevRootMotionJointName_.clear();

    // メッシュが既にロード済みならSkeletonを即座に構築
    if (resource_.GetMesh() && resource_.GetMesh()->Ready()) {
        skeleton_ = CreateSkeleton(resource_.GetMesh()->RootNode());
        CaptureRestPose_(); // アニメーション適用前に rest を控える
        UpdateSkeleton(skeleton_);
        hasSkeleton_ = true;
    }
    // メッシュ未ロードの場合、UpdateAnimation内で遅延構築する
}

// ------------------------------------------------------------------
// CrossfadeAnimation: 前のAnimationとブレンド切り替え
// ------------------------------------------------------------------
void ModelObject::CrossfadeAnimation(const std::string& filePath, float blendDuration) {
    CrossfadeAnimation(filePath, 0, blendDuration);
}

void ModelObject::CrossfadeAnimation(const std::string& filePath, int animIndex, float blendDuration) {
    if (filePath.empty()) return;

    // まだアニメーションが未再生、またはブレンド秒数がほぼ0の場合は通常アタッチ
    if (!isAnimated_ || blendDuration <= 0.001f) {
        AttachAnimation(filePath, animIndex);
        return;
    }

    // 現在の Animation (A) を退避
    prevAnimation_ = animation_;
    prevAnimationTime_ = animationTime_;
    blendDuration_ = blendDuration;
    blendFactor_ = 0.0f; // 補間割合 0.0 (=前アニメーション A から開始)
    animationRequested_ = true;

    // 現在のルートモーション担当Joint (A) を退避してから再判定させる。
    // Attach 直後（UpdateAnimation を1度も通らずに）Crossfade された場合は
    // A 側が未判定のままなので、ここで先に解決しておく。これを省くと
    // ブレンド中だけ A の焼き込み移動が復活し、一瞬ドリフトして見える。
    ResolveRootMotionJoint_();
    prevRootMotionJointName_ = rootMotionJointName_;
    rootMotionJointName_.clear();
    rootMotionResolved_ = false;

    // 次の Animation (B) をロード
    animation_ = RC::LoadAnimationFile(filePath, animIndex);
    animationTime_ = 0.0f;
    isAnimated_ = (animation_.duration > 0.0f && !animation_.nodeAnimations.empty());

    if (!isAnimated_) {
        // 読み込み失敗時は補間をスキップして終了（退避した状態も破棄する）
        blendFactor_ = 1.0f;
        prevRootMotionJointName_.clear();
        return;
    }

    // メッシュが既にロード済みでスケルトン未作成なら構築
    if (resource_.GetMesh() && resource_.GetMesh()->Ready() && !hasSkeleton_) {
        skeleton_ = CreateSkeleton(resource_.GetMesh()->RootNode());
        CaptureRestPose_(); // アニメーション適用前に rest を控える
        UpdateSkeleton(skeleton_);
        hasSkeleton_ = true;
    }
}

// ------------------------------------------------------------------
// UpdateAnimation
// ------------------------------------------------------------------

void ModelObject::UpdateAnimation(float dt) {
    if (!animationRequested_) return;

    // メッシュがまだロードされていない場合はスキップ
    if (!resource_.GetMesh() || !resource_.GetMesh()->Ready()) return;

    // アニメーションが未ロードの場合、メッシュの実ファイルパスでリトライ
    if (!isAnimated_) {
        const std::string& meshPath = resource_.GetMesh()->SourceFilePath();
        if (!meshPath.empty()) {
            animation_ = RC::LoadAnimationFile(meshPath);
            isAnimated_ = (animation_.duration > 0.0f && !animation_.nodeAnimations.empty());
        }
        if (!isAnimated_) {
            animationRequested_ = false; // リトライ失敗、以降は試みない
            return;
        }
    }

    // 遅延Skeleton構築（メッシュの非同期ロード完了を待つ）
    if (!hasSkeleton_) {
        skeleton_ = CreateSkeleton(resource_.GetMesh()->RootNode());
        CaptureRestPose_(); // アニメーション適用前に rest を控える
        UpdateSkeleton(skeleton_);
        hasSkeleton_ = true;
    }

    // ルートモーション担当Jointの判定（アニメーション or スケルトン更新後に一度だけ）
    ResolveRootMotionJoint_();

    // 切り替え補間中の場合：前のアニメーション(A)も「同時に再生しておく」
    if (blendFactor_ < 1.0f) {
        prevAnimationTime_ += dt;
        if (prevAnimation_.duration > 1e-6f) {
            prevAnimationTime_ = std::fmod(prevAnimationTime_, prevAnimation_.duration);
        } else {
            prevAnimationTime_ = 0.0f;
        }

        // 補間割合 t を進行
        blendFactor_ += dt / blendDuration_;
        if (blendFactor_ >= 1.0f) {
            blendFactor_ = 1.0f;
            prevAnimation_ = RC::Animation(); // 切り替え完了、前のデータ解放
            prevRootMotionJointName_.clear();
        }
    }

    // 次のアニメーション(B)時間を進める
    animationTime_ += dt;
    if (animation_.duration > 1e-6f) {
        animationTime_ = std::fmod(animationTime_, animation_.duration);
    } else {
        animationTime_ = 0.0f;
    }

    // Skeleton パス: 全Jointの階層行列を更新
    if (hasSkeleton_) {
        if (blendFactor_ < 1.0f && !prevAnimation_.nodeAnimations.empty()) {
            // 2つのAnimationを同時に再生＆ tB + (1-t)A ブレンド（回転はSlerp）
            ApplyAnimationBlend(skeleton_, prevAnimation_, prevAnimationTime_, animation_, animationTime_, blendFactor_,
                                prevRootMotionJointName_, rootMotionJointName_, &restTranslations_);
        } else {
            // 単一アニメーションの適用
            ApplyAnimation(skeleton_, animation_, animationTime_, rootMotionJointName_, &restTranslations_);
        }
        UpdateSkeleton(skeleton_);

        // スキニング行列パレット計算: T_i = InverseBindPose_i * SkeletonSpaceMatrix_i
        const auto mesh = resource_.GetMesh();
        if (mesh && mesh->HasSkinData()) {
            const auto &skinData = mesh->GetSkinData();
            const auto &ibpMatrices = skinData.inverseBindPoseMatrices;
            skinMatrices_.resize(ibpMatrices.size());

            for (const auto &[jointName, boneIdx] : skinData.jointNameToIndex) {
                // jointMapを使って高速検索（O(1)）
                auto jit = skeleton_.jointMap.find(jointName);
                if (jit != skeleton_.jointMap.end()) {
                    // T_i = IBP_i * SSM_i
                    skinMatrices_[boneIdx] = Multiply(
                        ibpMatrices[boneIdx],
                        skeleton_.joints[jit->second].skeletonSpaceMatrix);
                } else {
                    // ボーン名がSkeletonに無い場合は単位行列
                    skinMatrices_[boneIdx] = MakeIdentity4x4();
                }
            }
        }
    }

    // レガシーパス: アニメーションチャンネルが1つだけの場合
    // （AnimatedCube等の単純アニメーション）は transform_ に適用する。
    if (!hasSkeleton_ && !HasSkinData() && animation_.nodeAnimations.size() == 1) {
        auto it = animation_.nodeAnimations.begin();
        RC::NodeAnimation& rootNodeAnim = it->second;
        if (blendFactor_ < 1.0f && prevAnimation_.nodeAnimations.size() == 1) {
            auto prevIt = prevAnimation_.nodeAnimations.begin();
            RC::Vector3 posA = RC::CalculateValue(prevIt->second.translate, prevAnimationTime_);
            RC::Vector3 posB = RC::CalculateValue(rootNodeAnim.translate, animationTime_);
            RC::Quaternion rotA = RC::CalculateValue(prevIt->second.rotate, prevAnimationTime_);
            RC::Quaternion rotB = RC::CalculateValue(rootNodeAnim.rotate, animationTime_);
            RC::Vector3 sclA = RC::CalculateValue(prevIt->second.scale, prevAnimationTime_);
            RC::Vector3 sclB = RC::CalculateValue(rootNodeAnim.scale, animationTime_);

            transform_.translation = Lerp(posA, posB, blendFactor_);
            transform_.rotation = QuaternionToEuler(Slerp(rotA, rotB, blendFactor_));
            transform_.scale = Lerp(sclA, sclB, blendFactor_);
        } else {
            transform_.translation = RC::CalculateValue(rootNodeAnim.translate, animationTime_);
            transform_.rotation = QuaternionToEuler(RC::CalculateValue(rootNodeAnim.rotate, animationTime_));
            transform_.scale = RC::CalculateValue(rootNodeAnim.scale, animationTime_);
        }
    }
}

// ------------------------------------------------------------------
// DrawSkeleton: デバッグ描画
// ------------------------------------------------------------------
// 各Jointの skeletonSpaceMatrix × worldMatrix でワールド座標を算出し、
// Jointを球で、親子間を線で描画する。

// RenderCommon.h の RC 名前空間関数を使う前方宣言
namespace RC {
void DrawLine3D(const Vector3& a, const Vector3& b, const Vector4& color,
                bool depth);
void DrawSphereRings3D(const Vector3& center, float radius,
                       const Vector4& color, int segments,
                       bool depth);
} // namespace RC

void ModelObject::DrawSkeleton() {
    if (!hasSkeleton_ || skeleton_.joints.empty()) return;

    // モデルの描画に使われているワールド行列を求める。
    // SetWorldOverride 中（ボーン追従など）に Transform の TRS から作ると、
    // モデル本体と骨格の位置がずれてしまう。
    const RC::Matrix4x4 world = hasWorldOverride_
        ? worldOverride_
        : MakeAffineMatrix(transform_.scale, transform_.rotation,
                           transform_.translation);

    // 各Jointのワールド座標を先に求めておく（親の行列を二重計算しない）
    const size_t jointCount = skeleton_.joints.size();
    std::vector<RC::Vector3> jointPositions(jointCount);
    RC::Vector3 mn{FLT_MAX, FLT_MAX, FLT_MAX};
    RC::Vector3 mx{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (size_t i = 0; i < jointCount; ++i) {
        // jointWorldMatrix = skeletonSpaceMatrix * worldMatrix
        const RC::Matrix4x4 jointWorld =
            Multiply(skeleton_.joints[i].skeletonSpaceMatrix, world);

        // 行列の平行移動成分からワールド座標を取得
        const RC::Vector3 pos = {jointWorld.m[3][0], jointWorld.m[3][1],
                                 jointWorld.m[3][2]};
        jointPositions[i] = pos;

        // windows.h が min / max を関数マクロとして定義しているため、
        // 括弧で囲んでマクロ展開を止める（このリポジトリでは NOMINMAX を定義していない）。
        mn.x = (std::min)(mn.x, pos.x); mx.x = (std::max)(mx.x, pos.x);
        mn.y = (std::min)(mn.y, pos.y); mx.y = (std::max)(mx.y, pos.y);
        mn.z = (std::min)(mn.z, pos.z); mx.z = (std::max)(mx.z, pos.z);
    }

    // 色定義
    const RC::Vector4 jointColor = {1.0f, 1.0f, 0.0f, 1.0f}; // 黄色（Joint球）
    const RC::Vector4 boneColor  = {1.0f, 1.0f, 1.0f, 1.0f}; // 白（Bone線）

    // Joint球の半径は骨格の大きさから決める。
    // 固定値だとモデルのスケール（cm単位のモデル等）によって
    // 点にしか見えない／巨大すぎるという状態になるため。
    const float extent = (std::max)({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
    const float jointRadius = (extent > 1e-4f) ? extent * 0.015f : 0.02f;

    for (size_t i = 0; i < jointCount; ++i) {
        const RC::Vector3& jointPos = jointPositions[i];

        // Joint位置に球を描画
        RC::DrawSphereRings3D(jointPos, jointRadius, jointColor, 8, false);

        // 親がいれば親Jointとの間にBone線を描画
        if (const auto& parent = skeleton_.joints[i].parent) {
            RC::DrawLine3D(jointPositions[*parent], jointPos, boneColor, false);
        }
    }
}

bool ModelObject::HasSkinData() const {
    const auto mesh = resource_.GetMesh();
    return mesh && mesh->HasSkinData() && !skinMatrices_.empty();
}
