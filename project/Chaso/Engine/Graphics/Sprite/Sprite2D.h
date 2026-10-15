#pragma once
#include <cstdint>
#include <d3d12.h>
#include <d3d12.h>
#include <memory>
#include <string>
#include <iostream>
#include <wrl/client.h>

#include "Math/Math.h"
#include "Render/FrameResource.h" // RC::DynamicCB
#include "function/function.h"
#include "imgui/imgui.h"

#include "SpriteMesh2D.h"

/// @class Sprite2D
/// @brief 2D スプライト（テクスチャ付き矩形）の描画と管理を行うクラス
/// @details 共有メッシュ（SpriteMesh2D）を使用し、個別のトランスフォームやマテリアル（カラー、UV 変換）を保持して描画する。
class Sprite2D {
public:
  /// @brief コンストラクタ（初期化は Initialize で行う）
  Sprite2D() = default;
  /// @brief デストラクタ（共有メッシュの参照を手放す）
  ~Sprite2D();

  /// @brief 定数バッファの初期値と正射影行列を設定し、共有メッシュを関連付ける
  /// @param device       D3D12 デバイス
  /// @param mesh         共有頂点データ（SpriteMesh2D）の shared_ptr
  /// @param screenWidth  画面幅（行列計算用）
  /// @param screenHeight 画面高さ（行列計算用）
  void Initialize(ID3D12Device *device,
                  const std::shared_ptr<SpriteMesh2D> &mesh, float screenWidth,
                  float screenHeight);

  /// @brief トランスフォームからワールド行列・WVP 行列を再計算する
  /// @note 値は CPU 側に書くだけで、GPU への転送は Draw でのバインド時に行われる。
  void Update();

  /// @brief 描画する
  /// @param cmdList コマンドリスト
  /// @note 非表示・テクスチャ未設定・メッシュ未準備のときは何もしない。
  void Draw(ID3D12GraphicsCommandList *cmdList) const;

  // ===== 設定 / 取得 =====

  /// @brief 使用するテクスチャ（SRV）を設定する
  /// @param srv テクスチャの GPU ハンドル
  void SetTexture(D3D12_GPU_DESCRIPTOR_HANDLE srv) { srv_ = srv; }

  /// @brief 使用するテクスチャ（SRV）を取得する
  /// @return テクスチャの GPU ハンドル
  const D3D12_GPU_DESCRIPTOR_HANDLE &GetTexture() const { return srv_; }

  /// @brief 画面サイズを更新し、正射影行列を作り直す
  /// @param w 幅（ピクセル）
  /// @param h 高さ（ピクセル）
  void SetScreenSize(float w, float h);

  /// @brief スプライトのサイズ（ピクセル）を設定する
  /// @param w 幅
  /// @param h 高さ
  /// @note ワールド空間モード中は無視される（大きさは Transform.scale で決まるため）。
  void SetSize(float w, float h);

  /// @brief ワールド空間（3D）モードの ON / OFF
  /// @details ON にすると、正射影ではなくカメラの view / proj を使って描画される。
  ///          クアッドは原点中心・1×1・Y 上向きになり、Transform の
  ///          translation / rotation / scale がそのままワールド座標として効く。
  /// @param v true でワールド空間モード
  void SetWorldSpace(bool v) { worldSpace_ = v; }

  /// @brief ワールド空間モードか
  /// @return ワールド空間モードなら true
  bool IsWorldSpace() const { return worldSpace_; }

  /// @brief ワールド空間モードで使うカメラ行列を設定する
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  void SetCamera(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj) {
    camView_ = view;
    camProj_ = proj;
  }

  /// @brief 可視状態を設定する
  /// @param v true で表示
  void SetVisible(bool v) { visible_ = v; }

  /// @brief 可視状態か確認する
  /// @return 可視なら true
  bool Visible() const { return visible_; }

  /// @brief スプライトのカラー（乗算色）を設定する
  /// @param color RGBA カラー
  void SetColor(const RC::Vector4 &color) {
    cbMat_.dyn.Ptr()->color = RC::Vector4(color.x, color.y, color.z, color.w);
  }

  /// @brief スプライトのカラー（乗算色）を取得する
  /// @return RGBA カラー
  RC::Vector4 GetColor() { return cbMat_.dyn.Ptr()->color; }

  /// @brief トランスフォーム情報を取得する（読み書き可能）
  /// @return Transform への参照
  Transform &T() { return transform_; }

  /// @brief マテリアル情報を取得する（読み書き可能）
  /// @return SpriteMaterial 構造体へのポインタ
  SpriteMaterial *Mat() { return cbMat_.dyn.Ptr(); }

  /// @brief UV 変換行列を取得する（読み書き可能）
  /// @return UV 変換行列への参照
  RC::Matrix4x4 &UVTransform() { return cbMat_.dyn.Ptr()->uvTransform; }

  /// @brief 関連付けられたファイルパスを保存する（アセット管理用）
  /// @param path ファイルパス
  void SetFilePath(const std::string &path) { filePath_ = path; }

  /// @brief 関連付けられたファイルパスを取得する
  /// @return ファイルパス文字列
  const std::string &GetFilePath() const { return filePath_; }

private:
  /// @struct CBW
  /// @brief 行列用定数バッファ管理構造体
  struct CBW {
    RC::DynamicCB<TransformationMatrix> dyn; ///< 値は CPU 側。バインド時に今フレームの領域へ送る（CPU/GPU 並行のため）
  };

  /// @struct CBM
  /// @brief マテリアル用定数バッファ管理構造体
  struct CBM {
    RC::DynamicCB<SpriteMaterial> dyn; ///< 値は CPU 側。バインド時に今フレームの領域へ送る（CPU/GPU 並行のため）
  };

  /// @brief リソースの解放処理（共有メッシュの参照を手放す）
  void Release();

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< D3D12 デバイス

  std::shared_ptr<SpriteMesh2D> mesh_; ///< 共有メッシュ（頂点／インデックスバッファ）

  float screenW_ = 0.0f; ///< 画面幅（ピクセル）
  float screenH_ = 0.0f; ///< 画面高さ（ピクセル）

  CBW cbWVP_{}; ///< 行列用定数バッファ
  CBM cbMat_{}; ///< マテリアル用定数バッファ

  D3D12_GPU_DESCRIPTOR_HANDLE srv_{}; ///< 描画するテクスチャの SRV（0 なら描画しない）

  RC::Matrix4x4 view_{}; ///< スクリーン用ビュー行列（単位行列）
  RC::Matrix4x4 proj_{}; ///< スクリーン用の正射影行列（左上原点）

  bool worldSpace_ = false;   ///< true でワールド空間（3D）モード
  RC::Matrix4x4 camView_{};   ///< ワールド空間モードで使うビュー行列
  RC::Matrix4x4 camProj_{};   ///< ワールド空間モードで使うプロジェクション行列

  Transform transform_{{100, 100, 1}, {0, 0, 0}, {0, 0, 0}}; ///< スプライトの座標・回転・サイズ

  bool visible_ = true;  ///< false なら描画しない
  std::string filePath_; ///< 関連付けられたファイルパス（アセット管理用）
};
