#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/CameraComponent.h"
#include "ECS/PrimitiveMeshComponent.h"
#include "Common/Log/Log.h"
#include "RenderCommon.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// タイトル画面：水の下の砂地とサンゴ礁
//
//   - Title.json の空エンティティに NativeScript として 1 つ付けるだけで動く。
//     TitleScreenScript / TitleAmbientScript / TitleMouseRippleScript とは独立していて、互いに参照しない。
//   - 見た目は Water.PS.hlsl の「浅瀬」処理（crestTint > 0 のシーンだけ）が担う：
//     水底が近いほど水が明るく透き通るので、砂地とサンゴが水越しに見える。
//   - サンゴはすべてプリミティブ（Sphere / Cone / Cylinder）の組み合わせ。
//     「群生地（clusters）」の中心・半径・数を JSON で決め、中身は seed 付き乱数で毎回同じに並べる。
//       brain  : 潰した球（脳サンゴ）
//       branch : 外へ開く円錐の束（枝サンゴ）
//       table  : 細い柱の上の平たい円盤（テーブルサンゴ）
//       tube   : 立った円柱の束（カイメン・チューブ）
//       rock   : 暗い色の潰した球・箱（岩）
//   - 生成は OnCreate で一度だけ（シーン読み込み中なので止まっても見えない）。
//     どれも "transient" タグ付きなので、エディタでシーンを保存しても JSON には残らない。
//   - 飛び込み（スタート）／浮上（Result → Title）でカメラが水底より下へ行くと突き抜けてしまうので、
//     カメラの高さが hideBelowY より下にあるあいだは丸ごと非表示にする。
//
//   ※ エンジンの描画は Transform の親子を解決しないので、部位ごとにワールド姿勢を直接書く。
//     回転は v·Rx·Ry·Rz の順に掛かる。Y 軸の部品を Rx(a)→Rz(b) で傾けると
//     軸は (-cos a·sin b, cos a·cos b, sin a) を向く（TitleScreenScript のコメントと同じ式）。
// ============================================================================

/// @class TitleReefScript
/// @brief タイトル画面の水底（砂地・砂の盛り上がり・サンゴ礁）をプリミティブで生成する
/// @details JSON (scriptDataList) で設定できる項目:
///   "enabled"      : false で何も作らない
///   "seed"         : 並びを決める乱数の種（変えると別の並びになる）
///   "floorY"       : 砂地の高さ（m。水面は 0）
///   "floorSize"    : 砂地の一辺（m）
///   "sandColor"    : 砂の色 RGBA
///   "mounds"       : [[x, z, radius, height], ...] 砂の盛り上がり（浅瀬の明るい帯になる）
///   "clusters"     : [[x, z, radius, count], ...] サンゴの群生地
///   "scaleMin" / "scaleMax" : サンゴ 1 株の大きさの倍率
///   "maxTopY"      : サンゴの先端がこれより上に出ないようにする（水面の泡立ち判定 2m より深く）
///   "palette"      : [[r, g, b, a], ...] サンゴの色（ランダムに選ぶ）
///   "rockColor"    : 岩の色
///   "hideBelowY"   : カメラがこの高さより下にいるあいだは非表示（飛び込み・浮上の突き抜け防止）
class TitleReefScript : public ScriptableEntity {
public:
  bool enabled = true;
  int seed = 20260930;
  float floorY = -6.0f;
  float floorSize = 110.0f;
  RC::Vector4 sandColor = {0.95f, 0.88f, 0.70f, 1.0f};
  std::vector<RC::Vector4> mounds = {
      {-11.0f, -1.0f, 8.0f, 2.2f},
      {12.0f, 3.0f, 7.0f, 1.8f},
      {1.0f, 10.0f, 9.0f, 1.5f},
  };
  std::vector<RC::Vector4> clusters = {
      {-15.0f, 6.0f, 4.0f, 7.0f},  {14.5f, -5.5f, 4.5f, 8.0f}, {-14.0f, -6.5f, 4.0f, 7.0f},
      {13.0f, 7.5f, 3.5f, 6.0f},   {-3.0f, 8.5f, 3.0f, 4.0f},  {7.0f, -0.5f, 2.5f, 4.0f},
      {-7.5f, 0.5f, 2.5f, 4.0f},   {21.0f, 1.0f, 3.0f, 5.0f},  {-21.0f, 0.0f, 3.0f, 5.0f},
  };
  float scaleMin = 0.8f;
  float scaleMax = 1.4f;
  float maxTopY = -2.6f;
  std::vector<RC::Vector4> palette = {
      {1.00f, 0.52f, 0.55f, 1.0f}, // コーラルピンク
      {1.00f, 0.66f, 0.30f, 1.0f}, // オレンジ
      {0.98f, 0.86f, 0.36f, 1.0f}, // 黄
      {0.66f, 0.46f, 0.92f, 1.0f}, // 紫
      {0.46f, 0.86f, 0.70f, 1.0f}, // ミント
      {0.95f, 0.40f, 0.70f, 1.0f}, // マゼンタ
  };
  RC::Vector4 rockColor = {0.42f, 0.40f, 0.36f, 1.0f};
  float hideBelowY = -2.0f;

  /// @brief 設定項目を JSON に書き出す
  /// @return 設定項目を格納した JSON（色・リストは数値配列で表す）
  nlohmann::json Serialize() override {
    // Vector4 を [x, y, z, w] 配列に、その配列のリストを二重配列に変換するヘルパー
    auto v4 = [](const RC::Vector4 &c) { return nlohmann::json{c.x, c.y, c.z, c.w}; };
    auto list = [&](const std::vector<RC::Vector4> &src) {
      nlohmann::json a = nlohmann::json::array();
      for (const auto &c : src) a.push_back(v4(c));
      return a;
    };
    return {
        {"enabled", enabled},
        {"seed", seed},
        {"floorY", floorY},
        {"floorSize", floorSize},
        {"sandColor", v4(sandColor)},
        {"mounds", list(mounds)},
        {"clusters", list(clusters)},
        {"scaleMin", scaleMin},
        {"scaleMax", scaleMax},
        {"maxTopY", maxTopY},
        {"palette", list(palette)},
        {"rockColor", v4(rockColor)},
        {"hideBelowY", hideBelowY},
    };
  }

  /// @brief JSON から設定項目を読み込む
  /// @param j 読み込み元の JSON（存在し型が合うキーだけ反映する）
  void Deserialize(const nlohmann::json &j) override {
    // キーが存在し型が一致する場合だけ上書きするヘルパー。
    // 色・座標は [x, y, z] または [x, y, z, w] を受け付け、w 省略時は 1 とする
    auto readF = [&](const char *key, float &out) {
      if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    auto toV4 = [](const nlohmann::json &c, RC::Vector4 &out) {
      if (!c.is_array() || c.size() < 3) return false;
      out.x = c[0].get<float>();
      out.y = c[1].get<float>();
      out.z = c[2].get<float>();
      out.w = c.size() >= 4 ? c[3].get<float>() : 1.0f;
      return true;
    };
    auto readV4 = [&](const char *key, RC::Vector4 &out) {
      if (j.contains(key)) toV4(j[key], out);
    };
    auto readList = [&](const char *key, std::vector<RC::Vector4> &out) {
      if (!j.contains(key) || !j[key].is_array()) return;
      out.clear();
      for (const auto &c : j[key]) {
        RC::Vector4 v{};
        if (toV4(c, v)) out.push_back(v);
      }
    };
    if (j.contains("enabled") && j["enabled"].is_boolean()) enabled = j["enabled"].get<bool>();
    if (j.contains("seed") && j["seed"].is_number()) seed = j["seed"].get<int>();
    readF("floorY", floorY);
    readF("floorSize", floorSize);
    readV4("sandColor", sandColor);
    readList("mounds", mounds);
    readList("clusters", clusters);
    readF("scaleMin", scaleMin);
    readF("scaleMax", scaleMax);
    // 最小・最大が逆に指定されていても乱数の範囲が壊れないよう入れ替える
    if (scaleMax < scaleMin) std::swap(scaleMin, scaleMax);
    readF("maxTopY", maxTopY);
    readList("palette", palette);
    readV4("rockColor", rockColor);
    readF("hideBelowY", hideBelowY);
  }

#if RC_ENABLE_IMGUI
  /// @brief インスペクタに部品数・表示状態と各パラメータの編集 UI、再生成ボタンを表示する
  void OnImGui() override {
    ImGui::Text("Parts: %d  Visible: %s", static_cast<int>(parts_.size()), shown_ ? "yes" : "no");
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::DragInt("Seed", &seed);
    ImGui::DragFloat("Floor Y", &floorY, 0.1f, -40.0f, -1.0f);
    ImGui::ColorEdit4("Sand Color", &sandColor.x);
    ImGui::DragFloat("Scale Min", &scaleMin, 0.01f, 0.1f, 5.0f);
    ImGui::DragFloat("Scale Max", &scaleMax, 0.01f, 0.1f, 5.0f);
    ImGui::DragFloat("Max Top Y", &maxTopY, 0.1f, -20.0f, 0.0f);
    ImGui::DragFloat("Hide Below Y", &hideBelowY, 0.1f, -40.0f, 0.0f);
    ImGui::TextDisabled("mounds / clusters / palette: JSON で編集");
    if (ImGui::Button("Rebuild")) {
      Clear();
      if (enabled) Build();
    }
  }
#endif

protected:
  /// @brief 有効なら砂地とサンゴ礁を一度だけ生成する
  void OnCreate() override {
    if (enabled) Build();
  }

  /// @brief カメラの高さに応じて水底の部品全体の表示・非表示を切り替える
  /// @param deltaTime 前フレームからの経過時間（秒）。このクラスでは使わない
  void OnUpdate(float /*deltaTime*/) override {
    if (parts_.empty()) return;
    // カメラが水底の近くまで潜ったら隠す（飛び込み・浮上でサンゴや砂地を突き抜けないように）
    const bool show = !CameraBelow(hideBelowY);
    if (show != shown_) SetShown(show);
  }

  /// @brief 生成した部品エンティティをすべて破棄する
  void OnDestroy() override { Clear(); }

private:
  /// @brief 生成した部品 1 つ分のエンティティへの弱参照を保持する
  struct Part {
    std::weak_ptr<Entity> entity;
  };

  static constexpr float kPi = 3.14159265f;
  static constexpr float kTwoPi = 6.28318531f;

  // ------------------------------------------------------------------
  // 生成
  // ------------------------------------------------------------------

  /// @brief 砂地・砂の盛り上がり・サンゴ群生地をまとめて生成する
  /// @note seed で乱数を初期化するので、同じ設定なら毎回同じ並びになる
  void Build() {
    Scene *scene = GetScene();
    if (!scene) return;
    // 部品をまとめるフォルダを作り、乱数を seed で初期化する
    auto folder = scene->CreateEntity("TitleReef");
    if (folder) {
      folder->SetIsFolder(true);
      folder->SetTag("transient", 1);
      folder_ = folder;
    }
    rng_.seed(static_cast<uint32_t>(seed));

    // 砂地（Plane は 10x10 で生成されるので scale で広げる）
    AddPart(scene, "ReefFloor", PrimitiveType::Plane, sandColor, {0.0f, floorY, 0.0f}, {0.0f, 0.0f, 0.0f},
            {floorSize / 10.0f, 1.0f, floorSize / 10.0f}, 4.0f);

    // 砂の盛り上がり：大きく潰した球を砂地に半分埋める
    for (size_t i = 0; i < mounds.size(); ++i) {
      const RC::Vector4 &m = mounds[i];
      AddPart(scene, "ReefMound_" + std::to_string(i), PrimitiveType::Sphere, Shade(sandColor, 0.97f),
              {m.x, floorY, m.y}, {0.0f, Rand(0.0f, kTwoPi), 0.0f}, {m.z, (std::max)(m.w, 0.1f), m.z * Rand(0.7f, 1.0f)},
              4.0f);
    }

    // サンゴの群生地
    for (size_t c = 0; c < clusters.size(); ++c) {
      const RC::Vector4 &cl = clusters[c];
      const int count = (std::max)(static_cast<int>(cl.w), 0);
      for (int i = 0; i < count; ++i) {
        // 中心寄りに密に置く（sqrt を取らないので中心が濃い）
        const float r = cl.z * Rand(0.0f, 1.0f);
        const float a = Rand(0.0f, kTwoPi);
        const float x = cl.x + std::cos(a) * r;
        const float z = cl.y + std::sin(a) * r;
        const float baseY = GroundY(x, z);
        const float s = Rand(scaleMin, scaleMax);
        const std::string tag = "Reef_" + std::to_string(c) + "_" + std::to_string(i);
        // 種類を確率で選ぶ（枝 30% / 脳 22% / テーブル 16% / チューブ 16% / 岩 16%）
        const float pick = Rand(0.0f, 1.0f);
        if (pick < 0.30f) BuildBranch(scene, tag, x, baseY, z, s);
        else if (pick < 0.52f) BuildBrain(scene, tag, x, baseY, z, s);
        else if (pick < 0.68f) BuildTable(scene, tag, x, baseY, z, s);
        else if (pick < 0.84f) BuildTube(scene, tag, x, baseY, z, s);
        else BuildRock(scene, tag, x, baseY, z, s);
      }
    }

    // 生成直後のカメラ位置に合わせて表示状態を決める
    shown_ = true;
    SetShown(!CameraBelow(hideBelowY));
    Log::Print("[TitleReefScript] built " + std::to_string(parts_.size()) + " parts");
  }

  /// @brief 枝サンゴ：根元から外へ開く円錐の束を生成する
  /// @param scene 生成先のシーン
  /// @param tag 部品エンティティ名の接頭辞
  /// @param x 根元のワールド X 座標
  /// @param y 根元のワールド Y 座標（地面の高さ）
  /// @param z 根元のワールド Z 座標
  /// @param s 大きさの倍率
  void BuildBranch(Scene *scene, const std::string &tag, float x, float y, float z, float s) {
    const RC::Vector4 col = PickColor();
    const int n = 4 + static_cast<int>(Rand(0.0f, 3.0f));
    for (int k = 0; k < n; ++k) {
      const float len = ClampLen(y, Rand(1.3f, 2.3f) * s);
      const float rad = Rand(0.14f, 0.22f) * s;
      const float dir = Rand(0.0f, kTwoPi);
      const float tilt = Rand(0.15f, 0.65f);
      AddTilted(scene, tag + "_b" + std::to_string(k), PrimitiveType::Cone, Shade(col, Rand(0.85f, 1.05f)),
                {x + Rand(-0.25f, 0.25f) * s, y, z + Rand(-0.25f, 0.25f) * s}, dir, tilt, len, rad);
    }
  }

  /// @brief 脳サンゴ：潰した球を生成する
  /// @param scene 生成先のシーン
  /// @param tag 部品エンティティ名の接頭辞
  /// @param x 根元のワールド X 座標
  /// @param y 根元のワールド Y 座標（地面の高さ）
  /// @param z 根元のワールド Z 座標
  /// @param s 大きさの倍率
  void BuildBrain(Scene *scene, const std::string &tag, float x, float y, float z, float s) {
    const float r = Rand(0.8f, 1.4f) * s;
    // 高さは maxTopY を超えないよう抑える（最低 0.2m は残す）
    const float h = (std::min)(r * Rand(0.55f, 0.75f), (std::max)(maxTopY - y, 0.2f));
    AddPart(scene, tag + "_brain", PrimitiveType::Sphere, PickColor(), {x, y, z}, {0.0f, Rand(0.0f, kTwoPi), 0.0f},
            {r, h, r * Rand(0.8f, 1.0f)}, 16.0f);
  }

  /// @brief テーブルサンゴ：細い柱＋平たい円盤を生成する
  /// @param scene 生成先のシーン
  /// @param tag 部品エンティティ名の接頭辞
  /// @param x 根元のワールド X 座標
  /// @param y 根元のワールド Y 座標（地面の高さ）
  /// @param z 根元のワールド Z 座標
  /// @param s 大きさの倍率
  void BuildTable(Scene *scene, const std::string &tag, float x, float y, float z, float s) {
    const RC::Vector4 col = PickColor();
    const float stem = ClampLen(y, Rand(0.6f, 1.1f) * s);
    const float r = Rand(1.0f, 1.7f) * s;
    AddPart(scene, tag + "_stem", PrimitiveType::Cylinder, Shade(col, 0.8f), {x, y + stem * 0.5f, z}, {0.0f, 0.0f, 0.0f},
            {0.18f * s, stem, 0.18f * s}, 8.0f);
    AddPart(scene, tag + "_top", PrimitiveType::Cylinder, col, {x, y + stem, z},
            {Rand(-0.08f, 0.08f), Rand(0.0f, kTwoPi), Rand(-0.08f, 0.08f)}, {r, 0.16f * s, r * Rand(0.75f, 1.0f)}, 8.0f);
  }

  /// @brief チューブ（カイメン）：立った円柱の束を生成する
  /// @param scene 生成先のシーン
  /// @param tag 部品エンティティ名の接頭辞
  /// @param x 根元のワールド X 座標
  /// @param y 根元のワールド Y 座標（地面の高さ）
  /// @param z 根元のワールド Z 座標
  /// @param s 大きさの倍率
  void BuildTube(Scene *scene, const std::string &tag, float x, float y, float z, float s) {
    const RC::Vector4 col = PickColor();
    const int n = 3 + static_cast<int>(Rand(0.0f, 3.0f));
    for (int k = 0; k < n; ++k) {
      const float len = ClampLen(y, Rand(0.7f, 1.6f) * s);
      const float rad = Rand(0.16f, 0.26f) * s;
      const float a = Rand(0.0f, kTwoPi);
      const float d = Rand(0.1f, 0.45f) * s;
      AddTilted(scene, tag + "_t" + std::to_string(k), PrimitiveType::Cylinder, Shade(col, Rand(0.85f, 1.05f)),
                {x + std::cos(a) * d, y, z + std::sin(a) * d}, a, Rand(0.0f, 0.25f), len, rad);
    }
  }

  /// @brief 岩：暗い色の潰した球か箱（40%）を生成する
  /// @param scene 生成先のシーン
  /// @param tag 部品エンティティ名の接頭辞
  /// @param x 根元のワールド X 座標
  /// @param y 根元のワールド Y 座標（地面の高さ）
  /// @param z 根元のワールド Z 座標
  /// @param s 大きさの倍率
  void BuildRock(Scene *scene, const std::string &tag, float x, float y, float z, float s) {
    const bool box = Rand(0.0f, 1.0f) < 0.4f;
    const float r = Rand(0.7f, 1.5f) * s;
    AddPart(scene, tag + "_rock", box ? PrimitiveType::Box : PrimitiveType::Sphere, Shade(rockColor, Rand(0.8f, 1.1f)),
            {x, y, z}, {Rand(-0.3f, 0.3f), Rand(0.0f, kTwoPi), Rand(-0.3f, 0.3f)},
            box ? RC::Vector3{r * 1.6f, r * 0.8f, r * 1.3f} : RC::Vector3{r, r * 0.6f, r * 0.85f}, 8.0f);
  }

  /// @brief Y 軸の部品（Cone / Cylinder。どちらも高さ 1・半径 1・中心が原点）を根元から傾けて置く
  /// @param scene 生成先のシーン
  /// @param name エンティティ名
  /// @param type プリミティブの種類（Cone / Cylinder を想定）
  /// @param color 色 RGBA
  /// @param base 根元のワールド座標
  /// @param dir 水平面上の傾ける向き（rad）
  /// @param tilt 鉛直からの傾き（rad）
  /// @param len 部品の長さ（Y 方向スケール）
  /// @param radius 部品の半径（X/Z 方向スケール）
  void AddTilted(Scene *scene, const std::string &name, PrimitiveType type, const RC::Vector4 &color,
                 const RC::Vector3 &base, float dir, float tilt, float len, float radius) {
    // 軸 = (-cos a·sin b, cos a·cos b, sin a)。水平成分が (cos dir, sin dir) を向くよう a, b を決める
    const float hx = std::cos(dir) * std::sin(tilt);
    const float hz = std::sin(dir) * std::sin(tilt);
    const float a = std::asin(std::clamp(hz, -1.0f, 1.0f));
    const float ca = (std::max)(std::cos(a), 1e-3f);
    const float b = std::asin(std::clamp(-hx / ca, -1.0f, 1.0f));
    // 部品の中心が原点なので、根元から軸方向へ長さの半分ずらした位置を中心にする
    const RC::Vector3 axis = {-ca * std::sin(b), ca * std::cos(b), std::sin(a)};
    const RC::Vector3 center = {base.x + axis.x * len * 0.5f, base.y + axis.y * len * 0.5f,
                                base.z + axis.z * len * 0.5f};
    AddPart(scene, name, type, color, center, {a, 0.0f, b}, {radius, len, radius}, 12.0f);
  }

  /// @brief プリミティブの部品エンティティを 1 つ生成し、フォルダ配下に登録する
  /// @param scene 生成先のシーン
  /// @param name エンティティ名
  /// @param type プリミティブの種類
  /// @param color 色 RGBA
  /// @param pos ワールド座標
  /// @param rot ワールド回転（オイラー角, rad）
  /// @param scale スケール
  /// @param shininess 鏡面反射の鋭さ
  void AddPart(Scene *scene, const std::string &name, PrimitiveType type, const RC::Vector4 &color,
               const RC::Vector3 &pos, const RC::Vector3 &rot, const RC::Vector3 &scale, float shininess) {
    // エンティティを作り、Transform と PrimitiveMesh を設定する
    auto e = scene->CreateEntity(name);
    if (!e) return;
    if (auto f = folder_.lock()) e->SetParentGuid(f->Guid());
    e->SetTag("transient", 1); // シーン保存に含めない
    auto &tr = e->AddComponent<TransformComponent>();
    tr.position = pos;
    tr.rotation = rot;
    tr.scale = scale;
    auto &pm = e->AddComponent<PrimitiveMeshComponent>();
    pm.type = type;
    pm.color = color;
    pm.shininess = shininess;
    // meshHandle は InitDynamicEntityRuntime が形状に合わせて作り、色もマテリアルへ反映する
    scene->InitDynamicEntityRuntime(*e);
    // 生成直後の 1 フレームだけ原点に見えるのを防ぐ
    if (pm.meshHandle >= 0) {
      if (auto *p = RC::GetPrimitiveMeshTransformPtr(pm.meshHandle)) *p = tr.ToTransform();
    }
    parts_.push_back({e});
  }

  /// @brief 生成した部品とフォルダのエンティティをすべて破棄する
  void Clear() {
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->Destroy();
    }
    parts_.clear();
    if (auto f = folder_.lock()) f->Destroy();
    folder_.reset();
  }

  /// @brief 全部品のアクティブ状態をまとめて切り替える
  /// @param show true で表示、false で非表示
  void SetShown(bool show) {
    shown_ = show;
    for (auto &p : parts_) {
      if (auto e = p.entity.lock()) e->SetActive(show);
    }
  }

  // ------------------------------------------------------------------
  // 補助
  // ------------------------------------------------------------------

  /// @brief メインカメラが指定の高さより下にいるかを調べる
  /// @param y 判定に使う高さ（ワールド Y）
  /// @return アクティブなメインカメラの Y 座標が y 未満なら true。カメラが無ければ false
  bool CameraBelow(float y) const {
    Scene *scene = GetScene();
    if (!scene) return false;
    for (const auto &e : scene->GetEntities()) {
      if (!e || !e->IsActive()) continue;
      auto *cam = e->GetComponent<CameraComponent>();
      if (!cam || !cam->isMain) continue;
      if (auto *tr = e->GetComponent<TransformComponent>()) return tr->position.y < y;
    }
    return false;
  }

  /// @brief 砂の盛り上がりを考えた地面の高さ（潰した球の上面）を求める
  /// @param x ワールド X 座標
  /// @param z ワールド Z 座標
  /// @return その地点の地面の Y 座標
  float GroundY(float x, float z) const {
    float y = floorY;
    // 楕円体の上面 h·sqrt(1 - d²) を盛り上がりごとに求め、最も高いものを採る
    for (const auto &m : mounds) {
      const float dx = x - m.x, dz = z - m.y;
      const float r = (std::max)(m.z, 0.01f);
      const float d2 = (dx * dx + dz * dz) / (r * r);
      if (d2 < 1.0f) y = (std::max)(y, floorY + m.w * std::sqrt(1.0f - d2) * 0.9f); // 少し埋める
    }
    return y;
  }

  /// @brief 先端が maxTopY を超えないよう長さを抑える
  /// @param baseY 根元の Y 座標
  /// @param len 希望する長さ
  /// @return 抑えた後の長さ（最低 0.2）
  float ClampLen(float baseY, float len) const { return (std::max)((std::min)(len, maxTopY - baseY), 0.2f); }

  /// @brief パレットからサンゴの色をランダムに 1 つ選ぶ
  /// @return 選んだ色。パレットが空なら薄いピンク
  RC::Vector4 PickColor() {
    if (palette.empty()) return {1.0f, 0.6f, 0.6f, 1.0f};
    const size_t i = static_cast<size_t>(Rand(0.0f, static_cast<float>(palette.size())));
    return palette[(std::min)(i, palette.size() - 1)];
  }

  /// @brief 色の明るさを係数倍する
  /// @param c 元の色 RGBA
  /// @param k RGB に掛ける係数
  /// @return RGB を k 倍して 1 で頭打ちにした色（アルファはそのまま）
  static RC::Vector4 Shade(const RC::Vector4 &c, float k) {
    return {(std::min)(c.x * k, 1.0f), (std::min)(c.y * k, 1.0f), (std::min)(c.z * k, 1.0f), c.w};
  }

  /// @brief seed 付き乱数で一様分布の実数を返す
  /// @param lo 下限
  /// @param hi 上限
  /// @return [lo, hi) の乱数
  float Rand(float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(rng_);
  }

  std::vector<Part> parts_;
  std::weak_ptr<Entity> folder_;
  std::mt19937 rng_;
  bool shown_ = true;
};

REGISTER_SCRIPT(TitleReefScript)
