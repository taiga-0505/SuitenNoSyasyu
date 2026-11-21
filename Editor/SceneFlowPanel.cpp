#include "SceneFlowPanel.h"

#include "Common/EngineConfig.h"

#if RC_ENABLE_IMGUI

#include "SceneFlow.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 表示の状態（パネルは 1 つしか無いので静的に持つ）
// ---------------------------------------------------------------------------
bool g_onlyCurrent = false;  ///< 今のシーンに当てはまる行だけ出す
char g_search[64] = {};      ///< 検索文字列
int g_focusRow = -1;         ///< 追加直後にスクロールして見せる行

const ImVec4 kWarnColor = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);  ///< 警告の文字色
const ImVec4 kErrorColor = ImVec4(1.0f, 0.4f, 0.35f, 1.0f); ///< エラーの文字色
const ImVec4 kDimColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);    ///< 補足など控えめに出す文字色

/// @brief 遷移元がパターン（"*" または末尾 "*" の前方一致）か
/// @param from 遷移元の文字列
/// @return パターンなら true
bool IsPattern(const std::string &from) {
  return from == "*" || (!from.empty() && from.back() == '*');
}

/// @brief 部分一致で検索する
/// @param hay    検索対象の文字列
/// @param needle 探す文字列（nullptr / 空なら常に一致）
/// @return 含まれていれば true
bool Contains(const std::string &hay, const char *needle) {
  if (!needle || !needle[0]) return true;
  return hay.find(needle) != std::string::npos;
}

/// @brief std::string を ImGui の入力用バッファへ写す
/// @details strncpy は MSVC で C4996（警告をエラー扱い）になるので memcpy で書く。
///          UTF-8 の途中で切らないよう、はみ出す場合は文字の先頭まで戻して切る。
/// @param dst 書き込み先のバッファ（必ず終端される）
/// @param src 写す文字列
template <size_t N>
void CopyToBuffer(char (&dst)[N], const std::string &src) {
  size_t len = (std::min)(src.size(), N - 1);
  if (len < src.size()) {
    while (len > 0 && (static_cast<unsigned char>(src[len]) & 0xC0) == 0x80) --len;
  }
  std::memcpy(dst, src.data(), len);
  dst[len] = '\0';
}

/// @brief 入力欄＋候補のドロップダウン
/// @param id      ImGui の ID
/// @param value   編集する値（入力・選択で書き換わる）
/// @param options 候補。"---" は区切り線
/// @param tips    options と同じ並びの説明（ツールチップ）。不要なら nullptr
/// @param hint    入力欄が空のときに出すヒント
/// @return 値が変わったら true
bool ComboInput(const char *id, std::string &value, const std::vector<std::string> &options,
                const std::vector<std::string> *tips = nullptr, const char *hint = "") {
  bool changed = false;
  ImGui::PushID(id);

  char buf[128];
  CopyToBuffer(buf, value);

  const float button = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x - button, 40.0f));
  if (ImGui::InputTextWithHint("##value", hint, buf, sizeof(buf))) {
    value = buf;
    changed = true;
  }
  ImGui::SameLine(0.0f, 0.0f);
  if (ImGui::ArrowButton("##open", ImGuiDir_Down)) {
    ImGui::OpenPopup("##list");
  }
  if (ImGui::BeginPopup("##list")) {
    for (size_t i = 0; i < options.size(); ++i) {
      const std::string &opt = options[i];
      if (opt == "---") {
        ImGui::Separator();
        continue;
      }
      if (ImGui::Selectable(opt.c_str(), opt == value)) {
        value = opt;
        changed = true;
      }
      if (tips && i < tips->size() && !(*tips)[i].empty()) {
        ImGui::SetItemTooltip("%s", (*tips)[i].c_str());
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

/// @brief 行の問題点を並べる（空なら問題なし）
/// @param flow   シーン遷移表
/// @param index  調べる行の番号
/// @param scenes 登録済みシーン名の一覧（空ならシーン名のチェックは省く）
/// @return 問題点の説明文の一覧
std::vector<std::string> RowProblems(const SceneFlow &flow, size_t index,
                                     const std::vector<std::string> &scenes) {
  std::vector<std::string> out;
  const auto &rules = flow.Rules();
  const SceneFlowRule &r = rules[index];
  const bool checkScenes = !scenes.empty();
  auto known = [&](const std::string &name) {
    return std::find(scenes.begin(), scenes.end(), name) != scenes.end();
  };

  if (r.from.empty()) {
    out.push_back("遷移元が空です");
  } else if (checkScenes && IsPattern(r.from)) {
    bool any = false;
    for (const auto &s : scenes) any |= SceneFlow::MatchFrom(r.from, s);
    if (!any) out.push_back("「" + r.from + "」に当てはまるシーンがありません");
  } else if (checkScenes && !known(r.from)) {
    out.push_back("遷移元のシーン「" + r.from + "」は登録されていません");
  }

  if (r.trigger.empty()) out.push_back("きっかけが空です");

  if (r.to.empty()) {
    out.push_back("遷移先が空です");
  } else if (r.to[0] == '$') {
    if (!flow.HasVariable(r.to)) out.push_back("変数「" + r.to + "」は登録されていません");
  } else if (checkScenes && !known(r.to)) {
    out.push_back("遷移先のシーン「" + r.to + "」は登録されていません");
  }

  const auto &names = SceneFlow::TransitionNames();
  if (std::find(names.begin(), names.end(), r.transition) == names.end()) {
    out.push_back("演出「" + r.transition + "」は使えません（dissolve として扱われます）");
  }

  for (size_t j = 0; j < index; ++j) {
    if (rules[j].from == r.from && rules[j].trigger == r.trigger) {
      out.push_back("同じ遷移元・きっかけの行が上にあるため、この行は使われません");
      break;
    }
  }
  return out;
}

/// @brief 「試す」で使う遷移先（$current の基準にするシーンも決める）
/// @param flow    シーン遷移表
/// @param r       試す行
/// @param current いまのシーン名
/// @return 解決した遷移先のシーン名
std::string TryTarget(const SceneFlow &flow, const SceneFlowRule &r, const std::string &current) {
  std::string from = current;
  if (!SceneFlow::MatchFrom(r.from, current) && !IsPattern(r.from)) from = r.from;
  return flow.ResolveTarget(r.to, from, "");
}

/// @brief 「使い方」の折りたたみ見出しの中身（説明文）を描く
void DrawHelp() {
  ImGui::TextWrapped(
      "スクリプトやゲームルールはシーン名を書かず、きっかけの名前だけで遷移します。\n"
      "  RequestTransition(\"start\");            // 表の行き先・演出で遷移\n"
      "  RequestTransition(\"stage\", \"Stage3\");  // 遷移先が $arg の行ならその名前へ\n"
      "  GameMode::EvaluateOutcome が返すきっかけ（cleared / gameover など）も同じ表で引きます。");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "遷移元は シーン名 / \"Stage*\"（前方一致）/ \"*\"（全シーン）。"
      "複数当てはまるときは 完全一致 → 長い前方一致 → * の順で優先し、同じ優先度なら上の行を使います。");
  ImGui::TextWrapped("編集はすぐゲームに反映されます。ファイル（%s）へは「保存」で書き出します。",
                     SceneFlow::Get().Path().c_str());
}

} // namespace

void SceneFlowPanel::Draw(bool *open) {
  if (open && !*open) return;
  ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Scene Flow (シーン遷移)", open)) {
    ImGui::End();
    return;
  }

  SceneFlow &flow = SceneFlow::Get();
  auto &rules = flow.Rules();
  const std::vector<std::string> scenes = flow.SceneNames();
  const std::string current = flow.CurrentScene();
  const auto variables = flow.Variables();

  // --- 候補リスト ----------------------------------------------------------
  std::vector<std::string> fromOptions = scenes;
  fromOptions.push_back("---");
  fromOptions.push_back("*");
  for (const auto &r : rules) {
    if (IsPattern(r.from) && r.from != "*" &&
        std::find(fromOptions.begin(), fromOptions.end(), r.from) == fromOptions.end()) {
      fromOptions.push_back(r.from);
    }
  }

  std::vector<std::string> toOptions = scenes;
  std::vector<std::string> toTips(scenes.size());
  toOptions.push_back("---");
  toTips.push_back("");
  for (const auto &v : variables) {
    toOptions.push_back(v.name);
    toTips.push_back(v.description);
  }

  std::vector<std::string> triggerOptions;
  for (const auto &r : rules) {
    if (!r.trigger.empty() &&
        std::find(triggerOptions.begin(), triggerOptions.end(), r.trigger) == triggerOptions.end()) {
      triggerOptions.push_back(r.trigger);
    }
  }
  std::sort(triggerOptions.begin(), triggerOptions.end());

  // --- ツールバー ----------------------------------------------------------
  if (ImGui::Button(flow.IsDirty() ? "保存 *" : "保存")) {
    flow.Save();
  }
  ImGui::SetItemTooltip("%s へ書き出します", flow.Path().c_str());
  ImGui::SameLine();
  if (ImGui::Button("再読み込み")) {
    if (flow.IsDirty()) {
      ImGui::OpenPopup("未保存の変更を破棄");
    } else {
      flow.Load(flow.Path());
    }
  }
  if (ImGui::BeginPopupModal("未保存の変更を破棄", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("保存していない変更を捨てて、ファイルから読み直しますか？");
    if (ImGui::Button("破棄して読み直す")) {
      flow.Load(flow.Path());
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("やめる")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::Checkbox("今のシーンの行だけ", &g_onlyCurrent);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::InputTextWithHint("##search", "検索（シーン・きっかけ・メモ）", g_search, sizeof(g_search));
  ImGui::SameLine();
  ImGui::TextColored(kDimColor, "今のシーン: %s", current.empty() ? "(なし)" : current.c_str());

  // --- 表 -----------------------------------------------------------------
  int removeIndex = -1;
  int duplicateIndex = -1;
  int moveFrom = -1, moveTo = -1;
  int problemRows = 0;

  const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("SceneFlowRules", 7, tableFlags)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18.0f);
    ImGui::TableSetupColumn("遷移元", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("きっかけ", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("遷移先", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("演出", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("メモ", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < rules.size(); ++i) {
      SceneFlowRule &r = rules[i];
      const std::vector<std::string> problems = RowProblems(flow, i, scenes);
      if (!problems.empty()) ++problemRows;

      const bool matchesCurrent = !current.empty() && SceneFlow::MatchFrom(r.from, current);
      if (g_onlyCurrent && !matchesCurrent) continue;
      if (g_search[0] && !Contains(r.from, g_search) && !Contains(r.trigger, g_search) &&
          !Contains(r.to, g_search) && !Contains(r.note, g_search)) {
        continue;
      }

      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      if (matchesCurrent) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.25f, 0.45f, 0.7f, 0.18f)));
      }
      if (g_focusRow == static_cast<int>(i)) {
        ImGui::SetScrollHereY();
        g_focusRow = -1;
      }

      // 状態
      ImGui::TableSetColumnIndex(0);
      if (!problems.empty()) {
        ImGui::TextColored(kErrorColor, "!");
        if (ImGui::BeginItemTooltip()) {
          for (const auto &p : problems) ImGui::BulletText("%s", p.c_str());
          ImGui::EndTooltip();
        }
      }

      // 遷移元
      ImGui::TableSetColumnIndex(1);
      if (ComboInput("from", r.from, fromOptions, nullptr, "シーン / Stage* / *")) flow.MarkDirty();

      // きっかけ
      ImGui::TableSetColumnIndex(2);
      if (ComboInput("trigger", r.trigger, triggerOptions, nullptr, "start など")) flow.MarkDirty();

      // 遷移先
      ImGui::TableSetColumnIndex(3);
      if (ComboInput("to", r.to, toOptions, &toTips, "シーン / $変数")) flow.MarkDirty();

      // 演出
      ImGui::TableSetColumnIndex(4);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##transition", r.transition.c_str())) {
        for (const auto &name : SceneFlow::TransitionNames()) {
          if (ImGui::Selectable(name.c_str(), name == r.transition)) {
            r.transition = name;
            flow.MarkDirty();
          }
        }
        ImGui::EndCombo();
      }

      // メモ
      ImGui::TableSetColumnIndex(5);
      {
        char note[256];
        CopyToBuffer(note, r.note);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##note", note, sizeof(note))) {
          r.note = note;
          flow.MarkDirty();
        }
      }

      // 操作
      ImGui::TableSetColumnIndex(6);
      {
        const std::string target = (r.to == SceneFlow::kVarArg) ? std::string() : TryTarget(flow, r, current);
        ImGui::BeginDisabled(target.empty());
        if (ImGui::SmallButton("試す")) {
          flow.Request(target, r.transition);
        }
        ImGui::EndDisabled();
        if (r.to == SceneFlow::kVarArg) {
          ImGui::SetItemTooltip("行き先をスクリプトが決める行なので、ここからは試せません");
        } else if (target.empty()) {
          ImGui::SetItemTooltip("いまは行き先がありません");
        } else {
          ImGui::SetItemTooltip("今すぐ %s へ遷移します（演出: %s）", target.c_str(), r.transition.c_str());
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
          moveFrom = static_cast<int>(i);
          moveTo = static_cast<int>(i) - 1;
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < rules.size()) {
          moveFrom = static_cast<int>(i);
          moveTo = static_cast<int>(i) + 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("複製")) duplicateIndex = static_cast<int>(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("削除")) removeIndex = static_cast<int>(i);
      }

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // ループの外で並びを変える（ループ中に消すと添字がずれる）
  if (moveFrom >= 0 && moveTo >= 0) {
    std::swap(rules[static_cast<size_t>(moveFrom)], rules[static_cast<size_t>(moveTo)]);
    flow.MarkDirty();
  }
  if (duplicateIndex >= 0) {
    SceneFlowRule copy = rules[static_cast<size_t>(duplicateIndex)];
    rules.insert(rules.begin() + duplicateIndex + 1, copy);
    g_focusRow = duplicateIndex + 1;
    flow.MarkDirty();
  }
  if (removeIndex >= 0) {
    rules.erase(rules.begin() + removeIndex);
    flow.MarkDirty();
  }

  if (ImGui::Button("+ 行を追加")) {
    SceneFlowRule r;
    r.from = current;
    rules.push_back(r);
    g_focusRow = static_cast<int>(rules.size()) - 1;
    flow.MarkDirty();
  }
  ImGui::SetItemTooltip("遷移元が今のシーンの行を末尾に足します");

  // --- 未登録のきっかけ ---------------------------------------------------
  const auto missing = flow.Missing(); // 追加でリストが変わるのでコピーして回す
  if (!missing.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kWarnColor);
    const bool openMissing = ImGui::CollapsingHeader(
        ("未登録のきっかけ (" + std::to_string(missing.size()) + ")###missing").c_str(),
        ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();
    if (openMissing) {
      ImGui::TextColored(kDimColor, "スクリプトやゲームルールが遷移しようとしたが、表に行が無かったもの");
      for (const auto &m : missing) {
        ImGui::PushID((m.from + "/" + m.trigger).c_str());
        ImGui::BulletText("%s / %s  （%d 回）", m.from.c_str(), m.trigger.c_str(), m.count);
        ImGui::SameLine();
        if (ImGui::SmallButton("追加")) {
          SceneFlowRule r;
          r.from = m.from;
          r.trigger = m.trigger;
          rules.push_back(r);
          g_focusRow = static_cast<int>(rules.size()) - 1;
          flow.RemoveMissing(m.from, m.trigger);
          flow.MarkDirty();
        }
        ImGui::SetItemTooltip("遷移元ときっかけを埋めた行を足します。遷移先を選んでください");
        ImGui::SameLine();
        if (ImGui::SmallButton("無視")) flow.RemoveMissing(m.from, m.trigger);
        ImGui::PopID();
      }
    }
  }

  // --- チェック -----------------------------------------------------------
  std::vector<std::string> unreached;
  for (const auto &s : scenes) {
    bool reached = false;
    for (const auto &r : rules) reached |= (r.to == s);
    if (!reached) unreached.push_back(s);
  }
  {
    ImGui::Spacing();
    const std::string header = "チェック（問題のある行 " + std::to_string(problemRows) + "）###check";
    if (problemRows > 0) ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    const bool openCheck = ImGui::CollapsingHeader(header.c_str());
    if (problemRows > 0) ImGui::PopStyleColor();
    if (openCheck) {
      if (problemRows == 0) {
        ImGui::TextColored(kDimColor, "問題のある行はありません");
      } else {
        for (size_t i = 0; i < rules.size(); ++i) {
          for (const auto &p : RowProblems(flow, i, scenes)) {
            ImGui::BulletText("%d 行目（%s / %s）: %s", static_cast<int>(i) + 1, rules[i].from.c_str(),
                              rules[i].trigger.c_str(), p.c_str());
          }
        }
      }
      if (!unreached.empty()) {
        std::string list;
        for (const auto &s : unreached) list += (list.empty() ? "" : ", ") + s;
        ImGui::Spacing();
        ImGui::TextColored(kDimColor, "どの行の遷移先にもなっていないシーン: %s", list.c_str());
        ImGui::TextColored(kDimColor, "（起動シーンや、$変数・$arg 経由で遷移するシーンなら問題ありません）");
      }
    }
  }

  // --- 変数 ---------------------------------------------------------------
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("遷移先に使える変数")) {
    if (ImGui::BeginTable("SceneFlowVars", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("変数", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("説明");
      ImGui::TableSetupColumn("今の値", ImGuiTableColumnFlags_WidthFixed, 140.0f);
      ImGui::TableHeadersRow();
      for (const auto &v : variables) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(v.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s%s", v.description.c_str(), v.builtin ? "（組み込み）" : "");
        ImGui::TableSetColumnIndex(2);
        if (v.name == SceneFlow::kVarArg) {
          ImGui::TextColored(kDimColor, "(スクリプト次第)");
        } else {
          const std::string value = flow.ResolveTarget(v.name, current, "");
          ImGui::TextUnformatted(value.empty() ? "(なし)" : value.c_str());
        }
      }
      ImGui::EndTable();
    }
    ImGui::TextColored(kDimColor, "変数は Application 側で SceneFlow::Get().RegisterVariable() で登録します");
  }

  // --- 使い方 -------------------------------------------------------------
  if (ImGui::CollapsingHeader("使い方")) {
    DrawHelp();
  }

  ImGui::End();
}

#else

void SceneFlowPanel::Draw(bool *) {}

#endif // RC_ENABLE_IMGUI
