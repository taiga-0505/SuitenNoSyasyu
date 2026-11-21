#pragma once

#include "Math/Math.h"
#include "Math/MathTypes.h"
#include <string>
#include <vector>
#include <map>

namespace RC {

/// @struct KeyframeVector3
/// @brief Vector3 値のキーフレーム
struct KeyframeVector3 {
    float time;        ///< 時刻（秒）
    RC::Vector3 value; ///< その時刻の値
};

/// @struct KeyframeQuaternion
/// @brief Quaternion 値のキーフレーム
struct KeyframeQuaternion {
    float time;           ///< 時刻（秒）
    RC::Quaternion value; ///< その時刻の値
};

/// @struct NodeAnimation
/// @brief 1 ノード分の平行移動・回転・拡縮のキーフレーム列
struct NodeAnimation {
    std::vector<KeyframeVector3> translate;  ///< 平行移動のキーフレーム
    std::vector<KeyframeQuaternion> rotate;  ///< 回転のキーフレーム
    std::vector<KeyframeVector3> scale;      ///< 拡縮のキーフレーム
};

/// @struct Animation
/// @brief 1 クリップ分のアニメーション（ノード名ごとのキーフレーム列）
struct Animation {
    // 解析に失敗した場合はここが 0 のまま返るため、必ず初期化しておくこと
    float duration = 0.0f; ///< アニメーション全体の尺（秒）
    std::map<std::string, NodeAnimation> nodeAnimations; ///< ノード名 → そのノードのアニメーション
};

/// @brief アニメーションファイルを読み込む（最初のアニメーションを使用）
/// @param filePath ファイルパス
/// @return 解析された Animation 構造体
/// @note 結果は（パス, インデックス）をキーにキャッシュされる。詳細は下記オーバーロードを参照。
Animation LoadAnimationFile(const std::string& filePath);

/// @brief アニメーションファイルからインデックス指定でアニメーションを読み込む
/// @param filePath  ファイルパス
/// @param animIndex アニメーションインデックス（0 始まり）
/// @return 解析された Animation 構造体
/// @details 解析結果は（パス, インデックス）をキーにキャッシュされる。
///          2 回目以降は assimp によるファイル読み込みが起きないため、
///          「歩き→攻撃」のようなクリップ切り替えを毎フレーム判定しても
///          ディスクアクセスによるヒッチが出ない。
///          解析に失敗した場合も空の結果をキャッシュする（毎回リトライさせないため）。
/// @note スレッドセーフではない。描画スレッドからのみ呼ぶこと。
Animation LoadAnimationFile(const std::string& filePath, int animIndex);

/// @brief アニメーションファイル内のアニメーション数を取得する
/// @param filePath ファイルパス
/// @return アニメーション数（ファイルが無効なら 0）
/// @note 結果はパスをキーにキャッシュされる。
int GetAnimationCount(const std::string& filePath);

/// @brief アニメーションのキャッシュを全て破棄する
/// @details ファイルを差し替えて再読み込みさせたい場合に呼ぶ。
///          常駐させたくない場合はシーン遷移時に呼んでもよいが、
///          遷移のたびに再パースが走るため通常は保持したままでよい。
void ClearAnimationCache();

/// @brief Vector3 のキーフレームから指定時刻の値を線形補間で求める
/// @param keyframes キーフレーム配列（空不可）
/// @param time      時刻（秒）
/// @return 補間された Vector3（範囲外は先頭／末尾の値）
Vector3 CalculateValue(const std::vector<KeyframeVector3>& keyframes, float time);

/// @brief Quaternion のキーフレームから指定時刻の値を球面線形補間で求める
/// @param keyframes キーフレーム配列（空不可）
/// @param time      時刻（秒）
/// @return 補間された Quaternion（範囲外は先頭／末尾の値）
Quaternion CalculateValue(const std::vector<KeyframeQuaternion>& keyframes, float time);

} // namespace RC
