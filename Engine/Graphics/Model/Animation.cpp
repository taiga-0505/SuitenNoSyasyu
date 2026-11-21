#include "Animation.h"
#include "Common/ResourcePath.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cassert>
#include <filesystem>
#include <map>
#include <utility>

namespace RC {

namespace {
/// @brief 内部共通: 指定インデックスのアニメーションをパースする
/// @param scene     assimp で読み込んだシーン（nullptr 可）
/// @param animIndex アニメーションインデックス（0 始まり）
/// @return 解析結果（scene が無効・範囲外なら空の Animation）
Animation ParseAnimation_(const aiScene* scene, int animIndex) {
    Animation animation;
    if (!scene || animIndex < 0 || static_cast<unsigned>(animIndex) >= scene->mNumAnimations) {
        return animation;
    }

    aiAnimation* animationAssimp = scene->mAnimations[animIndex];
    animation.duration = float(animationAssimp->mDuration / animationAssimp->mTicksPerSecond);

    // NodeAnimation を解析する（キーの時刻は tick から秒へ換算する）
    for (uint32_t channelIndex = 0; channelIndex < animationAssimp->mNumChannels; ++channelIndex) {
        aiNodeAnim* nodeAnimationAssimp = animationAssimp->mChannels[channelIndex];
        NodeAnimation& nodeAnimation = animation.nodeAnimations[nodeAnimationAssimp->mNodeName.C_Str()];

        // 平行移動
        for (uint32_t keyIndex = 0; keyIndex < nodeAnimationAssimp->mNumPositionKeys; ++keyIndex) {
            aiVectorKey& keyAssimp = nodeAnimationAssimp->mPositionKeys[keyIndex];
            KeyframeVector3 keyframe;
            keyframe.time = float(keyAssimp.mTime / animationAssimp->mTicksPerSecond);
            keyframe.value = { keyAssimp.mValue.x, keyAssimp.mValue.y, keyAssimp.mValue.z };
            nodeAnimation.translate.push_back(keyframe);
        }

        // 回転
        for (uint32_t keyIndex = 0; keyIndex < nodeAnimationAssimp->mNumRotationKeys; ++keyIndex) {
            aiQuatKey& keyAssimp = nodeAnimationAssimp->mRotationKeys[keyIndex];
            KeyframeQuaternion keyframe;
            keyframe.time = float(keyAssimp.mTime / animationAssimp->mTicksPerSecond);
            keyframe.value = { keyAssimp.mValue.x, keyAssimp.mValue.y, keyAssimp.mValue.z, keyAssimp.mValue.w };
            nodeAnimation.rotate.push_back(keyframe);
        }

        // 拡縮
        for (uint32_t keyIndex = 0; keyIndex < nodeAnimationAssimp->mNumScalingKeys; ++keyIndex) {
            aiVectorKey& keyAssimp = nodeAnimationAssimp->mScalingKeys[keyIndex];
            KeyframeVector3 keyframe;
            keyframe.time = float(keyAssimp.mTime / animationAssimp->mTicksPerSecond);
            keyframe.value = { keyAssimp.mValue.x, keyAssimp.mValue.y, keyAssimp.mValue.z };
            nodeAnimation.scale.push_back(keyframe);
        }
    }

    return animation;
}

/// @brief 解析済みアニメーションのキャッシュ
/// @details assimp の ReadFile はファイル I/O とパースを伴うため、クリップを切り替えるたびに
///          呼ぶとフレームが飛ぶ。同じ (パス, インデックス) は一度だけ解析して使い回す。
std::map<std::pair<std::string, int>, Animation> g_animationCache;

/// @brief アニメーション数のキャッシュ
std::map<std::string, int> g_animationCountCache;
} // namespace

Animation LoadAnimationFile(const std::string& filePath) {
    return LoadAnimationFile(filePath, 0);
}

Animation LoadAnimationFile(const std::string& filePath, int animIndex) {
    const auto key = std::make_pair(filePath, animIndex);
    auto it = g_animationCache.find(key);
    if (it != g_animationCache.end()) {
        return it->second;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(Chaso::ResolvePath(filePath).c_str(), aiProcess_MakeLeftHanded);
    Animation parsed = ParseAnimation_(scene, animIndex);

    // 失敗（空の Animation）もキャッシュする。毎フレーム再パースを試みさせないため
    g_animationCache.emplace(key, parsed);
    return parsed;
}

int GetAnimationCount(const std::string& filePath) {
    auto it = g_animationCountCache.find(filePath);
    if (it != g_animationCountCache.end()) {
        return it->second;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(Chaso::ResolvePath(filePath).c_str(), aiProcess_MakeLeftHanded);
    const int count = scene ? static_cast<int>(scene->mNumAnimations) : 0;
    g_animationCountCache.emplace(filePath, count);
    return count;
}

void ClearAnimationCache() {
    g_animationCache.clear();
    g_animationCountCache.clear();
}

Vector3 CalculateValue(const std::vector<KeyframeVector3>& keyframes, float time) {
    assert(!keyframes.empty()); // キーがないものは返す値がわからないのでダメ
    if (keyframes.empty()) {
        return Vector3{0.0f, 0.0f, 0.0f}; // 安全なフォールバック
    }
    if (keyframes.size() == 1 || time <= keyframes[0].time) {
        return keyframes[0].value;
    }

    for (size_t index = 0; index < keyframes.size() - 1; ++index) {
        size_t nextIndex = index + 1;
        if (keyframes[index].time <= time && time <= keyframes[nextIndex].time) {
            float t = (time - keyframes[index].time) / (keyframes[nextIndex].time - keyframes[index].time);
            return Lerp(keyframes[index].value, keyframes[nextIndex].value, t);
        }
    }

    return (*keyframes.rbegin()).value;
}

Quaternion CalculateValue(const std::vector<KeyframeQuaternion>& keyframes, float time) {
    assert(!keyframes.empty()); // キーがないものは返す値がわからないのでダメ
    if (keyframes.empty()) {
        return Quaternion{0.0f, 0.0f, 0.0f, 1.0f}; // 単位クオータニオン（安全なフォールバック）
    }
    if (keyframes.size() == 1 || time <= keyframes[0].time) {
        return keyframes[0].value;
    }

    for (size_t index = 0; index < keyframes.size() - 1; ++index) {
        size_t nextIndex = index + 1;
        if (keyframes[index].time <= time && time <= keyframes[nextIndex].time) {
            float t = (time - keyframes[index].time) / (keyframes[nextIndex].time - keyframes[index].time);
            return Slerp(keyframes[index].value, keyframes[nextIndex].value, t);
        }
    }

    return (*keyframes.rbegin()).value;
}

} // namespace RC
