#pragma once
#include <string>
#include "Common/ResourcePath.h"
#include <vector>
#include <memory>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <cmath>
#include <nlohmann/json.hpp>
#include "Common/Log/Log.h"

#include "ECS/Entity.h"
#include "ECS/TransformComponent.h"
#include "ECS/ModelRendererComponent.h"
#include "ECS/ColliderComponent.h"
#include "ECS/LightComponent.h"
#include "ECS/CameraComponent.h"

/// @struct PlayerSpawnData
/// @brief レベルデータの PlayerSpawn（プレイヤーの出現位置）
struct PlayerSpawnData {
  RC::Vector3 translation = {0.0f, 0.0f, 0.0f}; ///< 位置
  RC::Vector3 rotation = {0.0f, 0.0f, 0.0f};    ///< 回転（オイラー角、ラジアン）
};

/// @struct EnemySpawnData
/// @brief レベルデータの EnemySpawn（敵の出現位置）
struct EnemySpawnData {
  RC::Vector3 translation = {0.0f, 0.0f, 0.0f}; ///< 位置
  RC::Vector3 rotation = {0.0f, 0.0f, 0.0f};    ///< 回転（オイラー角、ラジアン）
  std::string fileName;                         ///< レベルデータの file_name（無ければ空）
};

/// @class LevelLoader
/// @brief Blender レベルエディタから出力された JSON レベルデータを読み込み、
///        Entity のリストに変換するユーティリティクラス
class LevelLoader {
public:
  /// @brief モデルファイルの基準ディレクトリを設定する
  /// @param dir ベースディレクトリパス（例: "Resources/model/"）
  void SetModelBaseDirectory(const std::string& dir) {
    modelBaseDir_ = dir;
    // 末尾にスラッシュがなければ追加
    if (!modelBaseDir_.empty() && modelBaseDir_.back() != '/' && modelBaseDir_.back() != '\\') {
      modelBaseDir_ += '/';
    }
  }

  /// @brief JSON ファイルからレベルデータを読み込む
  /// @param filePathIn JSON ファイルのパス（Chaso::ResolvePath で解決する）
  /// @return 読み込み成功時 true
  /// @note 呼ぶたびに Entity のリストは作り直すが、スポーンデータのリストは追記になる。
  bool LoadFromFile(const std::string& filePathIn) {
    entities_.clear();

    // ゲーム側 Resources → エンジン側 (Chaso/Resources) の順で探す
    const std::string filePath = Chaso::ResolvePath(filePathIn);
    if (!std::filesystem::exists(filePath)) {
      Log::Print("[LevelLoader] File not found: " + filePath);
      return false;
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
      Log::Print("[LevelLoader] Failed to open: " + filePath);
      return false;
    }

    nlohmann::json deserialized;
    try {
      file >> deserialized;
    } catch (const nlohmann::json::exception& e) {
      Log::Print("[LevelLoader] JSON parse error: " + std::string(e.what()));
      return false;
    }

    assert(deserialized.is_object());
    assert(deserialized.contains("name"));
    assert(deserialized["name"].is_string());

    std::string name = deserialized["name"].get<std::string>();
    if (name != "scene") {
      Log::Print("[LevelLoader] Invalid level data: name is '" + name + "', expected 'scene'");
      return false;
    }

    if (!deserialized.contains("objects")) {
      Log::Print("[LevelLoader] No 'objects' array found");
      return false;
    }

    // 回転の単位。Blender の rotation_euler はラジアンなので既定は false（＝ラジアン）。
    // 手書きのレベルデータなどで度数法を使いたい場合だけ root に
    // "rotation_in_degrees": true を入れる。
    // TransformComponent::rotation はエンジン全体でラジアン統一なので、
    // ここで吸収しないとライトの向きが数十ラジアンずれる。
    rotationInDegrees_ = false;
    if (deserialized.contains("rotation_in_degrees")) {
      rotationInDegrees_ = deserialized["rotation_in_degrees"].get<bool>();
    }
    hasMainCamera_ = false;

    for (auto& object : deserialized["objects"]) {
      assert(object.contains("type"));
      std::string type = object["type"].get<std::string>();

      if (type == "MESH") {
        ParseObjectRecursive(object, 0);
      } else if (type == "LIGHT") {
        ParseLight(object, 0);
      } else if (type == "CAMERA") {
        ParseCamera(object, 0);
      } else if (type == "PlayerSpawn") {
        PlayerSpawnData spawn;
        // rotation の単位変換は MESH / LIGHT / CAMERA と揃えること。
        // ここだけ生値のままだと、度数法のレベルデータでスポーン向きが壊れる。
        ReadSpawnTransform(object, spawn.translation, spawn.rotation);
        playerSpawns_.push_back(spawn);
      } else if (type == "EnemySpawn") {
        EnemySpawnData spawn;
        ReadSpawnTransform(object, spawn.translation, spawn.rotation);
        if (object.contains("file_name")) {
            spawn.fileName = object["file_name"].get<std::string>();
        }
        enemySpawns_.push_back(spawn);
      }
    }

    Log::Print("[LevelLoader] Loaded: " + filePath +
               " (" + std::to_string(entities_.size()) + " entities)");
    return true;
  }

  /// @brief 読み込んだ Entity のリストを取得する
  /// @return Entity のリスト
  const std::vector<std::shared_ptr<Entity>>& GetEntities() const {
    return entities_;
  }

  /// @brief 読み込んだ Entity のリストをムーブで取得する（所有権移転）
  /// @return Entity のリスト（呼び出し後、内部のリストは空になる）
  std::vector<std::shared_ptr<Entity>> TakeEntities() {
    return std::move(entities_);
  }

  /// @brief プレイヤースポーンデータのリストを取得する
  /// @return プレイヤースポーンデータのリスト
  const std::vector<PlayerSpawnData>& GetPlayerSpawns() const {
    return playerSpawns_;
  }

  /// @brief 敵スポーンデータのリストを取得する
  /// @return 敵スポーンデータのリスト
  const std::vector<EnemySpawnData>& GetEnemySpawns() const {
    return enemySpawns_;
  }

  /// @brief レベル側が「メインカメラ」を持っているか
  /// @details true のときは、シーン JSON 側のカメラより
  ///          レベル側のカメラを優先させたいことを意味する。
  ///          呼び出し元（DataDrivenScene）が既存カメラの isMain を降格させる判断に使う。
  /// @return main 指定のカメラがあれば true
  bool HasMainCamera() const { return hasMainCamera_; }

private:
  std::vector<std::shared_ptr<Entity>> entities_; ///< 読み込んだ Entity
  std::vector<PlayerSpawnData> playerSpawns_;     ///< 読み込んだプレイヤースポーン
  std::vector<EnemySpawnData> enemySpawns_;       ///< 読み込んだ敵スポーン
  std::string modelBaseDir_ = "Resources/model/"; ///< モデルファイルの基準ディレクトリ
  bool rotationInDegrees_ = false; ///< JSON の rotation が度数法か（既定はラジアン）
  bool hasMainCamera_ = false;     ///< レベル側にメインカメラ指定があったか

  // =================================================================
  // A-05: 共通ヘルパ
  // =================================================================

  /// @brief transform ブロックを TransformComponent へ流し込む
  /// @details MESH / LIGHT / CAMERA で同じ処理を使うため切り出した。
  ///          rotation は rotationInDegrees_ に従ってラジアンへ揃える。
  /// @param object    レベルデータのオブジェクト（transform が無ければ何もしない）
  /// @param transform 書き込み先の TransformComponent
  void ApplyTransform(const nlohmann::json& object, TransformComponent& transform) {
    if (!object.contains("transform")) return;
    const auto& tr = object["transform"];
    if (tr.contains("translation")) {
      transform.position.x = tr["translation"][0].get<float>();
      transform.position.y = tr["translation"][1].get<float>();
      transform.position.z = tr["translation"][2].get<float>();
    }
    if (tr.contains("rotation")) {
      const float k = rotationInDegrees_ ? (3.14159265358979323846f / 180.0f) : 1.0f;
      transform.rotation.x = tr["rotation"][0].get<float>() * k;
      transform.rotation.y = tr["rotation"][1].get<float>() * k;
      transform.rotation.z = tr["rotation"][2].get<float>() * k;
    }
    if (tr.contains("scaling")) {
      transform.scale.x = tr["scaling"][0].get<float>();
      transform.scale.y = tr["scaling"][1].get<float>();
      transform.scale.z = tr["scaling"][2].get<float>();
    }
  }

  /// @brief スポーン系（PlayerSpawn / EnemySpawn）の transform を読む
  /// @details ApplyTransform と同じ単位変換を通すためのラッパ。
  ///          スポーンは Entity を作らないので TransformComponent を経由できない。
  /// @param object         レベルデータのオブジェクト（transform が無ければ何もしない）
  /// @param outTranslation 位置の書き込み先
  /// @param outRotation    回転（ラジアン）の書き込み先
  void ReadSpawnTransform(const nlohmann::json& object,
                          RC::Vector3& outTranslation, RC::Vector3& outRotation) {
    if (!object.contains("transform")) return;
    const auto& tr = object["transform"];
    if (tr.contains("translation")) {
      outTranslation.x = tr["translation"][0].get<float>();
      outTranslation.y = tr["translation"][1].get<float>();
      outTranslation.z = tr["translation"][2].get<float>();
    }
    if (tr.contains("rotation")) {
      const float k = rotationInDegrees_ ? (3.14159265358979323846f / 180.0f) : 1.0f;
      outRotation.x = tr["rotation"][0].get<float>() * k;
      outRotation.y = tr["rotation"][1].get<float>() * k;
      outRotation.z = tr["rotation"][2].get<float>() * k;
    }
  }

  /// @brief オイラー角（ラジアン）から前方ベクトルを求める
  /// @details DataDrivenScene のシャドウパスが
  ///          pitch = asin(-dir.y) / yaw = atan2(dir.x, dir.z) で
  ///          向き→回転を求めているので、その逆変換にあたる。
  ///          こうしておけば JSON に direction が無くても
  ///          Blender の回転からライトの向きが復元できる。
  /// @param rotRad オイラー角（ラジアン）
  /// @return 前方ベクトル（単位ベクトル）
  static RC::Vector3 DirectionFromEuler(const RC::Vector3& rotRad) {
    const float cp = std::cos(rotRad.x);
    return {
      std::sin(rotRad.y) * cp,
      -std::sin(rotRad.x),
      std::cos(rotRad.y) * cp
    };
  }

  /// @brief color 配列（RGB でも RGBA でも可）を読む
  /// @param j        color 配列
  /// @param fallback 配列でない、または要素が 3 未満のときに返す値
  /// @return 読み込んだ色（RGB だけなら a = 1）
  static RC::Vector4 ReadColor(const nlohmann::json& j, const RC::Vector4& fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return {
      j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
      (j.size() >= 4) ? j[3].get<float>() : 1.0f
    };
  }

  // =================================================================
  // A-05: LIGHT
  // =================================================================
  // Blender のライトは light_type（SUN / POINT / SPOT / AREA）で種類が決まる。
  // 対応するライトコンポーネントを付けるだけで、ハンドル生成は
  // DataDrivenScene::InitializeRuntimeResources が面倒を見てくれる。

  /// @brief LIGHT オブジェクトからライトの Entity を作る（子も再帰的に読む）
  /// @param object     レベルデータのオブジェクト（disabled なら何もしない）
  /// @param parentGuid 親 Entity の GUID（0 なら親なし）
  void ParseLight(const nlohmann::json& object, uint64_t parentGuid) {
    if (object.contains("disabled") && object["disabled"].get<bool>()) return;

    std::string name = object.contains("name") ? object["name"].get<std::string>() : "Light";
    auto entity = std::make_shared<Entity>(name);
    if (parentGuid != 0) entity->SetParentGuid(parentGuid);

    auto& transform = entity->AddComponent<TransformComponent>();
    ApplyTransform(object, transform);

    // 向き：direction が明示されていればそれを使い、無ければ回転から導出する
    RC::Vector3 direction = DirectionFromEuler(transform.rotation);
    if (object.contains("direction") && object["direction"].size() == 3) {
      direction.x = object["direction"][0].get<float>();
      direction.y = object["direction"][1].get<float>();
      direction.z = object["direction"][2].get<float>();
    }

    const RC::Vector4 color =
        object.contains("color") ? ReadColor(object["color"], {1.0f, 1.0f, 1.0f, 1.0f})
                                 : RC::Vector4{1.0f, 1.0f, 1.0f, 1.0f};
    // Blender は energy(W)、エンジンは intensity。桁が違うのでレベル側で調整する前提。
    float intensity = 1.0f;
    if (object.contains("intensity")) intensity = object["intensity"].get<float>();
    else if (object.contains("energy")) intensity = object["energy"].get<float>();

    const std::string lightType =
        object.contains("light_type") ? object["light_type"].get<std::string>() : "SUN";

    if (lightType == "POINT") {
      auto& l = entity->AddComponent<PointLightComponent>();
      l.color = color;
      l.intensity = intensity;
      if (object.contains("radius")) l.radius = object["radius"].get<float>();
      if (object.contains("decay")) l.decay = object["decay"].get<float>();
    } else if (lightType == "SPOT") {
      auto& l = entity->AddComponent<SpotLightComponent>();
      l.color = color;
      l.intensity = intensity;
      l.direction = direction;
      // Blender 側には無い独自パラメータ。指定が無ければエンティティ中心のまま。
      if (object.contains("offset") && object["offset"].size() >= 3) {
        auto& o = object["offset"];
        l.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      }
      if (object.contains("distance")) l.distance = object["distance"].get<float>();
      if (object.contains("decay")) l.decay = object["decay"].get<float>();
      // Blender の spot_size は「円錐の全開き角(ラジアン)」なので半分にしてから cos を取る
      if (object.contains("spot_size")) {
        l.cosAngle = std::cos(object["spot_size"].get<float>() * 0.5f);
      } else if (object.contains("cosAngle")) {
        l.cosAngle = object["cosAngle"].get<float>();
      }
    } else if (lightType == "AREA") {
      auto& l = entity->AddComponent<AreaLightComponent>();
      l.color = color;
      l.intensity = intensity;
      if (object.contains("range")) l.range = object["range"].get<float>();
      if (object.contains("decay")) l.decay = object["decay"].get<float>();
      if (object.contains("size") && object["size"].size() >= 2) {
        // Blender の size は辺の長さ。エンジンは半分の長さを持つ。
        l.halfWidth  = object["size"][0].get<float>() * 0.5f;
        l.halfHeight = object["size"][1].get<float>() * 0.5f;
      }
      if (object.contains("two_sided")) l.twoSided = object["two_sided"].get<bool>();
    } else { // SUN / それ以外は平行光源として扱う
      auto& l = entity->AddComponent<DirectionalLightComponent>();
      l.color = color;
      l.intensity = intensity;
      l.direction = direction;
    }

    entities_.push_back(entity);
    ParseChildren(object, entity->Guid());
  }

  // =================================================================
  // A-05: CAMERA
  // =================================================================

  /// @brief CAMERA オブジェクトからカメラの Entity を作る（子も再帰的に読む）
  /// @param object     レベルデータのオブジェクト（disabled なら何もしない）
  /// @param parentGuid 親 Entity の GUID（0 なら親なし）
  void ParseCamera(const nlohmann::json& object, uint64_t parentGuid) {
    if (object.contains("disabled") && object["disabled"].get<bool>()) return;

    std::string name = object.contains("name") ? object["name"].get<std::string>() : "Camera";
    auto entity = std::make_shared<Entity>(name);
    if (parentGuid != 0) entity->SetParentGuid(parentGuid);

    auto& transform = entity->AddComponent<TransformComponent>();
    ApplyTransform(object, transform);

    auto& cam = entity->AddComponent<CameraComponent>();
    // Blender の angle / angle_y はラジアン。fovY を直接書いても良い。
    if (object.contains("fovY")) cam.fovY = object["fovY"].get<float>();
    else if (object.contains("angle_y")) cam.fovY = object["angle_y"].get<float>();
    else if (object.contains("angle")) cam.fovY = object["angle"].get<float>();

    if (object.contains("nearZ")) cam.nearZ = object["nearZ"].get<float>();
    else if (object.contains("clip_start")) cam.nearZ = object["clip_start"].get<float>();

    if (object.contains("farZ")) cam.farZ = object["farZ"].get<float>();
    else if (object.contains("clip_end")) cam.farZ = object["clip_end"].get<float>();

    // 既定は false。レベル側で main を明示したものだけをメインカメラ候補とする。
    // （シーン JSON 側のカメラを勝手に奪わないため）
    cam.isMain = object.contains("main") ? object["main"].get<bool>() : false;
    if (cam.isMain) hasMainCamera_ = true;

    entities_.push_back(entity);
    ParseChildren(object, entity->Guid());
  }

  /// @brief children を型ごとに振り分けて再帰する
  /// @param object     親オブジェクト（children が無ければ何もしない）
  /// @param parentGuid 親 Entity の GUID
  void ParseChildren(const nlohmann::json& object, uint64_t parentGuid) {
    if (!object.contains("children")) return;
    for (auto& child : object["children"]) {
      if (!child.contains("type")) continue;
      const std::string childType = child["type"].get<std::string>();
      if (childType == "MESH") {
        ParseObjectRecursive(child, parentGuid);
      } else if (childType == "LIGHT") {
        ParseLight(child, parentGuid);
      } else if (childType == "CAMERA") {
        ParseCamera(child, parentGuid);
      }
    }
  }

  /// @brief MESH オブジェクトから Entity を作る（子も再帰的に読む）
  /// @details file_name があれば ModelRendererComponent、collider があれば ColliderComponent を付ける。
  /// @param object     レベルデータのオブジェクト（disabled なら何もしない）
  /// @param parentGuid 親 Entity の GUID（0 なら親なし）
  void ParseObjectRecursive(const nlohmann::json& object, uint64_t parentGuid) {
    if (object.contains("disabled") && object["disabled"].get<bool>()) {
      return; // 無効フラグが立っている場合は読み込みをスキップする
    }

    std::string name = object.contains("name") ? object["name"].get<std::string>() : "Unnamed";
    auto entity = std::make_shared<Entity>(name);

    if (parentGuid != 0) {
      entity->SetParentGuid(parentGuid);
    }

    // --- TransformComponent ---
    auto& transform = entity->AddComponent<TransformComponent>();
    ApplyTransform(object, transform);

    // --- ModelRendererComponent ---
    if (object.contains("file_name")) {
      auto& renderer = entity->AddComponent<ModelRendererComponent>();
      std::string fileName = object["file_name"].get<std::string>();
      std::string baseName = fileName.substr(0, fileName.find_last_of('.'));
      renderer.modelPath = modelBaseDir_ + baseName + "/" + fileName;
    }

    // --- ColliderComponent ---
    if (object.contains("collider")) {
      const auto& col = object["collider"];
      auto& collider = entity->AddComponent<ColliderComponent>();

      if (col.contains("type")) {
        std::string colType = col["type"].get<std::string>();
        if (colType == "BOX") {
          collider.shape = ColliderComponent::Shape::AABB;
        } else if (colType == "SPHERE") {
          collider.shape = ColliderComponent::Shape::Sphere;
        }
      }
      if (col.contains("center")) {
        collider.center.x = col["center"][0].get<float>();
        collider.center.y = col["center"][1].get<float>();
        collider.center.z = col["center"][2].get<float>();
      }
      if (col.contains("size")) {
        collider.size.x = col["size"][0].get<float>();
        collider.size.y = col["size"][1].get<float>();
        collider.size.z = col["size"][2].get<float>();
      }
    }

    entities_.push_back(entity);

    // --- children ---
    // MESH の下にライトやカメラを吊るせるよう、振り分けは ParseChildren に集約した
    ParseChildren(object, entity->Guid());
  }
};
