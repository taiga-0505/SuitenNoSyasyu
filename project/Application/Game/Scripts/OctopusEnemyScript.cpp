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
#include "Game/Framework/PlayerMotionTracker.h"
#include "Game/Framework/StateMachine.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// ============================================================================
// タコ（墨を吐く敵）
//
//   - 自機の前方 preferredDistance の位置に、ジェット推進（ドン…スーッ）で回り込む
//   - 射程内で前方にいると、体を膨らませて「ため」→ 墨玉（InkBlobScript）を吐く
//   - 墨玉が自機に届くと画面に墨が貼り付き、しばらく視界が悪くなる（InkScreenFx）
//   - 墨玉は撃ち落とせる。ため中にタコを倒せば吐かれない
//
//   サメ（突っ込んで HP を削る）・船（遠くから砲撃で HP を削る）に対して、
//   タコは HP ではなく「視界」を削る。ダメージは既定 0（inkDamage で変更可）。
//
//   スクリプト間の通信は他の敵と同じくタグのみ：
//     墨玉 → 自機  : pending_ink（強さ×100 の整数を加算）/ pending_damage
//
//   見た目：スポナーの modelPath が Resources/model/octopus/octopus_enemy.glb ならモデル（スキン付き 1 体）で、
//   足の動きはクリップ（Idle / Jet / Windup / Spit）。胴の中心がモデルの原点（＝当たり判定の中心）。
//   伸び縮み・ためで膨らむのは今までどおり Transform の scale で行う。modelPath が空なら球で代用する。
//
//   自機（メインカメラ）の検索と進行方向・速度の推定は PlayerMotionTracker（BirdEnemyScript と共通）。
// ============================================================================

namespace OctopusDetail {

constexpr float kPi = 3.14159265f;

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

// ----------------------------------------------------------------------------
// 墨の粒の「大きさと不透明度の時間変化」（Strategy）
//
//   以前は InkParticleScript::OnUpdate が ink_kind（0=飛沫 / 1=墨の煙 / 2=閃光）で switch していた。
//   種類が増えるたびに OnUpdate を書き換える必要があり、粒の移動処理と見た目の変化が混ざっていたため、
//   「経過割合 t → 大きさ・不透明度」の計算だけを差し替え可能な Strategy に切り出した。
//   粒はプールで使い回され、種類はタグ（int）で渡されるので、int → Strategy の対応は
//   InkFadeStyle::FromKind（Factory Method）の 1 か所だけに閉じ込めている。
// ----------------------------------------------------------------------------

/// @brief 粒の見た目の 1 時点の値
struct InkFadeSample {
  float scale; ///< 大きさ（Transform の scale）
  float alpha; ///< 不透明度
};

/// @brief 墨の粒の「大きさと不透明度の時間変化」の基底クラス（Strategy）
class InkFadeStyle {
public:
  virtual ~InkFadeStyle() = default;

  /// @brief 経過割合 t における大きさと不透明度を求める
  /// @param t 経過割合 0..1
  /// @param s0 初期の大きさ
  /// @param s1 最終の大きさ（広がる種類だけが使う）
  /// @param a0 初期の不透明度
  /// @return その時点の大きさと不透明度
  virtual InkFadeSample Evaluate(float t, float s0, float s1, float a0) const = 0;

  /// @brief タグの ink_kind に対応する Strategy を返す（Factory Method）
  /// @param kind 0=飛沫 / 1=墨の煙 / それ以外=閃光
  /// @return 対応する Strategy（状態を持たないので共有のインスタンス）
  static const InkFadeStyle &FromKind(int kind);
};

/// @brief 飛沫：縮みながら、だんだん薄れて消える（最後まで不透明だと黒い玉に見える）
class InkSplashFade final : public InkFadeStyle {
public:
  /// @brief 飛沫の大きさと不透明度を求める
  /// @param t 経過割合 0..1
  /// @param s0 初期の大きさ
  /// @param s1 未使用
  /// @param a0 初期の不透明度
  /// @return その時点の大きさと不透明度
  InkFadeSample Evaluate(float t, float s0, float s1, float a0) const override {
    (void)s1;
    return {s0 * (1.0f - 0.7f * t), a0 * (1.0f - t)};
  }
};

/// @brief 墨の煙：ふわっと広がって薄れる
class InkSmokeFade final : public InkFadeStyle {
public:
  /// @brief 墨の煙の大きさと不透明度を求める
  /// @param t 経過割合 0..1
  /// @param s0 初期の大きさ
  /// @param s1 最終の大きさ
  /// @param a0 初期の不透明度
  /// @return その時点の大きさと不透明度
  InkFadeSample Evaluate(float t, float s0, float s1, float a0) const override {
    const float easeOut = 1.0f - (1.0f - t) * (1.0f - t);
    return {s0 + (s1 - s0) * easeOut, a0 * std::pow(1.0f - t, 2.5f)}; // 早めに薄れさせて視界を塞がない
  }
};

/// @brief 閃光：一瞬で膨らんで消える
class InkFlashFade final : public InkFadeStyle {
public:
  /// @brief 閃光の大きさと不透明度を求める
  /// @param t 経過割合 0..1
  /// @param s0 初期の大きさ
  /// @param s1 最終の大きさ
  /// @param a0 初期の不透明度
  /// @return その時点の大きさと不透明度
  InkFadeSample Evaluate(float t, float s0, float s1, float a0) const override {
    const float easeOut = 1.0f - (1.0f - t) * (1.0f - t);
    return {s0 + (s1 - s0) * easeOut, a0 * (1.0f - t) * (1.0f - t)};
  }
};

inline const InkFadeStyle &InkFadeStyle::FromKind(int kind) {
  // Strategy は状態を持たないので、種類ごとに 1 つだけ作って共有する（書き換え不可の const）
  static const InkSplashFade splash{};
  static const InkSmokeFade smoke{};
  static const InkFlashFade flash{};
  // タグ（int）→ クラスの対応はここ 1 か所だけ。粒の更新側はもう種類を知らない
  switch (kind) {
  case 0: return splash;
  case 1: return smoke;
  default: return flash;
  }
}

} // namespace OctopusDetail

// ============================================================================
// 墨玉がはじける演出
// ============================================================================

/// @brief 墨のはじけ（飛沫・閃光・墨の煙）1 粒ぶんの動き
/// @details パラメータは生成側がタグで渡す（プールから使い回すため OnCreate では受け取れない）。
///          `ink_kind` 0=飛沫 / 1=墨の煙 / 2=閃光。速度・大きさ・寿命・不透明度は 1000 倍の整数。
///          Splash と同じく、終わったら非アクティブにしてプールへ戻す。
///          種類ごとの見た目の変化は OctopusDetail::InkFadeStyle（Strategy）に任せる。
class InkParticleScript : public ScriptableEntity {
protected:
  /// @brief 粒を動かし、種類に応じて大きさと不透明度を変化させ、寿命が来たらプールへ戻す
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(float dt) override {
    Entity *self = GetEntity();
    if (!self) return;
    // 初回、またはプールから再利用された直後にタグからパラメータを読み直す
    if (self->GetTagInt("reused", 0) == 1 || !started_) {
      self->ClearTag("reused");
      Start(*self);
    }
    if (finished_ || dt <= 0.0f) return;

    auto *tr = GetComponent<TransformComponent>();
    if (!tr) return;

    elapsed_ += dt;
    const float t = elapsed_ / (std::max)(life_, 0.01f);
    if (t >= 1.0f) {
      finished_ = true;
      self->SetActive(false);
      return;
    }

    // 水の抵抗（陰的）＋ゆるい重力
    const float k = 1.0f / (1.0f + drag_ * dt);
    vel_.x *= k; vel_.y *= k; vel_.z *= k;
    vel_.y -= gravity_ * dt;
    tr->position.x += vel_.x * dt;
    tr->position.y += vel_.y * dt;
    tr->position.z += vel_.z * dt;

    // 種類ごとに経過割合 t から大きさと不透明度を決める（種類の違いは Strategy が吸収する）
    const OctopusDetail::InkFadeSample sample = fade_->Evaluate(t, s0_, s1_, a0_);
    const float scale = (std::max)(sample.scale, 0.005f);
    const float alpha = sample.alpha;
    tr->scale = {scale, scale, scale};

    if (auto *pm = GetComponent<PrimitiveMeshComponent>()) {
      if (pm->meshHandle >= 0) {
        if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) mat->color.w = alpha;
      }
    }
  }

private:
  /// @brief タグから動きのパラメータを読み込み、状態を初期化する
  /// @param self 自身のエンティティ
  /// @note タグは 1000 倍の整数で渡されるので 1000 で割って戻す。
  void Start(Entity &self) {
    started_ = true;
    finished_ = false;
    elapsed_ = 0.0f;
    auto f = [&](const char *key, float def) {
      return static_cast<float>(self.GetTagInt(key, static_cast<int>(def * 1000.0f))) / 1000.0f;
    };
    fade_ = &OctopusDetail::InkFadeStyle::FromKind(self.GetTagInt("ink_kind", 0));
    vel_ = {f("ink_vx", 0.0f), f("ink_vy", 0.0f), f("ink_vz", 0.0f)};
    s0_ = f("ink_s0", 0.2f);
    s1_ = f("ink_s1", 0.2f);
    life_ = f("ink_life", 0.6f);
    a0_ = f("ink_a0", 1.0f);
    drag_ = f("ink_drag", 2.5f);
    gravity_ = f("ink_grav", 5.0f);
  }

  const OctopusDetail::InkFadeStyle *fade_ = nullptr; ///< 見た目の変化（ink_kind から Start で決める）
  RC::Vector3 vel_ = {0.0f, 0.0f, 0.0f};
  float s0_ = 0.2f, s1_ = 0.2f, life_ = 0.6f, a0_ = 1.0f, drag_ = 2.5f, gravity_ = 5.0f;
  float elapsed_ = 0.0f;
  bool started_ = false;
  bool finished_ = false;
};

REGISTER_SCRIPT(InkParticleScript)

namespace OctopusDetail {

/// @brief 墨玉がはじける演出を出す
/// @param scene   生成先のシーン
/// @param ctx     粒のスクリプトに渡すシーンコンテキスト（nullptr 可）
/// @param pos     はじけた位置
/// @param dir     墨玉が飛んでいた向き（＝自機へ向かう向き）
/// @param size    墨玉の大きさ（Transform の scale）
/// @details 墨玉は自機へ向かって飛んでくるので、撃ち落とす位置は自機の目の前になりやすい。
///          飛沫を全方向へ散らすと半分がカメラへ突っ込んで画面いっぱいの黒い玉になるため、
///          自機から離れる向き（-dir）と横方向にだけ散らす。
inline void SpawnInkBurst(Scene *scene, SceneContext *ctx, const RC::Vector3 &pos,
                          const RC::Vector3 &dir, float size) {
  if (!scene) return;

  static std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<float> U(0.0f, 1.0f);

  /// @brief 粒 1 つぶんの生成仕様（種類・初速・大きさ・寿命・抵抗・重力・色）
  struct Spec {
    int kind;
    RC::Vector3 vel;
    float s0, s1, life, drag, grav;
    RC::Vector4 color;
  };
  std::vector<Spec> specs;

  // ※ 以前は「閃光」と「墨の煙」として大きな半透明の球を 3 つ重ねていたが、
  //   撃ち落とす位置が自機の目の前なので、画面の中央に半透明の球が重なって見えるだけだった。
  //   球を重ねる表現はやめ、小さな飛沫だけで「はじけた」ことを伝える。
  //   （InkParticleScript の kind 1 / 2 は残してあるので、必要になったらここで足せる）

  // 飛沫：自機から離れる向きの半球へ散らす
  for (int i = 0; i < 18; ++i) {
    // 球面上に一様に散らしてから、自機側（dir 側）へ向かう成分を裏返す
    const float z = U(rng) * 2.0f - 1.0f;
    const float a = U(rng) * 6.2831853f;
    const float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
    RC::Vector3 n = {std::cos(a) * r, z, std::sin(a) * r};
    const float toward = n.x * dir.x + n.y * dir.y + n.z * dir.z;
    if (toward > 0.0f) {
      n = {n.x - 2.0f * toward * dir.x, n.y - 2.0f * toward * dir.y, n.z - 2.0f * toward * dir.z};
    }
    const float sp = 4.0f + U(rng) * 5.0f;
    const RC::Vector3 v = {n.x * sp - dir.x * 2.0f,
                           n.y * sp - dir.y * 2.0f + 1.0f,
                           n.z * sp - dir.z * 2.0f};
    // 大きさは墨玉の 1〜2 割。以前（2〜4 割）は目の前だと拳大の黒い玉に見えた
    const float s = size * (0.08f + U(rng) * 0.1f);
    specs.push_back({0, v, s, s, 0.35f + U(rng) * 0.25f, 3.0f, 6.0f, {0.06f, 0.03f, 0.09f, 0.9f}});
  }

  // エフェクトフォルダ（WaterBullet と同じ場所へまとめる）と、使い回せる粒を集める
  uint64_t folder = 0;
  std::vector<std::shared_ptr<Entity>> pool;
  for (const auto &e : scene->GetEntities()) {
    if (!e) continue;
    if (folder == 0 && e->IsFolder() && e->GetName() == "Effects") folder = e->Guid();
    if (!e->IsActive() && !e->IsPendingDestroy() && e->GetName() == "InkParticle" && pool.size() < specs.size()) {
      pool.push_back(e);
    }
  }
  if (folder == 0) {
    auto f = scene->CreateEntity("Effects");
    f->SetIsFolder(true);
    folder = f->Guid();
  }

  // 仕様ごとに粒を用意する。プールに残りがあれば使い回し、足りない分だけ新規に作る
  auto toTag = [](float v) { return static_cast<int>(v * 1000.0f); };
  for (size_t i = 0; i < specs.size(); ++i) {
    const Spec &sp = specs[i];
    std::shared_ptr<Entity> e;
    const bool isNew = (i >= pool.size());
    if (!isNew) {
      e = pool[i];
      e->SetActive(true);
      e->SetTag("reused", 1);
    } else {
      e = scene->CreateEntity("InkParticle");
      if (!e) continue;
    }
    e->SetParentGuid(folder);

    auto *tr = e->GetComponent<TransformComponent>();
    if (!tr) tr = &e->AddComponent<TransformComponent>();
    tr->position = pos;
    tr->scale = {sp.s0, sp.s0, sp.s0};

    auto *pm = e->GetComponent<PrimitiveMeshComponent>();
    if (!pm) {
      pm = &e->AddComponent<PrimitiveMeshComponent>();
      pm->type = PrimitiveType::Sphere;
    }
    pm->color = sp.color;

    // InkParticleScript へ動きのパラメータをタグで渡す（タグは int なので 1000 倍の固定小数）
    e->SetTag("no_shadow", 1); // エフェクトの粒は影を落とさない
    e->SetTag("ink_kind", sp.kind);
    e->SetTag("ink_vx", toTag(sp.vel.x));
    e->SetTag("ink_vy", toTag(sp.vel.y));
    e->SetTag("ink_vz", toTag(sp.vel.z));
    e->SetTag("ink_s0", toTag(sp.s0));
    e->SetTag("ink_s1", toTag(sp.s1));
    e->SetTag("ink_life", toTag(sp.life));
    e->SetTag("ink_a0", toTag(sp.color.w));
    e->SetTag("ink_drag", toTag(sp.drag));
    e->SetTag("ink_grav", toTag(sp.grav));

    auto *nsc = e->GetComponent<NativeScriptComponent>();
    if (!nsc) {
      nsc = &e->AddComponent<NativeScriptComponent>();
      nsc->AddScript("InkParticleScript");
      nsc->SetScene(scene);
      if (ctx) nsc->SetSceneContext(ctx);
    }

    if (isNew) scene->InitDynamicEntityRuntime(*e);

    // 色（使い回しの粒は前回の色と透明度が残っている）と Transform をその場で反映する
    if (pm->meshHandle >= 0) {
      // ライティングを切って平たい墨にする（陰影とハイライトが付くとボールに見える）
      RC::SetPrimitiveMeshLightingMode(pm->meshHandle, LightingMode::None);
      if (auto *mat = RC::GetPrimitiveMeshMaterialPtr(pm->meshHandle)) {
        mat->color = sp.color;
        mat->shininess = 0.0f;
        mat->environmentCoefficient = 0.0f;
      }
      if (auto *pmTr = RC::GetPrimitiveMeshTransformPtr(pm->meshHandle)) {
        pmTr->scale = tr->scale;
        pmTr->rotation = tr->rotation;
        pmTr->translation = tr->position;
      }
    }
  }
}

} // namespace OctopusDetail

// ============================================================================
// 墨玉
// ============================================================================

/// @brief タコが吐く墨玉。自機へゆるく追尾して飛び、届いたら画面に墨を貼る
/// @details WaterBullet は水面（y=0）を跨ぐと消えるうえ、当たり判定が is_player タグ依存なので、
///          水中から水上の自機へも届くよう専用に持つ。
///          撃ち落とし判定も自前で行う（is_enemy を付けると敵 HP の集計や HP バーに混ざるため）。
///          パラメータは生成時に pendingData（Deserialize）で受け取るだけなので、すべて private にしている。
class InkBlobScript : public ScriptableEntity {
protected:
  /// @brief 墨玉のパラメータを JSON へ書き出す
  /// @return 速度・追尾・寿命・判定半径・墨の強さ・ダメージ・向きを格納した json
  nlohmann::json Serialize() override {
    return {
        {"speed", speed_},
        {"homingRate", homingRate_},
        {"lifetime", lifetime_},
        {"hitRadius", hitRadius_},
        {"shootRadius", shootRadius_},
        {"inkAmount", inkAmount_},
        {"damage", damage_},
        {"dir", {direction_.x, direction_.y, direction_.z}},
    };
  }

  /// @brief JSON から墨玉のパラメータを読み込む（含まれる項目だけ上書きする）
  /// @param j 読み込み元の json（タコが発射時に pendingData として渡す）
  void Deserialize(const nlohmann::json &j) override {
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    f("speed", speed_);
    f("homingRate", homingRate_);
    f("lifetime", lifetime_);
    f("hitRadius", hitRadius_);
    f("shootRadius", shootRadius_);
    f("inkAmount", inkAmount_);
    if (j.contains("damage") && j["damage"].is_number()) damage_ = j["damage"].get<int>();
    if (j.contains("dir") && j["dir"].is_array() && j["dir"].size() == 3) {
      direction_ = {j["dir"][0].get<float>(), j["dir"][1].get<float>(), j["dir"][2].get<float>()};
    }
  }

  /// @brief 進行方向を正規化し（長さ 0 なら +Z）、脈打ちの基準スケールを保存する
  void OnCreate() override {
    const float l = OctopusDetail::Len(direction_);
    if (l > 1e-4f) direction_ = {direction_.x / l, direction_.y / l, direction_.z / l};
    else direction_ = {0.0f, 0.0f, 1.0f};
    elapsed_ = 0.0f;
    if (auto *tr = GetComponent<TransformComponent>()) baseScale_ = tr->scale;
  }

  /// @brief 自機へゆるく追尾しながら進み、命中と自機の弾による撃ち落としを判定する
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(float dt) override {
    if (done_ || dt <= 0.0f) return;
    auto *tr = GetComponent<TransformComponent>();
    Scene *scene = GetScene();
    if (!tr || !scene) return;

    elapsed_ += dt;
    if (elapsed_ >= lifetime_) { Finish(); return; }

    // 自機（カメラ）を取得する。キャッシュが無効になったときだけ探し直す
    std::shared_ptr<Entity> cam = target_.lock();
    if (!cam || cam->IsPendingDestroy() || !cam->IsActive()) {
      cam = PlayerMotionTracker::FindPlayerCamera(scene);
      target_ = cam;
    }

    // --- 追尾：向きを目標方向へ最大 homingRate [rad/s] だけ回す ---
    if (cam) {
      if (auto *camTr = cam->GetComponent<TransformComponent>()) {
        RC::Vector3 to = {camTr->position.x - tr->position.x,
                          camTr->position.y - tr->position.y,
                          camTr->position.z - tr->position.z};
        const float dist = OctopusDetail::Len(to);

        // 命中：墨の強さを ×100 の整数で pending_ink に加算し、ダメージがあれば pending_damage にも積む
        if (dist <= hitRadius_) {
          const int ink = cam->GetTagInt("pending_ink", 0);
          cam->SetTag("pending_ink", ink + static_cast<int>(std::clamp(inkAmount_, 0.0f, 1.0f) * 100.0f));
          if (damage_ > 0) {
            cam->SetTag("pending_damage", cam->GetTagInt("pending_damage", 0) + damage_);
          }
          Log::Print("[InkBlobScript] Ink hit the player!");
          Finish();
          return;
        }

        // 現在の向きと目標方向の角度差を求め、今フレームで回せる割合 t だけ線形補間して正規化する
        if (dist > 1e-3f && homingRate_ > 0.0f) {
          to = {to.x / dist, to.y / dist, to.z / dist};
          const float d = std::clamp(direction_.x * to.x + direction_.y * to.y + direction_.z * to.z, -1.0f, 1.0f);
          const float angle = std::acos(d);
          const float maxStep = homingRate_ * dt;
          if (angle > 1e-4f) {
            const float t = (std::min)(1.0f, maxStep / angle);
            RC::Vector3 nd = {direction_.x + (to.x - direction_.x) * t,
                              direction_.y + (to.y - direction_.y) * t,
                              direction_.z + (to.z - direction_.z) * t};
            const float l = OctopusDetail::Len(nd);
            if (l > 1e-4f) direction_ = {nd.x / l, nd.y / l, nd.z / l};
          }
        }
      }
    }

    tr->position.x += direction_.x * speed_ * dt;
    tr->position.y += direction_.y * speed_ * dt;
    tr->position.z += direction_.z * speed_ * dt;

    // ぷるぷる脈打たせる（ただの黒い球に見せない）
    const float wob = std::sin(elapsed_ * 18.0f) * 0.12f;
    tr->scale = {baseScale_.x * (1.0f + wob), baseScale_.y * (1.0f - wob), baseScale_.z * (1.0f + wob)};

    // --- 撃ち落とし：自機の弾が近くにいれば両方消す ---
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive() || e->IsPendingDestroy()) continue;
      if (e->GetName() != "PlayerBullet") continue;
      auto *btr = e->GetComponent<TransformComponent>();
      if (!btr) continue;
      const RC::Vector3 d = {btr->position.x - tr->position.x,
                             btr->position.y - tr->position.y,
                             btr->position.z - tr->position.z};
      if (d.x * d.x + d.y * d.y + d.z * d.z <= shootRadius_ * shootRadius_) {
        // 弾はプール運用（WaterBullet は非アクティブ化で回収、再利用時に reused で初期化される）
        e->SetActive(false);
        if (cam) cam->SetTag("score_add", cam->GetTagInt("score_add", 0) + 1);
        Log::Print("[InkBlobScript] Shot down.");
        OctopusDetail::SpawnInkBurst(scene, GetSceneContext(), tr->position, direction_,
                                     (std::max)(baseScale_.x, 0.2f));
        Finish();
        return;
      }
    }
  }

private:
  /// @brief 墨玉を終了させて自身を破棄する（以降の更新は行わない）
  void Finish() {
    done_ = true;
    if (Entity *self = GetEntity()) self->Destroy();
  }

  float speed_ = 13.0f;        ///< 飛ぶ速さ (m/s)
  float homingRate_ = 1.1f;    ///< 追尾の強さ（rad/s）。0 で直進
  float lifetime_ = 5.0f;      ///< 何秒で消えるか
  float hitRadius_ = 1.8f;     ///< 自機（カメラ）にこの距離まで来たら命中
  float shootRadius_ = 1.3f;   ///< 自機の弾がこの距離まで来たら撃ち落とされる
  float inkAmount_ = 1.0f;     ///< 命中時に貼る墨の強さ 0..1
  int damage_ = 0;             ///< 命中時のダメージ（既定は視界だけ奪う）
  RC::Vector3 direction_ = {0.0f, 0.0f, 1.0f}; ///< 進行方向（OnCreate で正規化し、追尾で毎フレーム更新する）

  std::weak_ptr<Entity> target_;
  RC::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
  float elapsed_ = 0.0f;
  bool done_ = false;
};

REGISTER_SCRIPT(InkBlobScript)

// ============================================================================
// タコ本体
// ============================================================================

/// @brief タコの敵。ジェット推進で自機の前方水中に陣取り、ためてから墨玉を吐いて視界を奪う
/// @details 状態遷移・ジェット推進と水の抵抗による移動・ため中の膨らみ／明滅・墨玉の発射を担う。
///
///          行動は State パターン（StateMachine.h）で組んでいる：
///            Wait（待機）→ Approach（接近）→ Windup（ため）→ Recover（立て直し）→ Approach …
///          以前は enum OctopusState + switch で 1 つの OnUpdate に全状態の処理と遷移条件が並んでいた。
///          状態ごとのタイマーや「ため中に撃たれたらひるむ」といった状態固有の反応が
///          本体のあちこちに散らばっていたので、状態 1 つ = クラス 1 つに分け、
///          次の状態は各状態クラス自身が決めるようにした（本体は fsm_.Update を呼ぶだけ）。
///          状態クラスは private な入れ子クラスなので、friend 無しで本体の private メンバを使える。
///
///          生存中／撃破後の振り分け・被弾フラッシュは基底（DamageableScript の Template Method）が行い、
///          このクラスは OnUpdateAlive / OnUpdateDefeated / OnDamaged だけを上書きする。
class OctopusEnemyScript : public EnemyBaseScript {
public:
  /// @brief インスペクタに状態表示・各パラメータの編集欄・即時発射ボタンを表示する
  void OnImGui() override {
    EnemyBaseScript::OnImGui();
#if RC_ENABLE_IMGUI
    ImGui::Text("State: %s", fsm_.CurrentName());
    ImGui::DragFloat("Detect Dist##Octo", &detectDistance_, 0.5f, 5.0f, 150.0f);
    ImGui::DragFloat("Preferred Dist##Octo", &preferredDistance_, 0.5f, 3.0f, 80.0f);
    ImGui::DragFloat("Side Offset##Octo", &sideOffset_, 0.1f, 0.0f, 30.0f);
    ImGui::DragFloat("Height Offset##Octo", &heightOffset_, 0.1f, -20.0f, 20.0f);
    ImGui::Separator();
    ImGui::DragFloat("Jet Interval##Octo", &jetInterval_, 0.05f, 0.2f, 5.0f);
    ImGui::DragFloat("Jet Impulse##Octo", &jetImpulse_, 0.1f, 0.0f, 40.0f);
    ImGui::DragFloat("Water Drag##Octo", &waterDrag_, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Max Speed##Octo", &maxSpeed_, 0.1f, 1.0f, 60.0f);
    ImGui::Separator();
    ImGui::DragFloat("Fire Min Dist##Octo", &fireMinDistance_, 0.5f, 0.0f, 100.0f);
    ImGui::DragFloat("Fire Max Dist##Octo", &fireMaxDistance_, 0.5f, 1.0f, 150.0f);
    ImGui::DragFloat("Fire Interval##Octo", &fireInterval_, 0.1f, 0.3f, 20.0f);
    ImGui::DragFloat("Windup##Octo", &windupDuration_, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Recover##Octo", &recoverDuration_, 0.05f, 0.0f, 5.0f);
    ImGui::DragFloat("Ink Speed##Octo", &inkSpeed_, 0.1f, 1.0f, 60.0f);
    ImGui::DragFloat("Ink Homing##Octo", &inkHoming_, 0.05f, 0.0f, 10.0f);
    ImGui::SliderFloat("Ink Amount##Octo", &inkAmount_, 0.1f, 1.0f);
    ImGui::DragInt("Ink Damage##Octo", &inkDamage_, 1, 0, 10);
    if (ImGui::Button("Spit Now##Octo")) {
      if (auto *tr = GetComponent<TransformComponent>()) {
        if (auto cam = PlayerMotionTracker::FindPlayerCamera(GetScene())) {
          if (auto *camTr = cam->GetComponent<TransformComponent>()) SpitInk(GetScene(), tr, camTr->position);
        }
      }
    }
#endif
  }

protected:
  /// @brief 基底の体力に加えて、タコのパラメータを JSON へ書き出す
  /// @return 全パラメータを格納した json
  nlohmann::json Serialize() override {
    nlohmann::json j = EnemyBaseScript::Serialize();
    j["detectDistance"] = detectDistance_;
    j["preferredDistance"] = preferredDistance_;
    j["sideOffset"] = sideOffset_;
    j["heightOffset"] = heightOffset_;
    j["surfaceMargin"] = surfaceMargin_;
    j["jetInterval"] = jetInterval_;
    j["jetImpulse"] = jetImpulse_;
    j["waterDrag"] = waterDrag_;
    j["maxSpeed"] = maxSpeed_;
    j["fireMinDistance"] = fireMinDistance_;
    j["fireMaxDistance"] = fireMaxDistance_;
    j["fireInterval"] = fireInterval_;
    j["firstFireDelay"] = firstFireDelay_;
    j["windupDuration"] = windupDuration_;
    j["recoverDuration"] = recoverDuration_;
    j["recoilSpeed"] = recoilSpeed_;
    j["inkSpeed"] = inkSpeed_;
    j["inkHoming"] = inkHoming_;
    j["inkAmount"] = inkAmount_;
    j["inkDamage"] = inkDamage_;
    j["inkBlobScale"] = inkBlobScale_;
    j["inkBlobColor"] = {inkBlobColor_.x, inkBlobColor_.y, inkBlobColor_.z, inkBlobColor_.w};
    j["squash"] = squash_;
    j["windupSwell"] = windupSwell_;
    j["windupColor"] = {windupColor_.x, windupColor_.y, windupColor_.z, windupColor_.w};
    return j;
  }

  /// @brief JSON から基底の体力とタコのパラメータを読み込む（含まれる項目だけ上書きする）
  /// @param j 読み込み元の json
  void Deserialize(const nlohmann::json &j) override {
    EnemyBaseScript::Deserialize(j);
    auto f = [&](const char *k, float &o) { if (j.contains(k) && j[k].is_number()) o = j[k].get<float>(); };
    auto c4 = [&](const char *k, RC::Vector4 &o) {
      if (j.contains(k) && j[k].is_array() && j[k].size() == 4) {
        o = {j[k][0].get<float>(), j[k][1].get<float>(), j[k][2].get<float>(), j[k][3].get<float>()};
      }
    };
    f("detectDistance", detectDistance_);
    f("preferredDistance", preferredDistance_);
    f("sideOffset", sideOffset_);
    f("heightOffset", heightOffset_);
    f("surfaceMargin", surfaceMargin_);
    f("jetInterval", jetInterval_);
    f("jetImpulse", jetImpulse_);
    f("waterDrag", waterDrag_);
    f("maxSpeed", maxSpeed_);
    f("fireMinDistance", fireMinDistance_);
    f("fireMaxDistance", fireMaxDistance_);
    f("fireInterval", fireInterval_);
    f("firstFireDelay", firstFireDelay_);
    f("windupDuration", windupDuration_);
    f("recoverDuration", recoverDuration_);
    f("recoilSpeed", recoilSpeed_);
    f("inkSpeed", inkSpeed_);
    f("inkHoming", inkHoming_);
    f("inkAmount", inkAmount_);
    if (j.contains("inkDamage") && j["inkDamage"].is_number()) inkDamage_ = j["inkDamage"].get<int>();
    f("inkBlobScale", inkBlobScale_);
    c4("inkBlobColor", inkBlobColor_);
    f("squash", squash_);
    f("windupSwell", windupSwell_);
    c4("windupColor", windupColor_);
  }

  /// @brief タコとしての既定値（HP・撃破演出時間）を設定し、陣取る側と噴射の位相を決める
  /// @note 初期状態（WaitState）を入れるため、状態クラスの定義より後（クラス定義の外）で定義する。
  void OnCreate() override;

  /// @brief 自機の進行方向の推定、陣取る位置の計算、状態ごとの行動、向きと見た目の更新を行う
  /// @param dt 前フレームからの経過秒数
  void OnUpdateAlive(float dt) override {
    TransformComponent *tr = BeginFrame(dt);
    if (!tr) return;
    Scene *scene = GetScene();

    // 自機（カメラ）を取得する。探し直したときの速度推定のやり直しは PlayerMotionTracker が行う
    const TransformComponent *camTr = player_.AcquirePlayer(scene);
    if (!camTr) return;

    // --- 自機の進行方向（レールの向き）と速度を推定する ---
    player_.Update(*camTr, dt);
    const RC::Vector3 &camFwd = player_.Forward();
    const RC::Vector3 camRight = player_.Right();

    const RC::Vector3 toCam = {camTr->position.x - tr->position.x,
                               camTr->position.y - tr->position.y,
                               camTr->position.z - tr->position.z};
    const float dist = OctopusDetail::Len(toCam);
    // 自機から見て前方にいるか（XZ）
    const float frontDot = (dist > 1e-3f) ? (-(toCam.x * camFwd.x + toCam.z * camFwd.z) / dist) : 0.0f;
    const bool inFront = frontDot > 0.35f;

    // 陣取る位置：自機の前方・少し横・少し下。ゆっくり左右に揺れる
    const float sway = std::sin(time_ * 0.6f) * 2.5f;
    RC::Vector3 home = {
        camTr->position.x + camFwd.x * preferredDistance_ + camRight.x * (sideSign_ * sideOffset_ + sway),
        camTr->position.y + heightOffset_ + std::sin(time_ * 1.3f) * 0.8f,
        camTr->position.z + camFwd.z * preferredDistance_ + camRight.z * (sideSign_ * sideOffset_ + sway)};
    // タコは水から出ない。自機が水上にいるときは水面すぐ下に陣取り、そこから墨を吐き上げる
    if (!waterChecked_) {
      hasWater_ = OctopusDetail::FindWaterY(scene, waterY_);
      waterChecked_ = true;
    }
    if (hasWater_) home.y = (std::min)(home.y, waterY_ - surfaceMargin_);

    // 状態クラスが読む今フレームの状況をまとめてから、現在の状態を 1 フレーム進める
    frame_.tr = tr;
    frame_.scene = scene;
    frame_.camTr = camTr;
    frame_.toCam = toCam;
    frame_.dist = dist;
    frame_.inFront = inFront;
    frame_.home = home;
    swell_ = 0.0f; // ため中の膨らみ 0..1（WindupState だけが書く）
    fsm_.Update(*this, dt);

    // --- 向き：常に自機の方を向く（Y 軸のみ） ---
    if (dist > 1e-3f) tr->rotation.y = std::atan2(toCam.x, toCam.z);

    // --- 見た目：噴射の伸び縮み＋ためで膨らむ ---
    jetPulse_ = (std::max)(0.0f, jetPulse_ - dt * 3.0f);
    const float p = jetPulse_ * jetPulse_ * squash_;
    const float breathe = std::sin(time_ * 2.2f) * 0.04f;
    const float s = 1.0f + swell_ * windupSwell_;
    tr->scale = {baseScale_.x * s * (1.0f - p * 0.6f + breathe),
                 baseScale_.y * s * (1.0f + p - breathe),
                 baseScale_.z * s * (1.0f - p * 0.6f + breathe)};
  }

  /// @brief 撃破後：しぼみながら沈む
  /// @param dt 前フレームからの経過秒数
  void OnUpdateDefeated(float dt) override {
    TransformComponent *tr = BeginFrame(dt);
    if (!tr) return;
    const float k = std::clamp(1.0f - GetDefeatTimer() / (std::max)(GetDefeatDuration(), 0.01f), 0.0f, 1.0f);
    tr->scale = {baseScale_.x * (0.4f + 0.6f * k), baseScale_.y * (0.2f + 0.8f * k), baseScale_.z * (0.4f + 0.6f * k)};
    tr->position.y -= 1.5f * dt;
    PlayModelClip("Idle", 0.3f); // ため中に倒されたら足を戻す
  }

  /// @brief 被弾のたびに呼ばれる。撃たれたときの反応は現在の状態クラスに任せる（ため中ならひるむ）
  /// @param damage 受けたダメージ量
  /// @note 状態クラスの関数を呼ぶため、状態クラスの定義より後（クラス定義の外）で定義する。
  void OnDamaged(int damage) override;

private:
  // ------------------------------------------------------------------
  // 状態（State パターン）。定義はクラス定義の後ろ
  // ------------------------------------------------------------------
  using Machine = StateMachine<OctopusEnemyScript>; ///< タコ用のステートマシン
  class StateBase;     ///< タコの状態の共通基底（被弾時の反応を足したもの）
  class WaitState;     ///< 待機：漂いながら自機が索敵距離に入るのを待つ
  class ApproachState; ///< 接近：ジェット推進で陣取る位置へ向かい、撃てるならために入る
  class WindupState;   ///< ため：膨らんで明滅し、終わったら墨を吐く
  class RecoverState;  ///< 立て直し：吐いた直後（またはひるんだ直後）の硬直

  /// @brief 生存中の 1 フレームぶんの状況（OnUpdateAlive が計算し、状態クラスが読む）
  struct FrameInfo {
    TransformComponent *tr = nullptr;           ///< 自身の Transform
    Scene *scene = nullptr;                     ///< 所属シーン
    const TransformComponent *camTr = nullptr;  ///< 自機（カメラ）の Transform
    RC::Vector3 toCam = {0.0f, 0.0f, 0.0f};     ///< 自身から自機へのベクトル
    float dist = 0.0f;                          ///< 自機までの距離
    bool inFront = false;                       ///< 自機から見て前方にいるか
    RC::Vector3 home = {0.0f, 0.0f, 0.0f};      ///< 陣取る位置
  };

  /// @brief 生存中・撃破後に共通する毎フレームの前処理（時間を進める）
  /// @param dt 前フレームからの経過秒数
  /// @return 自身の Transform。dt が 0 以下、または Transform / シーンが無ければ nullptr（このフレームは何もしない）
  TransformComponent *BeginFrame(float dt) {
    if (dt <= 0.0f) return nullptr;
    auto *tr = GetComponent<TransformComponent>();
    if (!tr || !GetScene()) return nullptr;
    time_ += dt;
    return tr;
  }

  /// @brief モデルで描いているときだけクリップを再生する（球で代用しているときは何もしない）
  /// @param name クリップ名（Idle / Jet / Windup / Spit）
  /// @param blend クロスフェード秒数
  /// @param force 同じクリップでも頭から再生し直すか
  void PlayModelClip(const char *name, float blend, bool force = false) {
    if (AnimationComponent *anim = ModelAnimation()) anim->PlayClip(name, blend, force);
  }

  /// @brief 色（4 成分ベクトル）を線形補間する
  /// @param a t = 0 のときの値
  /// @param b t = 1 のときの値
  /// @param t 補間係数
  /// @return 補間結果
  static RC::Vector4 Lerp4(const RC::Vector4 &a, const RC::Vector4 &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
  }

  /// @brief 速度で動かして、水の抵抗で減速させる（陰的に解くので dt が大きくても発散しない）
  /// @param tr 移動させる自身の Transform
  /// @param dt 前フレームからの経過秒数
  /// @note 速度は maxSpeed で頭打ちにし、水面から surfaceMargin の半分より上には出さない。
  void Drift(TransformComponent *tr, float dt) {
    const float k = 1.0f / (1.0f + waterDrag_ * dt);
    velocity_.x *= k;
    velocity_.y *= k;
    velocity_.z *= k;
    const float sp = OctopusDetail::Len(velocity_);
    if (sp > maxSpeed_) {
      velocity_.x *= maxSpeed_ / sp;
      velocity_.y *= maxSpeed_ / sp;
      velocity_.z *= maxSpeed_ / sp;
    }
    tr->position.x += velocity_.x * dt;
    tr->position.y += velocity_.y * dt;
    tr->position.z += velocity_.z * dt;
    if (hasWater_ && tr->position.y > waterY_ - surfaceMargin_ * 0.5f) {
      tr->position.y = waterY_ - surfaceMargin_ * 0.5f;
      if (velocity_.y > 0.0f) velocity_.y = 0.0f;
    }
  }

  /// @brief 墨玉を吐く
  /// @param scene 墨玉を生成するシーン
  /// @param tr 自身の Transform（吐く位置の基準）
  /// @param targetPos 狙う自機（カメラ）の位置
  void SpitInk(Scene *scene, const TransformComponent *tr, const RC::Vector3 &targetPos) {
    if (!scene || !tr) return;

    // 自機の未来位置を少しだけ読む（完全に読むと避けようがない）
    const RC::Vector3 &camVel = player_.Velocity();
    const float flight = OctopusDetail::Len({targetPos.x - tr->position.x,
                                             targetPos.y - tr->position.y,
                                             targetPos.z - tr->position.z}) /
                         (std::max)(inkSpeed_, 1.0f);
    const RC::Vector3 aim = {targetPos.x + camVel.x * flight * 0.5f,
                             targetPos.y,
                             targetPos.z + camVel.z * flight * 0.5f};

    // 口（自機側の表面）から出す
    RC::Vector3 dir = {aim.x - tr->position.x, aim.y - tr->position.y, aim.z - tr->position.z};
    const float l = OctopusDetail::Len(dir);
    if (l < 1e-3f) return;
    dir = {dir.x / l, dir.y / l, dir.z / l};
    const float mouth = (std::max)({baseScale_.x, baseScale_.y, baseScale_.z}) * 0.6f;

    // 墨玉エンティティを生成し、InkBlobScript へ速度・追尾・墨の強さ・ダメージ・向きを pendingData で渡す
    auto blob = scene->CreateEntity("InkBlob");
    if (!blob) return;
    auto &btr = blob->AddComponent<TransformComponent>();
    btr.position = {tr->position.x + dir.x * mouth, tr->position.y + dir.y * mouth, tr->position.z + dir.z * mouth};
    btr.scale = {inkBlobScale_, inkBlobScale_, inkBlobScale_};

    auto &pm = blob->AddComponent<PrimitiveMeshComponent>();
    pm.type = PrimitiveType::Sphere;
    pm.color = inkBlobColor_;

    auto &nsc = blob->AddComponent<NativeScriptComponent>();
    nsc.AddScript("InkBlobScript");
    if (!nsc.scripts.empty()) {
      nsc.scripts.back().pendingData = {
          {"speed", inkSpeed_},
          {"homingRate", inkHoming_},
          {"inkAmount", inkAmount_},
          {"damage", inkDamage_},
          {"dir", {dir.x, dir.y, dir.z}},
      };
    }
    nsc.SetScene(scene);
    if (GetSceneContext()) nsc.SetSceneContext(GetSceneContext());

    scene->InitDynamicEntityRuntime(*blob);

    // 生成直後に PrimitiveMesh の Transform を合わせて原点でのチラつきを防ぐ
    if (auto *newPm = blob->GetComponent<PrimitiveMeshComponent>()) {
      if (newPm->meshHandle >= 0) {
        if (auto *pmTr = RC::GetPrimitiveMeshTransformPtr(newPm->meshHandle)) {
          pmTr->scale = btr.scale;
          pmTr->rotation = btr.rotation;
          pmTr->translation = btr.position;
        }
      }
    }
    Log::Print("[OctopusEnemyScript] Spit ink!");
  }

  // --- 索敵・位置取り ---
  float detectDistance_ = 45.0f;     ///< この距離に自機が来たら動き出す
  float preferredDistance_ = 20.0f;  ///< 自機の前方このくらいの距離に陣取る
  float sideOffset_ = 6.0f;          ///< 自機の正面から左右へずれる量（個体ごとに符号が変わる）
  float heightOffset_ = -1.5f;       ///< 自機からの高さのずれ（負で下）
  float surfaceMargin_ = 1.2f;       ///< 水面からこれ以上は上がらない（水面の下 m）

  // --- ジェット推進 ---
  float jetInterval_ = 1.1f;   ///< 1 回噴いてから次に噴くまで（秒）
  float jetImpulse_ = 9.0f;    ///< 1 回の噴射で得る速さ (m/s)
  float waterDrag_ = 1.8f;     ///< 水の抵抗（1/s）。噴射後にスーッと減速する
  float maxSpeed_ = 14.0f;     ///< 最高速 (m/s)

  // --- 墨 ---
  float fireMinDistance_ = 8.0f;   ///< 墨を吐く最小距離（これより近いと吐かない）
  float fireMaxDistance_ = 32.0f;  ///< 墨を吐く最大距離
  float fireInterval_ = 4.0f;      ///< 吐いてから次に吐けるまで（秒）
  float firstFireDelay_ = 1.5f;    ///< 動き出してから最初に吐けるまで（秒）
  float windupDuration_ = 0.9f;    ///< ため（膨らむ）時間。この間に倒せば吐かれない
  float recoverDuration_ = 0.6f;   ///< 吐いたあと動けない時間
  float recoilSpeed_ = 6.0f;       ///< 吐いた反動で後ろへ下がる速さ
  float inkSpeed_ = 13.0f;         ///< 墨玉の速さ（InkBlobScript の speed へ渡す）
  float inkHoming_ = 1.1f;         ///< 墨玉の追尾の強さ（InkBlobScript の homingRate へ渡す）
  float inkAmount_ = 1.0f;         ///< 命中時に貼る墨の強さ（InkBlobScript の inkAmount へ渡す）
  int inkDamage_ = 0;              ///< 墨玉の命中ダメージ（InkBlobScript の damage へ渡す）
  float inkBlobScale_ = 0.7f;      ///< 墨玉の大きさ
  RC::Vector4 inkBlobColor_ = {0.05f, 0.03f, 0.08f, 1.0f}; ///< 墨玉の色

  // --- 見た目 ---
  float squash_ = 0.28f;           ///< 噴射時の伸び縮み
  float windupSwell_ = 0.35f;      ///< ため時に膨らむ量
  RC::Vector4 windupColor_ = {0.55f, 0.15f, 0.75f, 1.0f}; ///< ため時に明滅する色（予兆）

  // --- 実行時の状態 ---
  Machine fsm_;                  ///< 行動の状態（Wait / Approach / Windup / Recover）
  PlayerMotionTracker player_;   ///< 自機の追跡と進行方向・速度の推定
  FrameInfo frame_;              ///< 今フレームの状況（状態クラスが読む）
  float swell_ = 0.0f;           ///< ため中の膨らみ 0..1（毎フレーム 0 に戻し、WindupState が書く）
  RC::Vector3 baseScale_ = {1.0f, 1.0f, 1.0f};
  RC::Vector3 velocity_ = {0.0f, 0.0f, 0.0f};
  float sideSign_ = 1.0f;
  float time_ = 0.0f;
  float jetTimer_ = 0.0f;
  float jetPulse_ = 0.0f;
  float fireCooldown_ = 0.0f;
  bool waterChecked_ = false;
  bool hasWater_ = false;
  float waterY_ = 0.0f;
};

// ============================================================================
// タコの状態クラス
// ============================================================================

/// @brief タコの状態の共通基底。IState に「撃たれたときの反応」を足したもの
/// @details 撃たれたときにどうするか（ため中ならひるむ、それ以外は何もしない）は状態ごとに違うので、
///          本体で状態を判定して分岐するのではなく、状態クラスの仮想関数で差し替える（ポリモーフィズム）。
class OctopusEnemyScript::StateBase : public IState<OctopusEnemyScript> {
public:
  /// @brief 生存中に被弾したときの反応（既定は何もしない）
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  virtual void OnHit(OctopusEnemyScript &owner, Machine &machine) {
    (void)owner;
    (void)machine;
  }
};

/// @brief 待機：慣性で漂いながら、自機が索敵距離に入るのを待つ
class OctopusEnemyScript::WaitState final : public OctopusEnemyScript::StateBase {
public:
  /// @brief 漂い、自機が索敵距離に入ったら接近へ
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) override;

  /// @brief 状態名
  /// @return "Wait"
  const char *Name() const override { return "Wait"; }
};

/// @brief 接近：一定間隔のジェット噴射で陣取る位置へ向かい、撃てる条件がそろえばために入る
class OctopusEnemyScript::ApproachState final : public OctopusEnemyScript::StateBase {
public:
  /// @brief 噴射して漂い、クールダウン明けに前方・射程内ならためへ
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) override;

  /// @brief 状態名
  /// @return "Approach"
  const char *Name() const override { return "Approach"; }
};

/// @brief ため：その場で踏ん張って膨らみ、明滅（予兆）してから墨を吐く
class OctopusEnemyScript::WindupState final : public OctopusEnemyScript::StateBase {
public:
  /// @brief ため始め：明滅用に元の色を保存し、足をためのクリップにする
  /// @param owner タコ本体
  /// @param machine 未使用
  void OnEnter(OctopusEnemyScript &owner, Machine &machine) override;

  /// @brief 膨らみ・明滅を進め、ためきったら墨を吐いて立て直しへ
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) override;

  /// @brief ため中に撃たれるとひるんで中断する（吐かせないチャンスを作る）
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  void OnHit(OctopusEnemyScript &owner, Machine &machine) override;

  /// @brief 状態名
  /// @return "Windup"
  const char *Name() const override { return "Windup"; }

private:
  float timer_ = 0.0f; ///< ため始めてからの経過秒数
};

/// @brief 立て直し：吐いた直後（またはひるんだ直後）の硬直
class OctopusEnemyScript::RecoverState final : public OctopusEnemyScript::StateBase {
public:
  /// @brief 漂いながら硬直時間を待ち、終わったら接近へ
  /// @param owner タコ本体
  /// @param machine 遷移の予約に使うステートマシン
  /// @param dt 前フレームからの経過秒数
  void OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) override;

  /// @brief 状態名
  /// @return "Recover"
  const char *Name() const override { return "Recover"; }

private:
  float timer_ = 0.0f; ///< 硬直してからの経過秒数
};

// ---------------------------------------------------------------------------
// 状態クラスの処理（全状態クラスが完全型になってから定義する。ChangeState<次の状態> に必要）
// ---------------------------------------------------------------------------

void OctopusEnemyScript::WaitState::OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) {
  // 慣性で漂いながら、自機が索敵距離に入るのを待つ
  owner.Drift(owner.frame_.tr, dt);
  if (owner.frame_.dist <= owner.detectDistance_) {
    machine.ChangeState<ApproachState>();
    Log::Print("[OctopusEnemyScript] Detected player -> Approach");
  }
}

void OctopusEnemyScript::ApproachState::OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) {
  const FrameInfo &f = owner.frame_;
  TransformComponent *tr = f.tr;

  // 一定間隔で目標へ向かって噴射。噴射と噴射の間は抵抗でスーッと減速する
  owner.jetTimer_ -= dt;
  if (owner.jetTimer_ <= 0.0f) {
    owner.jetTimer_ += owner.jetInterval_;
    RC::Vector3 to = {f.home.x - tr->position.x, f.home.y - tr->position.y, f.home.z - tr->position.z};
    const float l = OctopusDetail::Len(to);
    if (l > 0.5f) {
      // 近いほど弱く噴く（行き過ぎて振り子にならないように）
      const float k = std::clamp(l / 6.0f, 0.25f, 1.0f);
      owner.velocity_.x += to.x / l * owner.jetImpulse_ * k;
      owner.velocity_.y += to.y / l * owner.jetImpulse_ * k;
      owner.velocity_.z += to.z / l * owner.jetImpulse_ * k;
      owner.jetPulse_ = 1.0f;
      owner.PlayModelClip("Jet", 0.05f, true); // 足を閉じて水を押し出す（終わると Idle に戻る）
    }
  }
  owner.Drift(tr, dt);

  // クールダウンが明け、自機の前方かつ射程内にいれば「ため」に入る
  owner.fireCooldown_ -= dt;
  if (owner.fireCooldown_ <= 0.0f && f.inFront && f.dist >= owner.fireMinDistance_ &&
      f.dist <= owner.fireMaxDistance_) {
    machine.ChangeState<WindupState>();
  }
}

void OctopusEnemyScript::WindupState::OnEnter(OctopusEnemyScript &owner, Machine &machine) {
  (void)machine;
  owner.SaveOriginalColor();
  owner.PlayModelClip("Windup", 0.2f);
}

void OctopusEnemyScript::WindupState::OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) {
  const FrameInfo &f = owner.frame_;

  // その場で踏ん張って膨らむ（予兆）。強めの抵抗で止める
  owner.velocity_.x *= std::exp(-6.0f * dt);
  owner.velocity_.y *= std::exp(-6.0f * dt);
  owner.velocity_.z *= std::exp(-6.0f * dt);
  owner.Drift(f.tr, dt);

  timer_ += dt;
  const float t = std::clamp(timer_ / (std::max)(owner.windupDuration_, 0.01f), 0.0f, 1.0f);
  owner.swell_ = t * t;
  // 明滅がだんだん速くなる。被弾フラッシュ（赤）中はそちらを優先
  if (owner.GetFlashTimer() <= 0.0f) {
    const float blink = 0.5f + 0.5f * std::sin(timer_ * (10.0f + 30.0f * t));
    owner.ApplyColor(Lerp4(owner.GetOriginalColor(), owner.windupColor_, blink * (0.4f + 0.6f * t)));
  }

  if (timer_ >= owner.windupDuration_) {
    if (owner.GetFlashTimer() <= 0.0f) owner.RestoreColor();
    owner.SpitInk(f.scene, f.tr, f.camTr->position);
    // 反動で後ろへ
    if (f.dist > 1e-3f) {
      owner.velocity_.x -= f.toCam.x / f.dist * owner.recoilSpeed_;
      owner.velocity_.y -= f.toCam.y / f.dist * owner.recoilSpeed_;
      owner.velocity_.z -= f.toCam.z / f.dist * owner.recoilSpeed_;
    }
    owner.jetPulse_ = 1.0f;
    owner.PlayModelClip("Spit", 0.05f, true); // 吐いた反動で足を閉じる（終わると Idle に戻る）
    owner.fireCooldown_ = owner.fireInterval_;
    machine.ChangeState<RecoverState>();
  }
}

void OctopusEnemyScript::WindupState::OnHit(OctopusEnemyScript &owner, Machine &machine) {
  // ため中に撃たれるとひるんで中断（吐かせないチャンスを作る）
  owner.fireCooldown_ = owner.fireInterval_ * 0.5f;
  owner.PlayModelClip("Jet", 0.08f, true); // ひるんで足をすぼめる
  machine.ChangeState<RecoverState>();
}

void OctopusEnemyScript::RecoverState::OnUpdate(OctopusEnemyScript &owner, Machine &machine, float dt) {
  // 吐いた直後の硬直。終わったらすぐ噴射できるよう jetTimer_ を短くして接近へ戻る
  owner.Drift(owner.frame_.tr, dt);
  timer_ += dt;
  if (timer_ >= owner.recoverDuration_) {
    owner.jetTimer_ = 0.2f;
    machine.ChangeState<ApproachState>();
  }
}

// ---------------------------------------------------------------------------
// 本体の処理のうち、状態クラスに触れるもの
// ---------------------------------------------------------------------------

void OctopusEnemyScript::OnCreate() {
  EnemyBaseScript::OnCreate();
  // データ側で指定されていなければタコの既定値を使う
  SetDefaultHp(4);
  SetDefaultDeathDuration(1.6f);

  // モデルで描くときの足のクリップ（並びは octopus_enemy.glb のアニメーション順）
  SetupModelClips({{"Idle", "", 0, true, 1.0f},
                   {"Jet", "", 1, false, 1.0f},
                   {"Windup", "", 2, true, 1.0f},
                   {"Spit", "", 3, false, 1.0f}},
                  "Idle");

  // 初期状態は待機
  fsm_.ChangeState<WaitState>();
  fsm_.ApplyPending(*this);
  if (auto *tr = GetComponent<TransformComponent>()) {
    baseScale_ = tr->scale;
    // 左右どちらに陣取るかは生成位置で決める（同じウェーブの個体が重ならないように）
    sideSign_ = (std::fmod(std::fabs(tr->position.x * 0.37f + tr->position.z * 0.71f), 2.0f) < 1.0f) ? 1.0f : -1.0f;
    jetTimer_ = std::fmod(std::fabs(tr->position.x + tr->position.z), jetInterval_); // 噴射の位相もずらす
  }
  fireCooldown_ = firstFireDelay_;
}

void OctopusEnemyScript::OnDamaged(int damage) {
  (void)damage;
  if (IsDefeated()) return;
  // 撃たれたときの反応は状態ごとに違うので、現在の状態クラスに任せる（本体は状態を判定しない）。
  // 以前は被弾した瞬間に状態が切り替わっていたので、予約した遷移もすぐ反映する。
  if (StateBase *state = fsm_.As<StateBase>()) {
    state->OnHit(*this, fsm_);
    fsm_.ApplyPending(*this);
  }
}

REGISTER_SCRIPT(OctopusEnemyScript)
