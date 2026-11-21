#pragma once

// ModelManager
// -----------------------------------------------------------------------------
// 「モデル周り」の管理クラス
// - モデルはハンドル制（int）
// - 同じ .obj は Mesh を共有（shared_ptr のキャッシュ）
// - ModelObject 生成時に TextureManager を注入して mtl テクスチャも維持
// -----------------------------------------------------------------------------

#include <d3d12.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <future>

#include "struct.h"

class ModelObject;
class ModelMesh;

class TextureManager;   // 前方宣言
class SRVManager;       // 前方宣言
class PipelineManager;  // 前方宣言

namespace RC {

class FrameResource; // 前方宣言

/// @class ModelManager
/// @brief モデルリソースと描画オブジェクトをまとめて管理するマネージャクラス
/// @details モデルをハンドル（int）で管理し、同一ファイルのメッシュ共有や非同期ロードに対応する。
class ModelManager {
public:
  /// @brief デバイスとテクスチャマネージャを受け取って初期化する
  /// @param device D3D12 デバイス
  /// @param texman テクスチャマネージャ（マテリアル用テクスチャのロードに使う）
  void Init(ID3D12Device *device, TextureManager *texman);

  /// @brief CS スキニング用のパイプラインを設定する
  /// @param pm     パイプラインマネージャ
  /// @param srvMgr SRV マネージャ
  /// @note 設定後に Load したスキニングモデルに CS スキニングが自動で設定される。
  void SetSkinningCS(PipelineManager *pm, SRVManager *srvMgr);

  /// @brief デストラクタ
  ~ModelManager();

  /// @brief 終了処理。管理している全モデルとメッシュキャッシュを解放する
  void Term();

  /// @brief モデルファイルをロードしてハンドルを返す
  /// @details 既にロード済みのメッシュがある場合はキャッシュを再利用する。
  ///          メッシュの読み込みは別スレッドで行い、終わるまで ModelObject は準備中（IsReady が false）になる。
  /// @param path モデルファイルのパス（.obj など）
  /// @return モデルハンドル（Init 前なら -1）
  /// @note 同じパスでも呼ぶたびに新しいハンドルを返す（メッシュだけを共有する）。
  int Load(const std::string &path);

  /// @brief 指定したハンドルに対応するモデルを解放する
  /// @param handle モデルハンドル
  /// @note 実体の破棄は GPU が使い終わるまで遅らせる（DeferredReleaseQueue）。
  void Unload(int handle);

  /// @brief 有効なハンドルか確認する
  /// @param handle モデルハンドル
  /// @return 有効なら true
  bool IsValid(int handle) const;

  /// @brief ハンドルからモデルオブジェクトの実体を取得する
  /// @param handle モデルハンドル
  /// @return ModelObject へのポインタ。無効なハンドルの場合は nullptr
  ::ModelObject *Get(int handle);

  /// @brief ハンドルからモデルオブジェクトの実体を取得する（const 版）
  /// @param handle モデルハンドル
  /// @return ModelObject への const ポインタ。無効なハンドルの場合は nullptr
  const ::ModelObject *Get(int handle) const;

  /// @brief ハンドルからモデルのトランスフォーム情報へのポインタを取得する
  /// @param handle モデルハンドル
  /// @return Transform 構造体へのポインタ。無効なハンドルの場合は nullptr
  Transform *GetTransformPtr(int handle);

  /// @brief モデルの乗算カラーを設定する
  /// @param handle モデルハンドル
  /// @param color  乗算する RGBA カラー
  void SetColor(int handle, const Vector4 &color);

  /// @brief モデルの光沢度（Shininess）を設定する
  /// @param handle    モデルハンドル
  /// @param shininess 光沢度（0.0 で鏡面反射なし）
  void SetShininess(int handle, float shininess);

  /// @brief モデルのライティングモードを設定する
  /// @param handle モデルハンドル
  /// @param m      ライティングモード（Phong、Lambert、なし等）
  void SetLightingMode(int handle, LightingMode m);

  /// @brief ライティングモードの個別オーバーライドを解除し、
  ///        シーンの DirectionalLight のモードに追従させる
  /// @param handle モデルハンドル
  void ClearLightingModeOverride(int handle);

  /// @brief 指定したモデルハンドルのメッシュを別のファイルで差し替える
  /// @param handle モデルハンドル
  /// @param path   新しいモデルファイルのパス
  /// @note テクスチャは新しいメッシュの mtl のものに戻す。
  void SetMesh(int handle, const std::string &path);

  /// @brief バッチ描画用の内部カーソルをリセットする
  /// @details 同一モデルを複数箇所で描画する際のインスタンスカウント管理に使う。
  /// @param handle モデルハンドル
  void ResetCursor(int handle);

  /// @brief アニメーションをアタッチする（モデルに紐づけられたファイルパスを使う）
  /// @param handle モデルハンドル
  void AttachAnimation(int handle);

  /// @brief アニメーションを別ファイルからアタッチする
  /// @param handle   モデルハンドル
  /// @param filePath アニメーションファイルのパス
  void AttachAnimation(int handle, const std::string& filePath);

  /// @brief アニメーションを別ファイルからインデックス指定でアタッチする
  /// @param handle    モデルハンドル
  /// @param filePath  アニメーションファイルのパス
  /// @param animIndex アニメーションインデックス（0 始まり）
  void AttachAnimation(int handle, const std::string& filePath, int animIndex);

  /// @brief 別ファイルのアニメーションへクロスフェードでスムーズに切り替える
  /// @param handle        モデルハンドル
  /// @param filePath      新しいアニメーションファイルのパス
  /// @param blendDuration 切り替えにかけるブレンド時間（秒。既定 0.2 秒）
  void CrossfadeAnimation(int handle, const std::string& filePath, float blendDuration = 0.2f);

  /// @brief 別ファイルのインデックス指定アニメーションへクロスフェードでスムーズに切り替える
  /// @param handle        モデルハンドル
  /// @param filePath      アニメーションファイルのパス
  /// @param animIndex     アニメーションインデックス（0 始まり）
  /// @param blendDuration 切り替えにかけるブレンド時間（秒。既定 0.2 秒）
  void CrossfadeAnimation(int handle, const std::string& filePath, int animIndex, float blendDuration = 0.2f);

  /// @brief モデルのアニメーション状態を更新する
  /// @param handle モデルハンドル
  /// @param dt     経過時間（秒）
  void UpdateAnimation(int handle, float dt);

  /// @brief モデルにアタッチされているアニメーションの再生時間（秒）を取得する
  /// @param handle モデルハンドル
  /// @return 再生時間（秒。無効なハンドルなら 0）
  float GetAnimationDuration(int handle) const;

  /// @brief 管理している全モデルのバッチカーソルと CS スキニングの Dispatch 済みフラグをリセットする
  /// @details 毎フレームの描画開始前に呼ぶ想定。
  void ResetAllBatchCursors();

  /// @brief 管理している全モデルのうち、CS スキニングが有効かつ未実行のものをまとめて Dispatch する
  /// @param cl    コマンドリスト
  /// @param frame フレームリソース
  void DispatchAllSkinning(ID3D12GraphicsCommandList* cl, RC::FrameResource& frame);

private:
  /// @struct Slot
  /// @brief モデルオブジェクト保持用スロット
  struct Slot {
    std::unique_ptr<::ModelObject> ptr; ///< モデルオブジェクト実体
    bool inUse = false;                ///< 使用中フラグ
  };

  /// @brief 未使用スロットを検索・確保する
  /// @return 確保したスロットのインデックス（空きが無ければ末尾に追加する）
  /// @note mtx_ をロックした状態で呼ぶこと。
  int AllocSlot_();

  /// @brief メッシュをキャッシュから取得するか、新規にロードする
  /// @param path メッシュファイルのパス
  /// @return 共有メッシュオブジェクト（ロードに失敗したら nullptr）
  /// @note ロード中はロックを外すので、別スレッドから同時に呼んでもよい。
  std::shared_ptr<::ModelMesh> GetOrLoadMesh_(const std::string &path);

private:
  ID3D12Device *device_ = nullptr;     ///< デバイス
  TextureManager *texman_ = nullptr;   ///< テクスチャマネージャ

  std::vector<Slot> models_;           ///< モデルオブジェクトのリスト
  std::unordered_map<std::string, std::shared_ptr<::ModelMesh>> meshCache_; ///< メッシュの共有キャッシュ

  mutable std::recursive_mutex mtx_;   ///< 排他制御用ミューテックス
  std::unordered_map<std::string, std::shared_future<void>> loadingTasks_; ///< ロードタスクの管理

  // CS スキニング用
  PipelineManager *pipelineMgr_ = nullptr; ///< パイプラインマネージャ（SetSkinningCS で設定）
  SRVManager *srvMgr_ = nullptr;           ///< SRV マネージャ（SetSkinningCS で設定）
};

} // namespace RC
