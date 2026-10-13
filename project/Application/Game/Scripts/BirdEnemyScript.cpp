#include "EnemyBaseScript.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "ECS/NativeScriptComponent.h"
#include "ECS/WaterComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// ============================================================================
// 鳥（羽を飛ばす敵）
//
//   - 水上の空を飛ぶ。自機の前方 preferredDistance・上空 flyHeight の位置へ回り込む
//   - 射程内で前方にいると、翼を高く振り上げて「ため」→ 翼を振り下ろして羽根を扇状に放つ
//   - 羽根（FeatherScript）は自機に届くと HP を削る。撃ち落とせる
//   - 1 回撃つごとに自機の正面を横切って反対側へ移る（同じ位置から撃ち続けない）
//
//   サメ（水面を突っ込む）・船（遠くから砲撃）・タコ（墨で視界）に対して、
//   鳥は「上から」来る。照準を上へ振らせることで、水面ばかり見ていられなくする。
//
//   見た目はプリミティブの組み合わせ。本体（胴）はスポナーが作る球で、
//   頭・くちばし・尾・翼（内側/外側 × 左右）を別エンティティとして生成し、毎フレーム姿勢を書き込む。
//   ※ エンジンの描画は Transform の親子を解決しない（PrimitiveMesh へは local の値がそのまま渡る）ため、
//     部位のワールド姿勢はこのスクリプトが自前で合成している。
//
//   スクリプト間の通信は他の敵と同じくタグのみ：
//     羽根 → 自機 : pending_damage（同じフレームに複数当たっても 1 発ぶん）
//
//   依存：撃破・被弾時の羽毛の粒は OctopusEnemyScript.cpp の InkParticleScript を名前で使い回す。
// ============================================================================

namespace BirdDetail {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

/// @brief シーンのメインカメラ（isMain 優先。無ければ最初のカメラ）を探す
/// @param scene 検索するシーン
/// @return 見つかったカメラのエンティティ。無ければ nullptr
inline std::shared_ptr<Entity> FindPlayerCamera(Scene *scene) {
  if (!scene) return nullptr;
  std::shared_ptr<Entity> fallback;
  for (const auto &e : scene->GetEntities()) {
    if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
    auto *cam = e->GetComponent<CameraComponent>();
    if (!cam || !e->GetComponent<TransformComponent>()) continue;
    if (cam->isMain) return e;
    if (!fallback) fallback = e;
  }
  return fallback;
}

/// @brief シーンの水面の高さ（WaterComponent の Transform.y）を取得する
/// @param scene 検索するシーン
/// @param outY 見つかった水面の高さの格納先
/// @return 水面が見つかれば true、無ければ false
inline bool FindWaterY(Scene *scene, float &outY) {
  if (!scene) return false;
  for (const auto &e : scene->GetEntities()) {
    if (!e || !e->GetComponent<WaterComponent>()) continue;
    auto *tr = e->GetComponent<TransformComponent>();
    outY = tr ? tr->position.y : 0.0f;
    return true;
  }
  return false;
}

/// @brief ベクトルの長さを求める
/// @param v 対象のベクトル
/// @return v の長さ
inline float Len(const RC::Vector3 &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
/// @brief ベクトルの差 a - b を求める
/// @param a 引かれるベクトル
/// @param b 引くベクトル
/// @return a - b
inline RC::Vector3 Sub(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
/// @brief ベクトルの和 a + b を求める
/// @param a 1 つ目のベクトル
/// @param b 2 つ目のベクトル
/// @return a + b
inline RC::Vector3 Add(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
/// @brief ベクトルをスカラー倍する
/// @param a 対象のベクトル
/// @param s 倍率
/// @return a * s
inline RC::Vector3 Scale(const RC::Vector3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
/// @brief ベクトルを正規化する
/// @param v 対象のベクトル
/// @param fallback 長さがほぼ 0 で正規化できないときに返す値
/// @return 正規化したベクトル（または fallback）
inline RC::Vector3 Normalize(const RC::Vector3 &v, const RC::Vector3 &fallback = {0.0f, 0.0f, 1.0f}) {
  const float l = Len(v);
  return (l > 1e-5f) ? RC::Vector3{v.x / l, v.y / l, v.z / l} : fallback;
}

/// @brief 角度差を -π..π に丸める
/// @param a 角度 (rad)
/// @return -π..π に丸めた角度 (rad)
inline float WrapAngle(float a) {
  while (a > kPi) a -= 2.0f * kPi;
  while (a < -kPi) a += 2.0f * kPi;
  return a;
}

// ----------------------------------------------------------------------------
// 3x3 回転行列（エンジンと同じ「行ベクトル × 行列」の規約。v' = v * M）
//
//   エンジンの MakeAffineMatrix は R = Rx(rot.x) * Ry(rot.y) * Rz(rot.z) で、
//   Z が最後（ワールド軸）にかかる。鳥の「機首方位→ピッチ→ロール」をそのまま
//   rotation に入れるとロールがワールド Z 軸回りになってしまうので、
//   欲しい回転を行列で合成してから、エンジンの順序のオイラー角へ分解し直す。
// ----------------------------------------------------------------------------
/// @brief 3x3 回転行列（行ベクトル × 行列の規約）
struct Mat3 {
  float m[3][3];
};

/// @brief 行列の積 a * b を求める
/// @param a 左側の行列（行ベクトル規約なので先に適用される）
/// @param b 右側の行列
/// @return a * b
inline Mat3 Mul(const Mat3 &a, const Mat3 &b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
  return r;
}
/// @brief X / Y / Z 軸まわりの回転行列を作る（RotX / RotY / RotZ 共通）
/// @param t 回転角 (rad)
/// @return 回転行列
/// @note Math.cpp の MakeRotateMatrix(X/Y/Z) と同じ並び
inline Mat3 RotX(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{1, 0, 0}, {0, c, s}, {0, -s, c}}};
}
inline Mat3 RotY(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{c, 0, -s}, {0, 1, 0}, {s, 0, c}}};
}
inline Mat3 RotZ(float t) {
  const float c = std::cos(t), s = std::sin(t);
  return {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}};
}
/// @brief 行ベクトル v を M で変換する（v * M）
/// @param v 変換するベクトル
/// @param M 変換行列
/// @return v * M
inline RC::Vector3 Apply(const RC::Vector3 &v, const Mat3 &M) {
  return {v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0],
          v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
          v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2]};
}
/// @brief M = Rx(a) * Ry(b) * Rz(c) となる (a, b, c) を返す（TransformComponent::rotation に入れる値）
/// @details 展開すると m02 = -sin b, m12 = sin a cos b, m22 = cos a cos b, m01 = sin c cos b, m00 = cos b cos c。
/// @param M 分解する回転行列
/// @return エンジンの回転順序でのオイラー角 (rad)
inline RC::Vector3 ToEngineEuler(const Mat3 &M) {
  const float sb = std::clamp(-M.m[0][2], -1.0f, 1.0f);
  const float b = std::asin(sb);
  // しきい値を 0.9999 にすると cos b ≈ 0.014 まで下の近似に落ち、姿勢が 0.02 ずれる（数値検証で確認）。
  // cos b ≈ 0.0014 までは通常の式で float 精度が足りるので、ぎりぎりまで通常の式を使う。
  if (std::fabs(sb) < 0.999999f) {
    return {std::atan2(M.m[1][2], M.m[2][2]), b, std::atan2(M.m[0][1], M.m[0][0])};
  }
  // ジンバルロック（真上・真下を向いた）：c = 0 に寄せる。m10 = sin a sin b, m11 = cos a
  return {std::atan2(M.m[1][0] * sb, M.m[1][1]), b, 0.0f};
}
/// @brief 機首方位 yaw・機首上げ pitchUp・右翼上げ roll から姿勢行列を作る
/// @details ロール→ピッチ→ヨーの順に（機体の軸で）回す。+Z が機首、+X が右翼、+Y が背中。
///          Rx は正の角で機首が下がる向きなので pitchUp は符号を反転して渡す。
/// @param yaw 機首方位 (rad)
/// @param pitchUp 機首上げ角 (rad、正で上向き)
/// @param roll ロール角 (rad、正で右翼が上がる)
/// @return 機体の姿勢行列
inline Mat3 BodyMatrix(float yaw, float pitchUp, float roll) {
  return Mul(Mul(RotZ(roll), RotX(-pitchUp)), RotY(yaw));
}

/// @brief PrimitiveMesh の見た目を今の Transform に合わせる（生成直後の原点チラつき対策）
/// @param e 対象のエンティティ
inline void SyncPrimitive(Entity &e) {
  auto *pm = e.GetComponent<PrimitiveMeshComponent>();
  auto *tr = e.GetComponent<TransformComponent>();
  if (!pm || !tr || pm->meshHandle < 0) return;
  if (auto *pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
    pmTr->scale = tr->scale;
    pmTr->rotation = tr->rotation;
    pmTr->translation = tr->position;
  }
}

/// @brief PrimitiveMesh のマテリアル色を変更する
/// @param e 対象のエンティティ
/// @param c 設定する色（RGBA）
inline void SetPrimitiveColor(Entity &e, const RC::Vector4 &c) {
  auto *pm = e.GetComponent<PrimitiveMeshComponent>();
  if (!pm || pm->meshHandle < 0) return;
  if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) mat->color = c;
}

/// @brief 色（4 成分ベクトル）を線形補間する
/// @param a t = 0 のときの値
/// @param b t = 1 のときの値
/// @param t 補間係数
/// @return 補間結果
inline RC::Vector4 Lerp4(const RC::Vector4 &a, const RC::Vector4 &b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

/// @brief 羽毛の粒を散らす（被弾・撃破・羽根の撃ち落とし）
/// @details 粒の動きは InkParticleScript（kind 0：縮みながら薄れる）を使い回す。
///          墨と違ってふわっと漂わせたいので、抵抗を強め・重力を弱めにする。
///          エンティティ名を "BirdPuff" にして、墨の粒（InkParticle）とは別のプールで回す。
/// @param scene 生成先のシーン
/// @param ctx 粒のスクリプトに渡すシーンコンテキスト（nullptr 可）
/// @param pos 散らす中心位置
/// @param count 粒の数
/// @param size 粒の基準の大きさ
/// @param color 粒の色（w は初期の不透明度）
inline void SpawnFeatherPuff(Scene *scene, SceneContext *ctx, const RC::Vector3 &pos, int count, float size,
                             const RC::Vector4 &color) {
  if (!scene || count <= 0) return;
  static std::mt19937 rng{20260928u}; // 見た目だけなので固定シードで十分
  std::uniform_real_distribution<float> U(0.0f, 1.0f);

  // "Effects" フォルダと、再利用できる非アクティブな "BirdPuff" を必要数まで集める
  uint64_t folder = 0;
  std::vector<std::shared_ptr<Entity>> pool;
  for (const auto &e : scene->GetEntities()) {
    if (!e) continue;
    if (folder == 0 && e->IsFolder() && e->GetName() == "Effects") folder = e->Guid();
    if (!e->IsActive() && !e->IsPendingDestroy() && e->GetName() == "BirdPuff" &&
        static_cast<int>(pool.size()) < count) {
      pool.push_back(e);
    }
  }
  if (folder == 0) {
    auto f = scene->CreateEntity("Effects");
    f->SetIsFolder(true);
    folder = f->Guid();
  }

  auto toTag = [](float v) { return static_cast<int>(v * 1000.0f); };
  for (int i = 0; i < count; ++i) {
    // 球面上に一様に散らす（上向きを少し強めに）
    const float z = U(rng) * 2.0f - 1.0f;
    const float a = U(rng) * 2.0f * kPi;
    const float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
    const float sp = 2.0f + U(rng) * 4.0f;
    const RC::Vector3 vel = {std::cos(a) * r * sp, z * sp + 1.5f, std::sin(a) * r * sp};
    const float s = size * (0.5f + U(rng) * 0.6f);
    const float life = 0.8f + U(rng) * 0.7f;

    // プールに残りがあれば使い回し、足りない分だけ新規に作る
    std::shared_ptr<Entity> e;
    const bool isNew = (i >= static_cast<int>(pool.size()));
    if (!isNew) {
      e = pool[static_cast<size_t>(i)];
      e->SetActive(true);
      e->SetTag("reused", 1);
    } else {
      e = scene->CreateEntity("BirdPuff");
      if (!e) continue;
    }
    e->SetParentGuid(folder);

    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) tr = &e->AddComponent<TransformComponent>();
    tr->position = pos;
    tr->scale = {s, s, s};

    auto *pm = e->GetComponent<PrimitiveMeshComponent>();
    if (!pm) {
      pm = &e->AddComponent<PrimitiveMeshComponent>();
      pm->type = PrimitiveType::Sphere;
    }
    pm->color = color;

    // InkParticleScript へ動きのパラメータをタグで渡す（タグは int なので 1000 倍の固定小数）
    e->SetTag("no_shadow", 1);
    e->SetTag("ink_kind", 0);
    e->SetTag("ink_vx", toTag(vel.x));
    e->SetTag("ink_vy", toTag(vel.y));
    e->SetTag("ink_vz", toTag(vel.z));
    e->SetTag("ink_s0", toTag(s));
    e->SetTag("ink_s1", toTag(s));
    e->SetTag("ink_life", toTag(life));
    e->SetTag("ink_a0", toTag(color.w));
    e->SetTag("ink_drag", toTag(4.0f));
    e->SetTag("ink_grav", toTag(1.2f));

    auto *nsc = e->GetComponent<NativeScriptComponent>();
    if (!nsc) {
      nsc = &e->AddComponent<NativeScriptComponent>();
      nsc->AddScript("InkParticleScript");
      nsc->SetScene(scene);
      if (ctx) nsc->SetSceneContext(ctx);
    }
    if (isNew) scene->InitDynamicEntityRuntime(*e);

    if (pm->meshHandle >= 0) {
      if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
        mat->color = color; // 使い回しの粒は前回の透明度が残っている
        mat->shininess = 0.0f;
        mat->environmentCoefficient = 0.0f;
      }
      SyncPrimitive(*e);
    }
  }
}

} // namespace BirdDetail

// ============================================================================
// 羽根（鳥が放つ弾）
// ============================================================================

/// @brief 鳥が放つ羽根。まっすぐ飛び、自機に届いたら HP を削る。撃ち落とせる
/// @details WaterBullet（EnemyBullet）を使わないのは、あちらは重力が掛かる・向きを持たない・
///          水面 y=0 固定で消えるため。羽根は「軸を進行方向へ向けて、くるくる回りながら直進」させたい。
///          撃ち落とし判定は InkBlobScript と同じく自前で持つ（is_enemy を付けると HP バーや敵数に混ざる）。
class FeatherScript : public ScriptableEntity {
public:
  float speed = 20.0f;       ///< 飛ぶ速さ (m/s)
  float lifetime = 4.0f;     ///< 何秒で消えるか
  float hitRadius = 1.3f;    ///< 自機（カメラ）にこの距離まで来たら命中
  float shootRadius = 1.1f;  ///< 自機の弾がこの距離まで来たら撃ち落とされる
  int damage = 1;            ///< 命中時のダメージ
  float spinSpeed = 14.0f;   ///< 軸まわりの回転 (rad/s)
  float waterY = 0.0f;       ///< これより下へ行ったら消える（水面）
  RC::Vector3 direction = {0.0f, 0.0f, 1.0f}; ///< 進行方向（OnCreate で正規化する）

protected:
  /// @brief 羽根のパラメータを JSON へ書き出す
  /// @return 速度・寿命・判定半径・ダメージ・回転速度・水面高さ・向きを格納した json
  nlohmann::json Serialize() override {
    return {
        {"speed", speed},
        {"lifetime", lifetime},
        {"hitRadius", hitRadius},
        {"shootRadius", shootRadius},
        {"damage", damage},
        {"spinSpeed", spinSpeed},
        {"waterY", waterY},
        {"dir", {direction.x, direction.y, direction.z}},
    };
  }

  /// @brief JSON から羽根のパラメータを読み込む（含まれる項目だけ上書きする）
  /// @param j 読み込み元の json（鳥が発射時に pendingData として渡す）
  void Deserialize(const nlohmann::json &j) override {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    f("speed", speed);
    f("lifetime", lifetime);
    f("hitRadius", hitRadius);
    f("shootRadius", shootRadius);
    f("spinSpeed", spinSpeed);
    f("waterY", waterY);
    if (j.contains("damage") && j["damage"].is_number()) damage = j["damage"].get<int>();
    if (j.contains("dir") && j["dir"].is_array() && j["dir"].size() == 3) {
      direction = {j["dir"][0].get<float>(), j["dir"][1].get<float>(), j["dir"][2].get<float>()};
    }
  }

  /// @brief 進行方向を正規化し、基準スケールを保存して初期の向きを付ける
  void OnCreate() override {
    direction = BirdDetail::Normalize(direction);
    elapsed_ = 0.0f;
    if (auto *tr = GetComponent<TransformComponent>()) {
      baseScale_ = tr->scale;
      ApplyOrientation(*tr);
    }
  }

  /// @brief 直進させ、自機への命中・水面への落下・自機の弾による撃ち落としを判定する
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(float dt) override {
    if (done_ || dt <= 0.0f) return;
    auto *tr = GetComponent<TransformComponent>();
    Scene *scene = GetScene();
    if (!tr || !scene) return;

    elapsed_ += dt;
    if (elapsed_ >= lifetime) { Finish(); return; }

    // 自機（カメラ）を取得する。キャッシュが無効になったときだけ探し直す
    std::shared_ptr<Entity> cam = target_.lock();
    if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
      cam = BirdDetail::FindPlayerCamera(scene);
      target_ = cam;
    }

    const RC::Vector3 from = tr->position;
    tr->position = BirdDetail::Add(tr->position, BirdDetail::Scale(direction, speed * dt));
    const RC::Vector3 to = tr->position;
    ApplyOrientation(*tr);

    // --- 命中：このフレームの移動線分と自機の距離で見る（速いのですり抜け防止） ---
    if (cam) {
      if (auto *camTr = cam->GetComponent<TransformComponent>()) {
        if (SegmentPointDistSq(from, to, camTr->position) <= hitRadius * hitRadius) {
          // 扇状に撃つので同じフレームに 2 枚当たることがある。足し算にすると 1 回で 2 削れて
          // 理不尽なので、そのフレームの被ダメージは「一番大きい 1 発」にそろえる。
          const int cur = cam->GetTagInt("pending_damage", 0);
          cam->SetTag("pending_damage", (std::max)(cur, damage));
          Log::Print("[FeatherScript] Feather hit the player!");
          Finish();
          return;
        }
      }
    }

    // --- 水面に落ちたら消える ---
    if (to.y < waterY) { Finish(); return; }

    // --- 撃ち落とし：自機の弾が近くを通れば両方消す ---
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
      if (e->GetName() != "PlayerBullet") continue;
      auto *btr = e->GetComponent<TransformComponent>();
      if (!btr) continue;
      if (SegmentPointDistSq(from, to, btr->position) > shootRadius * shootRadius) continue;
      e->SetActive(false); // 自機の弾はプール運用（再利用時に reused で初期化される）
      if (cam) cam->SetTag("score_add", cam->GetTagInt("score_add", 0) + 1);
      BirdDetail::SpawnFeatherPuff(scene, GetSceneContext(), tr->position, 4,
                                   (std::max)(baseScale_.x, 0.05f) * 0.8f, {0.95f, 0.95f, 0.92f, 0.9f});
      Finish();
      return;
    }
  }

private:
  /// @brief 線分 ab と点 p の最短距離の 2 乗を求める
  /// @param a 線分の始点
  /// @param b 線分の終点
  /// @param p 点
  /// @return 最短距離の 2 乗
  static float SegmentPointDistSq(const RC::Vector3 &a, const RC::Vector3 &b, const RC::Vector3 &p) {
    const RC::Vector3 ab = BirdDetail::Sub(b, a);
    const float l2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = 0.0f;
    if (l2 > 1e-8f) {
      t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y + (p.z - a.z) * ab.z) / l2;
      t = std::clamp(t, 0.0f, 1.0f);
    }
    const RC::Vector3 d = BirdDetail::Sub(p, BirdDetail::Add(a, BirdDetail::Scale(ab, t)));
    return d.x * d.x + d.y * d.y + d.z * d.z;
  }

  /// @brief 羽根の軸（ローカル +Z）を進行方向へ向け、軸まわりに回す
  /// @param tr 書き込み先の Transform
  void ApplyOrientation(TransformComponent &tr) {
    const float yaw = std::atan2(direction.x, direction.z);
    const float horiz = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const float pitchUp = std::atan2(direction.y, horiz);
    tr.rotation = BirdDetail::ToEngineEuler(BirdDetail::BodyMatrix(yaw, pitchUp, elapsed_ * spinSpeed));
    // ひらひら（幅だけ脈打たせる）
    const float wob = 1.0f + 0.25f * std::sin(elapsed_ * 22.0f);
    tr.scale = {baseScale_.x * wob, baseScale_.y, baseScale_.z};
  }

  /// @brief 羽根を終了させて自身を破棄する（以降の更新は行わない）
  void Finish() {
    done_ = true;
    if (Entity *self = GetEntity()) self->Destroy();
  }

  std::weak_ptr<Entity> target_;
  RC::Vector3 baseScale_ = {0.1f, 0.03f, 0.45f};
  float elapsed_ = 0.0f;
  bool done_ = false;
};

REGISTER_SCRIPT(FeatherScript)

// ============================================================================
// 鳥本体
// ============================================================================

/// @brief 鳥の行動状態（待機 → 接近 → ため → 投擲 → 立て直し）
enum class BirdState { Wait, Approach, Windup, Throw, Recover };

/// @brief 鳥の敵。自機の前方上空に陣取り、ためてから羽根を扇状に放つ
/// @details 状態遷移・飛行（arrive 操舵）・姿勢と羽ばたき・部位エンティティの姿勢合成・撃破後の落下を担う。
class BirdEnemyScript : public EnemyBaseScript {
public:
  // --- 索敵・位置取り ---
  float detectDistance = 70.0f;     ///< この距離に自機が来たら動き出す
  float preferredDistance = 22.0f;  ///< 自機の前方このくらいの距離に陣取る
  float sideOffset = 7.0f;          ///< 自機の正面から左右へずれる量（撃つたびに左右が入れ替わる）
  float flyHeight = 7.0f;           ///< 水面からの飛行高度 (m)
  float ceilingY = 1000.0f;         ///< これより上へは行かない（洞窟の天井など。ワールド y）
  bool swapSideAfterFire = true;    ///< 撃ったあと自機の正面を横切って反対側へ移る

  // --- 飛行 ---
  float maxSpeed = 15.0f;       ///< 最高速 (m/s)
  float maxAccel = 16.0f;       ///< 最大加速度 (m/s²)。小さいほど大回りになる
  float arriveRadius = 10.0f;   ///< 目標までこの距離を切ると減速を始める
  float turnRate = 3.5f;        ///< 機首の向きを変える速さ (rad/s)
  float bankFactor = 1.0f;      ///< 旋回時の傾きの強さ（1 で横加速度どおり）
  float maxBankDeg = 45.0f;     ///< 旋回時の傾きの上限（度）

  // --- 羽ばたき ---
  float flapFrequency = 2.6f;   ///< 巡航時の羽ばたき (Hz)
  float flapAmplitudeDeg = 34.0f;
  float dihedralDeg = 6.0f;     ///< 翼の上反角（羽ばたきの中心）

  // --- 攻撃（羽根） ---
  float fireMinDistance = 8.0f;
  float fireMaxDistance = 38.0f;
  float fireInterval = 3.2f;       ///< 撃ってから次に撃てるまで（秒）
  float firstFireDelay = 2.0f;     ///< 動き出してから最初に撃てるまで（秒）
  float windupDuration = 0.75f;    ///< ため（翼を振り上げる）時間。この間に撃たれるとひるんで中断
  float throwDuration = 0.18f;     ///< 翼を振り下ろす時間。振り下ろしきった瞬間に羽根が出る
  float recoverDuration = 0.6f;    ///< 撃ったあと陣取りに戻るまで（秒）
  int featherCount = 5;            ///< 1 回に放つ羽根の枚数（扇状）
  float featherSpreadDeg = 9.0f;   ///< 隣り合う羽根の角度差
  float featherSpeed = 17.0f;      ///< 22m 先の自機まで約 1.3 秒。細い羽根を撃ち落とせる猶予として取った
  int featherDamage = 1;
  float featherLead = 0.5f;        ///< 自機の移動をどれだけ読むか（0..1。1 で完全な偏差撃ち）
  float featherScale = 1.0f;       ///< 羽根の大きさの倍率
  RC::Vector4 featherColor = {0.96f, 0.96f, 0.93f, 1.0f};

  // --- 見た目 ---
  RC::Vector4 wingColor = {0.62f, 0.66f, 0.72f, 1.0f};     ///< 翼（内側）と尾
  RC::Vector4 wingTipColor = {0.16f, 0.17f, 0.2f, 1.0f};   ///< 翼の先
  RC::Vector4 headColor = {0.97f, 0.97f, 0.95f, 1.0f};
  RC::Vector4 beakColor = {0.98f, 0.72f, 0.16f, 1.0f};
  RC::Vector4 windupColor = {1.0f, 0.55f, 0.15f, 1.0f};    ///< ため時に明滅する色（予兆）

protected:
  /// @brief 基底の体力に加えて、鳥のパラメータを JSON へ書き出す
  /// @return 全パラメータを格納した json
  nlohmann::json Serialize() override {
    nlohmann::json j = EnemyBaseScript::Serialize();
    auto c4 = [](const RC::Vector4 &c) { return nlohmann::json::array({c.x, c.y, c.z, c.w}); };
    j["detectDistance"] = detectDistance;
    j["preferredDistance"] = preferredDistance;
    j["sideOffset"] = sideOffset;
    j["flyHeight"] = flyHeight;
    j["ceilingY"] = ceilingY;
    j["swapSideAfterFire"] = swapSideAfterFire;
    j["maxSpeed"] = maxSpeed;
    j["maxAccel"] = maxAccel;
    j["arriveRadius"] = arriveRadius;
    j["turnRate"] = turnRate;
    j["bankFactor"] = bankFactor;
    j["maxBankDeg"] = maxBankDeg;
    j["flapFrequency"] = flapFrequency;
    j["flapAmplitudeDeg"] = flapAmplitudeDeg;
    j["dihedralDeg"] = dihedralDeg;
    j["fireMinDistance"] = fireMinDistance;
    j["fireMaxDistance"] = fireMaxDistance;
    j["fireInterval"] = fireInterval;
    j["firstFireDelay"] = firstFireDelay;
    j["windupDuration"] = windupDuration;
    j["throwDuration"] = throwDuration;
    j["recoverDuration"] = recoverDuration;
    j["featherCount"] = featherCount;
    j["featherSpreadDeg"] = featherSpreadDeg;
    j["featherSpeed"] = featherSpeed;
    j["featherDamage"] = featherDamage;
    j["featherLead"] = featherLead;
    j["featherScale"] = featherScale;
    j["featherColor"] = c4(featherColor);
    j["wingColor"] = c4(wingColor);
    j["wingTipColor"] = c4(wingTipColor);
    j["headColor"] = c4(headColor);
    j["beakColor"] = c4(beakColor);
    j["windupColor"] = c4(windupColor);
    return j;
  }

  /// @brief JSON から基底の体力と鳥のパラメータを読み込む（含まれる項目だけ上書きする）
  /// @param j 読み込み元の json
  void Deserialize(const nlohmann::json &j) override {
    EnemyBaseScript::Deserialize(j);
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto i = [&](const char *k, int &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<int>(); };
    auto c4 = [&](const char *k, RC::Vector4 &o) {
      if (j.contains(k) && j[k].is_array() && j[k].size() == 4) {
        o = {j[k][0].get<float>(), j[k][1].get<float>(), j[k][2].get<float>(), j[k][3].get<float>()};
      }
    };
    f("detectDistance", detectDistance);
    f("preferredDistance", preferredDistance);
    f("sideOffset", sideOffset);
    f("flyHeight", flyHeight);
    f("ceilingY", ceilingY);
    if (j.contains("swapSideAfterFire") && j["swapSideAfterFire"].is_boolean()) {
      swapSideAfterFire = j["swapSideAfterFire"].get<bool>();
    }
    f("maxSpeed", maxSpeed);
    f("maxAccel", maxAccel);
    f("arriveRadius", arriveRadius);
    f("turnRate", turnRate);
    f("bankFactor", bankFactor);
    f("maxBankDeg", maxBankDeg);
    f("flapFrequency", flapFrequency);
    f("flapAmplitudeDeg", flapAmplitudeDeg);
    f("dihedralDeg", dihedralDeg);
    f("fireMinDistance", fireMinDistance);
    f("fireMaxDistance", fireMaxDistance);
    f("fireInterval", fireInterval);
    f("firstFireDelay", firstFireDelay);
    f("windupDuration", windupDuration);
    f("throwDuration", throwDuration);
    f("recoverDuration", recoverDuration);
    i("featherCount", featherCount);
    f("featherSpreadDeg", featherSpreadDeg);
    f("featherSpeed", featherSpeed);
    i("featherDamage", featherDamage);
    f("featherLead", featherLead);
    f("featherScale", featherScale);
    c4("featherColor", featherColor);
    c4("wingColor", wingColor);
    c4("wingTipColor", wingTipColor);
    c4("headColor", headColor);
    c4("beakColor", beakColor);
    c4("windupColor", windupColor);
  }

  /// @brief 鳥としての既定値（HP・撃破演出時間・泡なし）を設定し、陣取る側と羽ばたきの位相を決める
  void OnCreate() override {
    EnemyBaseScript::OnCreate();
    // 空中の敵なので、撃破時の泡（水中向けの演出）は出さない。代わりに羽毛を散らす
    spawnDeathBubbles = false;
    if (!hpFromData) {
      hp = 2;
      maxHp = 2;
    }
    if (!deathDurationFromData) deathDuration = 2.4f;

    state_ = BirdState::Wait;
    if (auto *tr = GetComponent<TransformComponent>()) {
      baseScale_ = tr->scale;
      spawnPos_ = tr->position;
      yaw_ = tr->rotation.y;
      // 左右どちらに陣取るか・羽ばたきの位相は生成位置で決める（同じウェーブの個体がそろわないように）
      sideSign_ = (std::fmod(std::fabs(tr->position.x * 0.37f + tr->position.z * 0.71f), 2.0f) < 1.0f) ? 1.0f : -1.0f;
      flapPhase_ = std::fmod(std::fabs(tr->position.x * 1.3f + tr->position.z * 0.7f), 2.0f * BirdDetail::kPi);
    }
    fireCooldown_ = firstFireDelay;
  }

  /// @brief 生成した部位エンティティをまとめて破棄する
  void OnDestroy() override {
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    parts_.clear();
  }

  /// @brief 状態ごとの行動、姿勢・羽ばたきの更新、部位の姿勢書き込みを行う
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(float dt) override {
    EnemyBaseScript::OnUpdate(dt);
    if (dt <= 0.0f) return;

    auto *tr = GetComponent<TransformComponent>();
    Scene *scene = GetScene();
    if (!tr || !scene) return;
    time_ += dt;

    // 水面の高さは最初に一度だけ調べる（飛行高度と落下時の着水判定に使う）
    if (!waterChecked_) {
      hasWater_ = BirdDetail::FindWaterY(scene, waterY_);
      waterChecked_ = true;
    }
    // 部位は最初の Update で作る（OnCreate 時点ではまだスポナーの生成処理の途中のため）
    if (!partsBuilt_) BuildParts(scene);

    // 被弾フラッシュ（赤）を部位にも反映する。胴の色は基底の flashTimer が戻すので、部位だけ面倒を見る
    if (flashTimer > 0.0f && !partsFlashing_) {
      partsFlashing_ = true;
      for (auto &p : parts_) {
        if (auto e = p.entity.lock()) BirdDetail::SetPrimitiveColor(*e, {1.0f, 0.2f, 0.2f, 1.0f});
      }
    } else if (flashTimer <= 0.0f && partsFlashing_) {
      partsFlashing_ = false;
      for (auto &p : parts_) {
        if (auto e = p.entity.lock()) BirdDetail::SetPrimitiveColor(*e, p.color);
      }
    }

    if (isDead) {
      UpdateDeath(tr, dt);
      UpdateParts(tr->position);
      return;
    }

    // 自機（カメラ）を取得する。探し直したときは前フレーム位置が別物になるので速度推定をやり直す
    std::shared_ptr<Entity> cam = target_.lock();
    if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
      cam = BirdDetail::FindPlayerCamera(scene);
      target_ = cam;
      hasLastCamPos_ = false;
    }
    if (!cam) { UpdateParts(tr->position); return; }
    auto *camTr = cam->GetComponent<TransformComponent>();
    if (!camTr) { UpdateParts(tr->position); return; }

    UpdatePlayerMotion(*camTr, dt);

    const RC::Vector3 camRight = {camFwd_.z, 0.0f, -camFwd_.x};
    const RC::Vector3 toCam = BirdDetail::Sub(camTr->position, tr->position);
    const float dist = BirdDetail::Len(toCam);
    const float horizDist = std::sqrt(toCam.x * toCam.x + toCam.z * toCam.z);
    // 自機から見て前方にいるか（XZ）
    const float frontDot = (horizDist > 1e-3f) ? (-(toCam.x * camFwd_.x + toCam.z * camFwd_.z) / horizDist) : 0.0f;
    const bool inFront = frontDot > 0.35f;

    // 陣取る位置：自機の前方・横・上空。ゆっくり左右に流れ、上下にうねる
    const float sway = std::sin(time_ * 0.45f) * 3.0f;
    const float baseY = (hasWater_ ? waterY_ : camTr->position.y) + flyHeight;
    RC::Vector3 home = {
        camTr->position.x + camFwd_.x * preferredDistance + camRight.x * (sideSign_ * sideOffset + sway),
        baseY + std::sin(time_ * 0.9f) * 1.0f,
        camTr->position.z + camFwd_.z * preferredDistance + camRight.z * (sideSign_ * sideOffset + sway)};
    home.y = (std::min)(home.y, ceilingY - 1.0f);

    windupAmount_ = 0.0f;
    throwAmount_ = 0.0f;

    switch (state_) {
    case BirdState::Wait: {
      // 生成地点の上空をゆったり旋回して待つ
      const float r = 6.0f;
      const RC::Vector3 circle = {spawnPos_.x + std::cos(time_ * 0.5f) * r, spawnPos_.y,
                                  spawnPos_.z + std::sin(time_ * 0.5f) * r};
      Steer(tr, circle, {0.0f, 0.0f, 0.0f}, maxSpeed * 0.5f, dt);
      if (dist <= detectDistance) {
        state_ = BirdState::Approach;
        // 陣取る側は「いま自機から見てどちらにいるか」で決める。スポナーは 2 体目以降を左右交互に
        // 出すので、同じウェーブの鳥が同じ位置に重ならない（生成位置のハッシュだと重なることがある）
        const float s = -(toCam.x * camRight.x + toCam.z * camRight.z);
        if (std::fabs(s) > 0.5f) sideSign_ = (s > 0.0f) ? 1.0f : -1.0f;
        Log::Print("[BirdEnemyScript] Detected player -> Approach");
      }
      break;
    }

    case BirdState::Approach: {
      // 自機と一緒に動く目標なので、自機の速度を足して追う（足さないと常に置いていかれる）
      Steer(tr, home, camVel_, maxSpeed, dt);
      fireCooldown_ -= dt;
      const float toHome = BirdDetail::Len(BirdDetail::Sub(home, tr->position));
      // 目標のそばまで来ていて、前方・射程内なら撃つ（横切っている途中では撃たない）
      if (fireCooldown_ <= 0.0f && inFront && dist >= fireMinDistance && dist <= fireMaxDistance &&
          toHome < arriveRadius * 1.5f) {
        state_ = BirdState::Windup;
        stateTimer_ = 0.0f;
        SaveOriginalColor();
      }
      break;
    }

    case BirdState::Windup: {
      // その場でホバリングしながら翼を振り上げる（予兆）。強めに減速して止まる
      const float k = std::exp(-5.0f * dt);
      velocity_ = BirdDetail::Add(BirdDetail::Scale(BirdDetail::Sub(velocity_, camVel_), k), camVel_);
      Integrate(tr, dt);
      stateTimer_ += dt;
      const float t = std::clamp(stateTimer_ / (std::max)(windupDuration, 0.01f), 0.0f, 1.0f);
      windupAmount_ = 1.0f - (1.0f - t) * (1.0f - t);
      // 明滅がだんだん速くなる。被弾フラッシュ（赤）中はそちらを優先
      if (flashTimer <= 0.0f) {
        const float blink = 0.5f + 0.5f * std::sin(stateTimer_ * (10.0f + 30.0f * t));
        TintAll(windupColor, blink * (0.35f + 0.65f * t));
      }
      if (stateTimer_ >= windupDuration) {
        state_ = BirdState::Throw;
        stateTimer_ = 0.0f;
      }
      break;
    }

    case BirdState::Throw: {
      // 翼を一気に振り下ろす。振り下ろしきった瞬間に羽根が出る
      Integrate(tr, dt);
      stateTimer_ += dt;
      windupAmount_ = 1.0f;
      throwAmount_ = std::clamp(stateTimer_ / (std::max)(throwDuration, 0.01f), 0.0f, 1.0f);
      if (stateTimer_ >= throwDuration) {
        if (flashTimer <= 0.0f) TintAll(windupColor, 0.0f);
        ThrowFeathers(scene, *tr, camTr->position);
        // 反動で少し浮き上がって後ろへ下がる
        const RC::Vector3 back = BirdDetail::Normalize({-toCam.x, 0.0f, -toCam.z});
        velocity_ = BirdDetail::Add(velocity_, {back.x * 4.0f, 3.0f, back.z * 4.0f});
        if (swapSideAfterFire) sideSign_ = -sideSign_;
        state_ = BirdState::Recover;
        stateTimer_ = 0.0f;
        fireCooldown_ = fireInterval;
      }
      break;
    }

    case BirdState::Recover:
      // 陣取る位置へ戻りながら一定時間待ち、接近状態へ戻る
      Steer(tr, home, camVel_, maxSpeed, dt);
      stateTimer_ += dt;
      if (stateTimer_ >= recoverDuration) state_ = BirdState::Approach;
      break;
    }

    UpdateAttitude(toCam, dt);
    UpdateFlap(dt);
    UpdateParts(tr->position);
  }

  /// @brief 基底の被ダメージ処理に加え、羽毛を散らし、撃破なら錐もみ落下、ため中ならひるませる
  /// @param damage 受けるダメージ量
  void TakeDamage(int damage) override {
    const bool wasCharging = (state_ == BirdState::Windup || state_ == BirdState::Throw);
    EnemyBaseScript::TakeDamage(damage);
    if (Entity *self = GetEntity()) {
      if (auto *tr = self->GetComponent<TransformComponent>()) {
        BirdDetail::SpawnFeatherPuff(GetScene(), GetSceneContext(), tr->position, isDead ? 16 : 5,
                                     (std::max)(baseScale_.z, 0.3f) * 0.12f, {0.97f, 0.97f, 0.95f, 0.95f});
      }
    }
    if (isDead) {
      // 撃ち落とされた：翼が止まり、錐もみしながら落ちる
      deathSpin_ = (sideSign_ > 0.0f) ? 6.0f : -6.0f;
      if (flashTimer <= 0.0f) TintAll(windupColor, 0.0f);
      return;
    }
    // ため中に撃たれるとひるんで中断（撃たせないチャンスを作る）
    if (wasCharging) {
      if (flashTimer <= 0.0f) TintAll(windupColor, 0.0f);
      state_ = BirdState::Recover;
      stateTimer_ = 0.0f;
      fireCooldown_ = fireInterval * 0.5f;
      velocity_.y += 2.0f;
    }
  }

public:
  /// @brief インスペクタに状態表示・各パラメータの編集欄・即時投擲ボタンを表示する
  void OnImGui() override {
    EnemyBaseScript::OnImGui();
#if RC_ENABLE_IMGUI
    ImGui::Text("State: %s  speed %.1f  parts %zu", StateName(state_), BirdDetail::Len(velocity_), parts_.size());
    ImGui::DragFloat("Detect Dist##Bird", &detectDistance, 0.5f, 5.0f, 200.0f);
    ImGui::DragFloat("Preferred Dist##Bird", &preferredDistance, 0.5f, 3.0f, 80.0f);
    ImGui::DragFloat("Side Offset##Bird", &sideOffset, 0.1f, 0.0f, 30.0f);
    ImGui::DragFloat("Fly Height##Bird", &flyHeight, 0.1f, 1.0f, 40.0f);
    ImGui::DragFloat("Ceiling Y##Bird", &ceilingY, 0.5f, -100.0f, 1000.0f);
    ImGui::Checkbox("Swap Side After Fire##Bird", &swapSideAfterFire);
    ImGui::Separator();
    ImGui::DragFloat("Max Speed##Bird", &maxSpeed, 0.1f, 1.0f, 60.0f);
    ImGui::DragFloat("Max Accel##Bird", &maxAccel, 0.1f, 1.0f, 100.0f);
    ImGui::DragFloat("Arrive Radius##Bird", &arriveRadius, 0.1f, 0.5f, 50.0f);
    ImGui::DragFloat("Turn Rate##Bird", &turnRate, 0.05f, 0.1f, 20.0f);
    ImGui::DragFloat("Bank Factor##Bird", &bankFactor, 0.05f, 0.0f, 3.0f);
    ImGui::DragFloat("Flap Freq##Bird", &flapFrequency, 0.05f, 0.1f, 10.0f);
    ImGui::DragFloat("Flap Amp##Bird", &flapAmplitudeDeg, 0.5f, 0.0f, 80.0f);
    ImGui::Separator();
    ImGui::DragFloat("Fire Min Dist##Bird", &fireMinDistance, 0.5f, 0.0f, 100.0f);
    ImGui::DragFloat("Fire Max Dist##Bird", &fireMaxDistance, 0.5f, 1.0f, 150.0f);
    ImGui::DragFloat("Fire Interval##Bird", &fireInterval, 0.1f, 0.3f, 20.0f);
    ImGui::DragFloat("Windup##Bird", &windupDuration, 0.05f, 0.0f, 5.0f);
    ImGui::DragInt("Feather Count##Bird", &featherCount, 1, 1, 15);
    ImGui::DragFloat("Feather Spread##Bird", &featherSpreadDeg, 0.1f, 0.0f, 45.0f);
    ImGui::DragFloat("Feather Speed##Bird", &featherSpeed, 0.1f, 1.0f, 80.0f);
    ImGui::DragInt("Feather Damage##Bird", &featherDamage, 1, 0, 10);
    ImGui::SliderFloat("Feather Lead##Bird", &featherLead, 0.0f, 1.0f);
    if (ImGui::Button("Throw Now##Bird")) {
      if (auto *tr = GetComponent<TransformComponent>()) {
        if (auto cam = BirdDetail::FindPlayerCamera(GetScene())) {
          if (auto *camTr = cam->GetComponent<TransformComponent>()) ThrowFeathers(GetScene(), *tr, camTr->position);
        }
      }
    }
#endif
  }

private:
  // ------------------------------------------------------------------
  // 部位
  // ------------------------------------------------------------------
  /// @brief 部位の種類
  enum class PartKind { Head, Beak, Tail, WingInner, WingOuter };

  /// @brief 生成した部位 1 つぶんの情報（エンティティと、胴基準の大きさ・位置・色）
  struct Part {
    std::weak_ptr<Entity> entity;
    PartKind kind;
    float side;             ///< 翼の左右（+1 右 / -1 左）。翼以外は 0
    RC::Vector3 size;       ///< 胴の長さ u を 1 とした大きさ
    RC::Vector3 offset;     ///< 胴の中心からの位置（u 単位・胴のローカル）
    RC::Vector4 color;      ///< 部位の元の色（フラッシュ・明滅から戻すときに使う）
  };

  /// @brief 頭・くちばし・尾・翼を生成する
  /// @details 大きさは胴（スポナーが作る球）の z スケール u を基準にしているので、
  ///          スポナーの enemyScale を変えれば鳥全体の大きさが変わる。
  ///          当たり判定は胴の球だけ（翼には付けない：翼の端に当たるのは見た目より判定が大きく感じるため）。
  /// @param scene 部位を生成するシーン
  void BuildParts(Scene *scene) {
    partsBuilt_ = true;
    Entity *self = GetEntity();
    if (!scene || !self) return;

    /// @brief 部位 1 つぶんの生成仕様（種類・左右・形状・大きさ・位置・色）
    struct Spec { PartKind kind; float side; PrimitiveType type; RC::Vector3 size; RC::Vector3 offset; RC::Vector4 color; };
    const std::array<Spec, 7> specs = {{
        // size は「直径（球）／辺の長さ（箱）」、offset は胴の中心から。どちらも胴の半長 u を 1 とする
        {PartKind::Head, 0.0f, PrimitiveType::Sphere, {0.55f, 0.55f, 0.6f}, {0.0f, 0.25f, 0.95f}, headColor},
        {PartKind::Beak, 0.0f, PrimitiveType::Sphere, {0.12f, 0.1f, 0.4f}, {0.0f, 0.2f, 1.35f}, beakColor},
        {PartKind::Tail, 0.0f, PrimitiveType::Box, {0.7f, 0.06f, 0.7f}, {0.0f, 0.08f, -1.05f}, wingColor},
        {PartKind::WingInner, 1.0f, PrimitiveType::Box, {1.35f, 0.08f, 0.9f}, {0.3f, 0.18f, 0.1f}, wingColor},
        {PartKind::WingInner, -1.0f, PrimitiveType::Box, {1.35f, 0.08f, 0.9f}, {-0.3f, 0.18f, 0.1f}, wingColor},
        {PartKind::WingOuter, 1.0f, PrimitiveType::Box, {1.25f, 0.06f, 0.6f}, {0.0f, 0.0f, -0.12f}, wingTipColor},
        {PartKind::WingOuter, -1.0f, PrimitiveType::Box, {1.25f, 0.06f, 0.6f}, {0.0f, 0.0f, -0.12f}, wingTipColor},
    }};

    // 仕様どおりに部位を生成する。位置・大きさは UpdateParts で毎フレーム書き込むので仮の値でよい
    auto *selfTr = self->GetComponent<TransformComponent>();
    for (const Spec &sp : specs) {
      auto e = scene->CreateEntity(self->GetName() + "_part");
      if (!e) continue;
      e->SetParentGuid(self->Guid()); // Hierarchy で鳥の下にまとまるように（描画上の親子ではない）
      auto &ptr = e->AddComponent<TransformComponent>();
      if (selfTr) ptr.position = selfTr->position;
      ptr.scale = {0.01f, 0.01f, 0.01f};
      auto &pm = e->AddComponent<PrimitiveMeshComponent>();
      pm.type = sp.type;
      pm.color = sp.color;
      scene->InitDynamicEntityRuntime(*e);
      BirdDetail::SyncPrimitive(*e);
      parts_.push_back({e, sp.kind, sp.side, sp.size, sp.offset, sp.color});
    }
  }

  /// @brief 部位の姿勢を胴に合わせて書き込む
  /// @param bodyPos 胴（自身）のワールド位置
  /// @note 外側の翼は内側の翼の外端を基準にするため、内側の翼を先に処理する。
  void UpdateParts(const RC::Vector3 &bodyPos) {
    const float u = (std::max)(std::fabs(baseScale_.z), 0.05f);
    const BirdDetail::Mat3 body = BirdDetail::BodyMatrix(yaw_, pitchUp_, roll_);

    // 翼の付け根（内側の翼の外端＝外側の翼の付け根）を左右ぶん覚えておく
    RC::Vector3 elbow[2] = {{0, 0, 0}, {0, 0, 0}};
    BirdDetail::Mat3 innerRot[2] = {body, body};
    for (auto &p : parts_) {
      if (p.kind != PartKind::WingInner) continue;
      const int si = (p.side > 0.0f) ? 0 : 1;
      // 羽ばたき：胴の前後軸（Z）まわり。右翼は +角で先が上がり、左翼は符号が逆
      const BirdDetail::Mat3 local = BirdDetail::RotZ(p.side * innerAngle_);
      innerRot[si] = BirdDetail::Mul(local, body);
      const RC::Vector3 shoulder = BirdDetail::Scale(p.offset, u);
      const RC::Vector3 halfSpan = BirdDetail::Apply({p.side * p.size.x * u * 0.5f, 0.0f, 0.0f}, local);
      const RC::Vector3 center = BirdDetail::Add(shoulder, halfSpan);
      elbow[si] = BirdDetail::Add(center, halfSpan);
      Place(p, BirdDetail::Add(bodyPos, BirdDetail::Apply(center, body)), innerRot[si], u);
    }
    for (auto &p : parts_) {
      switch (p.kind) {
      case PartKind::WingOuter: {
        const int si = (p.side > 0.0f) ? 0 : 1;
        // 外側の翼は内側に対してさらに曲がる（少し遅れて付いてくる）
        const BirdDetail::Mat3 innerLocal = BirdDetail::RotZ(p.side * innerAngle_);
        const BirdDetail::Mat3 local = BirdDetail::Mul(BirdDetail::RotZ(p.side * outerAngle_), innerLocal);
        const RC::Vector3 halfSpan = BirdDetail::Apply({p.side * p.size.x * u * 0.5f, 0.0f, 0.0f}, local);
        const RC::Vector3 center = BirdDetail::Add(BirdDetail::Add(elbow[si], halfSpan), BirdDetail::Scale(p.offset, u));
        Place(p, BirdDetail::Add(bodyPos, BirdDetail::Apply(center, body)), BirdDetail::Mul(local, body), u);
        break;
      }
      case PartKind::Tail: {
        // 尾は機首上げ時に後ろ端が下がる（ブレーキ）。RotX は正の角で前が下がる＝後ろが上がる向きなので負にする
        const BirdDetail::Mat3 local = BirdDetail::RotX(-0.1f - 0.4f * windupAmount_);
        const RC::Vector3 center = BirdDetail::Scale(p.offset, u);
        Place(p, BirdDetail::Add(bodyPos, BirdDetail::Apply(center, body)), BirdDetail::Mul(local, body), u);
        break;
      }
      case PartKind::Head:
      case PartKind::Beak:
        Place(p, BirdDetail::Add(bodyPos, BirdDetail::Apply(BirdDetail::Scale(p.offset, u), body)), body, u);
        break;
      default:
        break;
      }
    }

    // 胴
    if (auto *tr = GetComponent<TransformComponent>()) tr->rotation = BirdDetail::ToEngineEuler(body);
  }

  /// @brief 部位エンティティにワールド位置・回転・大きさを書き込む
  /// @param p 対象の部位
  /// @param worldPos 部位中心のワールド位置
  /// @param rot 部位のワールド回転行列
  /// @param u 大きさの基準（胴の z スケール）
  void Place(const Part &p, const RC::Vector3 &worldPos, const BirdDetail::Mat3 &rot, float u) {
    auto e = p.entity.lock();
    if (!e) return;
    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) return;
    tr->position = worldPos;
    tr->rotation = BirdDetail::ToEngineEuler(rot);
    // 球は半径 1、箱は一辺 1 で生成されるので、箱だけ size をそのまま、球は半分にする
    const float k = (p.kind == PartKind::Head || p.kind == PartKind::Beak) ? 0.5f : 1.0f;
    tr->scale = {p.size.x * u * k, p.size.y * u * k, p.size.z * u * k};
  }

  /// @brief 胴と全部位の色を元の色から tint へ amount だけ寄せる（0 で元どおり）
  /// @param tint 寄せる先の色
  /// @param amount 寄せる度合い（0..1）
  void TintAll(const RC::Vector4 &tint, float amount) {
    if (amount <= 0.0f) RestoreColor();
    else ApplyColor(BirdDetail::Lerp4(originalColor, tint, amount));
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) BirdDetail::SetPrimitiveColor(*e, BirdDetail::Lerp4(p.color, tint, amount));
    }
  }

  // ------------------------------------------------------------------
  // 動き
  // ------------------------------------------------------------------

  /// @brief 自機の進行方向と速度（OctopusEnemyScript と同じ取り方）
  /// @details 視点操作で向きが揺れても陣取る位置がぶれないよう、カメラの回転ではなく移動方向を使う。
  ///          ウェーブ中はレールが止まるので、最後に動いていた向きをそのまま使い続ける。
  /// @param camTr 自機（カメラ）の Transform
  /// @param dt 前フレームからの経過秒数
  void UpdatePlayerMotion(const TransformComponent &camTr, float dt) {
    if (hasLastCamPos_) {
      const float vx = (camTr.position.x - lastCamPos_.x) / dt;
      const float vz = (camTr.position.z - lastCamPos_.z) / dt;
      const float sp = std::sqrt(vx * vx + vz * vz);
      const float vb = (std::min)(1.0f, dt * 4.0f);
      camVel_.x += (vx - camVel_.x) * vb;
      camVel_.z += (vz - camVel_.z) * vb;
      if (sp > 0.5f) {
        const float blend = (std::min)(1.0f, dt * 3.0f);
        camFwd_.x += (vx / sp - camFwd_.x) * blend;
        camFwd_.z += (vz / sp - camFwd_.z) * blend;
      } else if (!hasFwd_) {
        camFwd_ = {std::sin(camTr.rotation.y), 0.0f, std::cos(camTr.rotation.y)};
      }
      hasFwd_ = true;
    } else if (!hasFwd_) {
      camFwd_ = {std::sin(camTr.rotation.y), 0.0f, std::cos(camTr.rotation.y)};
    }
    const float l = std::sqrt(camFwd_.x * camFwd_.x + camFwd_.z * camFwd_.z);
    if (l > 1e-4f) { camFwd_.x /= l; camFwd_.z /= l; }
    lastCamPos_ = camTr.position;
    hasLastCamPos_ = true;
  }

  /// @brief 目標へ向かう（arrive 操舵）。目標が baseVel で動いているならそれも足す
  /// @param tr 移動させる自身の Transform
  /// @param target 目標位置
  /// @param baseVel 目標自体の移動速度
  /// @param topSpeed 目標へ向かう成分の最高速
  /// @param dt 前フレームからの経過秒数
  void Steer(TransformComponent *tr, const RC::Vector3 &target, const RC::Vector3 &baseVel, float topSpeed, float dt) {
    const RC::Vector3 to = BirdDetail::Sub(target, tr->position);
    const float d = BirdDetail::Len(to);
    RC::Vector3 desired = baseVel;
    if (d > 1e-3f) {
      const float sp = topSpeed * (std::min)(1.0f, d / (std::max)(arriveRadius, 0.1f));
      desired = BirdDetail::Add(desired, BirdDetail::Scale(to, sp / d));
    }
    // 速度差を 0.5 秒で詰めるくらいの加速度を出し、maxAccel で頭打ちにする
    RC::Vector3 acc = BirdDetail::Scale(BirdDetail::Sub(desired, velocity_), 2.0f);
    const float al = BirdDetail::Len(acc);
    if (al > maxAccel) acc = BirdDetail::Scale(acc, maxAccel / al);
    lastAccel_ = acc;
    velocity_ = BirdDetail::Add(velocity_, BirdDetail::Scale(acc, dt));
    Integrate(tr, dt);
  }

  /// @brief 速度を上限で抑えて位置を進め、天井と水面の間に収める
  /// @param tr 移動させる自身の Transform
  /// @param dt 前フレームからの経過秒数
  void Integrate(TransformComponent *tr, float dt) {
    const float sp = BirdDetail::Len(velocity_);
    const float cap = maxSpeed * 1.5f; // 自機の速度を足すぶん少し余裕を持たせる
    if (sp > cap) velocity_ = BirdDetail::Scale(velocity_, cap / sp);
    tr->position = BirdDetail::Add(tr->position, BirdDetail::Scale(velocity_, dt));
    // 天井と水面の間に収める
    const float floorY = (hasWater_ ? waterY_ : -1000.0f) + 1.5f;
    if (tr->position.y > ceilingY - 0.5f) { tr->position.y = ceilingY - 0.5f; velocity_.y = (std::min)(velocity_.y, 0.0f); }
    if (tr->position.y < floorY) { tr->position.y = floorY; velocity_.y = (std::max)(velocity_.y, 0.0f); }
  }

  /// @brief 機首方位・ピッチ・バンクを速度と状態から決める
  /// @param toCam 自身から自機（カメラ）へのベクトル
  /// @param dt 前フレームからの経過秒数
  void UpdateAttitude(const RC::Vector3 &toCam, float dt) {
    const float horiz = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);
    const bool facePlayer = (state_ == BirdState::Windup || state_ == BirdState::Throw) || horiz < 2.0f;

    // 機首方位：飛んでいる向き。ため・ホバリング中は自機の方
    float targetYaw = yaw_;
    if (facePlayer) targetYaw = std::atan2(toCam.x, toCam.z);
    else targetYaw = std::atan2(velocity_.x, velocity_.z);
    const float dy = BirdDetail::WrapAngle(targetYaw - yaw_);
    const float step = turnRate * dt;
    yaw_ = BirdDetail::WrapAngle(yaw_ + std::clamp(dy, -step, step));

    // ピッチ：上昇・下降に合わせる。ためでは胸を張って機首を上げる
    float targetPitch = std::atan2(velocity_.y, (std::max)(horiz, 1.0f));
    targetPitch = std::clamp(targetPitch, -30.0f * BirdDetail::kDeg, 30.0f * BirdDetail::kDeg);
    targetPitch += windupAmount_ * (1.0f - throwAmount_) * 28.0f * BirdDetail::kDeg;
    pitchUp_ += (targetPitch - pitchUp_) * (std::min)(1.0f, dt * 6.0f);

    // バンク：横向きの加速度ぶん傾ける（右へ曲がるときは右翼が下がる＝負のロール）
    float targetRoll = 0.0f;
    if (!facePlayer) {
      const RC::Vector3 right = {std::cos(yaw_), 0.0f, -std::sin(yaw_)};
      const float lat = lastAccel_.x * right.x + lastAccel_.z * right.z;
      targetRoll = -std::atan(lat / 9.8f) * bankFactor;
    }
    const float maxBank = maxBankDeg * BirdDetail::kDeg;
    targetRoll = std::clamp(targetRoll, -maxBank, maxBank);
    roll_ += (targetRoll - roll_) * (std::min)(1.0f, dt * 4.0f);
  }

  /// @brief 羽ばたきの角度（内側・外側の翼）を決める
  /// @param dt 前フレームからの経過秒数
  void UpdateFlap(float dt) {
    const float amp = flapAmplitudeDeg * BirdDetail::kDeg;
    const float dihedral = dihedralDeg * BirdDetail::kDeg;

    // 加速・上昇しているほど強く羽ばたく。巡航中は弱めに（滑空っぽく）
    const float effort = std::clamp(BirdDetail::Len(lastAccel_) / (std::max)(maxAccel, 0.1f) + velocity_.y * 0.08f, 0.35f, 1.0f);
    const float freq = flapFrequency * (0.8f + 0.4f * effort) * (1.0f + windupAmount_ * 1.5f);
    flapPhase_ += 2.0f * BirdDetail::kPi * freq * dt;
    if (flapPhase_ > 2.0f * BirdDetail::kPi) flapPhase_ -= 2.0f * BirdDetail::kPi;

    float inner = dihedral + amp * effort * std::sin(flapPhase_);
    float outer = amp * 0.6f * effort * std::sin(flapPhase_ - 0.7f);

    if (windupAmount_ > 0.0f) {
      // ため：翼を高く掲げて小刻みに震わせる → 振り下ろし
      const float tremble = std::sin(time_ * 40.0f) * 4.0f * BirdDetail::kDeg;
      const float raisedInner = 70.0f * BirdDetail::kDeg + tremble;
      const float raisedOuter = 25.0f * BirdDetail::kDeg;
      const float downInner = -40.0f * BirdDetail::kDeg;
      const float downOuter = -20.0f * BirdDetail::kDeg;
      const float ti = throwAmount_ * throwAmount_;
      const float wInner = raisedInner + (downInner - raisedInner) * ti;
      const float wOuter = raisedOuter + (downOuter - raisedOuter) * ti;
      inner += (wInner - inner) * windupAmount_;
      outer += (wOuter - outer) * windupAmount_;
    }
    innerAngle_ = inner;
    outerAngle_ = outer;
  }

  /// @brief 撃破後：翼が止まって錐もみしながら落ち、水面に落ちたらゆっくり沈む
  /// @param tr 自身の Transform
  /// @param dt 前フレームからの経過秒数
  void UpdateDeath(TransformComponent *tr, float dt) {
    const bool inWater = hasWater_ && tr->position.y <= waterY_;
    // 空中：重力で落としつつ水平速度を減衰させ、ロールで錐もみ・機首を下げる
    if (!inWater) {
      velocity_.y -= 9.8f * dt;
      velocity_.x *= std::exp(-0.8f * dt);
      velocity_.z *= std::exp(-0.8f * dt);
      roll_ += deathSpin_ * dt;
      pitchUp_ += (-50.0f * BirdDetail::kDeg - pitchUp_) * (std::min)(1.0f, dt * 3.0f);
      innerAngle_ = 15.0f * BirdDetail::kDeg + std::sin(time_ * 25.0f) * 10.0f * BirdDetail::kDeg;
      outerAngle_ = 35.0f * BirdDetail::kDeg;
    } else {
      // 着水：初回だけしぶきを出し、一定速度で沈めながら翼を畳む
      if (!splashed_) {
        splashed_ = true;
        BirdDetail::SpawnFeatherPuff(GetScene(), GetSceneContext(), tr->position, 8,
                                     (std::max)(baseScale_.z, 0.3f) * 0.12f, {0.85f, 0.93f, 1.0f, 0.8f});
      }
      velocity_ = {velocity_.x * std::exp(-4.0f * dt), -0.8f, velocity_.z * std::exp(-4.0f * dt)};
      innerAngle_ += (-5.0f * BirdDetail::kDeg - innerAngle_) * (std::min)(1.0f, dt * 3.0f);
      outerAngle_ += (0.0f - outerAngle_) * (std::min)(1.0f, dt * 3.0f);
      roll_ *= std::exp(-2.0f * dt);
    }
    tr->position = BirdDetail::Add(tr->position, BirdDetail::Scale(velocity_, dt));
  }

  /// @brief 羽根を扇状に放つ
  /// @details 真ん中の 1 枚だけが自機（の少し先）を正確に狙い、両脇は「狙い点から左右へ
  ///          tan(角度差) × 距離 だけずれた点」を狙う。放つ位置は翼の端から端へ並べるが、
  ///          狙い点は羽根ごとに計算するので、翼の幅ぶん外れることはない。
  ///          止まっていれば真ん中の 1 枚だけが当たり、左右へ逃げると脇の羽根に当たる。
  /// @param scene 羽根を生成するシーン
  /// @param tr 自身の Transform（放つ位置の基準）
  /// @param targetPos 狙う自機（カメラ）の位置
  void ThrowFeathers(Scene *scene, const TransformComponent &tr, const RC::Vector3 &targetPos) {
    if (!scene) return;
    // 着弾までの時間 × 自機の速度 × featherLead だけ先を狙う（偏差撃ち）
    const float flight = BirdDetail::Len(BirdDetail::Sub(targetPos, tr.position)) / (std::max)(featherSpeed, 1.0f);
    const float lead = std::clamp(featherLead, 0.0f, 1.0f);
    const RC::Vector3 aim = {targetPos.x + camVel_.x * flight * lead, targetPos.y, targetPos.z + camVel_.z * flight * lead};

    const RC::Vector3 toAim = BirdDetail::Sub(aim, tr.position);
    const float aimDist = BirdDetail::Len(toAim);
    if (aimDist < 1e-3f) return;
    // 狙いの横方向（水平面内で、狙い方向に直交）
    const RC::Vector3 side = BirdDetail::Normalize({toAim.z, 0.0f, -toAim.x}, {1.0f, 0.0f, 0.0f});

    const float u = (std::max)(std::fabs(baseScale_.z), 0.05f);
    const float halfSpan = u * 2.2f; // 翼の端から端まで並べる
    const int n = (std::max)(featherCount, 1);
    const float fs = (std::max)(featherScale, 0.05f) * u;

    for (int k = 0; k < n; ++k) {
      const float c = (n == 1) ? 0.0f : (static_cast<float>(k) / static_cast<float>(n - 1)) * 2.0f - 1.0f; // -1..1
      const float offsetDeg = (static_cast<float>(k) - static_cast<float>(n - 1) * 0.5f) * featherSpreadDeg;
      const RC::Vector3 spawn = BirdDetail::Add(tr.position, BirdDetail::Scale(side, c * halfSpan));
      const RC::Vector3 goal = BirdDetail::Add(aim, BirdDetail::Scale(side, std::tan(offsetDeg * BirdDetail::kDeg) * aimDist));
      const RC::Vector3 dir = BirdDetail::Normalize(BirdDetail::Sub(goal, spawn));

      // 羽根エンティティを生成し、FeatherScript へ速度・ダメージ・水面高さ・向きを pendingData で渡す
      auto f = scene->CreateEntity("Feather");
      if (!f) continue;
      auto &ftr = f->AddComponent<TransformComponent>();
      ftr.position = spawn;
      ftr.scale = {0.11f * fs, 0.03f * fs, 0.5f * fs};

      auto &pm = f->AddComponent<PrimitiveMeshComponent>();
      pm.type = PrimitiveType::Sphere;
      pm.color = featherColor;

      auto &nsc = f->AddComponent<NativeScriptComponent>();
      nsc.AddScript("FeatherScript");
      if (!nsc.scripts.empty()) {
        nsc.scripts.back().pendingData = {
            {"speed", featherSpeed},
            {"damage", featherDamage},
            {"waterY", hasWater_ ? waterY_ : -1000.0f},
            {"dir", {dir.x, dir.y, dir.z}},
        };
      }
      nsc.SetScene(scene);
      if (GetSceneContext()) nsc.SetSceneContext(GetSceneContext());
      f->SetTag("no_shadow", 1);
      scene->InitDynamicEntityRuntime(*f);
      BirdDetail::SyncPrimitive(*f);
    }
    Log::Print("[BirdEnemyScript] Threw feathers!");
  }

  /// @brief 状態の表示名を返す（ImGui 表示用）
  /// @param s 状態
  /// @return 状態名の文字列
  static const char *StateName(BirdState s) {
    switch (s) {
    case BirdState::Wait: return "Wait";
    case BirdState::Approach: return "Approach";
    case BirdState::Windup: return "Windup";
    case BirdState::Throw: return "Throw";
    case BirdState::Recover: return "Recover";
    }
    return "?";
  }

  BirdState state_ = BirdState::Wait;
  std::weak_ptr<Entity> target_;
  std::vector<Part> parts_;
  bool partsBuilt_ = false;
  bool partsFlashing_ = false;

  RC::Vector3 baseScale_ = {0.45f, 0.4f, 0.95f};
  RC::Vector3 spawnPos_ = {0.0f, 0.0f, 0.0f};
  RC::Vector3 velocity_ = {0.0f, 0.0f, 0.0f};
  RC::Vector3 lastAccel_ = {0.0f, 0.0f, 0.0f};
  RC::Vector3 camFwd_ = {0.0f, 0.0f, 1.0f};
  RC::Vector3 camVel_ = {0.0f, 0.0f, 0.0f};
  RC::Vector3 lastCamPos_ = {0.0f, 0.0f, 0.0f};
  bool hasLastCamPos_ = false;
  bool hasFwd_ = false;

  float yaw_ = 0.0f;
  float pitchUp_ = 0.0f;
  float roll_ = 0.0f;
  float flapPhase_ = 0.0f;
  float innerAngle_ = 0.0f;
  float outerAngle_ = 0.0f;
  float windupAmount_ = 0.0f;  ///< ためで翼を振り上げている度合い 0..1
  float throwAmount_ = 0.0f;   ///< 振り下ろしの進み 0..1
  float deathSpin_ = 0.0f;
  bool splashed_ = false;

  float sideSign_ = 1.0f;
  float time_ = 0.0f;
  float stateTimer_ = 0.0f;
  float fireCooldown_ = 0.0f;
  bool waterChecked_ = false;
  bool hasWater_ = false;
  float waterY_ = 0.0f;
};

REGISTER_SCRIPT(BirdEnemyScript)
