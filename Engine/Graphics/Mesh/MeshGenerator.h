#pragma once
#include "struct.h"
#include <vector>

namespace RC {

/// @class MeshGenerator
/// @brief 基本的なプリミティブ形状のメッシュデータを生成する静的クラス
class MeshGenerator {
public:
  /// @brief 平面メッシュ（XZ 平面）を生成する
  /// @param width     幅
  /// @param height    奥行き
  /// @param segmentsW 横方向の分割数
  /// @param segmentsH 縦方向の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GeneratePlane(float width = 1.0f, float height = 1.0f,
                                 uint32_t segmentsW = 1, uint32_t segmentsH = 1);

  /// @brief 直方体メッシュを生成する
  /// @param width  幅
  /// @param height 高さ
  /// @param depth  奥行き
  /// @return 生成されたメッシュデータ
  static ModelData GenerateBox(float width = 1.0f, float height = 1.0f,
                               float depth = 1.0f);

  /// @brief 円盤メッシュ（XZ 平面）を生成する
  /// @param radius   半径
  /// @param segments 円周の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateCircle(float radius = 1.0f, uint32_t segments = 32);

  /// @brief リング状（ドーナツ状）の円板メッシュ（XZ 平面）を生成する
  /// @param innerRadius 内半径
  /// @param outerRadius 外半径
  /// @param segments    円周の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateRing(float innerRadius = 0.5f,
                                float outerRadius = 1.0f,
                                uint32_t segments = 32);

  /// @brief 拡張リングメッシュを生成する
  /// @details 角度指定や UV の方向、生成する平面などを指定できる詳細版。
  /// @param innerRadius  内半径
  /// @param outerRadius  外半径
  /// @param segments     円周の分割数
  /// @param innerColor   内側の頂点カラー
  /// @param outerColor   外側の頂点カラー
  /// @param startAngle   開始角度（度）
  /// @param endAngle     終了角度（度）
  /// @param isVerticalUV UV を縦方向に生成するか
  /// @param isXY         true なら XY 平面、false なら XZ 平面に生成
  /// @return 生成されたメッシュデータ
  /// @note VertexData に色を持たないため、innerColor / outerColor は現状使われない。
  ///       内外の区別は UV（内側が 1、外側が 0）でシェーダ側が行う。
  static ModelData GenerateRingEx(float innerRadius = 0.5f,
                                  float outerRadius = 1.0f,
                                  uint32_t segments = 32,
                                  const Vector4 &innerColor = {1, 1, 1, 1},
                                  const Vector4 &outerColor = {1, 1, 1, 1},
                                  float startAngle = 0.0f,
                                  float endAngle = 360.0f,
                                  bool isVerticalUV = false,
                                  bool isXY = false);

  /// @brief 球体メッシュを生成する
  /// @param radius 半径
  /// @param slices 経度方向の分割数（縦）
  /// @param stacks 緯度方向の分割数（横）
  /// @return 生成されたメッシュデータ
  static ModelData GenerateSphere(float radius = 1.0f, uint32_t slices = 32,
                                  uint32_t stacks = 16);

  /// @brief 円柱メッシュを生成する（上下の蓋あり）
  /// @param radius   半径
  /// @param height   高さ
  /// @param segments 円周の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateCylinder(float radius = 0.5f, float height = 1.0f,
                                    uint32_t segments = 32);

  /// @brief エフェクト用円柱メッシュを生成する
  /// @details 上下の蓋がなく、UV 展開や傾斜（上下半径の差）などを調整できるエフェクト向けの生成関数。
  ///          底面が Y=0、上面が Y=height になる。
  /// @param topRadius    上面の半径
  /// @param bottomRadius 底面の半径
  /// @param height       高さ
  /// @param segments     円周の分割数
  /// @param startAngle   開始角度（度）
  /// @param endAngle     終了角度（度）
  /// @param isVerticalUV UV を縦方向に生成するか
  /// @param flipV        V 座標を反転させるか
  /// @return 生成されたメッシュデータ
  /// @note インデックスは生成せず、三角形リストとして頂点を並べる（パーティクル描画用）。
  static ModelData GenerateEffectCylinder(
      float topRadius = 1.0f, float bottomRadius = 1.0f, float height = 3.0f,
      uint32_t segments = 32, float startAngle = 0.0f, float endAngle = 360.0f,
      bool isVerticalUV = true, bool flipV = false);

  /// @brief 円錐メッシュを生成する（底面の蓋あり）
  /// @param radius   底面の半径
  /// @param height   高さ
  /// @param segments 底面の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateCone(float radius = 0.5f, float height = 1.0f,
                                uint32_t segments = 32);

  /// @brief トーラス（円環）メッシュを生成する
  /// @param majorRadius   主半径（中心から管の中心まで）
  /// @param minorRadius   副半径（管自体の半径）
  /// @param majorSegments 主半径方向の分割数
  /// @param minorSegments 副半径方向の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateTorus(float majorRadius = 1.0f,
                                 float minorRadius = 0.2f,
                                 uint32_t majorSegments = 32,
                                 uint32_t minorSegments = 16);

  /// @brief カプセルメッシュを生成する
  /// @param radius 半径
  /// @param height 高さ（球体部分を含む全高）
  /// @param slices 経度方向の分割数
  /// @param stacks 緯度方向の分割数
  /// @return 生成されたメッシュデータ
  static ModelData GenerateCapsule(float radius = 0.5f, float height = 2.0f,
                                   uint32_t slices = 32, uint32_t stacks = 16);

  /// @brief 単一の三角形メッシュを生成する
  /// @param v0 第 1 頂点
  /// @param v1 第 2 頂点
  /// @param v2 第 3 頂点
  /// @return 生成されたメッシュデータ
  static ModelData GenerateTriangle(const Vector3 &v0, const Vector3 &v1,
                                    const Vector3 &v2);
};

} // namespace RC
