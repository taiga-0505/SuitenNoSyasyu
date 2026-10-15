#pragma once
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "Math/Math.h"
#include "function/function.h" // CreateBufferResource
#include "struct.h"

/// @class Primitive2D
/// @brief 画面空間（2D）での簡易図形描画を管理するクラス
/// @details 線、矩形、円、三角形などのプリミティブ図形を、ピクセル座標指定で描画する。
///          定数バッファをリングバッファとして管理し、1 フレームに複数の図形をまとめて描画できる。
class Primitive2D {
public:
  /// @brief コンストラクタ（GPU リソースは Initialize で作る）
  Primitive2D() = default;
  /// @brief デストラクタ（GPU リソースを解放する）
  ~Primitive2D();

  /// @brief 定数バッファ・頂点バッファを作成し、初期パラメータを設定する
  /// @param device  D3D12 デバイス
  /// @param screenW 初期画面幅（ピクセル）
  /// @param screenH 初期画面高さ（ピクセル）
  /// @note 既にリソースがある場合は解放してから作り直す。
  void Initialize(ID3D12Device *device, float screenW, float screenH);

  /// @brief 描画先のスクリーンサイズを更新する
  /// @param w 幅（ピクセル）
  /// @param h 高さ（ピクセル）
  void SetScreenSize(float w, float h);

  /// @brief 描画に使用するテクスチャ（スプライト用など）を設定する
  /// @param srv テクスチャの GPU ハンドル
  void SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE srv) { srv_ = srv; }

  // ---- 図形の設定 ----

  /// @brief 次の Draw で描く図形を線にする
  /// @param p0        開始座標（ピクセル）
  /// @param p1        終了座標（ピクセル）
  /// @param thickness 線の太さ（ピクセル）
  /// @param color     描画色
  /// @param feather   境界のぼかし強度（1.0 で通常）
  void SetLine(const RC::Vector2 &p0, const RC::Vector2 &p1, float thickness,
               const RC::Vector4 &color, float feather = 1.0f);

  /// @brief 次の Draw で描く図形を矩形にする
  /// @param mn        左上座標（ピクセル）
  /// @param mx        右下座標（ピクセル）
  /// @param fillMode  塗りつぶしモード（kWire なら輪郭のみ）
  /// @param thickness 輪郭描画時の線の太さ
  /// @param color     描画色
  /// @param feather   境界のぼかし強度
  void SetRect(const RC::Vector2 &mn, const RC::Vector2 &mx,
               kFillMode fillMode, float thickness,
               const RC::Vector4 &color, float feather = 1.0f);

  /// @brief 次の Draw で描く図形を円にする
  /// @param c         中心座標（ピクセル）
  /// @param r         半径（ピクセル）
  /// @param fillMode  塗りつぶしモード（kWire なら輪郭のみ）
  /// @param thickness 輪郭描画時の線の太さ
  /// @param color     描画色
  /// @param feather   境界のぼかし強度
  void SetCircle(const RC::Vector2 &c, float r, kFillMode fillMode,
                 float thickness, const RC::Vector4 &color,
                 float feather = 1.0f);

  /// @brief 次の Draw で描く図形を三角形にする
  /// @param p0        頂点 0（ピクセル）
  /// @param p1        頂点 1（ピクセル）
  /// @param p2        頂点 2（ピクセル）
  /// @param fillMode  塗りつぶしモード（kWire なら輪郭のみ）
  /// @param thickness 輪郭描画時の線の太さ
  /// @param color     描画色
  /// @param feather   境界のぼかし強度
  void SetTriangle(const RC::Vector2 &p0, const RC::Vector2 &p1,
                   const RC::Vector2 &p2, kFillMode fillMode,
                   float thickness, const RC::Vector4 &color,
                   float feather = 1.0f);

  /// @brief 次の Draw で描く図形をスプライト（テクスチャ付き矩形）にする
  /// @param mn    左上座標（ピクセル）
  /// @param mx    右下座標（ピクセル）
  /// @param color テクスチャに乗算する色
  /// @param uvMin UV 左上
  /// @param uvMax UV 右下
  /// @note テクスチャは SetTexture で設定したものを使う。
  void SetSpriteRect(const RC::Vector2 &mn, const RC::Vector2 &mx,
                     const RC::Vector4 &color,
                     const RC::Vector2 &uvMin = {0, 0},
                     const RC::Vector2 &uvMax = {1, 1});

  /// @brief フレームの開始を宣言し、定数バッファの書き込み先を次のフレーム領域へ切り替える
  /// @note 毎フレーム、最初の Draw より前に呼ぶ。
  void BeginFrame();

  /// @brief 直前に Set〜 で設定した図形を 1 つ描画する
  /// @param cmdList コマンドリスト
  void Draw(ID3D12GraphicsCommandList *cmdList);

private:
  /// @brief GPU リソースを解放する
  void Release();

public:
  /// @struct Vertex
  /// @brief 2D 描画用の頂点構造体
  struct Vertex {
    RC::Vector4 pos; ///< 座標（クリップ空間）
    RC::Vector2 uv;  ///< テクスチャ座標
  };

private:
  /// @struct VB
  /// @brief 頂点バッファリソース管理用
  struct VB {
    Microsoft::WRL::ComPtr<ID3D12Resource> res; ///< 頂点バッファ
    D3D12_VERTEX_BUFFER_VIEW view{};            ///< 頂点バッファビュー
  };

  /// @struct CB
  /// @brief 定数バッファリソース管理用テンプレート
  template <class T> struct CB {
    Microsoft::WRL::ComPtr<ID3D12Resource> res; ///< 定数バッファ
    T *map = nullptr;                           ///< マップ先の CPU アドレス
  };

  /// @struct UInt4
  /// @brief シェーダーの uint4 に対応する 4 要素の符号なし整数
  struct UInt4 {
    uint32_t x, y, z, w;
  };

  /// @struct Params
  /// @brief シェーダーに送る図形パラメータ構造体（16 バイトアライメント必須）
  struct alignas(16) Params {
    RC::Vector4 A;       ///< 線・三角形の頂点（xy = p0、zw = p1）
    RC::Vector4 B;       ///< 矩形・スプライトの範囲（xy = 左上、zw = 右下）。三角形では xy = p2
    RC::Vector4 C;       ///< 画面サイズと線の設定（xy = 画面幅・高さ、z = 太さ、w = ぼかし）
    RC::Vector4 D;       ///< 円の中心と半径（xy = 中心、z = 半径）
    UInt4 U;             ///< x = 形状（SHAPE_*）、y = フラグ（FLAG_*）
    RC::Vector4 Color;   ///< 描画色
    RC::Vector4 UVRect;  ///< スプライト用 UV 範囲（xy = 左上、zw = 右下）
  };

  /// @struct DummyVS
  /// @brief 頂点シェーダー用のダミー定数バッファ（単位行列を入れておくだけ）
  struct alignas(16) DummyVS {
    RC::Matrix4x4 World; ///< ワールド行列（単位行列）
    RC::Matrix4x4 WVP;   ///< WVP 行列（単位行列）
  };

  /// @enum ShapeType
  /// @brief 形状識別用の定数
  enum : uint32_t {
    SHAPE_LINE = 0,     ///< 線
    SHAPE_RECT = 1,     ///< 矩形
    SHAPE_CIRCLE = 2,   ///< 円
    SHAPE_SPRITE = 3,   ///< スプライト
    SHAPE_TRIANGLE = 4  ///< 三角形
  };

  /// @enum Flags
  /// @brief 描画オプションフラグ（FLAG_STROKE = 輪郭のみ、FLAG_TEX = テクスチャを使う）
  enum : uint32_t { FLAG_STROKE = 1, FLAG_TEX = 2 };

  static constexpr uint32_t kMaxDrawPerFrame = 4096; ///< 1 フレームあたりの最大描画数
  /// @brief CB リングをフレームごとに分ける数（GPU が読み終わる前に次フレームが上書きしないように）
  /// @details Dx12Core は最大 2 フレーム先行する（Present 後に次のバックバッファ分だけ待つ）。
  ///          FontManager / FrameResource と同じく 3 にしておく。
  static constexpr uint32_t kFrameCount = 3;
  /// @brief 256 バイト境界に切り上げる（CBV のサイズ制約のため）
  /// @param v 切り上げ前のサイズ（バイト）
  /// @return 256 の倍数に切り上げたサイズ
  static constexpr uint32_t Align256(uint32_t v) { return (v + 255u) & ~255u; }

  Params paramsCPU_{}; ///< CPU 側のパラメータキャッシュ（Set〜 で書き、Draw で CB へコピーする）

  // リング化した CB（一括描画用）
  Microsoft::WRL::ComPtr<ID3D12Resource> cbParamsRes_; ///< 全フレーム分の Params を並べた定数バッファ
  uint8_t *cbParamsMap_ = nullptr;                     ///< cbParamsRes_ のマップ先
  uint32_t cbStride_ = 0;                              ///< 1 スロットのサイズ（256 バイト境界）
  uint32_t cbCursor_ = 0;      ///< 現フレーム領域の中での書き込み位置（0〜kMaxDrawPerFrame-1）
  uint32_t frameSlot_ = 0;     ///< 現在書き込んでいるフレーム領域（0〜kFrameCount-1）

  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< D3D12 デバイス
  float screenW_ = 0.0f; ///< 画面幅（ピクセル）
  float screenH_ = 0.0f; ///< 画面高さ（ピクセル）

  VB vb_{};               ///< フルスクリーン三角形の頂点バッファ
  CB<Params> cbParams_{}; ///< 単発用の Params 定数バッファ（現在は cbParamsRes_ を使う）
  CB<DummyVS> cbDummy_{}; ///< 頂点シェーダー用ダミー定数バッファ

  D3D12_GPU_DESCRIPTOR_HANDLE srv_{}; ///< 現在バインドされている SRV
  bool visible_ = true;              ///< false なら Draw で何もしない
};
