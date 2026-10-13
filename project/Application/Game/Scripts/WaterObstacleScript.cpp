#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "Common/Log/Log.h"
#include "Scene.h"
#include "RenderCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// =====================================================================
// D-01: 水面の障害物をシーンデータから決める
// =====================================================================
// 以前は障害物の座標と半径が RenderWater.cpp に直接書かれていた。
// 「どれが障害物か」はシーンの中身を知っているアプリ側にしか決められないので、
// エンジンには受け口（RC::SetWaterObstacles）だけを残し、
// 中身をこのスクリプトが毎フレーム入れる。
//
// 水面エンティティに 1 つ載せて使う。自分の Y がそのまま水面の高さになる。
//
// 何を障害物とみなすか
// --------------------
// 既定は「`is_terrain` タグを持ち、かつ**上下の範囲が水面をまたぐ**もの」。
// 水面をまたぐかどうかで見るのがちょうどよい理由は、
//   ・水面より上にしか無いもの（浮いている板など）は波を乱さない
//   ・水面より下にしか無いもの（海底・深いところのサンゴ）も水面には出ない
// の 2 つを同時に落とせるから。Game.json では岩 3 つだけが残り、
// 海底とサンゴ 4 つは自動的に外れる。
// タグ `water_obstacle` を 1 / 0 で明示すれば、この判定を上書きできる。
//
// 半径をどう決めるか
// ------------------
// シェーダは障害物を「半径 r の円」として扱い、内側の波を平らに潰し（insideMask）、
// 円周上で波を反射させる。つまり r は**水際での見た目の大きさ**であるべき。
//
// 岩は箱なので円とは完全には一致しない。外接円（XZ の対角線の半分）を採ると
// 面の中央にわずかな平らな余白ができ、内接円を採ると角から波が突き抜ける。
// 水面では**波が岩を突き抜けるほうが破綻して見える**ので外接円を既定にした。
// 余白が気になる場合は radiusScale で詰められる。
//
// なお以前のハードコード値は 7 / 8 / 9 だったが、岩の実寸から出る外接半径は
// 3.54 / 4.24 / 4.95 で、**およそ 2 倍大きく取られていた**。
// そのぶん岩から離れた場所に平らな輪ができ、反射波もその外側から始まっていた。
//
// 注意: 半径を実寸に直すと反射の到達範囲も半分になる
// -----------------------------------------------
// 反射の届く距離は「半径 × reflectRange」で決まる。半径を 7 → 3.54 に直すと
// 到達距離も 21m → 10.6m に縮む。
// 「実効振幅（insideMask × 減衰）が 0.25 以上で、かつ岩の外側」という
// 実際に反射が見える帯で数えると、**乗る頂点が 8.1 個から 3.5 個へ減る**
// （水面メッシュの頂点間隔は 1.95m ＝ 技術的負債 D-06）。
// 3 個程度では波として認識できないので、そのままだと C-03 の反射は
// 直す前より見えなくなる。
//
// つまり旧ハードコードの過大な半径は、**粗い水面メッシュを偶然ごまかしていた**。
// 半径と到達範囲という別々の役割が 1 つの数字に混ざっていたということ。
// ここでは 2 つを分け、失われた到達距離は reflectRange 側で取り戻している
// （3.0 → 6.0 で、旧実装とほぼ同じ 7.2 / 10.1 頂点に戻る）。
//   半径        = 岩の水際の大きさ。insideMask と反射の起点を決める（実寸に従う）
//   reflectRange = 反射がどこまで届くか。見え方のチューニング値（自由に決めてよい）
// D-06（水面メッシュの LOD）が入って頂点が細かくなれば reflectRange は下げられる。

/// @brief 水面の障害物リストをシーンから集めてシェーダへ渡すスクリプト
class WaterObstacleScript : public ScriptableEntity {
public:
  // --- 選び方 ---
  /// @brief `is_terrain` のうち水面をまたぐものを自動で拾うか
  /// @details false にすると `water_obstacle` タグが 1 のものだけになる。
  bool autoDetect = true;
  /// @brief 自動判定で使うタグ名
  std::string terrainTag = "is_terrain";
  /// @brief 水面をまたぐ判定の余裕(m)。波の高さぶん見ておく
  float crossMargin = 0.5f;

  // --- 半径 ---
  /// @brief 求めた半径に掛ける倍率。見た目を見ながら詰めるための逃げ道
  float radiusScale = 1.0f;
  /// @brief 半径の下限(m)。極端に小さい岩で反射が消えるのを防ぐ
  float minRadius = 0.5f;

  // --- 反射のチューニング（シェーダの gObstacleCount.y / .z） ---
  /// @brief 反射の強さ（1.0 で入射波と同じ振幅）
  /// @warning **0 にしても反射は消えない。** シェーダも CPU 側の WaterSurface も
  ///          `(値 > 0) ? 値 : 1.0` というフォールバックを持っているため、
  ///          0 を渡すと 1.0（完全反射）に化ける（技術的負債 D-14）。
  ///          そのため下限を kMinReflect で切ってある。反射を消したいときは
  ///          障害物そのものを外すこと。
  float reflectStrength = 1.0f;
  /// @brief 反射の到達範囲（障害物半径に対する倍率）
  /// @details 同じ理由で 0 は使えない。下限は kMinReflect。
  float reflectRange = 3.0f;

  bool debugDraw = true; ///< 採用した障害物の円をギズモ描画する

  /// @brief 障害物の選び方・半径・反射の設定を JSON に書き出す
  /// @return 設定項目を格納した JSON
  nlohmann::json Serialize() override {
    return {
        {"autoDetect", autoDetect},
        {"terrainTag", terrainTag},
        {"crossMargin", crossMargin},
        {"radiusScale", radiusScale},
        {"minRadius", minRadius},
        {"reflectStrength", reflectStrength},
        {"reflectRange", reflectRange},
        {"debugDraw", debugDraw},
    };
  }

  /// @brief JSON から障害物の選び方・半径・反射の設定を読み込む
  /// @param j 読み込み元の JSON（存在するキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    if (j.contains("autoDetect")) autoDetect = j["autoDetect"].get<bool>();
    if (j.contains("terrainTag")) terrainTag = j["terrainTag"].get<std::string>();
    if (j.contains("crossMargin")) crossMargin = j["crossMargin"].get<float>();
    if (j.contains("radiusScale")) radiusScale = j["radiusScale"].get<float>();
    if (j.contains("minRadius")) minRadius = j["minRadius"].get<float>();
    if (j.contains("reflectStrength")) reflectStrength = j["reflectStrength"].get<float>();
    if (j.contains("reflectRange")) reflectRange = j["reflectRange"].get<float>();
    if (j.contains("debugDraw")) debugDraw = j["debugDraw"].get<bool>();
  }

protected:
  /// @brief シーンから障害物を集め直して水面シェーダへ渡し、反射パラメータを設定する
  /// @param deltaTime 前フレームからの経過時間（秒）。未使用
  void OnUpdate(float deltaTime) override {
    (void)deltaTime;
    // 障害物は動いたり増減したりしうるので、毎フレーム作り直して渡す
    Collect();
    RC::SetWaterObstacles(picked_.empty() ? nullptr : picked_.data(),
                          static_cast<int>(picked_.size()));
    // 0 を渡すとシェーダ側のフォールバックで 1.0 / 3.0 に化けるので下限を切る（D-14）
    RC::SetWaterReflectParams((std::max)(reflectStrength, kMinReflect),
                              (std::max)(reflectRange, kMinReflect));
  }

  /// @brief 水面シェーダへ渡している障害物リストを空にする
  void OnDestroy() override {
    // このスクリプトが居なくなったら障害物も無くす。
    // 残したままだと、水面だけ残ったシーンで岩の無い場所の波が平らになる。
    RC::SetWaterObstacles(nullptr, 0);
  }

public:
  /// @brief 採用した障害物の円と、反射の届く範囲の円を水面の高さに描く
  void OnDebugRender() override {
    if (!debugDraw) return;
    const float waterY = WaterHeight();
    const RC::Vector4 col = {0.2f, 0.9f, 1.0f, 1.0f};

    for (const auto &o : picked_) {
      // 障害物は「半径 r の円」として扱われる。水面の高さに円を描いて、
      // 岩の実寸とどれだけ合っているかを目で見て確かめられるようにする。
      DrawCircleXZ({o.x, waterY, o.z}, o.w, col, 32);
      // 反射波が届く外周。ここまでが波の跳ね返りに使われる
      DrawCircleXZ({o.x, waterY, o.z}, o.w * reflectRange,
                   {col.x, col.y, col.z, 0.3f}, 32);
    }
  }

#if RC_ENABLE_IMGUI
  /// @brief 選び方・半径・反射の設定 UI と、採用した障害物の一覧を表示する
  void OnImGui() override {
    ImGui::Checkbox("Auto Detect", &autoDetect);
    {
      char buf[64] = {};
      std::snprintf(buf, sizeof(buf), "%s", terrainTag.c_str());
      if (ImGui::InputText("Terrain Tag", buf, sizeof(buf))) terrainTag = buf;
    }
    ImGui::DragFloat("Cross Margin", &crossMargin, 0.05f, 0.0f, 10.0f);
    ImGui::DragFloat("Radius Scale", &radiusScale, 0.01f, 0.05f, 5.0f);
    ImGui::DragFloat("Min Radius", &minRadius, 0.05f, 0.0f, 20.0f);
    ImGui::Separator();
    // 下限を 0 にしない。0 はシェーダ側で 1.0 に化けて逆に最大反射になる（D-14）
    ImGui::DragFloat("Reflect Strength", &reflectStrength, 0.02f, kMinReflect, 3.0f);
    ImGui::DragFloat("Reflect Range", &reflectRange, 0.05f, kMinReflect, 12.0f);
    ImGui::Checkbox("Debug Draw", &debugDraw);

    ImGui::Separator();
    // 採用結果の一覧
    ImGui::Text("Water Y: %.2f   Picked: %zu / %d", WaterHeight(), picked_.size(),
                RC::GetMaxWaterObstacles());
    for (size_t i = 0; i < picked_.size(); ++i) {
      ImGui::BulletText("%s  (%.1f, %.1f)  r=%.2f",
                        i < pickedNames_.size() ? pickedNames_[i].c_str() : "?",
                        picked_[i].x, picked_[i].z, picked_[i].w);
    }
    if (skipped_ > 0) {
      // 上限を超えたぶんは黙って捨てるのではなく、捨てたことを見せる。
      // 「岩を足したのに波が反応しない」の原因がここだと分かるように。
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                         "%d 個は上限を超えたため未反映（遠いものから捨てています）", skipped_);
    }
  }
#endif

private:
  /// @brief 水面の高さ（このエンティティの Y）
  /// @return 水面の Y 座標（Transform が無ければ 0）
  float WaterHeight() {
    if (auto *tr = GetComponent<TransformComponent>()) return tr->position.y;
    return 0.0f;
  }

  /// @brief 障害物として扱うかどうかを判定する
  /// @param e 対象エンティティ
  /// @param tr その Transform
  /// @param waterY 水面の高さ
  /// @return 障害物として扱うなら true
  bool IsObstacle(Entity &e, const TransformComponent &tr, float waterY) const {
    // 明示指定があればそれが最優先。0 と書けば地形でも障害物にしない。
    if (e.HasTag("water_obstacle")) {
      return e.GetTagInt("water_obstacle", 0) != 0;
    }
    if (!autoDetect) return false;
    if (e.GetTagInt(terrainTag, 0) != 1) return false;

    // 上下の範囲が水面をまたぐか。
    // PrimitiveMesh は 1 辺 1 の箱を Transform でスケールする作りなので、
    // 高さ方向の半分は scale.y * 0.5 になる。
    const float halfY = std::fabs(tr.scale.y) * 0.5f + crossMargin;
    const float lo = tr.position.y - halfY;
    const float hi = tr.position.y + halfY;
    return (lo <= waterY) && (waterY <= hi);
  }

  /// @brief 水際での半径を Transform のスケールから求める
  /// @details 形状ごとに素のメッシュの大きさが違うので、そこを吸収する。
  ///          箱：GenerateBox(1,1,1) は ±0.5 なので XZ の半分は scale*0.5。
  ///              円で近似するため対角線の半分（外接円）を採る。
  ///          球・円柱：素の半径が 1.0 なので、そのまま scale が半径になる。
  ///          タグ `water_obstacle_radius`（10 倍した整数）があればそれを優先する。
  /// @param e 対象エンティティ
  /// @param tr その Transform
  /// @return 水際での半径(m)。radiusScale を掛けて minRadius で下限を切った値（タグ指定時はその値）
  float ComputeRadius(Entity &e, const TransformComponent &tr) const {
    // タグで半径が固定されていればそれを優先する（10 倍した整数で持っているので 0.1 を掛ける）
    if (e.HasTag("water_obstacle_radius")) {
      const float pinned = static_cast<float>(e.GetTagInt("water_obstacle_radius", 0)) * 0.1f;
      if (pinned > 0.0f) return pinned;
    }

    const float sx = std::fabs(tr.scale.x);
    const float sz = std::fabs(tr.scale.z);

    float r;
    const auto *pm = e.GetComponent<PrimitiveMeshComponent>();
    if (pm && (pm->type == PrimitiveType::Sphere || pm->type == PrimitiveType::Cylinder)) {
      // 素の半径が 1.0 のメッシュ。XZ で潰れている場合は大きいほうに合わせる
      r = (std::max)(sx, sz);
    } else {
      // 箱（およびそれ以外）。XZ 断面の外接円。
      // 内接円にすると角から波が突き抜けて見えるため、外接を既定にしている。
      r = 0.5f * std::sqrt(sx * sx + sz * sz);
    }

    r *= radiusScale;
    return (std::max)(r, minRadius);
  }

  /// @brief シーンを走査して障害物リストを作る
  void Collect() {
    picked_.clear();
    pickedNames_.clear();
    skipped_ = 0;

    Scene *scene = GetScene();
    if (!scene) return;

    const float waterY = WaterHeight();
    Entity *self = GetEntity();

    // 上限（シェーダの gObstacles[4]）を超える場合に「どれを捨てるか」を
    // 決められるよう、いったん全部集めてから距離で選ぶ。

    /// @brief シェーダへ渡す障害物の候補（送信データと、選別用の距離キー・名前）
    struct Candidate {
      RC::Vector4 data;
      float key;
      std::string name;
    };
    std::vector<Candidate> candidates;

    // 近いものを残す基準点。カメラがあればカメラ、無ければ水面の中心。
    const RC::Vector3 origin = ViewOrigin(scene);

    // 障害物の条件を満たすものを候補に集める（並べ替えのキーは基準点からの XZ 距離の 2 乗）
    for (const auto &e : scene->GetEntities()) {
      if (!e || e.get() == self || !e->IsActive()) continue;
      auto *tr = e->GetComponent<TransformComponent>();
      if (!tr) continue;
      if (!IsObstacle(*e, *tr, waterY)) continue;

      const float radius = ComputeRadius(*e, *tr);
      const float dx = tr->position.x - origin.x;
      const float dz = tr->position.z - origin.z;

      candidates.push_back({{tr->position.x, tr->position.y, tr->position.z, radius},
                            dx * dx + dz * dz,
                            e->GetName()});
    }

    const int maxCount = RC::GetMaxWaterObstacles();
    if (static_cast<int>(candidates.size()) > maxCount) {
      // 遠いものから捨てる。手前の岩ほど波との関係が目に入るため。
      std::partial_sort(candidates.begin(), candidates.begin() + maxCount, candidates.end(),
                        [](const Candidate &a, const Candidate &b) { return a.key < b.key; });
      skipped_ = static_cast<int>(candidates.size()) - maxCount;
      candidates.resize(static_cast<size_t>(maxCount));
    }

    // 残った候補を採用リストへ移す
    picked_.reserve(candidates.size());
    pickedNames_.reserve(candidates.size());
    for (const auto &c : candidates) {
      picked_.push_back(c.data);
      pickedNames_.push_back(c.name);
    }
  }

  /// @brief 上限を超えたときに「近い」を測る基準点
  /// @param scene 探索するシーン
  /// @return "Camera" という名前のエンティティの位置。無ければ自分（水面）の位置、それも無ければ原点
  RC::Vector3 ViewOrigin(Scene *scene) {
    if (Entity *cam = scene->FindEntityByName("Camera")) {
      if (auto *tr = cam->GetComponent<TransformComponent>()) return tr->position;
    }
    if (auto *tr = GetComponent<TransformComponent>()) return tr->position;
    return {0.0f, 0.0f, 0.0f};
  }

  /// @brief XZ 平面に円を描く（水面上の輪を見せるため）
  /// @param center 円の中心
  /// @param radius 半径（0 以下なら描かない）
  /// @param color 線の色
  /// @param segments 円を近似する線分の数（3 未満なら描かない）
  static void DrawCircleXZ(const RC::Vector3 &center, float radius, const RC::Vector4 &color,
                           int segments) {
    if (radius <= 0.0f || segments < 3) return;
    const float step = 6.28318530718f / static_cast<float>(segments);
    RC::Vector3 prev = {center.x + radius, center.y, center.z};
    for (int i = 1; i <= segments; ++i) {
      const float a = step * static_cast<float>(i);
      const RC::Vector3 cur = {center.x + std::cos(a) * radius, center.y,
                               center.z + std::sin(a) * radius};
      RC::DrawLine3D(prev, cur, color, true);
      prev = cur;
    }
  }

  /// @brief 反射パラメータの下限。0 を渡すとシェーダ側で 1.0 / 3.0 に化けるため（D-14）
  static constexpr float kMinReflect = 0.01f;

  std::vector<RC::Vector4> picked_;      ///< 今フレームに採用した障害物
  std::vector<std::string> pickedNames_; ///< 同上の名前（ImGui 表示用）
  int skipped_ = 0;                      ///< 上限を超えて捨てた数
};

REGISTER_SCRIPT(WaterObstacleScript)
