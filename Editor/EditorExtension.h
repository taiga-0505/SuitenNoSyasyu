#pragma once

// ============================================================================
// EditorExtension — Application 側からエディタへ機能を登録する口
// ----------------------------------------------------------------------------
// エディタ（project/Chaso/Editor）はエンジンの一部なので、特定のゲームの型や
// スクリプトを知ってはいけない。ゲーム専用のデバッグ機能は Application 側に書き、
// ここへ登録してエディタから呼んでもらう。
//
// 登録できるもの
//   ・パネル … Window メニューに並び、開いている間 ImGui で描く
//
// 使い方（Application 側の .cpp）
//
//   #include "EditorExtension.h"
//
//   CHASO_EDITOR_EXTENSION(MyGameEditor) {
//     EditorExtension::AddPanel("Game Debug", [](Scene* scene) {
//       ImGui::Text("...");
//     });
//   }
//
// 登録は静的初期化（main より前）で行われる。保存先は関数内 static なので
// 初期化順の問題は起きない。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

class Scene;

namespace EditorExtension {

/// @struct Panel
/// @brief エディタのパネル 1 件
struct Panel {
  std::string name;                        ///< ウィンドウ名（Window メニューにも出る）
  std::function<void(Scene *scene)> draw;  ///< ImGui::Begin 〜 End の中身を描く
  bool open = false;                       ///< 開いているか
};

/// @brief パネルを追加する
/// @param name          ウィンドウ名
/// @param draw          中身を描く関数
/// @param openByDefault 起動時に開いておくか
void AddPanel(std::string name, std::function<void(Scene *scene)> draw,
              bool openByDefault = false);

/// @brief 登録済みのパネル（登録順）
/// @return パネル一覧への参照（関数内 static なので静的初期化の順序に依存しない）
std::vector<Panel> &Panels();

} // namespace EditorExtension

/// @brief Application 側でエディタ拡張を登録するためのマクロ
/// @details 直後に関数本体を書く。本体は静的初期化で 1 回だけ実行される。
#define CHASO_EDITOR_EXTENSION(Name)                                   \
  static void ChasoEditorExtension_##Name();                           \
  namespace {                                                          \
  struct ChasoEditorExtensionRegistrar_##Name {                        \
    ChasoEditorExtensionRegistrar_##Name() {                           \
      ChasoEditorExtension_##Name();                                   \
    }                                                                  \
  };                                                                   \
  ChasoEditorExtensionRegistrar_##Name g_chasoEditorExtension_##Name;  \
  }                                                                    \
  static void ChasoEditorExtension_##Name()
