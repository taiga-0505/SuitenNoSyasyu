#pragma once

/// @brief 見せ用の自動デモ（一時的なもの）
/// @details kEnabled が true のあいだ、人の操作なしで流れをループする。kMode で 2 種類から選ぶ。
///
///   Mode::Clear（クリアを見せる）:
///     Select（kClearStage の面へカーソルを移して kSelectHold 秒後に自動出航）
///       → ステージ: 開始演出 → kClearPlayTime 秒ふつうに進む（近くの敵へ自動で照準を合わせて撃つ）
///         → 画面を暗転し、そのあいだにレールの終点の手前（kFinalRunDistance m）へ移る（ウェーブは打ち切り）
///         → 明けると前方にサメが現れる（ゲームのウェーブと同じ SharkEnemyScript。潜って近づき、
///            浮上しながら詰めてくる）→ 水面に出てきたところへ照準を合わせて撃ち倒す
///         → レールが終点に着いてクリア演出（ClearAscendScript の海図）→ "cleared"（Result へ ascend）
///       → Result（kResultHold 秒後に「セレクトへ」を選び、kResultConfirmDelay 秒後に決定）
///       → Select → …（以下くり返し）
///     デモ中は自機が被弾しない（demo_invincible）。クリアは通常どおり記録・保存される。
///     ステージ側の制御は ClearRunDemo.h（DeathSinkScript から呼ぶ）。
///
///   Mode::GameOver（力尽きるのを見せる）:
///     Title（kTitleAutoStart のとき kTitleHold 秒後に自動で「スタート」）
///       → Select（kSelectHold 秒後に選択中の面で自動出航）
///       → ステージ: 開始演出 → レールが最初のウェーブ地点に着いて WAVE 1 が始まる
///         → kFrenzyDelay 秒後、サメの群れ（SharkFrenzyDemo）が一斉に襲いかかり、一気に食われて力尽きる
///         → 通常の力尽きた演出（DeathSinkScript：沈む）→ 遷移表のきっかけ "gameover"（Result へ dive）
///       → Result → Select → …（Clear と同じ）
///
///   Mode::Effects（パーティクル・エフェクトを見せる）:
///     Select（kEffectsStage の面へカーソルを移して kSelectHold 秒後に自動出航）
///       → ステージ: 開始演出は飛ばし、レールを出発点で止めたまま、見せ場を順に流す（EffectsShowcaseDemo.h）
///         （どれも連打はせず 1 発ずつ。サメに噛まれるのも 1 回だけ）
///           1. 水面の墨と武器の切り替え（通常 → 拡散 → 重い弾を 1 発ずつ水面へ撃つ）
///           2. 宝箱を撃って開ける
///           3. 鳥を撃ち落とす（羽毛が散って落ちる）
///           4. タコの墨玉を撃ち落とす → 次の墨玉はあえて受ける（画面に墨）→ タコを倒す
///           5. サメの突進を受ける（被弾の演出）→ 浮いてきたところを撃ち倒す
///           6. 重い弾 1 発で船を撃沈する（ヒットストップ＋スロー、浸水して沈む）
///         → 深海色へ暗転し、遷移表のきっかけ kEffectsExitTrigger（Select へ dive）で戻る
///       → Select → …（以下くり返し）
///
///   人が操作した場合はそちらが優先される（自動決定までの待ち時間中に通常どおり操作できる）。
///   見せ終わったら kEnabled を false に戻すだけで元の挙動になる。
namespace TransitionDemo {
inline constexpr bool kEnabled = false; ///< デモを有効にするか（見せ終わったら false に戻す）

/// @brief デモの種類
/// @details これは「どちらのデモを流すか」を選ぶコンパイル時の設定値としてだけ使う。
///          種類ごとの振る舞い（ステージ側の進行）は enum で分岐させず、StageDemo.h の
///          StageDemo 派生クラス（ClearStageDemo / GameOverStageDemo）に分けてあり、
///          この値を見るのは Factory Method の CreateStageDemo() だけ。
///          Title / Select / Result 側は定数（待ち時間など）を読むだけなので、ここに残す。
enum class Mode {
  Clear,    ///< 終盤のサメを倒してクリア → Result → Select をくり返す
  GameOver, ///< サメの群れに食われて力尽きる → Result → Select をくり返す
  Effects,  ///< パーティクル・エフェクトの見せ場を順に流す → 暗転して Select をくり返す
};
inline constexpr Mode kMode = Mode::Effects; ///< どのデモを流すか

// ---- ステージ（Mode::Clear）：暗転で終盤へ飛び、サメを倒してクリア ----
inline constexpr int kClearStage = 0;               ///< 毎回遊ぶ面（0 始まり。0 = 1 面）
inline constexpr float kClearPlayTime = 2.5f;       ///< 開始演出が終わってから暗転を始めるまで（秒）
inline constexpr float kSkipFadeOut = 0.5f;         ///< 暗転にかける時間（秒）
inline constexpr float kSkipBlackHold = 0.35f;      ///< 暗転しきったまま待つ時間（秒。この間にレールを移す）
inline constexpr float kSkipFadeIn = 0.6f;          ///< 明けるまでの時間（秒）
inline constexpr float kFinalRunDistance = 10.0f;   ///< 終点の何 m 手前へ移るか
inline constexpr float kSharkStartDistance = 35.0f; ///< サメが現れる距離（カメラの前方。m。1 面のウェーブ 3 の aheadDistance と同じ）
inline constexpr float kSharkStartSide = 4.0f;      ///< サメが現れる横のずれ（m。正で右）
inline constexpr int kSharkHp = 3;                  ///< サメの体力（1 面のウェーブのサメと同じ maxHp）
inline constexpr float kSharkReadyDepth = 2.0f;     ///< サメがこの深さ（水面から。m）より浮いてきたら撃ち始める
inline constexpr float kFightTimeout = 6.0f;        ///< サメが出てから倒せなかったら強制的に倒すまで（秒。保険）
inline constexpr float kAfterKillDelay = 0.8f;      ///< 倒してからレールを再び進めるまで（秒）
inline constexpr float kAimTolerance = 0.045f;      ///< 照準のずれがこの角度（rad）以内なら撃つ
inline constexpr float kAimRange = 45.0f;           ///< 序盤に自動で狙う敵の最大距離（m）

// ---- ステージ（Mode::GameOver）：サメに一気に食われる ----
inline constexpr float kFrenzyDelay = 0.9f;       ///< 最初のウェーブが始まってから群れが動き出すまで（秒。WAVE 表示がせり出しきる頃）
inline constexpr float kWaveFallbackTime = 25.0f; ///< 開始演出後、ウェーブが始まらないまま経ったらこの秒数で群れを出す（保険）
inline constexpr int kSharkCount = 6;             ///< 襲いかかるサメの数（最大 8）
inline constexpr float kLungeTime = 1.4f;         ///< 1 匹が出現位置から噛みつくまで（秒）
inline constexpr float kBiteDistance = 3.4f;      ///< 噛みついたときのカメラからサメの原点までの距離（m）
inline constexpr float kSharkScale = 6.0f;        ///< 1 面のウェーブの enemyScale と同じ（クリアのデモのサメも同じ大きさ）
inline constexpr int kLethalDamage = 999;         ///< 噛みついたときに与えるダメージ（HP に関係なく力尽きる）

// ---- ステージ（Mode::Effects）：エフェクトの見せ場を順に流す ----
// 時間はすべて秒、距離は m。位置は出発点のカメラから見た前方距離（Ahead）・右へのずれ（Side）。
inline constexpr int kEffectsStage = 0;                         ///< 毎回遊ぶ面（0 始まり。0 = 1 面）
inline constexpr const char *kEffectsExitTrigger = "demo_end"; ///< 見せ終わったときの遷移表のきっかけ名
inline constexpr float kFxSettle = 0.8f;                        ///< 操作できるようになってから 1 つ目の見せ場まで
inline constexpr float kFxAimTolerance = 0.06f;                 ///< 照準のずれがこの角度（rad）以内なら撃つ
inline constexpr float kFxBulletSpeed = 40.0f;                  ///< 自機の弾の速さ（偏差撃ちの見積もり用。RailShooterController と同じ）
// 撃ち方：連打はせず 1 発ずつ撃ち、当たったか見届けてから次を撃つ（外れたときだけ撃ち直す）
inline constexpr float kFxShotMargin = 0.45f;   ///< 弾が届く見込み時間に足す、撃ち直すまでの待ち
inline constexpr int kFxMaxShots = 3;           ///< この発数撃っても倒せなければ強制的に倒す（保険）
// 1. 水面の墨と武器の切り替え（通常 → 拡散 → 重い弾 を 1 発ずつ、左・中央・右の水面へ）
inline constexpr float kFxInkAimLead = 0.5f;    ///< 撃つ前に照準を水面の狙う所へ寄せる時間
inline constexpr float kFxInkDistance = 11.0f;  ///< 水面を狙う距離
inline constexpr float kFxInkSpotSide = 3.5f;   ///< 左右の狙う所の横のずれ
inline constexpr float kFxInkWatch = 1.4f;      ///< 1 発撃ってから、墨がにじむのを見せる時間
inline constexpr float kFxInkAfter = 0.6f;      ///< 3 発撃ち終えてから次へ進むまで
// 2. 宝箱
inline constexpr float kFxChestSearch = 32.0f;  ///< シーンの宝箱を探す距離（見つからなければ前方に出す）
inline constexpr float kFxChestAhead = 13.0f;   ///< 宝箱を出すときの前方距離
inline constexpr float kFxChestSide = 5.0f;     ///< 宝箱を出すときの右へのずれ
inline constexpr float kFxChestTimeout = 5.0f;  ///< 開かなければ強制的に開けるまで（保険）
inline constexpr float kFxChestAfter = 1.8f;    ///< 開いてから次へ進むまで
// 3. 鳥
inline constexpr float kFxBirdAhead = 42.0f;    ///< 鳥を出す前方距離
inline constexpr float kFxBirdSide = 4.0f;      ///< 鳥を出す右へのずれ
inline constexpr float kFxBirdHeight = 8.0f;    ///< 鳥を出す高さ（水面から）
inline constexpr float kFxBirdFireRange = 32.0f; ///< この距離まで来たら撃ち始める
inline constexpr float kFxBirdTimeout = 10.0f;  ///< 落とせなければ強制的に倒すまで（保険）
inline constexpr float kFxBirdAfter = 2.2f;     ///< 落としてから次へ進むまで
// 4. タコ
inline constexpr float kFxOctoAhead = 24.0f;    ///< タコを出す前方距離
inline constexpr float kFxOctoSide = 2.0f;      ///< タコを出す右へのずれ
inline constexpr float kFxOctoDepth = 1.5f;     ///< タコを出す深さ（水面から）
inline constexpr float kFxBlobRescue = 6.0f;    ///< 撃ち落とす墨玉がこの距離まで来たら、飛んでいる弾を墨玉へ寄せる（保険）
inline constexpr float kFxBlobHitDistance = 2.6f; ///< 墨玉が消えたときの距離がこれ以下なら「自機に当たった」とみなす
inline constexpr float kFxOctoShootDownTimeout = 9.0f; ///< 墨玉が来ないまま経ったら次へ進むまで（保険）
inline constexpr float kFxOctoShootDownAfter = 1.2f;  ///< 撃ち落としてから次の墨玉を待ち始めるまで
inline constexpr float kFxOctoInkTimeout = 9.0f;      ///< 墨玉が当たらないまま経ったら墨を直接貼るまで（保険）
inline constexpr float kFxOctoInkLinger = 1.5f;       ///< 墨を受けてからタコを狙い始めるまで（墨を見せる）
inline constexpr float kFxOctoKillTimeout = 1.8f;     ///< タコを撃ち始めてから強制的に倒すまで（水中なので短め）
inline constexpr float kFxOctoAfter = 1.6f;           ///< タコを倒してから次へ進むまで
// 5. サメ
inline constexpr float kFxSharkAhead = 36.0f;   ///< サメを出す前方距離
inline constexpr float kFxSharkSide = 3.0f;     ///< サメを出す右へのずれ
inline constexpr float kFxSharkHitTimeout = 10.0f; ///< 突進が当たらないまま経ったら直接ダメージを入れるまで（保険）
inline constexpr float kFxSharkAfterHit = 0.6f;    ///< 被弾してから狙い直すまで
inline constexpr float kFxSharkKillTimeout = 9.0f; ///< 撃ち始めてから強制的に倒すまで（保険）
inline constexpr float kFxSharkBiteGuard = 12.0f;  ///< 撃ち倒す前にこの距離まで迫ったら強制的に倒す（2 回噛ませない）
inline constexpr float kFxSharkAfter = 2.0f;       ///< 倒してから次へ進むまで
// 6. 船
inline constexpr float kFxShipAhead = 46.0f;    ///< 船を出す前方距離
inline constexpr float kFxShipSide = 7.0f;      ///< 船を出す右へのずれ
inline constexpr int kFxShipHp = 1;             ///< 船の体力（重い弾 1 発で沈む。ゲームのウェーブでは 40）
inline constexpr float kFxShipLook = 1.5f;      ///< 船を出してから撃ち始めるまで（姿を見せる）
inline constexpr float kFxShipTimeout = 8.0f;   ///< 撃ち始めてから強制的に撃沈するまで（保険）
inline constexpr float kFxShipSink = 4.5f;      ///< 撃沈してから暗転を始めるまで（沈むのを見せる）
// 締め
inline constexpr float kFxOutroFade = 1.0f;     ///< 深海色へ暗転する時間
inline constexpr float kFxOutroHold = 0.2f;     ///< 暗転しきってから遷移を要求するまで

// ---- Result / Select / Title の自動操作 ----
inline constexpr float kResultHold = 3.0f;         ///< Result の成績が出そろってから「セレクトへ」を選ぶまで（秒）
inline constexpr float kResultConfirmDelay = 0.8f; ///< 「セレクトへ」を選んでから決定するまで（秒）
inline constexpr float kSelectHold = 2.5f;         ///< Select の UI が出てから出航するまで（秒）
inline constexpr float kSelectMoveAt = 1.2f;       ///< Mode::Clear で kClearStage へカーソルを動かすまで（秒）
inline constexpr bool kTitleAutoStart = true;      ///< Title でも自動で「スタート」を押すか
inline constexpr float kTitleHold = 3.0f;          ///< Title のメニューが操作できるようになってから「スタート」まで（秒）

/// @brief クリアのデモを流しているか
/// @return kEnabled かつ kMode が Clear なら true
inline constexpr bool IsClearDemo() { return kEnabled && kMode == Mode::Clear; }
/// @brief 力尽きるデモを流しているか
/// @return kEnabled かつ kMode が GameOver なら true
inline constexpr bool IsGameOverDemo() { return kEnabled && kMode == Mode::GameOver; }
/// @brief エフェクトのデモを流しているか
/// @return kEnabled かつ kMode が Effects なら true
inline constexpr bool IsEffectsDemo() { return kEnabled && kMode == Mode::Effects; }

/// @brief Select で決まった面へカーソルを動かすデモか（Clear / Effects）
/// @return 動かすなら true
inline constexpr bool UsesFixedStage() { return IsClearDemo() || IsEffectsDemo(); }
/// @brief Select でカーソルを動かす先の面
/// @return 面の番号（0 始まり）
inline constexpr int FixedStage() { return IsEffectsDemo() ? kEffectsStage : kClearStage; }
/// @brief ステージの開始演出（深海からの浮上・サメ追跡のカットシーン）を飛ばすか
/// @return 飛ばすなら true（Effects のときだけ。見せ場をすぐ始める）
inline constexpr bool SkipsStageIntro() { return IsEffectsDemo(); }
} // namespace TransitionDemo
