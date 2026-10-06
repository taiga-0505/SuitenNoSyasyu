#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "ECS/LightComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/CameraComponent.h"
#include "Graphics/PostProcess/PostProcess.h"
#include "Audio/AudioEngine.h"
#include "Common/Math/MathUtils.h"
#include "Render/Systems/RenderInteractiveWater.h"
#include "RenderCommon.h"
#include "Engine/Render/RenderContext.h"
#include "Framework/App.h"
#include "Game/Framework/WaterCameraFx.h"
#include "Scene.h"

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// タイトル画面：時間帯（昼 → 夕方 → 夜 → 明け方）と天候（雨・雪・雷雨）
//
//   - Title.json の空エンティティに NativeScript として 1 つ付けるだけで動く。
//     他のタイトル用スクリプトは参照しない（シーン内の Directional Light / Water / Skybox /
//     ColorGrade の値を毎フレーム上書きするだけ）。
//   - 時間帯：cycleSeconds で 1 周。schedule のキー（0〜1 の位置と look 名）の間を smoothstep で補間する。
//     look ごとに 平行光源（色・向き・強さ・環境光）／水面の色／空の乗算色／カラーグレード を持つ。
//   - 天候：晴れのあいだ clearMin〜clearMax 秒待ってから、weights の比率で 雨／雪／雷雨 を抽選。
//     fadeIn 秒かけて強まり、holdMin〜holdMax 秒続いて、fadeOut 秒で止む。
//     天候ごとの「暗さ・彩度・波の高さ」は rainLook / snowLook / stormLook で時間帯の上に掛ける。
//   - 雨・雪の粒は CPU で 3D 位置を動かし、毎フレーム画面へ投影して DrawLine / DrawCircle で描く。
//     真上から見下ろすカメラなので、雨は画面中央（消失点）へ向かう短い筋になる（透視として正しい）。
//     粒は「画面上のランダムな点 × 奥行き（d² 分布）」で生まれるので、全部が画面内に収まり無駄が無い。
//   - 雨粒が水面に届いたら RC::AddWaveSourceAtWorld で波紋を立てる（全体で 64/フレームの共有枠なので
//     rippleMaxPerFrame で上限を絞っている）。
//   - 雷：雷雨の強さが lightning.minLevel を超えているあいだ、ランダムな間隔で 2〜3 回の明滅
//     （画面全体の白い DrawBox ＋ 平行光源の一時的な増光）。thunderSoundPath があれば遅れて鳴らす。
//   - 再生中だけ書き換える。編集中に書き換えると、その値がシーンの保存で JSON に焼き付くため
//     （CaveLightZoneScript と同じ方針）。再生を止めたとき・破棄時に元の値へ戻す。
//   - カメラが水中に入ったら（飛び込み演出）雨・雪・稲光の描画は消す。
// ============================================================================

namespace {

RC::Vector3 V3(float x, float y, float z) { return RC::Vector3{x, y, z}; }
RC::Vector4 V4(float x, float y, float z, float w) { return RC::Vector4{x, y, z, w}; }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }
RC::Vector3 Lerp3(const RC::Vector3 &a, const RC::Vector3 &b, float t) {
  return V3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
}
RC::Vector4 Lerp4(const RC::Vector4 &a, const RC::Vector4 &b, float t) {
  return V4(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t));
}
float Smooth01(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
RC::Vector3 Normalize3(const RC::Vector3 &v, const RC::Vector3 &fallback) {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len < 1e-6f) return fallback;
  return V3(v.x / len, v.y / len, v.z / len);
}
/// @brief 色を灰色（輝度）へ寄せる。amount = 1 で完全に灰色
RC::Vector3 Desaturate(const RC::Vector3 &c, float amount) {
  const float l = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
  return Lerp3(c, V3(l, l, l), std::clamp(amount, 0.0f, 1.0f));
}

nlohmann::json J3(const RC::Vector3 &v) { return {v.x, v.y, v.z}; }
nlohmann::json J4(const RC::Vector4 &v) { return {v.x, v.y, v.z, v.w}; }

void ReadF(const nlohmann::json &j, const char *key, float &out) {
  if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
}
void ReadI(const nlohmann::json &j, const char *key, int &out) {
  if (j.contains(key) && j[key].is_number()) out = j[key].get<int>();
}
void ReadB(const nlohmann::json &j, const char *key, bool &out) {
  if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
}
void ReadS(const nlohmann::json &j, const char *key, std::string &out) {
  if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}
void ReadV3(const nlohmann::json &j, const char *key, RC::Vector3 &out) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return;
  const auto &a = j[key];
  out = V3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}
void ReadV4(const nlohmann::json &j, const char *key, RC::Vector4 &out) {
  if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return;
  const auto &a = j[key];
  out = V4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>());
}

/// @brief 行ベクトル × 行列（w も返す）
void TransformPoint(const RC::Vector3 &p, const RC::Matrix4x4 &m, float &x, float &y, float &z, float &w) {
  x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
  y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
  z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
  w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
}

} // namespace

/// @class TitleWeatherScript
/// @brief タイトル画面の時間帯サイクルとランダムな天候（雨・雪・雷雨）
class TitleWeatherScript : public ScriptableEntity {
public:
  // ------------------------------------------------------------------
  // 設定
  // ------------------------------------------------------------------

  /// @brief 時間帯 1 つぶんの見た目
  struct Look {
    RC::Vector4 lightColor{1.0f, 1.0f, 1.0f, 1.0f};
    RC::Vector3 lightDirection{0.0f, -1.0f, 0.0f};
    float lightIntensity = 1.0f;
    RC::Vector3 ambientColor{1.0f, 1.0f, 1.0f};
    float ambientIntensity = 0.0f;
    RC::Vector4 waterShallow{0.10f, 0.66f, 0.56f, 0.90f};
    RC::Vector4 waterDeep{0.02f, 0.26f, 0.36f, 0.96f};
    RC::Vector4 waterSss{0.10f, 0.62f, 0.52f, 0.9f};
    float waterEnvironment = 0.35f;
    float waterCrestTint = 0.55f;
    RC::Vector4 skyColor{1.0f, 1.0f, 1.0f, 1.0f};
    float gradeExposure = 0.1f;
    float gradeContrast = 1.05f;
    float gradeSaturation = 1.12f;
    float gradeTemperature = 0.0f;
    float gradeTint = 0.0f;

    nlohmann::json ToJson() const {
      return {{"lightColor", J4(lightColor)},
              {"lightDirection", J3(lightDirection)},
              {"lightIntensity", lightIntensity},
              {"ambientColor", J3(ambientColor)},
              {"ambientIntensity", ambientIntensity},
              {"waterShallow", J4(waterShallow)},
              {"waterDeep", J4(waterDeep)},
              {"waterSss", J4(waterSss)},
              {"waterEnvironment", waterEnvironment},
              {"waterCrestTint", waterCrestTint},
              {"skyColor", J4(skyColor)},
              {"gradeExposure", gradeExposure},
              {"gradeContrast", gradeContrast},
              {"gradeSaturation", gradeSaturation},
              {"gradeTemperature", gradeTemperature},
              {"gradeTint", gradeTint}};
    }
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      ReadV4(j, "lightColor", lightColor);
      ReadV3(j, "lightDirection", lightDirection);
      ReadF(j, "lightIntensity", lightIntensity);
      ReadV3(j, "ambientColor", ambientColor);
      ReadF(j, "ambientIntensity", ambientIntensity);
      ReadV4(j, "waterShallow", waterShallow);
      ReadV4(j, "waterDeep", waterDeep);
      ReadV4(j, "waterSss", waterSss);
      ReadF(j, "waterEnvironment", waterEnvironment);
      ReadF(j, "waterCrestTint", waterCrestTint);
      ReadV4(j, "skyColor", skyColor);
      ReadF(j, "gradeExposure", gradeExposure);
      ReadF(j, "gradeContrast", gradeContrast);
      ReadF(j, "gradeSaturation", gradeSaturation);
      ReadF(j, "gradeTemperature", gradeTemperature);
      ReadF(j, "gradeTint", gradeTint);
    }
    static Look Mix(const Look &a, const Look &b, float t) {
      Look r;
      r.lightColor = Lerp4(a.lightColor, b.lightColor, t);
      r.lightDirection = Normalize3(Lerp3(a.lightDirection, b.lightDirection, t), V3(0.0f, -1.0f, 0.0f));
      r.lightIntensity = Lerp(a.lightIntensity, b.lightIntensity, t);
      r.ambientColor = Lerp3(a.ambientColor, b.ambientColor, t);
      r.ambientIntensity = Lerp(a.ambientIntensity, b.ambientIntensity, t);
      r.waterShallow = Lerp4(a.waterShallow, b.waterShallow, t);
      r.waterDeep = Lerp4(a.waterDeep, b.waterDeep, t);
      r.waterSss = Lerp4(a.waterSss, b.waterSss, t);
      r.waterEnvironment = Lerp(a.waterEnvironment, b.waterEnvironment, t);
      r.waterCrestTint = Lerp(a.waterCrestTint, b.waterCrestTint, t);
      r.skyColor = Lerp4(a.skyColor, b.skyColor, t);
      r.gradeExposure = Lerp(a.gradeExposure, b.gradeExposure, t);
      r.gradeContrast = Lerp(a.gradeContrast, b.gradeContrast, t);
      r.gradeSaturation = Lerp(a.gradeSaturation, b.gradeSaturation, t);
      r.gradeTemperature = Lerp(a.gradeTemperature, b.gradeTemperature, t);
      r.gradeTint = Lerp(a.gradeTint, b.gradeTint, t);
      return r;
    }
  };

  /// @brief 天候が時間帯の見た目に掛ける補正（強さ 0 → 1 で無補正 → 満額）
  struct WeatherMod {
    float lightScale = 1.0f;      ///< 平行光源の強さに掛ける
    float lightDesaturate = 0.0f; ///< 光の色・環境光の色を灰色へ寄せる量
    float ambientAdd = 0.0f;      ///< 環境光の強さに足す（曇天の拡散光）
    float exposureAdd = 0.0f;
    float saturationScale = 1.0f;
    float contrastAdd = 0.0f;
    float temperatureAdd = 0.0f;
    float waterScale = 1.0f;      ///< 水面の色（rgb）に掛ける
    float skyScale = 1.0f;        ///< 空の乗算色（rgb）に掛ける
    float waveScale = 1.0f;       ///< 波の高さに掛ける（シーン初期値に対して）
    float whitecapAdd = 0.0f;     ///< 白波の量に足す
    RC::Vector4 haze{1.0f, 1.0f, 1.0f, 0.0f}; ///< 画面全体に薄く掛ける空気の色（a = 濃さ）。雪の白い霞など

    nlohmann::json ToJson() const {
      return {{"lightScale", lightScale},       {"lightDesaturate", lightDesaturate},
              {"ambientAdd", ambientAdd},       {"exposureAdd", exposureAdd},
              {"saturationScale", saturationScale}, {"contrastAdd", contrastAdd},
              {"temperatureAdd", temperatureAdd}, {"waterScale", waterScale},
              {"skyScale", skyScale},           {"waveScale", waveScale},
              {"whitecapAdd", whitecapAdd}, {"haze", J4(haze)}};
    }
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      ReadF(j, "lightScale", lightScale);
      ReadF(j, "lightDesaturate", lightDesaturate);
      ReadF(j, "ambientAdd", ambientAdd);
      ReadF(j, "exposureAdd", exposureAdd);
      ReadF(j, "saturationScale", saturationScale);
      ReadF(j, "contrastAdd", contrastAdd);
      ReadF(j, "temperatureAdd", temperatureAdd);
      ReadF(j, "waterScale", waterScale);
      ReadF(j, "skyScale", skyScale);
      ReadF(j, "waveScale", waveScale);
      ReadF(j, "whitecapAdd", whitecapAdd);
      ReadV4(j, "haze", haze);
    }
  };

  struct RainParams {
    int maxDrops = 300;           ///< 雷雨（強さ 1）のときの粒数。普通の雨は amount 倍
    float amount = 0.6f;          ///< 普通の雨の粒数の割合（雷雨 = 1）
    float speed = 16.0f;          ///< 落下速度 m/s（見栄えのため実際より速め）
    /// 横風 m/s（雷雨は stormWindScale 倍）。
    /// 真上視点では雨筋は「落下方向の消失点」= 画面中心 + f*(wind.x, wind.z)/speed へ向かって収束して見える。
    /// 横風が弱いと消失点が画面内（タイトル文字やメニューの上）に来て「そこへ雨が落ちている」ように見えるので、
    /// |wind.z| / speed を 0.5 以上にして消失点を画面の下側の外へ追い出している（画面上方向 = +z）。
    RC::Vector3 wind{-2.0f, 0.0f, -9.0f};
    float stormWindScale = 1.35f;
    float streakTime = 0.035f;    ///< 筋の長さ（何秒ぶんの移動量を線にするか）
    float nearDepth = 3.0f;       ///< カメラからこの距離より手前には生まない
    RC::Vector4 color{0.78f, 0.86f, 0.95f, 0.38f};
    float thickness = 1.3f;
    bool ripples = true;
    int rippleMaxPerFrame = 16;   ///< 波紋の上限（シミュレーション全体で 64/フレームの共有枠）
    float rippleChance = 0.6f;    ///< 着水した粒が波紋を立てる確率
    float rippleRadius = 0.010f;  ///< UV（0.01 = 1m）
    float rippleStrength = 0.05f; ///< 押し下げる高さ m
    /// 着水点に 2D の小さな輪を広げる。2D なので奥行きで隠れず、文字の上にも輪が出てしまうため既定は off
    /// （着水の表現は水面の波紋シミュレーション側だけで行う。こちらは文字に正しく隠れる）
    bool rings = false;
    int ringMax = 48;
    float ringSize = 0.35f;       ///< 輪の最大半径 m
    float ringLife = 0.35f;
    RC::Vector4 ringColor{0.85f, 0.92f, 1.0f, 0.45f};

    nlohmann::json ToJson() const {
      return {{"maxDrops", maxDrops},       {"amount", amount},
              {"speed", speed},             {"wind", J3(wind)},
              {"stormWindScale", stormWindScale}, {"streakTime", streakTime},
              {"nearDepth", nearDepth},     {"color", J4(color)},
              {"thickness", thickness},     {"ripples", ripples},
              {"rippleMaxPerFrame", rippleMaxPerFrame}, {"rippleChance", rippleChance},
              {"rippleRadius", rippleRadius}, {"rippleStrength", rippleStrength},
              {"rings", rings},             {"ringMax", ringMax},
              {"ringSize", ringSize},       {"ringLife", ringLife},
              {"ringColor", J4(ringColor)}};
    }
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      ReadI(j, "maxDrops", maxDrops);
      ReadF(j, "amount", amount);
      ReadF(j, "speed", speed);
      ReadV3(j, "wind", wind);
      ReadF(j, "stormWindScale", stormWindScale);
      ReadF(j, "streakTime", streakTime);
      ReadF(j, "nearDepth", nearDepth);
      ReadV4(j, "color", color);
      ReadF(j, "thickness", thickness);
      ReadB(j, "ripples", ripples);
      ReadI(j, "rippleMaxPerFrame", rippleMaxPerFrame);
      ReadF(j, "rippleChance", rippleChance);
      ReadF(j, "rippleRadius", rippleRadius);
      ReadF(j, "rippleStrength", rippleStrength);
      ReadB(j, "rings", rings);
      ReadI(j, "ringMax", ringMax);
      ReadF(j, "ringSize", ringSize);
      ReadF(j, "ringLife", ringLife);
      ReadV4(j, "ringColor", ringColor);
      maxDrops = std::clamp(maxDrops, 0, 1500);
      ringMax = std::clamp(ringMax, 0, 256);
      rippleMaxPerFrame = std::clamp(rippleMaxPerFrame, 0, 32);
    }
  };

  struct SnowParams {
    int maxFlakes = 200;
    float fallSpeedMin = 0.9f;    ///< m/s
    float fallSpeedMax = 1.6f;
    RC::Vector3 wind{-0.15f, 0.0f, -0.55f}; ///< 雨と同じく消失点を画面の下側の外へ
    float swayAmplitude = 0.5f;   ///< 左右の揺れ m/s
    float swayPeriodMin = 2.0f;
    float swayPeriodMax = 4.0f;
    float sizeMin = 0.02f;        ///< 雪片の半径 m（小さいものほど多くなる分布で選ぶ）
    float sizeMax = 0.06f;
    float maxPixelRadius = 4.0f;  ///< これより大きく映る（カメラ直近の）雪片は、ぼけた大きな玉にする
    float nearDepth = 2.0f;
    float meltTime = 0.3f;        ///< 着水してから消えるまで
    float flutter = 0.35f;        ///< ひらひら（細かく速い揺れ）の速さ m/s
    float farFade = 0.6f;         ///< 水面近く（遠く）の雪片の濃さ（手前 = 1）
    float haloAlpha = 0.0f;       ///< 雪片のまわりの淡い光の濃さ（0 で無し）
    float bokehMaxPixel = 8.0f;   ///< カメラ直近のぼけ玉の最大半径 px
    float bokehAlpha = 0.18f;     ///< ぼけ玉の濃さ
    RC::Vector4 color{1.0f, 1.0f, 1.0f, 0.9f};

    nlohmann::json ToJson() const {
      return {{"maxFlakes", maxFlakes},       {"fallSpeedMin", fallSpeedMin},
              {"fallSpeedMax", fallSpeedMax}, {"wind", J3(wind)},
              {"swayAmplitude", swayAmplitude}, {"swayPeriodMin", swayPeriodMin},
              {"swayPeriodMax", swayPeriodMax}, {"sizeMin", sizeMin},
              {"sizeMax", sizeMax},           {"maxPixelRadius", maxPixelRadius},
              {"nearDepth", nearDepth},       {"meltTime", meltTime},
              {"flutter", flutter},           {"farFade", farFade},
              {"haloAlpha", haloAlpha},       {"bokehMaxPixel", bokehMaxPixel},
              {"bokehAlpha", bokehAlpha},     {"color", J4(color)}};
    }
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      ReadI(j, "maxFlakes", maxFlakes);
      ReadF(j, "fallSpeedMin", fallSpeedMin);
      ReadF(j, "fallSpeedMax", fallSpeedMax);
      ReadV3(j, "wind", wind);
      ReadF(j, "swayAmplitude", swayAmplitude);
      ReadF(j, "swayPeriodMin", swayPeriodMin);
      ReadF(j, "swayPeriodMax", swayPeriodMax);
      ReadF(j, "sizeMin", sizeMin);
      ReadF(j, "sizeMax", sizeMax);
      ReadF(j, "maxPixelRadius", maxPixelRadius);
      ReadF(j, "nearDepth", nearDepth);
      ReadF(j, "meltTime", meltTime);
      ReadF(j, "flutter", flutter);
      ReadF(j, "farFade", farFade);
      ReadF(j, "haloAlpha", haloAlpha);
      ReadF(j, "bokehMaxPixel", bokehMaxPixel);
      ReadF(j, "bokehAlpha", bokehAlpha);
      ReadV4(j, "color", color);
      maxFlakes = std::clamp(maxFlakes, 0, 1500);
    }
  };

  struct LightningParams {
    bool enabled = true;
    float minLevel = 0.6f;        ///< 雷雨の強さがこれを超えているあいだだけ落ちる
    float intervalMin = 4.0f;
    float intervalMax = 11.0f;
    float screenAlpha = 0.55f;    ///< 画面全体を白くする量
    RC::Vector4 flashColor{0.86f, 0.90f, 1.0f, 1.0f};
    float lightBoost = 2.2f;      ///< 平行光源の強さに足す量
    float ambientBoost = 0.7f;    ///< 環境光に足す量
    float thunderDelayMin = 0.5f; ///< 光ってから雷鳴までの遅れ
    float thunderDelayMax = 1.8f;

    nlohmann::json ToJson() const {
      return {{"enabled", enabled},         {"minLevel", minLevel},
              {"intervalMin", intervalMin}, {"intervalMax", intervalMax},
              {"screenAlpha", screenAlpha}, {"flashColor", J4(flashColor)},
              {"lightBoost", lightBoost},   {"ambientBoost", ambientBoost},
              {"thunderDelayMin", thunderDelayMin}, {"thunderDelayMax", thunderDelayMax}};
    }
    void FromJson(const nlohmann::json &j) {
      if (!j.is_object()) return;
      ReadB(j, "enabled", enabled);
      ReadF(j, "minLevel", minLevel);
      ReadF(j, "intervalMin", intervalMin);
      ReadF(j, "intervalMax", intervalMax);
      ReadF(j, "screenAlpha", screenAlpha);
      ReadV4(j, "flashColor", flashColor);
      ReadF(j, "lightBoost", lightBoost);
      ReadF(j, "ambientBoost", ambientBoost);
      ReadF(j, "thunderDelayMin", thunderDelayMin);
      ReadF(j, "thunderDelayMax", thunderDelayMax);
    }
  };

  enum LookId { kDay = 0, kEvening, kNight, kDawn, kLookCount };
  enum class Weather { Clear = 0, Rain, Snow, Storm };

  struct ScheduleKey {
    float t = 0.0f; ///< 0〜1（1 周の中の位置）
    int look = kDay;
  };

  bool enabled = true;
  bool timeEnabled = true;
  float cycleSeconds = 180.0f; ///< 1 周（昼 → 夕 → 夜 → 明け方 → 昼）の秒数
  float startPhase = 0.0f;     ///< シーン開始時の位置（0〜1）
  std::array<Look, kLookCount> looks = DefaultLooks();
  std::vector<ScheduleKey> schedule = DefaultSchedule();

  bool weatherEnabled = true;
  float firstDelayMin = 25.0f; ///< シーン開始から最初の天候までの待ち
  float firstDelayMax = 45.0f;
  float clearMin = 30.0f;      ///< 天候が止んでから次が始まるまでの晴れ
  float clearMax = 60.0f;
  float holdMin = 25.0f;       ///< 天候が満額で続く長さ
  float holdMax = 45.0f;
  float fadeIn = 7.0f;
  float fadeOut = 9.0f;
  float weightRain = 1.0f;
  float weightSnow = 0.7f;
  float weightStorm = 0.5f;
  // 並び：lightScale, lightDesaturate, ambientAdd, exposureAdd, saturationScale, contrastAdd, temperatureAdd,
  //       waterScale, skyScale, waveScale, whitecapAdd, haze
  // 暗くする補正（lightScale / exposureAdd / waterScale）は、時間帯がもともと暗いほど弱めて掛ける（ApplyToScene）
  WeatherMod rainLook{0.65f, 0.6f, 0.10f, -0.12f, 0.7f, -0.04f, -0.08f, 0.85f, 0.6f, 1.15f, 0.05f,
                      {0.80f, 0.85f, 0.90f, 0.04f}};
  // 雪：鉛色の海（水を暗く・彩度を落とす）に白い雪片が映えるように。空気は白く霞ませる
  WeatherMod snowLook{0.75f, 0.85f, 0.18f, 0.05f, 0.45f, -0.04f, -0.22f, 0.7f, 0.85f, 0.7f, -0.2f,
                      {0.86f, 0.90f, 0.97f, 0.12f}};
  // 雷雨：暗く荒れた海。ただし環境光を足して、文字と水面が黒く潰れないように
  WeatherMod stormLook{0.5f, 0.7f, 0.16f, -0.18f, 0.6f, 0.0f, -0.10f, 0.75f, 0.45f, 1.35f, 0.2f,
                       {0.55f, 0.60f, 0.68f, 0.05f}};
  RainParams rain;
  SnowParams snow;
  LightningParams lightning;

  std::string rainSoundPath;    ///< ループする雨音（空なら鳴らさない）
  float rainVolume = 0.5f;
  std::string thunderSoundPath; ///< 雷鳴（空なら鳴らさない）
  float thunderVolume = 0.8f;

  // ------------------------------------------------------------------
  // Serialize / Deserialize
  // ------------------------------------------------------------------

  nlohmann::json Serialize() override {
    nlohmann::json sched = nlohmann::json::array();
    for (const auto &k : schedule) sched.push_back({k.t, LookName(k.look)});
    nlohmann::json lk = nlohmann::json::object();
    for (int i = 0; i < kLookCount; ++i) lk[LookName(i)] = looks[i].ToJson();
    return {{"enabled", enabled},
            {"timeEnabled", timeEnabled},
            {"cycleSeconds", cycleSeconds},
            {"startPhase", startPhase},
            {"looks", lk},
            {"schedule", sched},
            {"weather",
             {{"enabled", weatherEnabled},
              {"firstDelayMin", firstDelayMin},
              {"firstDelayMax", firstDelayMax},
              {"clearMin", clearMin},
              {"clearMax", clearMax},
              {"holdMin", holdMin},
              {"holdMax", holdMax},
              {"fadeIn", fadeIn},
              {"fadeOut", fadeOut},
              {"weights", {{"rain", weightRain}, {"snow", weightSnow}, {"storm", weightStorm}}}}},
            {"rainLook", rainLook.ToJson()},
            {"snowLook", snowLook.ToJson()},
            {"stormLook", stormLook.ToJson()},
            {"rain", rain.ToJson()},
            {"snow", snow.ToJson()},
            {"lightning", lightning.ToJson()},
            {"rainSoundPath", rainSoundPath},
            {"rainVolume", rainVolume},
            {"thunderSoundPath", thunderSoundPath},
            {"thunderVolume", thunderVolume}};
  }

  void Deserialize(const nlohmann::json &j) override {
    ReadB(j, "enabled", enabled);
    ReadB(j, "timeEnabled", timeEnabled);
    ReadF(j, "cycleSeconds", cycleSeconds);
    cycleSeconds = (std::max)(cycleSeconds, 1.0f);
    ReadF(j, "startPhase", startPhase);
    if (j.contains("looks") && j["looks"].is_object()) {
      for (int i = 0; i < kLookCount; ++i) {
        const char *name = LookName(i);
        if (j["looks"].contains(name)) looks[i].FromJson(j["looks"][name]);
      }
    }
    if (j.contains("schedule") && j["schedule"].is_array()) {
      std::vector<ScheduleKey> keys;
      for (const auto &k : j["schedule"]) {
        if (!k.is_array() || k.size() < 2 || !k[0].is_number() || !k[1].is_string()) continue;
        const int id = LookIdFromName(k[1].get<std::string>());
        if (id < 0) continue;
        keys.push_back({std::clamp(k[0].get<float>(), 0.0f, 1.0f), id});
      }
      if (!keys.empty()) {
        std::sort(keys.begin(), keys.end(), [](const ScheduleKey &a, const ScheduleKey &b) { return a.t < b.t; });
        schedule = std::move(keys);
      }
    }
    if (j.contains("weather") && j["weather"].is_object()) {
      const auto &w = j["weather"];
      ReadB(w, "enabled", weatherEnabled);
      ReadF(w, "firstDelayMin", firstDelayMin);
      ReadF(w, "firstDelayMax", firstDelayMax);
      ReadF(w, "clearMin", clearMin);
      ReadF(w, "clearMax", clearMax);
      ReadF(w, "holdMin", holdMin);
      ReadF(w, "holdMax", holdMax);
      ReadF(w, "fadeIn", fadeIn);
      ReadF(w, "fadeOut", fadeOut);
      if (w.contains("weights") && w["weights"].is_object()) {
        ReadF(w["weights"], "rain", weightRain);
        ReadF(w["weights"], "snow", weightSnow);
        ReadF(w["weights"], "storm", weightStorm);
      }
    }
    if (j.contains("rainLook")) rainLook.FromJson(j["rainLook"]);
    if (j.contains("snowLook")) snowLook.FromJson(j["snowLook"]);
    if (j.contains("stormLook")) stormLook.FromJson(j["stormLook"]);
    if (j.contains("rain")) rain.FromJson(j["rain"]);
    if (j.contains("snow")) snow.FromJson(j["snow"]);
    if (j.contains("lightning")) lightning.FromJson(j["lightning"]);
    ReadS(j, "rainSoundPath", rainSoundPath);
    ReadF(j, "rainVolume", rainVolume);
    ReadS(j, "thunderSoundPath", thunderSoundPath);
    ReadF(j, "thunderVolume", thunderVolume);
  }

  // ------------------------------------------------------------------
  // ImGui
  // ------------------------------------------------------------------

#if RC_ENABLE_IMGUI
  void OnImGui() override {
    ImGui::Checkbox("Enabled", &enabled);
    ImGui::Text("phase %.3f (%s)  weather: %s  level %.2f  drops %d  flakes %d", phase_, PhaseLabel(phase_),
                WeatherName(weather_), level_, activeDrops_, activeFlakes_);

    ImGui::SeparatorText("Time of Day");
    ImGui::Checkbox("Time Enabled", &timeEnabled);
    ImGui::DragFloat("Cycle Seconds", &cycleSeconds, 1.0f, 5.0f, 3600.0f);
    ImGui::SliderFloat("Phase", &phase_, 0.0f, 0.9999f);
    ImGui::SameLine();
    ImGui::TextDisabled("(drag to preview)");
    if (ImGui::Button("Day")) phase_ = 0.10f;
    ImGui::SameLine();
    if (ImGui::Button("Evening")) phase_ = 0.46f;
    ImGui::SameLine();
    if (ImGui::Button("Night")) phase_ = 0.70f;
    ImGui::SameLine();
    if (ImGui::Button("Dawn")) phase_ = 0.905f;
    ImGui::DragFloat("Start Phase", &startPhase, 0.005f, 0.0f, 1.0f);

    for (int i = 0; i < kLookCount; ++i) {
      if (ImGui::TreeNode(LookName(i))) {
        Look &l = looks[i];
        ImGui::ColorEdit4("Light Color", &l.lightColor.x);
        ImGui::DragFloat3("Light Direction", &l.lightDirection.x, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat("Light Intensity", &l.lightIntensity, 0.01f, 0.0f, 4.0f);
        ImGui::ColorEdit3("Ambient Color", &l.ambientColor.x);
        ImGui::DragFloat("Ambient Intensity", &l.ambientIntensity, 0.01f, 0.0f, 2.0f);
        ImGui::ColorEdit4("Water Shallow", &l.waterShallow.x);
        ImGui::ColorEdit4("Water Deep", &l.waterDeep.x);
        ImGui::ColorEdit4("Water SSS", &l.waterSss.x);
        ImGui::DragFloat("Water Environment", &l.waterEnvironment, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Water Crest Tint", &l.waterCrestTint, 0.01f, 0.0f, 2.0f);
        ImGui::ColorEdit4("Sky Color", &l.skyColor.x);
        ImGui::DragFloat("Exposure", &l.gradeExposure, 0.01f, -3.0f, 3.0f);
        ImGui::DragFloat("Contrast", &l.gradeContrast, 0.01f, 0.0f, 3.0f);
        ImGui::DragFloat("Saturation", &l.gradeSaturation, 0.01f, 0.0f, 3.0f);
        ImGui::DragFloat("Temperature", &l.gradeTemperature, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat("Tint", &l.gradeTint, 0.01f, -1.0f, 1.0f);
        ImGui::TreePop();
      }
    }

    ImGui::SeparatorText("Weather");
    ImGui::Checkbox("Weather Enabled (random)", &weatherEnabled);
    ImGui::Text("state %s  timer %.1f s", StateName(state_), stateTimer_);
    if (ImGui::Button("Clear")) ForceWeather(Weather::Clear);
    ImGui::SameLine();
    if (ImGui::Button("Rain")) ForceWeather(Weather::Rain);
    ImGui::SameLine();
    if (ImGui::Button("Snow")) ForceWeather(Weather::Snow);
    ImGui::SameLine();
    if (ImGui::Button("Storm")) ForceWeather(Weather::Storm);
    ImGui::SameLine();
    if (ImGui::Button("Strike")) StrikeLightning();
    ImGui::DragFloatRange2("First Delay", &firstDelayMin, &firstDelayMax, 0.5f, 0.0f, 600.0f);
    ImGui::DragFloatRange2("Clear", &clearMin, &clearMax, 0.5f, 0.0f, 600.0f);
    ImGui::DragFloatRange2("Hold", &holdMin, &holdMax, 0.5f, 0.0f, 600.0f);
    ImGui::DragFloat("Fade In", &fadeIn, 0.1f, 0.1f, 60.0f);
    ImGui::DragFloat("Fade Out", &fadeOut, 0.1f, 0.1f, 60.0f);
    ImGui::DragFloat("Weight Rain", &weightRain, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Weight Snow", &weightSnow, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Weight Storm", &weightStorm, 0.01f, 0.0f, 10.0f);

    auto editMod = [](const char *label, WeatherMod &m) {
      if (!ImGui::TreeNode(label)) return;
      ImGui::DragFloat("Light Scale", &m.lightScale, 0.01f, 0.0f, 2.0f);
      ImGui::DragFloat("Light Desaturate", &m.lightDesaturate, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloat("Ambient Add", &m.ambientAdd, 0.01f, 0.0f, 2.0f);
      ImGui::DragFloat("Exposure Add", &m.exposureAdd, 0.01f, -3.0f, 3.0f);
      ImGui::DragFloat("Saturation Scale", &m.saturationScale, 0.01f, 0.0f, 2.0f);
      ImGui::DragFloat("Contrast Add", &m.contrastAdd, 0.01f, -1.0f, 1.0f);
      ImGui::DragFloat("Temperature Add", &m.temperatureAdd, 0.01f, -1.0f, 1.0f);
      ImGui::DragFloat("Water Scale", &m.waterScale, 0.01f, 0.0f, 2.0f);
      ImGui::DragFloat("Sky Scale", &m.skyScale, 0.01f, 0.0f, 2.0f);
      ImGui::DragFloat("Wave Scale", &m.waveScale, 0.01f, 0.0f, 3.0f);
      ImGui::DragFloat("Whitecap Add", &m.whitecapAdd, 0.01f, -1.0f, 1.0f);
      ImGui::ColorEdit4("Haze", &m.haze.x);
      ImGui::TreePop();
    };
    editMod("Rain Look", rainLook);
    editMod("Snow Look", snowLook);
    editMod("Storm Look", stormLook);

    if (ImGui::TreeNode("Rain")) {
      ImGui::DragInt("Max Drops", &rain.maxDrops, 1, 0, 1500);
      ImGui::DragFloat("Amount (rain / storm)", &rain.amount, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloat("Speed", &rain.speed, 0.1f, 0.5f, 60.0f);
      ImGui::DragFloat3("Wind", &rain.wind.x, 0.05f, -20.0f, 20.0f);
      ImGui::DragFloat("Storm Wind Scale", &rain.stormWindScale, 0.01f, 0.0f, 5.0f);
      ImGui::DragFloat("Streak Time", &rain.streakTime, 0.001f, 0.0f, 0.3f);
      ImGui::DragFloat("Near Depth", &rain.nearDepth, 0.1f, 0.2f, 40.0f);
      ImGui::ColorEdit4("Color", &rain.color.x);
      ImGui::DragFloat("Thickness", &rain.thickness, 0.05f, 0.5f, 6.0f);
      ImGui::Checkbox("Ripples", &rain.ripples);
      ImGui::DragInt("Ripple Max/Frame", &rain.rippleMaxPerFrame, 1, 0, 32);
      ImGui::DragFloat("Ripple Chance", &rain.rippleChance, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloat("Ripple Radius (UV)", &rain.rippleRadius, 0.001f, 0.002f, 0.05f, "%.3f");
      ImGui::DragFloat("Ripple Strength", &rain.rippleStrength, 0.002f, 0.0f, 0.5f);
      ImGui::Checkbox("Rings", &rain.rings);
      ImGui::DragInt("Ring Max", &rain.ringMax, 1, 0, 256);
      ImGui::DragFloat("Ring Size (m)", &rain.ringSize, 0.01f, 0.02f, 3.0f);
      ImGui::DragFloat("Ring Life", &rain.ringLife, 0.01f, 0.05f, 3.0f);
      ImGui::ColorEdit4("Ring Color", &rain.ringColor.x);
      ImGui::TreePop();
    }
    if (ImGui::TreeNode("Snow")) {
      ImGui::DragInt("Max Flakes", &snow.maxFlakes, 1, 0, 1500);
      ImGui::DragFloatRange2("Fall Speed", &snow.fallSpeedMin, &snow.fallSpeedMax, 0.05f, 0.05f, 20.0f);
      ImGui::DragFloat3("Wind", &snow.wind.x, 0.05f, -20.0f, 20.0f);
      ImGui::DragFloat("Sway", &snow.swayAmplitude, 0.01f, 0.0f, 5.0f);
      ImGui::DragFloatRange2("Sway Period", &snow.swayPeriodMin, &snow.swayPeriodMax, 0.05f, 0.1f, 20.0f);
      ImGui::DragFloatRange2("Size (m)", &snow.sizeMin, &snow.sizeMax, 0.005f, 0.005f, 1.0f);
      ImGui::DragFloat("Max Pixel Radius", &snow.maxPixelRadius, 0.1f, 1.0f, 64.0f);
      ImGui::DragFloat("Near Depth", &snow.nearDepth, 0.1f, 0.2f, 40.0f);
      ImGui::DragFloat("Melt Time", &snow.meltTime, 0.01f, 0.0f, 3.0f);
      ImGui::DragFloat("Flutter", &snow.flutter, 0.01f, 0.0f, 3.0f);
      ImGui::DragFloat("Far Fade", &snow.farFade, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloat("Halo Alpha", &snow.haloAlpha, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloat("Bokeh Max Pixel", &snow.bokehMaxPixel, 0.5f, 1.0f, 128.0f);
      ImGui::DragFloat("Bokeh Alpha", &snow.bokehAlpha, 0.01f, 0.0f, 1.0f);
      ImGui::ColorEdit4("Color", &snow.color.x);
      ImGui::TreePop();
    }
    if (ImGui::TreeNode("Lightning")) {
      ImGui::Checkbox("Enabled", &lightning.enabled);
      ImGui::DragFloat("Min Level", &lightning.minLevel, 0.01f, 0.0f, 1.0f);
      ImGui::DragFloatRange2("Interval", &lightning.intervalMin, &lightning.intervalMax, 0.1f, 0.2f, 120.0f);
      ImGui::DragFloat("Screen Alpha", &lightning.screenAlpha, 0.01f, 0.0f, 1.0f);
      ImGui::ColorEdit4("Flash Color", &lightning.flashColor.x);
      ImGui::DragFloat("Light Boost", &lightning.lightBoost, 0.05f, 0.0f, 10.0f);
      ImGui::DragFloat("Ambient Boost", &lightning.ambientBoost, 0.01f, 0.0f, 5.0f);
      ImGui::DragFloatRange2("Thunder Delay", &lightning.thunderDelayMin, &lightning.thunderDelayMax, 0.05f, 0.0f,
                             10.0f);
      ImGui::TreePop();
    }
    ImGui::Text("rain sound: %s  thunder sound: %s", rainSoundPath.empty() ? "(none)" : rainSoundPath.c_str(),
                thunderSoundPath.empty() ? "(none)" : thunderSoundPath.c_str());
    ImGui::DragFloat("Rain Volume", &rainVolume, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Thunder Volume", &thunderVolume, 0.01f, 0.0f, 1.0f);
  }
#endif

protected:
  void OnCreate() override {
    rng_.seed(std::random_device{}());
    phase_ = startPhase - std::floor(startPhase);
    state_ = State::Clear;
    stateTimer_ = RandRange(firstDelayMin, firstDelayMax);
    drops_.clear();
    flakes_.clear();
    rings_.clear();
    if (!rainSoundPath.empty()) rainClip_ = AudioEngine::Get().LoadClip(rainSoundPath);
    if (!thunderSoundPath.empty()) thunderClip_ = AudioEngine::Get().LoadClip(thunderSoundPath);
  }

  void OnUpdate(float deltaTime) override {
    SceneContext *ctx = GetSceneContext();
    if (!ctx || !ctx->isPlaying() || !enabled) {
      // 再生を止めた／無効にした：書き換えた値を元に戻す（編集中の保存に焼き付けない）
      RestoreScene();
      StopRainSound();
      return;
    }
    const float dt = (std::max)(deltaTime, 0.0f);

    if (timeEnabled && dt > 0.0f) {
      phase_ += dt / (std::max)(cycleSeconds, 1.0f);
      phase_ -= std::floor(phase_);
    }
    if (dt > 0.0f) UpdateWeatherState(dt);
    UpdateLightning(dt);

    UpdateView();
    if (dt > 0.0f) {
      UpdateRain(dt);
      UpdateSnow(dt);
      UpdateRings(dt);
    }
    ApplyToScene();
    UpdateSound();
  }

  void OnRender() override {
    if (!enabled || !viewValid_ || camVisible_ <= 0.001f) return;
    const auto &rc = RC::GetRenderContext();
    const RC::Matrix4x4 &view = rc.View();
    const RC::Matrix4x4 viewProj = ::Multiply(view, rc.Proj());
    DrawHaze();
    DrawRain(view, viewProj);
    DrawSnow(view, viewProj);
    DrawFlash();
  }

  void OnDestroy() override {
    RestoreScene();
    StopRainSound();
    if (rainClip_ >= 0) AudioEngine::Get().UnloadClip(rainClip_);
    if (thunderClip_ >= 0) AudioEngine::Get().UnloadClip(thunderClip_);
    rainClip_ = thunderClip_ = -1;
  }

private:
  enum class State { Clear, FadeIn, Hold, FadeOut };

  struct Drop {
    RC::Vector3 pos{};
    RC::Vector3 vel{};
    float age = 0.0f;
    bool alive = false;
  };
  struct Flake {
    RC::Vector3 pos{};
    float fall = 1.0f;
    float size = 0.08f;
    float swayPhase = 0.0f;
    float swayFreq = 1.0f;
    float flutterPhase = 0.0f;
    float flutterFreq = 8.0f;
    float bright = 1.0f; ///< 雪片ごとの明るさのばらつき
    float age = 0.0f;
    float melt = -1.0f; ///< 着水後の経過（< 0 なら空中）
    bool alive = false;
  };
  struct Ring {
    RC::Vector3 pos{};
    float age = 0.0f;
  };

  // ------------------------------------------------------------------
  // 既定値
  // ------------------------------------------------------------------

  static const char *LookName(int i) {
    switch (i) {
    case kDay: return "day";
    case kEvening: return "evening";
    case kNight: return "night";
    case kDawn: return "dawn";
    default: return "day";
    }
  }
  static int LookIdFromName(const std::string &s) {
    for (int i = 0; i < kLookCount; ++i) {
      if (s == LookName(i)) return i;
    }
    return -1;
  }
  static const char *WeatherName(Weather w) {
    switch (w) {
    case Weather::Rain: return "rain";
    case Weather::Snow: return "snow";
    case Weather::Storm: return "storm";
    default: return "clear";
    }
  }
  static const char *StateName(State s) {
    switch (s) {
    case State::FadeIn: return "fade-in";
    case State::Hold: return "hold";
    case State::FadeOut: return "fade-out";
    default: return "clear";
    }
  }
  const char *PhaseLabel(float p) const {
    // いちばん近い（直前の）キーの look 名
    int id = schedule.empty() ? kDay : schedule.back().look;
    for (const auto &k : schedule) {
      if (k.t <= p) id = k.look;
    }
    return LookName(id);
  }

  static std::array<Look, kLookCount> DefaultLooks() {
    std::array<Look, kLookCount> l{};
    // 昼：Title.json の初期値と同じ（昼のあいだは今までの見た目のまま）
    l[kDay].lightColor = V4(1.0f, 0.95f, 0.86f, 1.0f);
    l[kDay].lightDirection = Normalize3(V3(0.12f, -1.0f, 0.10f), V3(0, -1, 0));
    l[kDay].lightIntensity = 1.1f;
    l[kDay].ambientColor = V3(1.0f, 1.0f, 1.0f);
    l[kDay].ambientIntensity = 0.0f;
    l[kDay].waterShallow = V4(0.10f, 0.66f, 0.56f, 0.90f);
    l[kDay].waterDeep = V4(0.02f, 0.26f, 0.36f, 0.96f);
    l[kDay].waterSss = V4(0.10f, 0.62f, 0.52f, 0.9f);
    l[kDay].waterEnvironment = 0.35f;
    l[kDay].waterCrestTint = 0.55f;
    l[kDay].skyColor = V4(1.0f, 1.0f, 1.0f, 1.0f);
    l[kDay].gradeExposure = 0.1f;
    l[kDay].gradeContrast = 1.05f;
    l[kDay].gradeSaturation = 1.12f;
    l[kDay].gradeTemperature = 0.0f;
    l[kDay].gradeTint = 0.0f;

    // 夕方：低い西日。真上視点では空の映り込みがほぼ無く「水の色 × 光の色」がそのまま見えるため、
    // 青緑の水に橙の光を掛けると緑だけが残って濁ったオリーブ色になる。水を青紫寄りにして
    // 夕暮れの群青＋橙の照り返し（補色）にする。光の橙もやや淡くして濁りを抑える
    l[kEvening].lightColor = V4(1.0f, 0.68f, 0.46f, 1.0f);
    l[kEvening].lightDirection = Normalize3(V3(0.75f, -0.55f, 0.30f), V3(0, -1, 0));
    l[kEvening].lightIntensity = 1.0f;
    l[kEvening].ambientColor = V3(1.0f, 0.66f, 0.55f);
    l[kEvening].ambientIntensity = 0.12f;
    l[kEvening].waterShallow = V4(0.08f, 0.18f, 0.60f, 0.90f);
    l[kEvening].waterDeep = V4(0.03f, 0.06f, 0.28f, 0.96f);
    l[kEvening].waterSss = V4(1.0f, 0.55f, 0.30f, 0.55f);
    l[kEvening].waterEnvironment = 0.45f;
    l[kEvening].waterCrestTint = 0.45f;
    l[kEvening].skyColor = V4(1.0f, 0.70f, 0.55f, 1.0f);
    l[kEvening].gradeExposure = 0.0f;
    l[kEvening].gradeContrast = 1.06f;
    l[kEvening].gradeSaturation = 1.05f;
    l[kEvening].gradeTemperature = 0.12f;
    l[kEvening].gradeTint = 0.10f;

    // 夜：青い月明かり。文字が読めるよう環境光を足す
    l[kNight].lightColor = V4(0.55f, 0.66f, 1.0f, 1.0f);
    l[kNight].lightDirection = Normalize3(V3(-0.25f, -1.0f, -0.35f), V3(0, -1, 0));
    l[kNight].lightIntensity = 0.55f;
    l[kNight].ambientColor = V3(0.45f, 0.55f, 0.85f);
    l[kNight].ambientIntensity = 0.40f;
    l[kNight].waterShallow = V4(0.05f, 0.30f, 0.42f, 0.92f);
    l[kNight].waterDeep = V4(0.01f, 0.06f, 0.16f, 0.97f);
    l[kNight].waterSss = V4(0.20f, 0.35f, 0.60f, 0.25f);
    l[kNight].waterEnvironment = 0.2f;
    l[kNight].waterCrestTint = 0.18f;
    l[kNight].skyColor = V4(0.25f, 0.30f, 0.50f, 1.0f);
    l[kNight].gradeExposure = -0.15f;
    l[kNight].gradeContrast = 1.06f;
    l[kNight].gradeSaturation = 0.85f;
    l[kNight].gradeTemperature = -0.30f;
    l[kNight].gradeTint = 0.02f;

    // 明け方：薄紅と紫
    l[kDawn].lightColor = V4(1.0f, 0.74f, 0.78f, 1.0f);
    l[kDawn].lightDirection = Normalize3(V3(-0.70f, -0.60f, 0.20f), V3(0, -1, 0));
    l[kDawn].lightIntensity = 0.8f;
    l[kDawn].ambientColor = V3(0.75f, 0.60f, 0.85f);
    l[kDawn].ambientIntensity = 0.2f;
    l[kDawn].waterShallow = V4(0.12f, 0.50f, 0.56f, 0.90f);
    l[kDawn].waterDeep = V4(0.04f, 0.16f, 0.32f, 0.96f);
    l[kDawn].waterSss = V4(0.60f, 0.40f, 0.60f, 0.5f);
    l[kDawn].waterEnvironment = 0.35f;
    l[kDawn].waterCrestTint = 0.35f;
    l[kDawn].skyColor = V4(0.90f, 0.75f, 0.85f, 1.0f);
    l[kDawn].gradeExposure = 0.0f;
    l[kDawn].gradeContrast = 1.04f;
    l[kDawn].gradeSaturation = 1.05f;
    l[kDawn].gradeTemperature = 0.08f;
    l[kDawn].gradeTint = 0.15f;
    return l;
  }

  static std::vector<ScheduleKey> DefaultSchedule() {
    // 180 秒で：昼 54s → 夕方へ 22s → 夕方 14s → 夜へ 18s → 夜 36s → 明け方へ 14s → 明け方 9s → 昼へ 13s
    return {{0.00f, kDay},  {0.30f, kDay},   {0.42f, kEvening}, {0.50f, kEvening},
            {0.60f, kNight}, {0.80f, kNight}, {0.88f, kDawn},    {0.93f, kDawn}};
  }

  // ------------------------------------------------------------------
  // 乱数
  // ------------------------------------------------------------------

  float Rand01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
  float RandRange(float a, float b) {
    if (b < a) std::swap(a, b);
    return a + (b - a) * Rand01();
  }

  // ------------------------------------------------------------------
  // 時間帯
  // ------------------------------------------------------------------

  /// @brief 現在の phase の見た目（schedule のキー間を smoothstep 補間。最後のキー → 最初のキーへ一周で戻る）
  Look EvaluateTimeLook(float p) const {
    if (schedule.empty()) return looks[kDay];
    if (schedule.size() == 1) return looks[schedule[0].look];
    const size_t n = schedule.size();
    // p を含む区間 [k0, k1) を探す（末尾 → 先頭は 1.0 を跨ぐ）
    size_t i0 = n - 1;
    for (size_t i = 0; i < n; ++i) {
      if (schedule[i].t <= p) i0 = i;
    }
    const size_t i1 = (i0 + 1) % n;
    float t0 = schedule[i0].t;
    float t1 = schedule[i1].t;
    float x = p;
    if (i1 == 0) t1 += 1.0f;               // 末尾 → 先頭
    if (x < t0) x += 1.0f;                 // p が先頭キーより前（末尾区間の中）
    const float span = t1 - t0;
    const float t = (span > 1e-5f) ? Smooth01((x - t0) / span) : 1.0f;
    return Look::Mix(looks[schedule[i0].look], looks[schedule[i1].look], t);
  }

  // ------------------------------------------------------------------
  // 天候
  // ------------------------------------------------------------------

  void UpdateWeatherState(float dt) {
    stateTimer_ -= dt;
    switch (state_) {
    case State::Clear:
      level_ = 0.0f;
      if (pendingWeather_ != Weather::Clear) {
        BeginWeather(pendingWeather_);
        pendingWeather_ = Weather::Clear;
      } else if (weatherEnabled && stateTimer_ <= 0.0f) {
        const Weather w = PickWeather();
        if (w == Weather::Clear) {
          stateTimer_ = RandRange(clearMin, clearMax);
        } else {
          BeginWeather(w);
        }
      }
      break;
    case State::FadeIn:
      level_ = (std::min)(1.0f, level_ + dt / (std::max)(fadeIn, 0.01f));
      if (level_ >= 1.0f) {
        state_ = State::Hold;
        stateTimer_ = RandRange(holdMin, holdMax);
      }
      break;
    case State::Hold:
      level_ = 1.0f;
      if (stateTimer_ <= 0.0f) state_ = State::FadeOut;
      break;
    case State::FadeOut: {
      // 強制切り替えの待ちがあるときは速めに止める
      const float dur = (pendingWeather_ != Weather::Clear) ? (std::min)(fadeOut, 2.0f) : fadeOut;
      level_ = (std::max)(0.0f, level_ - dt / (std::max)(dur, 0.01f));
      if (level_ <= 0.0f) {
        weather_ = Weather::Clear;
        state_ = State::Clear;
        stateTimer_ = RandRange(clearMin, clearMax);
      }
      break;
    }
    }
  }

  void BeginWeather(Weather w) {
    weather_ = w;
    state_ = State::FadeIn;
    lightningTimer_ = RandRange(lightning.intervalMin, lightning.intervalMax) * 0.5f;
  }

  Weather PickWeather() {
    const float wr = (std::max)(weightRain, 0.0f);
    const float ws = (std::max)(weightSnow, 0.0f);
    const float wt = (std::max)(weightStorm, 0.0f);
    const float sum = wr + ws + wt;
    if (sum <= 1e-5f) return Weather::Clear;
    const float r = Rand01() * sum;
    if (r < wr) return Weather::Rain;
    if (r < wr + ws) return Weather::Snow;
    return Weather::Storm;
  }

  /// @brief ImGui から天候を切り替える（今の天候が残っていれば先に素早く止める）
  void ForceWeather(Weather w) {
    if (w == Weather::Clear) {
      pendingWeather_ = Weather::Clear;
      if (state_ != State::Clear) state_ = State::FadeOut;
      return;
    }
    if (state_ == State::Clear || weather_ == w) {
      pendingWeather_ = Weather::Clear;
      BeginWeather(w);
      return;
    }
    pendingWeather_ = w;
    state_ = State::FadeOut;
  }

  /// @brief 天候の見た目補正（強さで無補正 → 満額へ）
  WeatherMod CurrentMod() const {
    const WeatherMod *m = nullptr;
    switch (weather_) {
    case Weather::Rain: m = &rainLook; break;
    case Weather::Snow: m = &snowLook; break;
    case Weather::Storm: m = &stormLook; break;
    default: return WeatherMod{};
    }
    const float t = Smooth01(level_);
    WeatherMod r;
    r.lightScale = Lerp(1.0f, m->lightScale, t);
    r.lightDesaturate = m->lightDesaturate * t;
    r.ambientAdd = m->ambientAdd * t;
    r.exposureAdd = m->exposureAdd * t;
    r.saturationScale = Lerp(1.0f, m->saturationScale, t);
    r.contrastAdd = m->contrastAdd * t;
    r.temperatureAdd = m->temperatureAdd * t;
    r.waterScale = Lerp(1.0f, m->waterScale, t);
    r.skyScale = Lerp(1.0f, m->skyScale, t);
    r.waveScale = Lerp(1.0f, m->waveScale, t);
    r.whitecapAdd = m->whitecapAdd * t;
    r.haze = m->haze;
    r.haze.w = m->haze.w * t;
    return r;
  }

  float RainLevel() const {
    if (weather_ == Weather::Storm) return Smooth01(level_);
    if (weather_ == Weather::Rain) return Smooth01(level_) * std::clamp(rain.amount, 0.0f, 1.0f);
    return 0.0f;
  }
  float SnowLevel() const { return (weather_ == Weather::Snow) ? Smooth01(level_) : 0.0f; }

  // ------------------------------------------------------------------
  // 雷
  // ------------------------------------------------------------------

  void StrikeLightning() {
    flashTime_ = 0.0f;
    flashActive_ = true;
    // 2〜3 回の明滅（時刻と強さ）
    pulseCount_ = (Rand01() < 0.5f) ? 2 : 3;
    pulseT_[0] = 0.0f;
    pulseA_[0] = 1.0f;
    pulseT_[1] = RandRange(0.10f, 0.18f);
    pulseA_[1] = RandRange(0.4f, 0.7f);
    pulseT_[2] = pulseT_[1] + RandRange(0.12f, 0.22f);
    pulseA_[2] = RandRange(0.6f, 0.95f);
    thunderTimer_ = RandRange(lightning.thunderDelayMin, lightning.thunderDelayMax);
  }

  void UpdateLightning(float dt) {
    // 明滅の包絡線：各パルスの指数減衰の最大
    flash_ = 0.0f;
    if (flashActive_) {
      flashTime_ += dt;
      for (int i = 0; i < pulseCount_; ++i) {
        const float e = flashTime_ - pulseT_[i];
        if (e >= 0.0f) flash_ = (std::max)(flash_, pulseA_[i] * std::exp(-e / 0.07f));
      }
      if (flashTime_ > pulseT_[pulseCount_ - 1] + 0.6f) flashActive_ = false;
    }
    if (thunderTimer_ > 0.0f && dt > 0.0f) {
      thunderTimer_ -= dt;
      if (thunderTimer_ <= 0.0f && thunderClip_ >= 0) {
        AudioEngine::Get().PlaySe(thunderClip_, thunderVolume * camVisible_);
      }
    }
    if (dt <= 0.0f) return;
    if (!lightning.enabled || weather_ != Weather::Storm || level_ < lightning.minLevel) return;
    lightningTimer_ -= dt;
    if (lightningTimer_ <= 0.0f) {
      StrikeLightning();
      lightningTimer_ = RandRange(lightning.intervalMin, lightning.intervalMax);
    }
  }

  // ------------------------------------------------------------------
  // カメラ（画面 ⇔ ワールド）
  // ------------------------------------------------------------------

  void UpdateView() {
    viewValid_ = false;
    Scene *scene = GetScene();
    auto cam = WaterCameraFx::FindMainCamera(scene);
    if (!cam) return;
    const auto *tr = cam->GetComponent<TransformComponent>();
    if (!tr) return;
    camPos_ = tr->position;
    waterY_ = WaterCameraFx::FindWaterY(scene);

    screenW_ = 1280.0f;
    screenH_ = 720.0f;
    auto &rc = RC::GetRenderContext();
    if (rc.Ctx() && rc.Ctx()->app && rc.Ctx()->app->width > 0 && rc.Ctx()->app->height > 0) {
      screenW_ = static_cast<float>(rc.Ctx()->app->width);
      screenH_ = static_cast<float>(rc.Ctx()->app->height);
    }
    view_ = rc.View();
    proj_ = rc.Proj();
    const RC::Ray center = RC::ScreenPointToRay({screenW_ * 0.5f, screenH_ * 0.5f}, screenW_, screenH_, view_, proj_);
    camFwd_ = Normalize3(center.direction, V3(0.0f, -1.0f, 0.0f));
    // カメラが水面の下（飛び込み演出）なら雨・雪は描かない。水面のすぐ上で徐々に消す
    camVisible_ = Smooth01((camPos_.y - waterY_ - 0.3f) / 1.5f);
    viewValid_ = std::isfinite(camFwd_.x) && std::isfinite(camFwd_.y) && std::isfinite(camFwd_.z);
  }

  /// @brief 画面上のランダムな点 × 奥行き（d² 分布 = 体積あたり一様）で、水面より上のワールド座標を作る
  bool RandomPointInView(float nearDepth, float margin, RC::Vector3 &out) {
    const float sx = RandRange(-margin, screenW_ + margin);
    const float sy = RandRange(-margin, screenH_ + margin);
    const RC::Ray ray = RC::ScreenPointToRay({sx, sy}, screenW_, screenH_, view_, proj_);
    const float dirFwd = ray.direction.x * camFwd_.x + ray.direction.y * camFwd_.y + ray.direction.z * camFwd_.z;
    if (dirFwd < 1e-4f) return false;
    const RC::Vector3 rel = V3(ray.origin.x - camPos_.x, ray.origin.y - camPos_.y, ray.origin.z - camPos_.z);
    const float originFwd = rel.x * camFwd_.x + rel.y * camFwd_.y + rel.z * camFwd_.z;
    // このレイが水面に届く奥行き（届かなければ 60m で打ち切り）
    float dMax = 60.0f;
    if (ray.direction.y < -1e-4f) {
      const float tw = (waterY_ - ray.origin.y) / ray.direction.y;
      if (tw > 0.0f) dMax = (std::min)(dMax, originFwd + tw * dirFwd);
    }
    const float dMin = (std::max)(nearDepth, originFwd + 0.01f);
    if (dMax <= dMin) return false;
    // pdf ∝ d²（遠いほど 1 ピクセルあたりの体積が大きい）
    const float a = dMin * dMin * dMin, b = dMax * dMax * dMax;
    const float d = std::cbrt(a + (b - a) * Rand01());
    const float t = (d - originFwd) / dirFwd;
    out = V3(ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t,
             ray.origin.z + ray.direction.z * t);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
  }

  /// @brief ワールド → 画面。カメラの手前 nearDepth 未満なら false。outDepth はビュー空間の奥行き
  bool Project(const RC::Vector3 &p, const RC::Matrix4x4 &view, const RC::Matrix4x4 &viewProj, float nearDepth,
               RC::Vector2 &outScreen, float &outDepth) const {
    float vx, vy, vz, vw;
    TransformPoint(p, view, vx, vy, vz, vw);
    if (vz < nearDepth) return false;
    float cx, cy, cz, cw;
    TransformPoint(p, viewProj, cx, cy, cz, cw);
    if (cw <= 1e-5f) return false;
    const float nx = cx / cw, ny = cy / cw;
    outScreen = {(nx + 1.0f) * 0.5f * screenW_, (1.0f - ny) * 0.5f * screenH_};
    outDepth = vz;
    return true;
  }

  /// @brief 粒を生む・巡回させる範囲の画面外へのはみ出し（px）
  float SpawnMargin() const { return 40.0f; }

  /// @brief 画面の外へ流れ出た粒を、同じ奥行きのまま反対側の端へ戻す（画面空間で巡回）
  /// @details 横風で粒は画面上を一方向へ流れる。出た粒を画面内のランダムな位置で生み直すと
  ///          風上側（流れの上流）へ流れ込む粒が無く、上流が薄く下流が濃い偏りになる。
  ///          出た粒を上流の端へ巡回させれば、どの奥行きでも画面全体の密度が一様に保たれる。
  /// @return 範囲内のまま／巡回できたら true。巡回先が水面の下などで置けなければ false（生み直す）
  bool WrapInView(RC::Vector3 &pos, const RC::Matrix4x4 &viewProj) const {
    RC::Vector2 s;
    float depth;
    if (!Project(pos, view_, viewProj, 0.05f, s, depth)) return pos.y <= camPos_.y; // カメラ直近・背後
    const float m = SpawnMargin();
    const float w = screenW_ + 2.0f * m, h = screenH_ + 2.0f * m;
    float x = s.x + m, y = s.y + m;
    if (x >= 0.0f && y >= 0.0f && x <= w && y <= h) return true;
    x -= std::floor(x / w) * w;
    y -= std::floor(y / h) * h;
    RC::Vector3 p;
    if (!ScreenToWorldAtDepth(x - m, y - m, depth, p) || p.y <= waterY_) return false;
    pos = p;
    return true;
  }

  /// @brief 画面座標 (sx, sy) を、ビュー空間の奥行き depth のワールド座標へ戻す
  bool ScreenToWorldAtDepth(float sx, float sy, float depth, RC::Vector3 &out) const {
    const RC::Ray ray = RC::ScreenPointToRay({sx, sy}, screenW_, screenH_, view_, proj_);
    const float dirFwd = ray.direction.x * camFwd_.x + ray.direction.y * camFwd_.y + ray.direction.z * camFwd_.z;
    if (dirFwd < 1e-4f) return false;
    const float originFwd = (ray.origin.x - camPos_.x) * camFwd_.x + (ray.origin.y - camPos_.y) * camFwd_.y +
                            (ray.origin.z - camPos_.z) * camFwd_.z;
    const float t = (depth - originFwd) / dirFwd;
    out = V3(ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t,
             ray.origin.z + ray.direction.z * t);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
  }

  bool OnScreen(const RC::Vector2 &s, float margin) const {
    return s.x >= -margin && s.y >= -margin && s.x <= screenW_ + margin && s.y <= screenH_ + margin;
  }

  // ------------------------------------------------------------------
  // 雨
  // ------------------------------------------------------------------

  RC::Vector3 RainWind() const {
    const float s = (weather_ == Weather::Storm) ? rain.stormWindScale : 1.0f;
    return V3(rain.wind.x * s, rain.wind.y * s, rain.wind.z * s);
  }

  void SpawnDrop(Drop &d) {
    RC::Vector3 p;
    d.alive = false;
    if (!viewValid_ || !RandomPointInView(rain.nearDepth, SpawnMargin(), p)) return;
    const RC::Vector3 w = RainWind();
    const float jitter = RandRange(0.85f, 1.15f);
    d.pos = p;
    d.vel = V3(w.x, -rain.speed * jitter + w.y, w.z);
    d.age = 0.0f;
    d.alive = true;
  }

  void UpdateRain(float dt) {
    const float lv = RainLevel();
    const int target = static_cast<int>(static_cast<float>(rain.maxDrops) * lv + 0.5f);
    if (static_cast<int>(drops_.size()) < rain.maxDrops) drops_.resize(rain.maxDrops);
    int ripplesLeft = (rain.ripples && camVisible_ > 0.0f) ? rain.rippleMaxPerFrame : 0;
    activeDrops_ = 0;
    const RC::Matrix4x4 viewProj = ::Multiply(view_, proj_);
    for (int i = 0; i < static_cast<int>(drops_.size()); ++i) {
      Drop &d = drops_[i];
      if (i >= target) {
        // 弱まってきたら、空中の粒は落とし切ってから消す（ぱっと消えないように）
        if (!d.alive) continue;
      } else if (!d.alive) {
        SpawnDrop(d);
        if (!d.alive) continue;
      }
      d.age += dt;
      d.pos.x += d.vel.x * dt;
      d.pos.y += d.vel.y * dt;
      d.pos.z += d.vel.z * dt;
      if (d.pos.y <= waterY_) {
        // 着水：波紋と輪
        if (ripplesLeft > 0 && Rand01() < rain.rippleChance) {
          if (RC::AddWaveSourceAtWorld(d.pos.x, d.pos.z, rain.rippleRadius, -rain.rippleStrength)) --ripplesLeft;
        }
        if (rain.rings && static_cast<int>(rings_.size()) < rain.ringMax) rings_.push_back({V3(d.pos.x, waterY_, d.pos.z), 0.0f});
        d.alive = false;
        if (i < target) SpawnDrop(d);
        if (!d.alive) continue;
      }
      // 横風で画面外へ流れたら反対側の端へ巡回（置けなければ生まれ直す）
      if (viewValid_ && !WrapInView(d.pos, viewProj)) {
        d.alive = false;
        if (i < target) SpawnDrop(d);
        if (!d.alive) continue;
      }
      ++activeDrops_;
    }
  }

  void UpdateRings(float dt) {
    for (auto &r : rings_) r.age += dt;
    rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
                                [&](const Ring &r) { return r.age >= rain.ringLife; }),
                 rings_.end());
  }

  void DrawRain(const RC::Matrix4x4 &view, const RC::Matrix4x4 &viewProj) {
    if (activeDrops_ <= 0 && rings_.empty()) return;
    const float vis = camVisible_;
    for (const Drop &d : drops_) {
      if (!d.alive) continue;
      const RC::Vector3 tail =
          V3(d.pos.x - d.vel.x * rain.streakTime, d.pos.y - d.vel.y * rain.streakTime, d.pos.z - d.vel.z * rain.streakTime);
      RC::Vector2 a, b;
      float da, db;
      if (!Project(d.pos, view, viewProj, rain.nearDepth * 0.5f, a, da)) continue;
      if (!Project(tail, view, viewProj, rain.nearDepth * 0.5f, b, db)) continue;
      // 生まれた直後はふわっと出す。手前ほど少し薄く（大きくぼけた粒の代わり）
      const float fadeIn = Smooth01(d.age / 0.12f);
      const float nearFade = Smooth01((da - rain.nearDepth * 0.5f) / (rain.nearDepth * 1.5f));
      RC::Vector4 c = rain.color;
      c.w *= vis * fadeIn * (0.4f + 0.6f * nearFade);
      if (c.w <= 0.003f) continue;
      RC::DrawLine(b, a, c, rain.thickness, 1.0f);
    }
    for (const Ring &r : rings_) {
      const float t = std::clamp(r.age / (std::max)(rain.ringLife, 0.01f), 0.0f, 1.0f);
      RC::Vector2 s;
      float depth;
      if (!Project(r.pos, view, viewProj, 0.1f, s, depth)) continue;
      const float focal = proj_.m[1][1] * screenH_ * 0.5f;
      const float px = (std::max)(1.0f, rain.ringSize * t * focal / (std::max)(depth, 0.1f));
      RC::Vector4 c = rain.ringColor;
      c.w *= vis * (1.0f - t);
      if (c.w <= 0.003f) continue;
      RC::DrawCircle(s, px, c, kWire, 1.0f);
    }
  }

  // ------------------------------------------------------------------
  // 雪
  // ------------------------------------------------------------------

  void SpawnFlake(Flake &f) {
    RC::Vector3 p;
    f.alive = false;
    if (!viewValid_ || !RandomPointInView(snow.nearDepth, SpawnMargin(), p)) return;
    f.pos = p;
    f.fall = RandRange(snow.fallSpeedMin, snow.fallSpeedMax);
    // 小さい雪片ほど多く（u^1.8）。大きいものはときどき
    f.size = snow.sizeMin + (snow.sizeMax - snow.sizeMin) * std::pow(Rand01(), 1.8f);
    f.flutterPhase = RandRange(0.0f, 6.2831853f);
    f.flutterFreq = RandRange(5.0f, 11.0f);
    f.bright = RandRange(0.65f, 1.0f);
    f.swayPhase = RandRange(0.0f, 6.2831853f);
    f.swayFreq = 6.2831853f / (std::max)(RandRange(snow.swayPeriodMin, snow.swayPeriodMax), 0.1f);
    f.age = 0.0f;
    f.melt = -1.0f;
    f.alive = true;
  }

  void UpdateSnow(float dt) {
    const float lv = SnowLevel();
    const int target = static_cast<int>(static_cast<float>(snow.maxFlakes) * lv + 0.5f);
    if (static_cast<int>(flakes_.size()) < snow.maxFlakes) flakes_.resize(snow.maxFlakes);
    activeFlakes_ = 0;
    const RC::Matrix4x4 viewProj = ::Multiply(view_, proj_);
    for (int i = 0; i < static_cast<int>(flakes_.size()); ++i) {
      Flake &f = flakes_[i];
      if (i >= target) {
        if (!f.alive) continue;
      } else if (!f.alive) {
        SpawnFlake(f);
        if (!f.alive) continue;
      }
      f.age += dt;
      if (f.melt >= 0.0f) {
        // 着水後：その場で溶けて消える
        f.melt += dt;
        if (f.melt >= snow.meltTime) {
          f.alive = false;
          if (i < target) SpawnFlake(f);
          if (!f.alive) continue;
        }
        ++activeFlakes_;
        continue;
      }
      const float sway = std::sin(f.age * f.swayFreq + f.swayPhase) * snow.swayAmplitude;
      const float swayZ = std::cos(f.age * f.swayFreq * 0.7f + f.swayPhase) * snow.swayAmplitude * 0.6f;
      // ひらひら：大きな揺れ（sway）に、速く小さな揺れ（flutter）を重ねる
      const float fl = f.age * f.flutterFreq + f.flutterPhase;
      const float flX = std::sin(fl) * snow.flutter;
      const float flZ = std::cos(fl * 1.3f) * snow.flutter;
      f.pos.x += (snow.wind.x + sway + flX) * dt;
      f.pos.y += (snow.wind.y - f.fall * (1.0f + 0.25f * std::sin(fl * 0.5f))) * dt;
      f.pos.z += (snow.wind.z + swayZ + flZ) * dt;
      if (f.pos.y <= waterY_) {
        f.pos.y = waterY_;
        f.melt = 0.0f;
      }
      if (viewValid_ && !WrapInView(f.pos, viewProj)) {
        f.alive = false;
        if (i < target) SpawnFlake(f);
        if (!f.alive) continue;
      }
      ++activeFlakes_;
    }
  }

  void DrawSnow(const RC::Matrix4x4 &view, const RC::Matrix4x4 &viewProj) {
    if (activeFlakes_ <= 0) return;
    const float focal = proj_.m[1][1] * screenH_ * 0.5f;
    const float maxDepth = (std::max)(camPos_.y - waterY_, snow.nearDepth + 1.0f);
    for (const Flake &f : flakes_) {
      if (!f.alive) continue;
      RC::Vector2 s;
      float depth;
      if (!Project(f.pos, view, viewProj, snow.nearDepth * 0.5f, s, depth)) continue;
      const float px = f.size * focal / (std::max)(depth, 0.1f);

      // 濃さ：遠い（水面近く）ほど薄く、雪片ごとにばらつかせる
      const float farT = std::clamp((depth - snow.nearDepth) / (maxDepth - snow.nearDepth), 0.0f, 1.0f);
      float a = snow.color.w * f.bright * Lerp(1.0f, snow.farFade, farT);
      a *= camVisible_ * Smooth01(f.age / 0.4f);
      if (f.melt >= 0.0f) a *= 1.0f - std::clamp(f.melt / (std::max)(snow.meltTime, 0.01f), 0.0f, 1.0f);
      if (a <= 0.003f) continue;
      RC::Vector4 c = snow.color;

      if (px > snow.maxPixelRadius) {
        // カメラ直近：ピントの合わない大きなぼけ玉（輪郭をすべてぼかす）
        const float r = (std::min)(px, snow.bokehMaxPixel);
        c.w = a * snow.bokehAlpha;
        RC::DrawCircle(s, r, c, kFill, r * 0.9f);
        continue;
      }
      const float r = (std::max)(px, 0.7f);
      // まわりの淡い光（大きめの雪片だけ。ふわっとした柔らかさを出す）
      if (snow.haloAlpha > 0.0f && r >= 2.0f) {
        c.w = a * snow.haloAlpha;
        RC::DrawCircle(s, r * 2.4f, c, kFill, r * 1.8f);
      }
      // 芯：輪郭を半径の 6 割ほどぼかして、くっきりした丸（泡・粒っぽさ）を避ける
      c.w = a;
      RC::DrawCircle(s, r, c, kFill, (std::max)(1.0f, r * 0.6f));
    }
  }

  void DrawHaze() {
    if (haze_.w <= 0.002f) return;
    RC::Vector4 c = haze_;
    c.w = std::clamp(haze_.w * camVisible_, 0.0f, 1.0f);
    RC::DrawBox({0.0f, 0.0f}, {screenW_, screenH_}, c, kFill, 0.0f);
  }

  void DrawFlash() {
    if (flash_ <= 0.002f || lightning.screenAlpha <= 0.0f) return;
    RC::Vector4 c = lightning.flashColor;
    c.w = std::clamp(flash_ * lightning.screenAlpha * camVisible_, 0.0f, 1.0f);
    RC::DrawBox({0.0f, 0.0f}, {screenW_, screenH_}, c, kFill, 0.0f);
  }

  // ------------------------------------------------------------------
  // シーンへの反映
  // ------------------------------------------------------------------

  struct Saved {
    bool valid = false;
    std::weak_ptr<Entity> lightEntity, waterEntity, skyEntity;
    RC::Vector4 lightColor{};
    RC::Vector3 lightDirection{};
    float lightIntensity = 1.0f;
    RC::Vector3 ambientColor{};
    float ambientIntensity = 0.0f;
    RC::Vector4 waterShallow{}, waterDeep{}, waterSss{};
    float waterEnvironment = 0.0f, waterCrestTint = 0.0f;
    float waveHeight = 0.0f, waveHeight2 = 0.0f, whitecapCoverage = 0.0f;
    RC::Vector4 skyColor{};
  };

  void CaptureScene() {
    if (saved_.valid) return;
    Scene *scene = GetScene();
    if (!scene) return;
    for (const auto &e : scene->GetEntities()) {
      if (!e) continue;
      if (saved_.lightEntity.expired()) {
        if (auto *l = e->GetComponent<DirectionalLightComponent>()) {
          saved_.lightEntity = e;
          saved_.lightColor = l->color;
          saved_.lightDirection = l->direction;
          saved_.lightIntensity = l->intensity;
          saved_.ambientColor = l->ambientColor;
          saved_.ambientIntensity = l->ambientIntensity;
        }
      }
      if (saved_.waterEntity.expired()) {
        if (auto *w = e->GetComponent<WaterComponent>()) {
          saved_.waterEntity = e;
          saved_.waterShallow = w->shallowColor;
          saved_.waterDeep = w->deepColor;
          saved_.waterSss = w->sssColor;
          saved_.waterEnvironment = w->environmentCoeff;
          saved_.waterCrestTint = w->crestTint;
          saved_.waveHeight = w->waveHeight;
          saved_.waveHeight2 = w->waveHeight2;
          saved_.whitecapCoverage = w->whitecapCoverage;
        }
      }
      if (saved_.skyEntity.expired()) {
        if (auto *s = e->GetComponent<SkyboxComponent>()) {
          saved_.skyEntity = e;
          saved_.skyColor = s->color;
        }
      }
    }
    saved_.valid = true;
  }

  void RestoreScene() {
    if (!saved_.valid) return;
    if (auto e = saved_.lightEntity.lock()) {
      if (auto *l = e->GetComponent<DirectionalLightComponent>()) {
        l->color = saved_.lightColor;
        l->direction = saved_.lightDirection;
        l->intensity = saved_.lightIntensity;
        l->ambientColor = saved_.ambientColor;
        l->ambientIntensity = saved_.ambientIntensity;
      }
    }
    if (auto e = saved_.waterEntity.lock()) {
      if (auto *w = e->GetComponent<WaterComponent>()) {
        w->shallowColor = saved_.waterShallow;
        w->deepColor = saved_.waterDeep;
        w->sssColor = saved_.waterSss;
        w->environmentCoeff = saved_.waterEnvironment;
        w->crestTint = saved_.waterCrestTint;
        w->waveHeight = saved_.waveHeight;
        w->waveHeight2 = saved_.waveHeight2;
        w->whitecapCoverage = saved_.whitecapCoverage;
      }
    }
    if (auto e = saved_.skyEntity.lock()) {
      if (auto *s = e->GetComponent<SkyboxComponent>()) s->color = saved_.skyColor;
    }
    saved_ = Saved{};
  }

  void ApplyToScene() {
    CaptureScene();
    const Look look = EvaluateTimeLook(phase_);
    WeatherMod mod = CurrentMod();
    // 暗くする補正は、時間帯の明るさ（昼の光の強さに対する割合）に応じて弱める。
    // 夜（もともと暗い）に雷雨の暗さを満額で重ねると、真っ黒に潰れてしまうため。
    {
      const float dayI = (std::max)(looks[kDay].lightIntensity, 0.01f);
      const float b = std::clamp(look.lightIntensity / dayI, 0.0f, 1.0f);
      mod.lightScale = Lerp(1.0f, mod.lightScale, b);
      mod.waterScale = Lerp(1.0f, mod.waterScale, b);
      if (mod.exposureAdd < 0.0f) mod.exposureAdd *= b;
    }
    haze_ = mod.haze;

    if (auto e = saved_.lightEntity.lock()) {
      if (auto *l = e->GetComponent<DirectionalLightComponent>()) {
        const RC::Vector3 lc = Desaturate(V3(look.lightColor.x, look.lightColor.y, look.lightColor.z), mod.lightDesaturate);
        const RC::Vector3 fc = V3(lightning.flashColor.x, lightning.flashColor.y, lightning.flashColor.z);
        const float f = std::clamp(flash_, 0.0f, 1.0f);
        const RC::Vector3 c = Lerp3(lc, fc, f);
        l->color = V4(c.x, c.y, c.z, look.lightColor.w);
        l->direction = look.lightDirection;
        l->intensity = look.lightIntensity * mod.lightScale + flash_ * lightning.lightBoost;
        l->ambientColor = Lerp3(Desaturate(look.ambientColor, mod.lightDesaturate), fc, f);
        l->ambientIntensity = look.ambientIntensity + mod.ambientAdd + flash_ * lightning.ambientBoost;
      }
    }
    if (auto e = saved_.waterEntity.lock()) {
      if (auto *w = e->GetComponent<WaterComponent>()) {
        auto scaleRgb = [](const RC::Vector4 &c, float s) { return V4(c.x * s, c.y * s, c.z * s, c.w); };
        w->shallowColor = scaleRgb(look.waterShallow, mod.waterScale);
        w->deepColor = scaleRgb(look.waterDeep, mod.waterScale);
        w->sssColor = look.waterSss;
        w->environmentCoeff = look.waterEnvironment;
        w->crestTint = look.waterCrestTint;
        w->waveHeight = saved_.waveHeight * mod.waveScale;
        w->waveHeight2 = saved_.waveHeight2 * mod.waveScale;
        w->whitecapCoverage = std::clamp(saved_.whitecapCoverage + mod.whitecapAdd, 0.0f, 1.0f);
      }
    }
    if (auto e = saved_.skyEntity.lock()) {
      if (auto *s = e->GetComponent<SkyboxComponent>()) {
        s->color = V4(look.skyColor.x * mod.skyScale, look.skyColor.y * mod.skyScale, look.skyColor.z * mod.skyScale,
                      look.skyColor.w);
      }
    }
    // カラーグレード：TitleScreenScript が積んだもの（積むときに 1 回だけ値を書く）を毎フレーム上書きする。
    // 自分では積まない（遷移の Dissolve より後ろに積まれるのを避けるため）
    if (PostProcess *pp = RC::GetRenderContext().GetPostProcess()) {
      if (pp->HasEffect(PostEffectType::ColorGrade)) {
        pp->SetGradeExposure(look.gradeExposure + mod.exposureAdd);
        pp->SetGradeContrast((std::max)(0.0f, look.gradeContrast + mod.contrastAdd));
        pp->SetGradeSaturation((std::max)(0.0f, look.gradeSaturation * mod.saturationScale));
        pp->SetGradeTemperature(std::clamp(look.gradeTemperature + mod.temperatureAdd, -1.0f, 1.0f));
        pp->SetGradeTint(std::clamp(look.gradeTint, -1.0f, 1.0f));
      }
    }
  }

  // ------------------------------------------------------------------
  // 音
  // ------------------------------------------------------------------

  void UpdateSound() {
    if (rainClip_ < 0) return;
    const float vol = (std::max)(RainLevel(), 0.0f) * rainVolume * camVisible_;
    if (vol > 0.005f) {
      if (rainVoice_ < 0 || !AudioEngine::Get().IsVoicePlaying(rainVoice_)) {
        rainVoice_ = AudioEngine::Get().PlaySe(rainClip_, vol, true);
      } else {
        AudioEngine::Get().SetVoiceVolume(rainVoice_, vol);
      }
    } else {
      StopRainSound();
    }
  }

  void StopRainSound() {
    if (rainVoice_ >= 0) AudioEngine::Get().StopVoice(rainVoice_);
    rainVoice_ = -1;
  }

  // ------------------------------------------------------------------
  // 状態
  // ------------------------------------------------------------------

  std::mt19937 rng_{20261006u};
  float phase_ = 0.0f;

  Weather weather_ = Weather::Clear;
  Weather pendingWeather_ = Weather::Clear;
  State state_ = State::Clear;
  float stateTimer_ = 0.0f;
  float level_ = 0.0f;

  // 雷
  bool flashActive_ = false;
  float flashTime_ = 0.0f;
  float flash_ = 0.0f;
  RC::Vector4 haze_{1.0f, 1.0f, 1.0f, 0.0f};
  int pulseCount_ = 0;
  float pulseT_[3] = {0.0f, 0.0f, 0.0f};
  float pulseA_[3] = {0.0f, 0.0f, 0.0f};
  float lightningTimer_ = 0.0f;
  float thunderTimer_ = 0.0f;

  // 画面
  bool viewValid_ = false;
  RC::Vector3 camPos_{};
  RC::Vector3 camFwd_{0.0f, -1.0f, 0.0f};
  float waterY_ = 0.0f;
  float camVisible_ = 1.0f;
  float screenW_ = 1280.0f, screenH_ = 720.0f;
  RC::Matrix4x4 view_{};
  RC::Matrix4x4 proj_{};

  // 粒
  std::vector<Drop> drops_;
  std::vector<Flake> flakes_;
  std::vector<Ring> rings_;
  int activeDrops_ = 0;
  int activeFlakes_ = 0;

  // 音
  int rainClip_ = -1;
  int thunderClip_ = -1;
  int rainVoice_ = -1;

  Saved saved_;
};

REGISTER_SCRIPT(TitleWeatherScript)
