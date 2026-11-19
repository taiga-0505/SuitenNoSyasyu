#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "Scene.h"
#include "RenderCommon.h"
#include "Game/Framework/GameSession.h"
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <random>
#include <climits>
#include <cstdint>
#include <cstdio>

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

// =====================================================================
// T-20: ステージのルート分岐システム
// =====================================================================
// レールは「ウェイポイントの配列」ひとつで表現し、分岐は
// 「到達したウェイポイントから次にどの index へ飛ぶか」で表現する。
// パスを複数本持たせるのではなく1本の配列に閉じたのは、
//   ・既存のシリアライズ形式（waypoints 配列）を壊さずに済む
//   ・分岐先も合流先も同じ index 空間なので「合流」が特別扱いにならない
//   ・エディタ上でも1つのリストを見れば全ルートが把握できる
// という理由から。分岐が無いウェイポイントは今までどおり index+1 へ直進する。

struct RailBranch;

/// @brief 分岐条件の判定に使う周辺情報
struct RailBranchContext {
    Entity* self = nullptr; ///< レールを載せているエンティティ（タグの参照先が空のとき使う）
    Scene* scene = nullptr; ///< タグの参照先を名前で探すシーン
};

// =====================================================================
// 分岐条件（ポリモーフィズム / Strategy）
// =====================================================================
// 以前は enum class BranchCondition と switch で「判定」「エディタの入力欄」「名前の変換」を
// それぞれ別の場所で分岐していたため、条件を 1 つ足すたびに 3〜4 か所の switch を直す必要があった。
// 条件 1 種類 = クラス 1 つにして、判定・名前・編集 UI を仮想関数でまとめた。
// 新しい条件はクラスを 1 つ足して RailBranchConditions::All() に並べるだけで済む。
//
// 条件オブジェクトは状態を持たない（比較値やキーは RailBranch 側のデータにある）ので、
// 種類ごとに 1 つのインスタンスを全分岐で共有している（Flyweight）。
// こうしておくと RailBranch はポインタ 1 つを持つだけの値型のままでコピーでき、
// シーン JSON の形式（"condition": "tag_equals" と各パラメータを並べる形）も変えずに済む。

/// @brief 分岐が成立する条件の基底クラス
class IRailBranchCondition {
public:
    virtual ~IRailBranchCondition() = default;

    /// @brief シリアライズ用の名前（"always" / "tag_equals" など。JSON の "condition" の値）
    /// @return 条件名
    virtual const char* Name() const = 0;

    /// @brief 条件が成立しているか判定する
    /// @param b 判定する分岐定義（比較値・キーなどのパラメータ）
    /// @param ctx 判定に使う周辺情報
    /// @return 成立していれば true
    virtual bool IsSatisfied(const RailBranch& b, const RailBranchContext& ctx) const = 0;

    /// @brief 無条件（必ず成立する）か
    /// @return 無条件なら true。残り地点数の見積もりで「本線」とみなすのに使う
    virtual bool IsUnconditional() const { return false; }

#if RC_ENABLE_IMGUI
    /// @brief この条件に必要なパラメータの入力欄を出す
    /// @param b 編集対象の分岐定義
    virtual void DrawParamEditor(RailBranch& b) const = 0;
#endif
};

/// @brief 分岐条件の一覧と、名前からの引き当て
/// @details 条件オブジェクトは種類ごとに 1 つだけ作って共有する（Flyweight）。
class RailBranchConditions {
public:
    /// @brief 条件の種類数
    static constexpr size_t kCount = 5;

    /// @brief 既定（無条件）の条件
    /// @return always 条件
    static const IRailBranchCondition& Default();

    /// @brief 全条件（エディタのコンボの並び順）
    /// @return 条件の配列
    static const std::array<const IRailBranchCondition*, kCount>& All();

    /// @brief シリアライズ用の名前から条件を引く（Name と対）
    /// @param name 条件名
    /// @return 対応する条件（未知の名前なら always）
    static const IRailBranchCondition& FromName(const std::string& name);

    /// @brief 条件が All() の何番目か
    /// @param condition 調べる条件
    /// @return 添字（見つからなければ 0）
    static int IndexOf(const IRailBranchCondition* condition);
};

/// @brief ウェイポイントに紐づく分岐の定義
/// @details シーン JSON と 1 対 1 の素朴なデータなので struct のままにしている。
///          どの条件でも全パラメータを持つのは、JSON の形式（全キーを書く）を保つため。
struct RailBranch {
    int target = -1;                              ///< 分岐先ウェイポイント index
    const IRailBranchCondition* condition = &RailBranchConditions::Default(); ///< 成立条件（共有インスタンス）
    std::string entityName = "player";            ///< タグを読むエンティティ名（空なら自分自身）
    std::string tagKey;                           ///< 参照するタグのキー
    int value = 1;                                ///< tag_equals / tag_at_least の比較値
    float chance = 0.5f;                          ///< chance の確率（0..1）
    int keyCode = 0;                              ///< key_held の DIK コード
};

/// @brief レール上の1点
struct Waypoint {
    RC::Vector3 pos;
    float waitTime = 0.0f;
    /// @brief この地点に到達したときに評価する分岐リスト（先頭から順に判定し、最初に成立したものを採用）
    std::vector<RailBranch> branches;
    /// @brief この地点がルートの終点かどうか
    /// @details 分岐で index が飛ぶようになると「配列の末尾＝終点」ではなくなる。
    ///          例：0→…→7 が本線の終点で、迂回ルートを 8,9,10 に置き 10→7 で合流する場合、
    ///          7 の次は index 8 なので何も指定しないと迂回ルートへ入り直して周回してしまう。
    ///          終点になる地点にはこのフラグを立てる。
    bool isEnd = false;
    /// @brief この地点で発生させるウェーブ戦闘の id（0 ならウェーブ無し）
    /// @details A-03。到達した時点でレールを止め、WaveManager へ開始を要求する。
    ///          同じウェーブがクリア済みで再度この地点を通った場合は止まらない。
    int waveId = 0;
};

namespace {

/// @brief 無条件（必ずこの分岐へ）
class RailAlwaysCondition final : public IRailBranchCondition {
public:
    /// @brief 条件名
    /// @return "always"
    const char* Name() const override { return "always"; }
    /// @brief 常に成立する
    /// @param b 分岐定義（未使用）
    /// @param ctx 周辺情報（未使用）
    /// @return 常に true
    bool IsSatisfied(const RailBranch& b, const RailBranchContext& ctx) const override {
        (void)b;
        (void)ctx;
        return true;
    }
    /// @brief 無条件である
    /// @return 常に true
    bool IsUnconditional() const override { return true; }
#if RC_ENABLE_IMGUI
    /// @brief パラメータは無いので説明だけ出す
    /// @param b 分岐定義（未使用）
    void DrawParamEditor(RailBranch& b) const override {
        (void)b;
        ImGui::TextUnformatted("(unconditional)");
    }
#endif
};

/// @brief 参照エンティティの int タグを比較する条件の共通部分（Template Method）
/// @details タグの読み方と編集 UI は共通で、比較の仕方（== か >=）だけが違うので、
///          判定の流れをここに固定して Compare だけを派生クラスに任せる。
class RailTagCompareCondition : public IRailBranchCondition {
public:
    /// @brief タグを読み、読めたら Compare で比較する
    /// @param b 分岐定義
    /// @param ctx 周辺情報
    /// @return 成立していれば true（タグが読めなければ false）
    bool IsSatisfied(const RailBranch& b, const RailBranchContext& ctx) const final {
        const int v = ReadTag(b, ctx);
        return (v != INT_MIN) && Compare(v, b.value);
    }
#if RC_ENABLE_IMGUI
    /// @brief エンティティ名・タグキー・比較値の入力欄を出す
    /// @param b 編集対象の分岐定義
    void DrawParamEditor(RailBranch& b) const override {
        char nameBuf[64] = {};
        char keyBuf[64] = {};
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", b.entityName.c_str());
        std::snprintf(keyBuf, sizeof(keyBuf), "%s", b.tagKey.c_str());
        if (ImGui::InputText("Entity", nameBuf, sizeof(nameBuf))) b.entityName = nameBuf;
        if (ImGui::InputText("Tag Key", keyBuf, sizeof(keyBuf))) b.tagKey = keyBuf;
        ImGui::DragInt("Value", &b.value, 1.0f);
    }
#endif

protected:
    /// @brief タグの値と比較値を比べる
    /// @param tagValue 読み取ったタグの値
    /// @param value 分岐に設定された比較値
    /// @return 成立していれば true
    virtual bool Compare(int tagValue, int value) const = 0;

private:
    /// @brief 分岐が参照するエンティティの int タグを読む
    /// @param b 参照先エンティティ名とタグキーを持つ分岐定義
    /// @param ctx 周辺情報
    /// @return タグの値。見つからなければ INT_MIN（＝条件は不成立扱い）
    static int ReadTag(const RailBranch& b, const RailBranchContext& ctx) {
        if (b.tagKey.empty()) return INT_MIN;

        // エンティティ名が空なら自分自身、そうでなければ名前でシーンから探す
        Entity* target = nullptr;
        if (b.entityName.empty()) {
            target = ctx.self;
        } else if (ctx.scene) {
            target = ctx.scene->FindEntityByName(b.entityName);
        }
        if (!target || !target->HasTag(b.tagKey)) return INT_MIN;
        return target->GetTagInt(b.tagKey, INT_MIN);
    }
};

/// @brief 参照エンティティの int タグが value と一致
class RailTagEqualsCondition final : public RailTagCompareCondition {
public:
    /// @brief 条件名
    /// @return "tag_equals"
    const char* Name() const override { return "tag_equals"; }

protected:
    /// @brief 一致するか
    /// @param tagValue タグの値
    /// @param value 比較値
    /// @return tagValue == value
    bool Compare(int tagValue, int value) const override { return tagValue == value; }
};

/// @brief 参照エンティティの int タグが value 以上
class RailTagAtLeastCondition final : public RailTagCompareCondition {
public:
    /// @brief 条件名
    /// @return "tag_at_least"
    const char* Name() const override { return "tag_at_least"; }

protected:
    /// @brief 以上か
    /// @param tagValue タグの値
    /// @param value 比較値
    /// @return tagValue >= value
    bool Compare(int tagValue, int value) const override { return tagValue >= value; }
};

/// @brief 指定キーが押されている（プレイヤーに選ばせる分岐）
class RailKeyHeldCondition final : public IRailBranchCondition {
public:
    /// @brief 条件名
    /// @return "key_held"
    const char* Name() const override { return "key_held"; }
    /// @brief keyCode のキーが押されているか
    /// @param b 分岐定義
    /// @param ctx 周辺情報（未使用）
    /// @return 押されていれば true（DIK コードが範囲外なら false）
    bool IsSatisfied(const RailBranch& b, const RailBranchContext& ctx) const override {
        (void)ctx;
        // DIK コードの有効範囲（1〜255）外は不成立
        if (b.keyCode <= 0 || b.keyCode > 255) return false;
        auto* input = Input::GetInstance();
        return input && input->IsKeyPressed(static_cast<uint8_t>(b.keyCode));
    }
#if RC_ENABLE_IMGUI
    /// @brief DIK コードの入力欄を出す
    /// @param b 編集対象の分岐定義
    void DrawParamEditor(RailBranch& b) const override {
        ImGui::DragInt("DIK Code", &b.keyCode, 1.0f, 0, 255);
    }
#endif
};

/// @brief chance の確率で成立（0.0〜1.0）
class RailChanceCondition final : public IRailBranchCondition {
public:
    /// @brief 条件名
    /// @return "chance"
    const char* Name() const override { return "chance"; }
    /// @brief 乱数を引いて chance 未満なら成立
    /// @param b 分岐定義
    /// @param ctx 周辺情報（未使用）
    /// @return 成立していれば true
    /// @note 呼ぶたびに乱数を引くため、同じ分岐でも結果が変わる
    bool IsSatisfied(const RailBranch& b, const RailBranchContext& ctx) const override {
        (void)ctx;
        return RandomUnit() < b.chance;
    }
#if RC_ENABLE_IMGUI
    /// @brief 確率の入力欄を出す
    /// @param b 編集対象の分岐定義
    void DrawParamEditor(RailBranch& b) const override {
        ImGui::SliderFloat("Chance", &b.chance, 0.0f, 1.0f);
    }
#endif

private:
    /// @brief 0.0〜1.0 の一様乱数を得る
    /// @return 0.0〜1.0 の乱数
    static float RandomUnit() {
        static std::mt19937 engine{ std::random_device{}() };
        static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(engine);
    }
};

} // namespace

const IRailBranchCondition& RailBranchConditions::Default() {
    static const RailAlwaysCondition kAlways{};
    return kAlways;
}

const std::array<const IRailBranchCondition*, RailBranchConditions::kCount>& RailBranchConditions::All() {
    static const RailTagEqualsCondition kTagEquals{};
    static const RailTagAtLeastCondition kTagAtLeast{};
    static const RailKeyHeldCondition kKeyHeld{};
    static const RailChanceCondition kChance{};
    // 並び順はエディタのコンボの並び（以前の enum の順）と同じにする
    static const std::array<const IRailBranchCondition*, kCount> kAll = {
        &Default(), &kTagEquals, &kTagAtLeast, &kKeyHeld, &kChance
    };
    return kAll;
}

const IRailBranchCondition& RailBranchConditions::FromName(const std::string& name) {
    for (const IRailBranchCondition* c : All()) {
        if (name == c->Name()) return *c;
    }
    return Default();
}

int RailBranchConditions::IndexOf(const IRailBranchCondition* condition) {
    const auto& all = All();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == condition) return static_cast<int>(i);
    }
    return 0;
}

// =====================================================================
// A-03: ウェーブ戦闘との連携（タグ契約）
// =====================================================================
// スクリプト同士は型ではなく Entity のタグだけで通信する。
// こうしておくと RailMovementScript / WaveManagerScript / WaveSpawnerScript が
// 互いのヘッダを include せずに済み、片方だけをシーンに置いても壊れない。
//
//   WaveManager エンティティ（既定名 "WaveManager"）のタグ
//     wave_request     : レールが立てる。開始してほしいウェーブ id。受理されると 0 に戻る
//     wave_active      : 進行中のウェーブ id（Idle なら 0）。WaveManager が書く
//     wave_cleared_id  : 直近にクリアしたウェーブ id。ウェーブ開始時に 0 へ戻る
//     waves_cleared    : 累計クリア数。分岐条件 tag_at_least から参照できる
//
//   自エンティティ（レールを載せているエンティティ）のタグ
//     rail_finished    : 終点に到達したら 1。シーン側の Result 遷移条件になる
//     rail_wp          : いま向かっているウェイポイント番号。毎フレーム更新する
//     rail_branch_to   : 直近に分岐で飛んだ先の番号（直進なら -1）
//     rail_branch_count: 分岐が成立した回数。増えた瞬間がルートを逸れた瞬間
//     intro_playing    : 読むだけ。DeepRiseIntroScript が開始演出（深海からの浮上）の
//                        あいだ 1 を立てる。1 のあいだレールは出発しない
//
// rail_wp / rail_branch_* は動作確認のために外へ出しているだけで、
// レールの挙動そのものには使っていない。T-20 の「分岐して合流した」ことは
// 番号が飛ぶところを見せるのが一番早く、それを撮影用の字幕から読めるようにした。
//
// 待ち合わせは waves_cleared ではなく wave_cleared_id の「一致」で見る。
// 累計や >= で見ると、分岐でウェーブ 2 を飛ばしてウェーブ 3 を先にクリアした場合に
// あとから来たウェーブ 2 が即座に成立してしまうため。
//
// 進行状態（移動中・地点での待ち・ウェーブ待ち・終点）は State パターンにしていない。
// enum + switch ではなく、isMoving（シーン JSON に保存され、インスペクタや Pause/Resume、
// ForceNextWaypoint から外部が直接切り替える設定値）と、待ち時間・待機ウェーブ id の
// 独立した値の組み合わせで決まっているため。状態クラスに分けると外部からの切り替えと
// JSON の isMoving の意味を保つための橋渡しが必要になり、かえって複雑になる。

/// @brief レール移動（パス移動）を制御するスクリプトコンポーネント
class RailMovementScript : public ScriptableEntity {
public:
    // -----------------------------------------------------------------
    // 外部スクリプトから使う API
    // -----------------------------------------------------------------

    /// @brief 次に向かうウェイポイントを外部から指定する
    /// @details A-03 のウェーブ戦闘やイベントスクリプトから、条件式に落とし込めない
    ///          分岐を直接指示するための入口。範囲外は無視する。
    /// @param index 次に向かうウェイポイント index
    /// @return 受理したら true
    bool ForceNextWaypoint(int index) {
        if (index < 0 || index >= static_cast<int>(waypoints_.size())) {
            Log::Print("[RailMovementScript] ForceNextWaypoint: index out of range");
            return false;
        }
        currentWaypointIndex_ = index;
        currentWaitTimer_ = 0.0f;
        isMoving_ = true;
        lastBranchLog_ = "forced -> " + std::to_string(index);
        return true;
    }

    /// @brief 進行を一時停止する（A-03 のウェーブ戦闘で使用）
    void Pause() { isMoving_ = false; }

    /// @brief 進行を再開する
    void Resume() { isMoving_ = true; }

    /// @brief 現在向かっているウェイポイント index を取得する
    /// @return 現在向かっているウェイポイント index
    int CurrentIndex() const { return currentWaypointIndex_; }

    /// @brief ウェーブのクリア待ちで停止しているかを取得する
    /// @return 待っているウェーブ id（0 なら待っていない）
    int WaitingWaveId() const { return waitingWaveId_; }

    /// @brief このラップで到達したウェイポイントの数を取得する
    /// @return 到達数（リザルトの達成率の分子）
    int ReachedCount() const { return reachedCount_; }

    /// @brief いま向かっている地点から終点までに、あと何地点通る予定か
    /// @details 分岐は「無条件（always）の分岐があればそれ、無ければ直進」とみなして数える。
    ///          条件付き分岐の行き先は走ってみるまで分からないので、本線を進む前提で見積もり、
    ///          実際に分岐した時点で数え直す（達成率が途中で少し跳ねるのは許容する）。
    ///          終点（isEnd）か配列の末尾で止める。ループ検出のために訪問済みを覚えておく。
    /// @return 現在向かっている地点を含めた残り地点数（終点到達後は 0）
    int RemainingCount() const {
        const int n = static_cast<int>(waypoints_.size());
        if (n == 0) return 0;
        int index = currentWaypointIndex_;
        if (index < 0 || index >= n) return 0; // 終点到達後は index が配列の外に出ている

        std::vector<char> visited(static_cast<size_t>(n), 0);
        int count = 0;
        while (index >= 0 && index < n && !visited[static_cast<size_t>(index)]) {
            visited[static_cast<size_t>(index)] = 1;
            ++count;
            const Waypoint& wp = waypoints_[static_cast<size_t>(index)];
            if (wp.isEnd) break;

            // 無条件分岐があればその先、無ければ直進として次の地点を決める
            int next = index + 1;
            for (const auto& b : wp.branches) {
                if (b.condition && b.condition->IsUnconditional() &&
                    b.target >= 0 && b.target < n && b.target != index) {
                    next = b.target;
                    break;
                }
            }
            index = next;
        }
        return count;
    }

    /// @brief 速度・フラグ・描画色・ウェイポイント（分岐含む）を JSON に書き出す
    /// @return シリアライズした JSON
    /// @note isEnd / waveId / branches は既定値なら省略する
    nlohmann::json Serialize() override {
        nlohmann::json j;
        j["speed"] = speed_;
        // ウェーブ待機中の停止は「一時的な状態」であって設定値ではない。
        // そのまま false を書くと、待機中にシーンを保存したときに
        // 二度と動かないレールがデータに焼き付く（D-09 と同じ形の事故）ため、
        // 待機が理由で止まっている場合は動く状態として書き出す。
        j["isMoving"] = (waitingWaveId_ != 0) ? true : isMoving_;
        j["loop"] = loop_;
        j["drawPath"] = drawPath_;
        j["waveManagerName"] = waveManagerName_;
        j["pathColor"] = { pathColor_.x, pathColor_.y, pathColor_.z, pathColor_.w };
        j["branchColor"] = { branchColor_.x, branchColor_.y, branchColor_.z, branchColor_.w };
        j["waveColor"] = { waveColor_.x, waveColor_.y, waveColor_.z, waveColor_.w };
        // ウェイポイント配列（既定値の項目は書かずにデータを小さく保つ）
        nlohmann::json wpArray = nlohmann::json::array();
        for (const auto& wp : waypoints_) {
            nlohmann::json wj;
            wj["pos"] = { wp.pos.x, wp.pos.y, wp.pos.z };
            wj["waitTime"] = wp.waitTime;
            if (wp.isEnd) wj["isEnd"] = true;
            if (wp.waveId != 0) wj["waveId"] = wp.waveId;
            if (!wp.branches.empty()) {
                nlohmann::json bArray = nlohmann::json::array();
                for (const auto& b : wp.branches) {
                    const IRailBranchCondition& cond =
                        b.condition ? *b.condition : RailBranchConditions::Default();
                    bArray.push_back({
                        {"target", b.target},
                        {"condition", cond.Name()},
                        {"entityName", b.entityName},
                        {"tagKey", b.tagKey},
                        {"value", b.value},
                        {"chance", b.chance},
                        {"keyCode", b.keyCode}
                    });
                }
                wj["branches"] = bArray;
            }
            wpArray.push_back(wj);
        }
        j["waypoints"] = wpArray;
        return j;
    }

    /// @brief JSON から設定とウェイポイント（分岐含む）を読み込む
    /// @param j 読み込む JSON（無いキーは現在値のまま）
    void Deserialize(const nlohmann::json& j) override {
        if (j.contains("speed")) speed_ = j["speed"].get<float>();
        if (j.contains("isMoving")) isMoving_ = j["isMoving"].get<bool>();
        if (j.contains("loop")) loop_ = j["loop"].get<bool>();
        if (j.contains("drawPath")) drawPath_ = j["drawPath"].get<bool>();
        if (j.contains("pathColor") && j["pathColor"].size() == 4) {
            pathColor_.x = j["pathColor"][0];
            pathColor_.y = j["pathColor"][1];
            pathColor_.z = j["pathColor"][2];
            pathColor_.w = j["pathColor"][3];
        }
        if (j.contains("branchColor") && j["branchColor"].size() == 4) {
            branchColor_.x = j["branchColor"][0];
            branchColor_.y = j["branchColor"][1];
            branchColor_.z = j["branchColor"][2];
            branchColor_.w = j["branchColor"][3];
        }
        if (j.contains("waveColor") && j["waveColor"].size() == 4) {
            waveColor_.x = j["waveColor"][0];
            waveColor_.y = j["waveColor"][1];
            waveColor_.z = j["waveColor"][2];
            waveColor_.w = j["waveColor"][3];
        }
        if (j.contains("waveManagerName")) waveManagerName_ = j["waveManagerName"].get<std::string>();
        // ウェイポイント配列は丸ごと置き換える
        if (j.contains("waypoints") && j["waypoints"].is_array()) {
            waypoints_.clear();
            for (const auto& wj : j["waypoints"]) {
                Waypoint wp;
                if (wj.contains("pos") && wj["pos"].size() == 3) {
                    wp.pos.x = wj["pos"][0];
                    wp.pos.y = wj["pos"][1];
                    wp.pos.z = wj["pos"][2];
                }
                if (wj.contains("waitTime")) wp.waitTime = wj["waitTime"].get<float>();
                if (wj.contains("isEnd")) wp.isEnd = wj["isEnd"].get<bool>();
                if (wj.contains("waveId")) wp.waveId = wj["waveId"].get<int>();
                // 分岐（無い場合は従来どおり直進のみ）
                if (wj.contains("branches") && wj["branches"].is_array()) {
                    for (const auto& bj : wj["branches"]) {
                        RailBranch b;
                        if (bj.contains("target")) b.target = bj["target"].get<int>();
                        if (bj.contains("condition")) b.condition = &RailBranchConditions::FromName(bj["condition"].get<std::string>());
                        if (bj.contains("entityName")) b.entityName = bj["entityName"].get<std::string>();
                        if (bj.contains("tagKey")) b.tagKey = bj["tagKey"].get<std::string>();
                        if (bj.contains("value")) b.value = bj["value"].get<int>();
                        if (bj.contains("chance")) b.chance = bj["chance"].get<float>();
                        if (bj.contains("keyCode")) b.keyCode = bj["keyCode"].get<int>();
                        wp.branches.push_back(b);
                    }
                }
                waypoints_.push_back(wp);
            }
        }
    }

    /// @brief 移動設定・状態表示・ウェーブ待機のスキップ・ウェイポイントと分岐の編集 UI を出す
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        // 基本設定
        ImGui::Checkbox("Is Moving", &isMoving_);
        ImGui::Checkbox("Loop", &loop_);
        ImGui::DragFloat("Speed", &speed_, 0.1f, 0.1f, 100.0f);
        ImGui::Checkbox("Draw Path", &drawPath_);
        if (drawPath_) {
            ImGui::ColorEdit4("Path Color", &pathColor_.x);
            ImGui::ColorEdit4("Branch Color", &branchColor_.x);
            ImGui::ColorEdit4("Wave Color", &waveColor_.x);
        }

        // WaveManager 名の編集（InputText 用に固定長バッファへ写して編集する）
        {
            char mgrBuf[64] = {};
            std::snprintf(mgrBuf, sizeof(mgrBuf), "%s", waveManagerName_.c_str());
            if (ImGui::InputText("Wave Manager", mgrBuf, sizeof(mgrBuf))) {
                waveManagerName_ = mgrBuf;
            }
        }

        // 進行状況の表示
        ImGui::Text("Waypoints (Count: %zu)", waypoints_.size());
        ImGui::Text("Current Index: %d", currentWaypointIndex_);
        if (!lastBranchLog_.empty()) {
            ImGui::Text("Last Route: %s", lastBranchLog_.c_str());
        }
        if (currentWaitTimer_ > 0.0f) {
            ImGui::Text("Waiting... %.2f sec", currentWaitTimer_);
        }

        // A-03 のウェーブ待機状況。止まっている理由がここで分かるようにしておく。
        if (waitingWaveId_ != 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Waiting for WAVE %d (at WP %d)", waitingWaveId_, waveWaypointIndex_);
            ImGui::SameLine();
            if (ImGui::SmallButton("Skip Wave")) {
                if (Entity* manager = FindWaveManager()) {
                    manager->SetTag("wave_force_clear", waitingWaveId_);
                }
            }
        }
        if (!clearedWaves_.empty()) {
            std::string cleared;
            for (int id : clearedWaves_) {
                if (!cleared.empty()) cleared += ", ";
                cleared += std::to_string(id);
            }
            ImGui::Text("Cleared Waves: %s", cleared.c_str());
        }

        // パスを最初からやり直す（ウェーブ待機・クリア記録・終点通知も含めて初期化し、先頭地点へ戻す）
        if (ImGui::Button("Restart Path")) {
            currentWaypointIndex_ = 0;
            currentWaitTimer_ = 0.0f;
            lastBranchLog_.clear();
            waitingWaveId_ = 0;
            waveWaypointIndex_ = -1;
            clearedWaves_.clear();
            reachedCount_ = 0;
            railFinishedNotified_ = false;
            isMoving_ = true;
            if (Entity* self = GetEntity()) self->ClearTag("rail_finished");
            if (!waypoints_.empty()) {
                if (auto* tr = GetComponent<TransformComponent>()) {
                    tr->position = waypoints_[0].pos;
                }
            }
        }
        ImGui::SameLine();

        if (ImGui::Button("Add Waypoint")) {
            if (!waypoints_.empty()) {
                // 最後の要素の座標だけをコピーして追加（分岐はコピーしない）
                Waypoint wp;
                wp.pos = waypoints_.back().pos;
                wp.waitTime = 0.0f;
                waypoints_.push_back(wp);
            } else if (auto* tr = GetComponent<TransformComponent>()) {
                waypoints_.push_back({ tr->position, 0.0f, {} });
            } else {
                waypoints_.push_back({ {0,0,0}, 0.0f, {} });
            }
        }

        // ウェイポイントごとの編集行（位置・待ち時間・終点・削除・ウェーブ ID・分岐）
        // 削除した場合はループ中の配列が変わるため、その場で break する
        ImGui::Separator();
        for (size_t i = 0; i < waypoints_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("WP %zu", i);
            ImGui::SameLine();
            ImGui::DragFloat3("##pos", &waypoints_[i].pos.x, 0.1f);
            ImGui::SameLine();
            ImGui::DragFloat("Wait(s)", &waypoints_[i].waitTime, 0.1f, 0.0f, 60.0f);
            ImGui::SameLine();
            ImGui::Checkbox("End", &waypoints_[i].isEnd);
            ImGui::SameLine();
            bool erased = false;
            if (ImGui::Button("X")) {
                waypoints_.erase(waypoints_.begin() + i);
                // インデックスの範囲外アクセスを防ぐ
                if (currentWaypointIndex_ >= static_cast<int>(waypoints_.size())) {
                    currentWaypointIndex_ = static_cast<int>(waypoints_.size()) - 1;
                    if (currentWaypointIndex_ < 0) currentWaypointIndex_ = 0;
                }
                erased = true;
            }
            if (!erased) {
                // A-03: この地点で発生させるウェーブ。0 なら通過するだけ。
                ImGui::Indent();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::DragInt("Wave ID", &waypoints_[i].waveId, 1.0f, 0, 99);
                if (waypoints_[i].waveId != 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(waveColor_.x, waveColor_.y, waveColor_.z, 1.0f),
                                       "(stops until wave %d is cleared)", waypoints_[i].waveId);
                }
                ImGui::Unindent();
                DrawBranchEditor(waypoints_[i], static_cast<int>(i));
            }
            ImGui::PopID();
            if (erased) break;
        }

#endif
    }

    /// @brief ウェイポイント（球）・直進線・分岐リンク・ループ線をデバッグ描画する
    void OnDebugRender() override {
#if RC_ENABLE_IMGUI
        // 選択時（または全描画モード時）のみパスとウェイポイントを描画
        if (drawPath_ && waypoints_.size() >= 1) {
            for (size_t i = 0; i < waypoints_.size(); ++i) {
                // 引数: center, radius, color, slices, stacks, depth
                // 分岐点は大きめの球、終点はさらに大きい球にして役割が見えるようにする
                const bool isBranchPoint = !waypoints_[i].branches.empty();
                const bool isEndPoint = waypoints_[i].isEnd;
                const bool isWavePoint = (waypoints_[i].waveId != 0);
                RC::DrawWireSphere3D(waypoints_[i].pos,
                                     isEndPoint ? 1.2f : (isBranchPoint ? 0.9f : 0.5f),
                                     isWavePoint ? waveColor_ : (isBranchPoint ? branchColor_ : pathColor_),
                                     16, 16, true);
                // ウェーブ地点は二重丸にして、分岐点と一目で見分けられるようにする
                if (isWavePoint) {
                    RC::DrawWireSphere3D(waypoints_[i].pos, 1.6f, waveColor_, 16, 16, true);
                }
                // 終点からは直進線を引かない（次の index は別ルートの起点になりうる）
                if (i < waypoints_.size() - 1 && !isEndPoint) {
                    RC::DrawLine3D(waypoints_[i].pos, waypoints_[i + 1].pos, pathColor_, true);
                }
            }
            // 分岐リンクは直進線と別色で描く（同じ線に重なると区別できないため
            // 少し上へ持ち上げた折れ線として描画する）
            for (size_t i = 0; i < waypoints_.size(); ++i) {
                for (const auto& b : waypoints_[i].branches) {
                    if (b.target < 0 || b.target >= static_cast<int>(waypoints_.size())) continue;
                    const RC::Vector3 from = waypoints_[i].pos;
                    const RC::Vector3 to   = waypoints_[b.target].pos;
                    const RC::Vector3 mid  = {
                        (from.x + to.x) * 0.5f,
                        (from.y + to.y) * 0.5f + 2.0f, // 直進線と分離するための持ち上げ
                        (from.z + to.z) * 0.5f
                    };
                    RC::DrawLine3D(from, mid, branchColor_, true);
                    RC::DrawLine3D(mid, to, branchColor_, true);
                }
            }
            // ループ時は末尾から先頭へ戻る線も引く
            if (loop_ && waypoints_.size() >= 2) {
                RC::DrawLine3D(waypoints_.back().pos, waypoints_.front().pos, pathColor_, true);
            }
        }
#endif
    }

protected:
    /// @brief ウェイポイントが無ければ現在位置を追加し、ランタイム専用タグと内部状態を初期化する
    void OnCreate() override {
        // 初期化時に何もウェイポイントがなければ、現在位置を追加しておく
        if (waypoints_.empty()) {
            if (auto* tr = GetComponent<TransformComponent>()) {
                waypoints_.push_back({ tr->position, 0.0f, {} });
            }
        }

        // ランタイム専用タグの消し込み。
        // タグは Entity::Serialize で JSON に往復するので、前回のプレイ中に保存された
        // rail_finished が残っていると、リトライした瞬間に Result へ飛んでしまう。
        if (Entity* self = GetEntity()) {
            self->ClearTag("rail_finished");
            // 動作確認のために公開しているタグも同じくランタイム専用。
            // 特に rail_branch_count は加算なので、消さないと保存のたびに
            // 前回までの回数が積み上がり、「今回のプレイで何回分岐したか」ではなく
            // 歴代の合計を表示することになる（D-09 と同じ形の焼き付き）。
            self->ClearTag("rail_wp");
            self->ClearTag("rail_branch_to");
            self->ClearTag("rail_branch_count");
            // レールが敷かれているシーンであることの目印。
            // シーン側のクリア判定が「全滅」か「終点到達」かを、型を知らずに選ぶために使う。
            // 保存されて JSON に焼き付いても意味は変わらず、毎回 OnCreate で立て直る。
            self->SetTag("has_rail", 1);
        }
        railFinishedNotified_ = false;
        waitingWaveId_ = 0;
        waveWaypointIndex_ = -1;
        clearedWaves_.clear();
        reachedCount_ = 0;
    }

    /// @brief 状態タグ・達成率を公開し、演出中／ゲームオーバー／ウェーブ待機／待ち時間を考慮してウェイポイントへ移動する
    /// @param deltaTime 前フレームからの経過時間（秒）
    void OnUpdate(float deltaTime) override {
        // いま向かっているウェイポイント番号を外へ公開する（動作確認用）。
        // 早期 return の前に置いてあるのは、待機中や終点でも最後の番号が
        // 残っていてほしいため。
        if (Entity* self = GetEntity()) {
            self->SetTag("rail_wp", currentWaypointIndex_);
        }

        // リザルトの「ウェイポイント達成率」を GameSession へ写す。
        // 到達数はこのスクリプトのメンバなのでシーンを抜けると失われる。
        // RailShooterController の score / hp と同じく毎フレーム上書きしておけば、
        // ゲームオーバーで途中終了しても最後の値が残る。
        GameSession::Get().SetWaypointProgress(reachedCount_, reachedCount_ + RemainingCount());

        if (waypoints_.empty()) return;

        // 見せ用デモ（ClearRunDemo）：暗転中に終点の手前（値は cm）へ移る。読んだらタグは消す
        if (Entity* self = GetEntity()) {
            const int jumpCm = self->GetTagInt("demo_rail_jump", 0);
            if (jumpCm > 0) {
                self->ClearTag("demo_rail_jump");
                DemoJumpNearEnd(static_cast<float>(jumpCm) / 100.0f);
            }
        }

        // 開始演出（DeepRiseIntroScript が深海から浮上させている）のあいだは出発しない。
        // タグは同じエンティティに立つ。演出が終わると外れるので、そこからレールが動く。
        if (Entity* self = GetEntity()) {
            if (self->GetTagInt("intro_playing", 0) == 1) return;
            // 決着後の演出（ClearAscendScript / DeathSinkScript）がカメラを動かしている間は進まない
            if (self->GetTagInt("result_transition_owner", 0) == 1) return;
            // 見せ用デモ（ClearRunDemo）がサメを倒すまで止めている
            if (self->GetTagInt("demo_rail_hold", 0) == 1) return;
        }

        // ゲームオーバー判定
        // ウェーブ待機の解除より先に見る。プレイヤーが力尽きたあとに
        // 生き残った敵が消えてウェーブがクリア扱いになり、動き出すのを防ぐため。
        if (Scene* scene = GetScene()) {
            for (auto& e : scene->GetEntities()) {
                if (e->GetTagInt("game_over", 0) == 1) {
                    return; // 移動停止
                }
            }
        }

        // A-03: ウェーブのクリア待ち
        // 待機中は isMoving を落として止めているので、下の !isMoving より先に処理する。
        if (waitingWaveId_ != 0) {
            if (!IsWaitingWaveCleared()) return;

            const int arrivedIndex = waveWaypointIndex_;
            clearedWaves_.push_back(waitingWaveId_);
            waitingWaveId_ = 0;
            waveWaypointIndex_ = -1;
            isMoving_ = true;
            if (arrivedIndex >= 0 && arrivedIndex < static_cast<int>(waypoints_.size())) {
                AdvanceFrom(arrivedIndex);
            }
            return; // 再開したフレームは移動せず、次フレームから進む
        }

        // 停止中、または index が範囲外（終点到達後など）なら動かない
        if (!isMoving_ ||
            currentWaypointIndex_ < 0 ||
            currentWaypointIndex_ >= static_cast<int>(waypoints_.size())) {
            return;
        }

        // 待機処理
        if (currentWaitTimer_ > 0.0f) {
            currentWaitTimer_ -= deltaTime;
            return; // 待機中は移動しない
        }

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        // 目標地点までの距離と今フレームの移動量を比べ、届くなら到達処理、届かないなら等速で近づく
        RC::Vector3 targetPos = waypoints_[currentWaypointIndex_].pos;
        RC::Vector3 diff = RC::Sub(targetPos, tr->position);
        float dist = RC::Length(diff);

        float moveAmount = speed_ * deltaTime;

        if (dist <= moveAmount) {
            // ターゲット地点に到達した
            tr->position = targetPos;

            const int arrivedIndex = currentWaypointIndex_;
            ++reachedCount_; // 達成率の分子。ウェーブ待機に入っても到達は到達として数える

            // ウェーブ地点なら、次の行き先を決める前に止まって決着を待つ。
            // 先に分岐を解決してしまうとウェーブの結果を条件に使えなくなるため、
            // ResolveNextIndex はクリア後（AdvanceFrom）まで遅らせている。
            if (BeginWaveWait(arrivedIndex)) return;

            AdvanceFrom(arrivedIndex);
        } else {
            // まだ到達していないので進む
            RC::Vector3 dir = RC::SafeNormalize(diff);
            tr->position = RC::Add(tr->position, RC::Mul(dir, moveAmount));

            // TODO: カメラやプレイヤーの向き（Rotation）を進行方向に向ける処理を将来的に追加する
        }
    }

    /// @brief 破棄時の処理（このクラスでは何もしない）
    void OnDestroy() override {
    }

private:
    /// @brief 指定ウェーブがこのラップですでにクリア済みか
    /// @param waveId 調べるウェーブ id
    /// @return クリア済みなら true
    bool IsWaveAlreadyCleared(int waveId) const {
        for (int id : clearedWaves_) {
            if (id == waveId) return true;
        }
        return false;
    }

    /// @brief WaveManager のエンティティを waveManagerName で引く
    /// @return 見つかったエンティティ（名前が空・シーン無し・居ない場合は nullptr）
    Entity* FindWaveManager() {
        if (waveManagerName_.empty()) return nullptr;
        Scene* scene = GetScene();
        if (!scene) return nullptr;
        return scene->FindEntityByName(waveManagerName_);
    }

    /// @brief ウェーブ地点に到達したので、WaveManager へ開始を要求して停止する
    /// @param arrivedIndex 到達したウェイポイントの index
    /// @return 実際に待機へ入ったら true（ウェーブ無し・クリア済み・WaveManager 不在なら false）
    bool BeginWaveWait(int arrivedIndex) {
        const int waveId = waypoints_[arrivedIndex].waveId;
        if (waveId == 0) return false;
        if (IsWaveAlreadyCleared(waveId)) return false;
        // 見せ用デモ（ClearRunDemo）が終盤へ飛ばしたあとは、ウェーブ地点で止まらない
        if (Entity* self = GetEntity()) {
            if (self->GetTagInt("demo_skip_waves", 0) == 1) return false;
        }

        Entity* manager = FindWaveManager();
        if (!manager) {
            // WaveManager が居ないシーンではウェーブ地点をただの通過点として扱う。
            // 止まったまま進めなくなるより、素通りして先へ行けたほうが原因が見えやすい。
            Log::Print("[RailMovementScript] wave manager '" + waveManagerName_ +
                       "' not found. skipping wave " + std::to_string(waveId));
            return false;
        }

        manager->SetTag("wave_request", waveId);
        waitingWaveId_ = waveId;
        waveWaypointIndex_ = arrivedIndex;
        isMoving_ = false;
        lastBranchLog_ = "wave " + std::to_string(waveId) + " at WP " + std::to_string(arrivedIndex);
        return true;
    }

    /// @brief 待っているウェーブが決着したか
    /// @return WaveManager の wave_cleared_id が待機中の id と一致すれば true（WaveManager が居なくなった場合も true）
    bool IsWaitingWaveCleared() {
        Entity* manager = FindWaveManager();
        if (!manager) return true; // 途中で居なくなったら止まり続けない
        return manager->GetTagInt("wave_cleared_id", 0) == waitingWaveId_;
    }

    /// @brief 終点に到達したことをシーンへ知らせる
    void NotifyRailFinished() {
        if (railFinishedNotified_) return;
        if (Entity* self = GetEntity()) {
            self->SetTag("rail_finished", 1);
            railFinishedNotified_ = true;
        }
    }

    /// @brief 到達したウェイポイントから次の行き先へ進む
    /// @details ウェーブ待機の前後で同じ処理を使うため切り出した。
    ///          分岐条件はここで評価されるので、ウェーブの結果を見て分岐させることもできる。
    /// @param arrivedIndex 到達したウェイポイントの index
    void AdvanceFrom(int arrivedIndex) {
        currentWaitTimer_ = waypoints_[arrivedIndex].waitTime;
        currentWaypointIndex_ = ResolveNextIndex(arrivedIndex);

        // 次の index が配列外（末尾を越えた／終点）なら、ループか停止かを決める
        if (currentWaypointIndex_ >= static_cast<int>(waypoints_.size())) {
            if (loop_) {
                currentWaypointIndex_ = 0; // ループする場合
                clearedWaves_.clear();     // 次のラップではウェーブをやり直す
                reachedCount_ = 0;         // 達成率もラップごとに数え直す
            } else {
                isMoving_ = false; // 終点に到達して停止
                NotifyRailFinished();
            }
        }
    }

    /// @brief 見せ用デモ：終点（isEnd か末尾）へ向かう最後の区間の、終点から distance m 手前へ移る
    /// @details ウェーブ待ちは解除し、次の行き先を終点にする。達成率が途中のまま残らないよう、
    ///          飛ばした地点も到達したものとして数える。
    /// @param distance 終点の何 m 手前へ置くか（区間より長ければ区間の始点）
    void DemoJumpNearEnd(float distance) {
        const int n = static_cast<int>(waypoints_.size());
        if (n < 2) return;
        int end = n - 1;
        for (int i = 0; i < n; ++i) {
            if (waypoints_[i].isEnd) { end = i; break; }
        }
        if (end <= 0) return;
        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;
        const RC::Vector3 a = waypoints_[end - 1].pos;
        const RC::Vector3 b = waypoints_[end].pos;
        const RC::Vector3 seg = RC::Sub(b, a);
        const float len = RC::Length(seg);
        const float d = (std::min)(distance, len);
        tr->position = (len > 1e-4f) ? RC::Sub(b, RC::Mul(RC::SafeNormalize(seg), d)) : a;
        waitingWaveId_ = 0;
        waveWaypointIndex_ = -1;
        currentWaypointIndex_ = end;
        currentWaitTimer_ = 0.0f;
        isMoving_ = true;
        reachedCount_ = (std::max)(reachedCount_, end);
        lastBranchLog_ = "demo jump -> " + std::to_string(end);
        Log::Print("[RailMovementScript] demo jump near end (WP " + std::to_string(end) + ")");
    }

    /// @brief 分岐条件が成立しているか判定する
    /// @param b 判定する分岐定義
    /// @return 成立していれば true
    /// @details 判定の中身は条件クラスに任せる（ポリモーフィズム）。
    bool IsBranchTaken(const RailBranch& b) {
        if (!b.condition) return false;
        RailBranchContext ctx;
        ctx.self = GetEntity();
        ctx.scene = GetScene();
        return b.condition->IsSatisfied(b, ctx);
    }

    /// @brief 到達したウェイポイントから、次に向かう index を決める
    /// @param arrivedIndex 到達したウェイポイントの index
    /// @return 次の index（分岐が成立しなければ arrivedIndex + 1）
    int ResolveNextIndex(int arrivedIndex) {
        const Waypoint& wp = waypoints_[arrivedIndex];

        // 終点なら分岐も直進もしない。範囲外を返して呼び出し側に停止させる。
        if (wp.isEnd) {
            lastBranchLog_ = "end at " + std::to_string(arrivedIndex);
            return static_cast<int>(waypoints_.size());
        }

        // 分岐を先頭から評価し、成立かつ行き先が有効な最初の分岐を採用する
        for (const auto& b : wp.branches) {
            if (!IsBranchTaken(b)) continue;

            if (b.target < 0 || b.target >= static_cast<int>(waypoints_.size())) {
                Log::Print("[RailMovementScript] branch target out of range at WP " +
                           std::to_string(arrivedIndex));
                continue;
            }
            // 自分自身へ戻す分岐は毎フレーム到達判定が成立して進まなくなるため弾く
            if (b.target == arrivedIndex) {
                Log::Print("[RailMovementScript] branch target points to itself at WP " +
                           std::to_string(arrivedIndex));
                continue;
            }
            lastBranchLog_ = std::string(b.condition->Name()) + " -> " + std::to_string(b.target);
            // 分岐が成立したことを外から読めるようにしておく（動作確認用）
            if (Entity* self = GetEntity()) {
                self->SetTag("rail_branch_to", b.target);
                self->SetTag("rail_branch_count",
                             self->GetTagInt("rail_branch_count", 0) + 1);
            }
            return b.target;
        }

        // どの分岐も成立しなければ直進
        lastBranchLog_ = "straight -> " + std::to_string(arrivedIndex + 1);
        if (Entity* self = GetEntity()) {
            self->SetTag("rail_branch_to", -1);
        }
        return arrivedIndex + 1;
    }

#if RC_ENABLE_IMGUI
    /// @brief 1ウェイポイントぶんの分岐編集 UI
    /// @param wp 編集対象のウェイポイント
    /// @param wpIndex 編集対象のウェイポイント index（追加する分岐の既定の行き先に使う）
    void DrawBranchEditor(Waypoint& wp, int wpIndex) {
        const std::string header = "Branches (" + std::to_string(wp.branches.size()) + ")";
        if (!ImGui::TreeNode(header.c_str())) return;

        // 追加する分岐の既定の行き先は次の地点（末尾なら 0）
        if (ImGui::Button("Add Branch")) {
            RailBranch b;
            b.target = (wpIndex + 1 < static_cast<int>(waypoints_.size())) ? wpIndex + 1 : 0;
            wp.branches.push_back(b);
        }

        // コンボの項目は条件クラスの一覧から作る（並びは RailBranchConditions::All() の順）
        const auto& conditions = RailBranchConditions::All();
        const char* conditionLabels[RailBranchConditions::kCount] = {};
        for (size_t ci = 0; ci < conditions.size(); ++ci) {
            conditionLabels[ci] = conditions[ci]->Name();
        }

        // 分岐ごとの編集。ID はウェイポイント側の PushID と衝突しないよう +1000 する
        for (size_t bi = 0; bi < wp.branches.size(); ++bi) {
            ImGui::PushID(static_cast<int>(bi) + 1000);
            auto& b = wp.branches[bi];

            int cond = RailBranchConditions::IndexOf(b.condition);
            if (ImGui::Combo("Condition", &cond, conditionLabels,
                             static_cast<int>(RailBranchConditions::kCount))) {
                if (cond >= 0 && cond < static_cast<int>(RailBranchConditions::kCount)) {
                    b.condition = conditions[static_cast<size_t>(cond)];
                }
            }
            ImGui::DragInt("Target WP", &b.target, 1.0f, 0,
                           static_cast<int>(waypoints_.size()) - 1);

            // 条件の種類に応じたパラメータ入力欄は条件クラス自身が出す（ポリモーフィズム）
            const IRailBranchCondition& condition =
                b.condition ? *b.condition : RailBranchConditions::Default();
            condition.DrawParamEditor(b);

            // 削除したら配列が変わるので、PopID してからループを抜ける
            if (ImGui::Button("Remove Branch")) {
                wp.branches.erase(wp.branches.begin() + bi);
                ImGui::PopID();
                break;
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
#endif

    // --- インスペクタ / JSON から設定する項目 ---
    float speed_ = 5.0f;                ///< 移動速度（m/秒）
    std::vector<Waypoint> waypoints_;   ///< レールのウェイポイント列（分岐を含む）
    int currentWaypointIndex_ = 0;      ///< 現在向かっているウェイポイント index
    bool isMoving_ = true;              ///< 進行中か（Pause / Resume・インスペクタで切り替わる）
    bool loop_ = false;                 ///< 終点に着いたら先頭へ戻るか

    /// @brief ウェーブ開始を要求する相手のエンティティ名（A-03）
    std::string waveManagerName_ = "WaveManager";

    bool drawPath_ = true;                                  ///< デバッグ用パス描画フラグ
    RC::Vector4 pathColor_ = { 1.0f, 1.0f, 0.0f, 1.0f };    ///< 直進パスの色（黄）
    RC::Vector4 branchColor_ = { 0.0f, 0.8f, 1.0f, 1.0f };  ///< 分岐パスの色（シアン）
    RC::Vector4 waveColor_ = { 1.0f, 0.35f, 0.1f, 1.0f };   ///< ウェーブ地点の色（橙）

    // --- 実行時の状態 ---
    float currentWaitTimer_ = 0.0f; ///< 到達した地点での残り待ち時間（秒）
    std::string lastBranchLog_;     ///< 直近に選ばれた分岐（ImGui 表示用）
    int reachedCount_ = 0;          ///< このラップで到達したウェイポイントの数（達成率用）

    // --- A-03 ウェーブ待機の状態 ---
    /// @brief クリアを待っているウェーブ id（0 なら待機していない）
    int waitingWaveId_ = 0;
    /// @brief 待機に入ったウェイポイント index。クリア後にここから次の行き先を解決する
    int waveWaypointIndex_ = -1;
    /// @brief 終点通知をすでに出したか（毎フレーム立て直さないためのラッチ）
    bool railFinishedNotified_ = false;
    /// @brief このラップですでにクリアしたウェーブ id
    /// @details 分岐で合流したルートが同じウェーブ地点をもう一度通ることがある。
    ///          そのたびに戦い直させると T-20 で踏んだ周回と同じ行き止まりになるため、
    ///          一度クリアしたウェーブは素通りする。loop で index 0 へ戻るときだけ空にする。
    std::vector<int> clearedWaves_;
};

REGISTER_SCRIPT(RailMovementScript)
