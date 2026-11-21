#include "KeyBindingsHelp.h"

#include "Common/EngineConfig.h"

#if RC_ENABLE_IMGUI

#include "imgui/imgui.h"

namespace {

/// @struct Binding
/// @brief 一覧の 1 行（キーと動作）
struct Binding {
  const char *key;    ///< キー（表示用の文字列）
  const char *action; ///< 動作の説明
  const char *when; ///< 有効になる条件（空なら常時）
};

/// @struct Group
/// @brief 見出し 1 つ分のキー一覧
struct Group {
  const char *title;    ///< 見出し
  const char *note;     ///< 見出しの下に出す補足（空なら出さない）
  const Binding *items; ///< 行の配列
  int count;            ///< 行数
};

// --- デバッグキー（Debug / Development ビルドのみ。配布用 Release では効かない） ---
const Binding kDebug[] = {
    {"F1", "ゲームカメラ / デバッグカメラの切り替え", "再生中"},
    {"F2", "スクリーンショット", ""},
    {"F3", "コライダーを全部表示", ""},
    {"F4", "デバッグ描画を全部表示", ""},
    {"F10", "録画（MP4）の開始 / 停止。撮影モード中ならゲーム画面だけが写る", ""},
    {"F11", "撮影モード（ゲーム画面だけを全画面にする）の開始 / 終了", ""},
};

// --- エディタのショートカット ---
const Binding kEditor[] = {
    {"Ctrl+S", "シーンを保存（未保存の Scene Flow も一緒に保存）", "編集モード"},
    {"Ctrl+Z", "元に戻す", "編集モード・文字入力中以外"},
    {"Ctrl+Y / Ctrl+Shift+Z", "やり直し", "編集モード・文字入力中以外"},
    {"Ctrl+C / Ctrl+V", "エンティティのコピー / 貼り付け", "編集モード・文字入力中以外"},
    {"Ctrl+Shift+N", "空のエンティティを作成（選択中があればその子）", "編集モード・文字入力中以外"},
};

// --- デバッグカメラ ---
const Binding kCamera[] = {
    {"W / A / S / D", "前後左右に移動", ""},
    {"E / Q", "上昇 / 下降", ""},
    {"矢印キー", "視点の回転", ""},
    {"Shift / Ctrl（移動中）", "速く / 遅く", ""},
    {"右ドラッグ / 中ドラッグ", "視点の回転", "Viewport の上"},
    {"Shift + 中ドラッグ", "平行移動", "Viewport の上"},
    {"Ctrl + 中ドラッグ", "前後移動（ズーム）", "Viewport の上"},
    {"ホイール", "前後移動（Shift で速く）", "Viewport の上"},
};

// --- ウィンドウ ---
const Binding kWindow[] = {
    {"Alt + 左ドラッグ", "ウィンドウを移動（ボーダーレス時）", ""},
};

const Group kGroups[] = {
    {"デバッグキー", "Debug / Development ビルドのみ（配布用 Release では効かない）", kDebug,
     static_cast<int>(sizeof(kDebug) / sizeof(kDebug[0]))},
    {"エディタ", "", kEditor, static_cast<int>(sizeof(kEditor) / sizeof(kEditor[0]))},
    {"デバッグカメラ", "F1 でデバッグカメラにしている間 / 編集モード中", kCamera,
     static_cast<int>(sizeof(kCamera) / sizeof(kCamera[0]))},
    {"ウィンドウ", "", kWindow, static_cast<int>(sizeof(kWindow) / sizeof(kWindow[0]))},
};

} // namespace

void KeyBindingsHelp::Draw(bool *open) {
  if (open && !*open) return;
  ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("キー操作一覧", open)) {
    ImGui::End();
    return;
  }

  for (const Group &g : kGroups) {
    if (!ImGui::CollapsingHeader(g.title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
    if (g.note && g.note[0]) {
      ImGui::TextDisabled("%s", g.note);
    }
    ImGui::PushID(g.title);
    if (ImGui::BeginTable("keys", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("動作", ImGuiTableColumnFlags_WidthStretch, 2.2f);
      ImGui::TableSetupColumn("条件", ImGuiTableColumnFlags_WidthStretch, 1.1f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < g.count; ++i) {
        const Binding &b = g.items[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(b.key);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", b.action);
        ImGui::TableSetColumnIndex(2);
        if (b.when && b.when[0]) {
          ImGui::TextDisabled("%s", b.when);
        } else {
          ImGui::TextDisabled("常時");
        }
      }
      ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::Spacing();
  }

  ImGui::End();
}

#else

void KeyBindingsHelp::Draw(bool *) {}

#endif // RC_ENABLE_IMGUI
