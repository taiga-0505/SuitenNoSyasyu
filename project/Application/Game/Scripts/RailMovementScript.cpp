#include "ECS/ScriptableEntity.h"
#include "ECS/ScriptRegistry.h"
#include "ECS/TransformComponent.h"
#include "Common/Math/MathUtils.h"
#include "Common/Log/Log.h"
#include "Input/Input.h"
#include "Scene.h"
#include "RenderCommon.h"
#include "Game/Framework/GameSession.h"
#include <vector>
#include <string>
#include <random>
#include <climits>
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

/// @brief 分岐が成立する条件の種類
enum class BranchCondition : int {
    Always = 0,   ///< 無条件（必ずこの分岐へ）
    TagEquals,    ///< 参照エンティティの int タグが value と一致
    TagAtLeast,   ///< 参照エンティティの int タグが value 以上
    KeyHeld,      ///< 指定キーが押されている（プレイヤーに選ばせる分岐）
    Chance,       ///< chance の確率で成立（0.0〜1.0）
};

static const char* ToString(BranchCondition c) {
    switch (c) {
    case BranchCondition::Always:     return "always";
    case BranchCondition::TagEquals:  return "tag_equals";
    case BranchCondition::TagAtLeast: return "tag_at_least";
    case BranchCondition::KeyHeld:    return "key_held";
    case BranchCondition::Chance:     return "chance";
    }
    return "always";
}

static BranchCondition ConditionFromString(const std::string& s) {
    if (s == "tag_equals")   return BranchCondition::TagEquals;
    if (s == "tag_at_least") return BranchCondition::TagAtLeast;
    if (s == "key_held")     return BranchCondition::KeyHeld;
    if (s == "chance")       return BranchCondition::Chance;
    return BranchCondition::Always;
}

/// @brief ウェイポイントに紐づく分岐の定義
struct RailBranch {
    int target = -1;                              ///< 分岐先ウェイポイント index
    BranchCondition condition = BranchCondition::Always;
    std::string entityName = "player";            ///< タグを読むエンティティ名（空なら自分自身）
    std::string tagKey;                           ///< 参照するタグのキー
    int value = 1;                                ///< TagEquals / TagAtLeast の比較値
    float chance = 0.5f;                          ///< Chance の確率（0..1）
    int keyCode = 0;                              ///< KeyHeld の DIK コード
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

/// @brief レール移動（パス移動）を制御するスクリプトコンポーネント
class RailMovementScript : public ScriptableEntity {
public:
    float speed = 5.0f;
    std::vector<Waypoint> waypoints;
    int currentWaypointIndex = 0;
    bool isMoving = true;
    bool loop = false;

    /// @brief ウェーブ開始を要求する相手のエンティティ名（A-03）
    std::string waveManagerName = "WaveManager";

    bool drawPath = true; // デバッグ用パス描画フラグ
    RC::Vector4 pathColor = { 1.0f, 1.0f, 0.0f, 1.0f };   // 直進パスの色（黄）
    RC::Vector4 branchColor = { 0.0f, 0.8f, 1.0f, 1.0f }; // 分岐パスの色（シアン）
    RC::Vector4 waveColor = { 1.0f, 0.35f, 0.1f, 1.0f };  // ウェーブ地点の色（橙）

    // -----------------------------------------------------------------
    // 外部スクリプトから使う API
    // -----------------------------------------------------------------

    /// @brief 次に向かうウェイポイントを外部から指定する
    /// @details A-03 のウェーブ戦闘やイベントスクリプトから、条件式に落とし込めない
    ///          分岐を直接指示するための入口。範囲外は無視する。
    /// @return 受理したら true
    bool ForceNextWaypoint(int index) {
        if (index < 0 || index >= static_cast<int>(waypoints.size())) {
            Log::Print("[RailMovementScript] ForceNextWaypoint: index out of range");
            return false;
        }
        currentWaypointIndex = index;
        currentWaitTimer = 0.0f;
        isMoving = true;
        lastBranchLog_ = "forced -> " + std::to_string(index);
        return true;
    }

    /// @brief 進行を一時停止する（A-03 のウェーブ戦闘で使用）
    void Pause() { isMoving = false; }

    /// @brief 進行を再開する
    void Resume() { isMoving = true; }

    /// @brief 現在向かっているウェイポイント index
    int CurrentIndex() const { return currentWaypointIndex; }

    /// @brief ウェーブのクリア待ちで停止しているか（0 なら待っていない）
    int WaitingWaveId() const { return waitingWaveId_; }

    /// @brief このラップで到達したウェイポイントの数（リザルトの達成率の分子）
    int ReachedCount() const { return reachedCount_; }

    /// @brief いま向かっている地点から終点までに、あと何地点通る予定か
    /// @details 分岐は「無条件（always）の分岐があればそれ、無ければ直進」とみなして数える。
    ///          条件付き分岐の行き先は走ってみるまで分からないので、本線を進む前提で見積もり、
    ///          実際に分岐した時点で数え直す（達成率が途中で少し跳ねるのは許容する）。
    ///          終点（isEnd）か配列の末尾で止める。ループ検出のために訪問済みを覚えておく。
    int RemainingCount() const {
        const int n = static_cast<int>(waypoints.size());
        if (n == 0) return 0;
        int index = currentWaypointIndex;
        if (index < 0 || index >= n) return 0; // 終点到達後は index が配列の外に出ている

        std::vector<char> visited(static_cast<size_t>(n), 0);
        int count = 0;
        while (index >= 0 && index < n && !visited[static_cast<size_t>(index)]) {
            visited[static_cast<size_t>(index)] = 1;
            ++count;
            const Waypoint& wp = waypoints[static_cast<size_t>(index)];
            if (wp.isEnd) break;

            int next = index + 1;
            for (const auto& b : wp.branches) {
                if (b.condition == BranchCondition::Always &&
                    b.target >= 0 && b.target < n && b.target != index) {
                    next = b.target;
                    break;
                }
            }
            index = next;
        }
        return count;
    }

private:
    float currentWaitTimer = 0.0f;
    std::string lastBranchLog_; ///< 直近に選ばれた分岐（ImGui 表示用）
    int reachedCount_ = 0;      ///< このラップで到達したウェイポイントの数（達成率用）

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

    /// @brief 指定ウェーブがこのラップですでにクリア済みか
    bool IsWaveAlreadyCleared(int waveId) const {
        for (int id : clearedWaves_) {
            if (id == waveId) return true;
        }
        return false;
    }

    /// @brief WaveManager のエンティティを引く（居なければ nullptr）
    Entity* FindWaveManager() {
        if (waveManagerName.empty()) return nullptr;
        Scene* scene = GetScene();
        if (!scene) return nullptr;
        return scene->FindEntityByName(waveManagerName);
    }

    /// @brief ウェーブ地点に到達したので、WaveManager へ開始を要求して停止する
    /// @return 実際に待機へ入ったら true
    bool BeginWaveWait(int arrivedIndex) {
        const int waveId = waypoints[arrivedIndex].waveId;
        if (waveId == 0) return false;
        if (IsWaveAlreadyCleared(waveId)) return false;

        Entity* manager = FindWaveManager();
        if (!manager) {
            // WaveManager が居ないシーンではウェーブ地点をただの通過点として扱う。
            // 止まったまま進めなくなるより、素通りして先へ行けたほうが原因が見えやすい。
            Log::Print("[RailMovementScript] wave manager '" + waveManagerName +
                       "' not found. skipping wave " + std::to_string(waveId));
            return false;
        }

        manager->SetTag("wave_request", waveId);
        waitingWaveId_ = waveId;
        waveWaypointIndex_ = arrivedIndex;
        isMoving = false;
        lastBranchLog_ = "wave " + std::to_string(waveId) + " at WP " + std::to_string(arrivedIndex);
        return true;
    }

    /// @brief 待っているウェーブが決着したか
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
    void AdvanceFrom(int arrivedIndex) {
        currentWaitTimer = waypoints[arrivedIndex].waitTime;
        currentWaypointIndex = ResolveNextIndex(arrivedIndex);

        if (currentWaypointIndex >= static_cast<int>(waypoints.size())) {
            if (loop) {
                currentWaypointIndex = 0; // ループする場合
                clearedWaves_.clear();    // 次のラップではウェーブをやり直す
                reachedCount_ = 0;        // 達成率もラップごとに数え直す
            } else {
                isMoving = false; // 終点に到達して停止
                NotifyRailFinished();
            }
        }
    }

    /// @brief 0.0〜1.0 の一様乱数
    static float RandomUnit() {
        static std::mt19937 engine{ std::random_device{}() };
        static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(engine);
    }

    /// @brief 分岐が参照するエンティティの int タグを読む
    /// @return 見つからなければ INT_MIN（＝条件は不成立扱い）
    int ReadBranchTag(const RailBranch& b) {
        if (b.tagKey.empty()) return INT_MIN;

        Entity* target = nullptr;
        if (b.entityName.empty()) {
            target = GetEntity();
        } else if (Scene* scene = GetScene()) {
            target = scene->FindEntityByName(b.entityName);
        }
        if (!target || !target->HasTag(b.tagKey)) return INT_MIN;
        return target->GetTagInt(b.tagKey, INT_MIN);
    }

    /// @brief 分岐条件が成立しているか判定する
    bool IsBranchTaken(const RailBranch& b) {
        switch (b.condition) {
        case BranchCondition::Always:
            return true;
        case BranchCondition::TagEquals: {
            const int v = ReadBranchTag(b);
            return (v != INT_MIN) && (v == b.value);
        }
        case BranchCondition::TagAtLeast: {
            const int v = ReadBranchTag(b);
            return (v != INT_MIN) && (v >= b.value);
        }
        case BranchCondition::KeyHeld: {
            if (b.keyCode <= 0 || b.keyCode > 255) return false;
            auto* input = Input::GetInstance();
            return input && input->IsKeyPressed(static_cast<uint8_t>(b.keyCode));
        }
        case BranchCondition::Chance:
            return RandomUnit() < b.chance;
        }
        return false;
    }

    /// @brief 到達したウェイポイントから、次に向かう index を決める
    /// @param arrivedIndex 到達したウェイポイントの index
    /// @return 次の index（分岐が成立しなければ arrivedIndex + 1）
    int ResolveNextIndex(int arrivedIndex) {
        const Waypoint& wp = waypoints[arrivedIndex];

        // 終点なら分岐も直進もしない。範囲外を返して呼び出し側に停止させる。
        if (wp.isEnd) {
            lastBranchLog_ = "end at " + std::to_string(arrivedIndex);
            return static_cast<int>(waypoints.size());
        }

        for (const auto& b : wp.branches) {
            if (!IsBranchTaken(b)) continue;

            if (b.target < 0 || b.target >= static_cast<int>(waypoints.size())) {
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
            lastBranchLog_ = std::string(ToString(b.condition)) + " -> " + std::to_string(b.target);
            // 分岐が成立したことを外から読めるようにしておく（動作確認用）
            if (Entity* self = GetEntity()) {
                self->SetTag("rail_branch_to", b.target);
                self->SetTag("rail_branch_count",
                             self->GetTagInt("rail_branch_count", 0) + 1);
            }
            return b.target;
        }

        lastBranchLog_ = "straight -> " + std::to_string(arrivedIndex + 1);
        if (Entity* self = GetEntity()) {
            self->SetTag("rail_branch_to", -1);
        }
        return arrivedIndex + 1;
    }

protected:
    nlohmann::json Serialize() override {
        nlohmann::json j;
        j["speed"] = speed;
        // ウェーブ待機中の停止は「一時的な状態」であって設定値ではない。
        // そのまま false を書くと、待機中にシーンを保存したときに
        // 二度と動かないレールがデータに焼き付く（D-09 と同じ形の事故）ため、
        // 待機が理由で止まっている場合は動く状態として書き出す。
        j["isMoving"] = (waitingWaveId_ != 0) ? true : isMoving;
        j["loop"] = loop;
        j["drawPath"] = drawPath;
        j["waveManagerName"] = waveManagerName;
        j["pathColor"] = { pathColor.x, pathColor.y, pathColor.z, pathColor.w };
        j["branchColor"] = { branchColor.x, branchColor.y, branchColor.z, branchColor.w };
        j["waveColor"] = { waveColor.x, waveColor.y, waveColor.z, waveColor.w };
        nlohmann::json wpArray = nlohmann::json::array();
        for (const auto& wp : waypoints) {
            nlohmann::json wj;
            wj["pos"] = { wp.pos.x, wp.pos.y, wp.pos.z };
            wj["waitTime"] = wp.waitTime;
            if (wp.isEnd) wj["isEnd"] = true;
            if (wp.waveId != 0) wj["waveId"] = wp.waveId;
            if (!wp.branches.empty()) {
                nlohmann::json bArray = nlohmann::json::array();
                for (const auto& b : wp.branches) {
                    bArray.push_back({
                        {"target", b.target},
                        {"condition", ToString(b.condition)},
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

    void Deserialize(const nlohmann::json& j) override {
        if (j.contains("speed")) speed = j["speed"].get<float>();
        if (j.contains("isMoving")) isMoving = j["isMoving"].get<bool>();
        if (j.contains("loop")) loop = j["loop"].get<bool>();
        if (j.contains("drawPath")) drawPath = j["drawPath"].get<bool>();
        if (j.contains("pathColor") && j["pathColor"].size() == 4) {
            pathColor.x = j["pathColor"][0];
            pathColor.y = j["pathColor"][1];
            pathColor.z = j["pathColor"][2];
            pathColor.w = j["pathColor"][3];
        }
        if (j.contains("branchColor") && j["branchColor"].size() == 4) {
            branchColor.x = j["branchColor"][0];
            branchColor.y = j["branchColor"][1];
            branchColor.z = j["branchColor"][2];
            branchColor.w = j["branchColor"][3];
        }
        if (j.contains("waveColor") && j["waveColor"].size() == 4) {
            waveColor.x = j["waveColor"][0];
            waveColor.y = j["waveColor"][1];
            waveColor.z = j["waveColor"][2];
            waveColor.w = j["waveColor"][3];
        }
        if (j.contains("waveManagerName")) waveManagerName = j["waveManagerName"].get<std::string>();
        if (j.contains("waypoints") && j["waypoints"].is_array()) {
            waypoints.clear();
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
                        if (bj.contains("condition")) b.condition = ConditionFromString(bj["condition"].get<std::string>());
                        if (bj.contains("entityName")) b.entityName = bj["entityName"].get<std::string>();
                        if (bj.contains("tagKey")) b.tagKey = bj["tagKey"].get<std::string>();
                        if (bj.contains("value")) b.value = bj["value"].get<int>();
                        if (bj.contains("chance")) b.chance = bj["chance"].get<float>();
                        if (bj.contains("keyCode")) b.keyCode = bj["keyCode"].get<int>();
                        wp.branches.push_back(b);
                    }
                }
                waypoints.push_back(wp);
            }
        }
    }

    void OnCreate() override {
        // 初期化時に何もウェイポイントがなければ、現在位置を追加しておく
        if (waypoints.empty()) {
            if (auto* tr = GetComponent<TransformComponent>()) {
                waypoints.push_back({ tr->position, 0.0f, {} });
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

    void OnUpdate(float deltaTime) override {
        // パス描画処理はOnImGui(選択時)に移動しました。

        // いま向かっているウェイポイント番号を外へ公開する（動作確認用）。
        // 早期 return の前に置いてあるのは、待機中や終点でも最後の番号が
        // 残っていてほしいため。
        if (Entity* self = GetEntity()) {
            self->SetTag("rail_wp", currentWaypointIndex);
        }

        // リザルトの「ウェイポイント達成率」を GameSession へ写す。
        // 到達数はこのスクリプトのメンバなのでシーンを抜けると失われる。
        // RailShooterController の score / hp と同じく毎フレーム上書きしておけば、
        // ゲームオーバーで途中終了しても最後の値が残る。
        GameSession::Get().SetWaypointProgress(reachedCount_, reachedCount_ + RemainingCount());

        if (waypoints.empty()) return;

        // 開始演出（DeepRiseIntroScript が深海から浮上させている）のあいだは出発しない。
        // タグは同じエンティティに立つ。演出が終わると外れるので、そこからレールが動く。
        if (Entity* self = GetEntity()) {
            if (self->GetTagInt("intro_playing", 0) == 1) return;
            // 決着後の演出（ClearAscendScript / DeathSinkScript）がカメラを動かしている間は進まない
            if (self->GetTagInt("result_transition_owner", 0) == 1) return;
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
            isMoving = true;
            if (arrivedIndex >= 0 && arrivedIndex < static_cast<int>(waypoints.size())) {
                AdvanceFrom(arrivedIndex);
            }
            return; // 再開したフレームは移動せず、次フレームから進む
        }

        if (!isMoving ||
            currentWaypointIndex < 0 ||
            currentWaypointIndex >= static_cast<int>(waypoints.size())) {
            return;
        }

        // 待機処理
        if (currentWaitTimer > 0.0f) {
            currentWaitTimer -= deltaTime;
            return; // 待機中は移動しない
        }

        auto* tr = GetComponent<TransformComponent>();
        if (!tr) return;

        RC::Vector3 targetPos = waypoints[currentWaypointIndex].pos;
        RC::Vector3 diff = RC::Sub(targetPos, tr->position);
        float dist = RC::Length(diff);

        float moveAmount = speed * deltaTime;

        if (dist <= moveAmount) {
            // ターゲット地点に到達した
            tr->position = targetPos;

            const int arrivedIndex = currentWaypointIndex;
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

    void OnDestroy() override {
    }

public:
    void OnImGui() override {
#if RC_ENABLE_IMGUI
        ImGui::Checkbox("Is Moving", &isMoving);
        ImGui::Checkbox("Loop", &loop);
        ImGui::DragFloat("Speed", &speed, 0.1f, 0.1f, 100.0f);
        ImGui::Checkbox("Draw Path", &drawPath);
        if (drawPath) {
            ImGui::ColorEdit4("Path Color", &pathColor.x);
            ImGui::ColorEdit4("Branch Color", &branchColor.x);
            ImGui::ColorEdit4("Wave Color", &waveColor.x);
        }

        {
            char mgrBuf[64] = {};
            std::snprintf(mgrBuf, sizeof(mgrBuf), "%s", waveManagerName.c_str());
            if (ImGui::InputText("Wave Manager", mgrBuf, sizeof(mgrBuf))) {
                waveManagerName = mgrBuf;
            }
        }

        ImGui::Text("Waypoints (Count: %zu)", waypoints.size());
        ImGui::Text("Current Index: %d", currentWaypointIndex);
        if (!lastBranchLog_.empty()) {
            ImGui::Text("Last Route: %s", lastBranchLog_.c_str());
        }
        if (currentWaitTimer > 0.0f) {
            ImGui::Text("Waiting... %.2f sec", currentWaitTimer);
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

        if (ImGui::Button("Restart Path")) {
            currentWaypointIndex = 0;
            currentWaitTimer = 0.0f;
            lastBranchLog_.clear();
            waitingWaveId_ = 0;
            waveWaypointIndex_ = -1;
            clearedWaves_.clear();
            reachedCount_ = 0;
            railFinishedNotified_ = false;
            isMoving = true;
            if (Entity* self = GetEntity()) self->ClearTag("rail_finished");
            if (!waypoints.empty()) {
                if (auto* tr = GetComponent<TransformComponent>()) {
                    tr->position = waypoints[0].pos;
                }
            }
        }
        ImGui::SameLine();

        if (ImGui::Button("Add Waypoint")) {
            if (!waypoints.empty()) {
                // 最後の要素の座標だけをコピーして追加（分岐はコピーしない）
                Waypoint wp;
                wp.pos = waypoints.back().pos;
                wp.waitTime = 0.0f;
                waypoints.push_back(wp);
            } else if (auto* tr = GetComponent<TransformComponent>()) {
                waypoints.push_back({ tr->position, 0.0f, {} });
            } else {
                waypoints.push_back({ {0,0,0}, 0.0f, {} });
            }
        }

        ImGui::Separator();
        for (size_t i = 0; i < waypoints.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("WP %zu", i);
            ImGui::SameLine();
            ImGui::DragFloat3("##pos", &waypoints[i].pos.x, 0.1f);
            ImGui::SameLine();
            ImGui::DragFloat("Wait(s)", &waypoints[i].waitTime, 0.1f, 0.0f, 60.0f);
            ImGui::SameLine();
            ImGui::Checkbox("End", &waypoints[i].isEnd);
            ImGui::SameLine();
            bool erased = false;
            if (ImGui::Button("X")) {
                waypoints.erase(waypoints.begin() + i);
                // インデックスの範囲外アクセスを防ぐ
                if (currentWaypointIndex >= static_cast<int>(waypoints.size())) {
                    currentWaypointIndex = static_cast<int>(waypoints.size()) - 1;
                    if (currentWaypointIndex < 0) currentWaypointIndex = 0;
                }
                erased = true;
            }
            if (!erased) {
                // A-03: この地点で発生させるウェーブ。0 なら通過するだけ。
                ImGui::Indent();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::DragInt("Wave ID", &waypoints[i].waveId, 1.0f, 0, 99);
                if (waypoints[i].waveId != 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(waveColor.x, waveColor.y, waveColor.z, 1.0f),
                                       "(stops until wave %d is cleared)", waypoints[i].waveId);
                }
                ImGui::Unindent();
                DrawBranchEditor(waypoints[i], static_cast<int>(i));
            }
            ImGui::PopID();
            if (erased) break;
        }

#endif
    }

    void OnDebugRender() override {
#if RC_ENABLE_IMGUI
        // 選択時（または全描画モード時）のみパスとウェイポイントを描画
        if (drawPath && waypoints.size() >= 1) {
            for (size_t i = 0; i < waypoints.size(); ++i) {
                // 引数: center, radius, color, slices, stacks, depth
                // 分岐点は大きめの球、終点はさらに大きい球にして役割が見えるようにする
                const bool isBranchPoint = !waypoints[i].branches.empty();
                const bool isEndPoint = waypoints[i].isEnd;
                const bool isWavePoint = (waypoints[i].waveId != 0);
                RC::DrawWireSphere3D(waypoints[i].pos,
                                     isEndPoint ? 1.2f : (isBranchPoint ? 0.9f : 0.5f),
                                     isWavePoint ? waveColor : (isBranchPoint ? branchColor : pathColor),
                                     16, 16, true);
                // ウェーブ地点は二重丸にして、分岐点と一目で見分けられるようにする
                if (isWavePoint) {
                    RC::DrawWireSphere3D(waypoints[i].pos, 1.6f, waveColor, 16, 16, true);
                }
                // 終点からは直進線を引かない（次の index は別ルートの起点になりうる）
                if (i < waypoints.size() - 1 && !isEndPoint) {
                    RC::DrawLine3D(waypoints[i].pos, waypoints[i + 1].pos, pathColor, true);
                }
            }
            // 分岐リンクは直進線と別色で描く（同じ線に重なると区別できないため
            // 少し上へ持ち上げた折れ線として描画する）
            for (size_t i = 0; i < waypoints.size(); ++i) {
                for (const auto& b : waypoints[i].branches) {
                    if (b.target < 0 || b.target >= static_cast<int>(waypoints.size())) continue;
                    const RC::Vector3 from = waypoints[i].pos;
                    const RC::Vector3 to   = waypoints[b.target].pos;
                    const RC::Vector3 mid  = {
                        (from.x + to.x) * 0.5f,
                        (from.y + to.y) * 0.5f + 2.0f, // 直進線と分離するための持ち上げ
                        (from.z + to.z) * 0.5f
                    };
                    RC::DrawLine3D(from, mid, branchColor, true);
                    RC::DrawLine3D(mid, to, branchColor, true);
                }
            }
            if (loop && waypoints.size() >= 2) {
                RC::DrawLine3D(waypoints.back().pos, waypoints.front().pos, pathColor, true);
            }
        }
#endif
    }

private:
#if RC_ENABLE_IMGUI
    /// @brief 1ウェイポイントぶんの分岐編集 UI
    void DrawBranchEditor(Waypoint& wp, int wpIndex) {
        const std::string header = "Branches (" + std::to_string(wp.branches.size()) + ")";
        if (!ImGui::TreeNode(header.c_str())) return;

        if (ImGui::Button("Add Branch")) {
            RailBranch b;
            b.target = (wpIndex + 1 < static_cast<int>(waypoints.size())) ? wpIndex + 1 : 0;
            wp.branches.push_back(b);
        }

        static const char* kConditionLabels[] = {
            "always", "tag_equals", "tag_at_least", "key_held", "chance"
        };

        for (size_t bi = 0; bi < wp.branches.size(); ++bi) {
            ImGui::PushID(static_cast<int>(bi) + 1000);
            auto& b = wp.branches[bi];

            int cond = static_cast<int>(b.condition);
            if (ImGui::Combo("Condition", &cond, kConditionLabels,
                             IM_ARRAYSIZE(kConditionLabels))) {
                b.condition = static_cast<BranchCondition>(cond);
            }
            ImGui::DragInt("Target WP", &b.target, 1.0f, 0,
                           static_cast<int>(waypoints.size()) - 1);

            switch (b.condition) {
            case BranchCondition::TagEquals:
            case BranchCondition::TagAtLeast: {
                char nameBuf[64] = {};
                char keyBuf[64] = {};
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", b.entityName.c_str());
                std::snprintf(keyBuf, sizeof(keyBuf), "%s", b.tagKey.c_str());
                if (ImGui::InputText("Entity", nameBuf, sizeof(nameBuf))) b.entityName = nameBuf;
                if (ImGui::InputText("Tag Key", keyBuf, sizeof(keyBuf))) b.tagKey = keyBuf;
                ImGui::DragInt("Value", &b.value, 1.0f);
                break;
            }
            case BranchCondition::KeyHeld:
                ImGui::DragInt("DIK Code", &b.keyCode, 1.0f, 0, 255);
                break;
            case BranchCondition::Chance:
                ImGui::SliderFloat("Chance", &b.chance, 0.0f, 1.0f);
                break;
            case BranchCondition::Always:
            default:
                ImGui::TextUnformatted("(unconditional)");
                break;
            }

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
};

REGISTER_SCRIPT(RailMovementScript)
