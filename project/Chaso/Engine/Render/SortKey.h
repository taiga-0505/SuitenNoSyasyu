#pragma once

// ============================================================================
// SortKey
// ----------------------------------------------------------------------------
// 描画コマンドの並び順を決める 64bit ソートキー。
// 同じ PSO / テクスチャのコマンドが連続するように並べ、ステート変更を減らす。
// ============================================================================

#include <cstdint>
#include <string_view>

namespace RC {

/// @namespace RC::SortKey
/// @brief 描画順序を決める 64bit ソートキーの構築用名前空間
/// @details キー構成: [63..56] レイヤー, [55..40] PSO ハッシュ, [39..24] テクスチャハッシュ, [23..0] 深度。
namespace SortKey {

/// @enum Layer
/// @brief 描画レイヤー（ソートキーの最上位 8bit。小さいほど先に描く）
enum Layer : uint8_t {
  kLayerOpaque = 0,      ///< 不透明
  kLayerTranslucent = 1, ///< 半透明
  kLayerGlass = 2,       ///< ガラス・加算等
  kLayerOverlay = 3,     ///< オーバーレイ（ギズモなど常に最後に描画）
};

/// @brief PSO のプレフィックス文字列から 16bit ハッシュを生成する
/// @param prefix PSO の識別文字列
/// @return 16bit ハッシュ値
inline uint16_t HashPSO(std::string_view prefix) {
  // FNV-1a（32bit）の結果を、上位 16bit と下位 16bit の XOR で 16bit に畳む
  uint32_t h = 2166136261u;
  for (char c : prefix) {
    h ^= static_cast<uint32_t>(c);
    h *= 16777619u;
  }
  return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFF));
}

/// @brief 各要素を組み合わせて 64bit ソートキーを生成する
/// @param layer   描画レイヤー
/// @param psoHash PSO ハッシュ
/// @param texHash テクスチャハッシュ
/// @param depth24 24bit 精度の深度値（前から順・後ろから順などの制御用。上位 8bit は捨てる）
/// @return 64bit ソートキー
inline uint64_t Make(Layer layer, uint16_t psoHash, uint16_t texHash,
                     uint32_t depth24 = 0) {
  return (uint64_t(layer) << 56) | (uint64_t(psoHash) << 40) |
         (uint64_t(texHash) << 24) | uint64_t(depth24 & 0x00FFFFFF);
}

} // namespace SortKey

} // namespace RC
