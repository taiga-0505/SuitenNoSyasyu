#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct IUnknown;

/// @namespace AudioHrtf
/// @brief Steam Audio による HRTF（頭部伝達関数）バイノーラルレンダリング
/// @details XAudio2 のソースボイスに挿す XAPO（モノラル入力 → ステレオ出力）を提供する。
///          X3DAudio のパンニング（左右の音量差だけ）と違い、耳に届く時間差・周波数特性の差まで再現するので、
///          ヘッドホンで前後・上下が聞き分けられる。
///
///          phonon.dll（Steam Audio 4.x）は実行時に LoadLibrary で読み込む。DLL が無い環境でも
///          起動できるよう、リンク時には依存しない（その場合 IsAvailable() が false になり、
///          AudioEngine は従来の X3DAudio で鳴らす）。
///
///          Steam Audio は一定の長さ（frameSize）のブロック単位でしか処理できないため、
///          XAPO 内で 1 ブロック分（約 10ms）をため込んでから処理する。そのぶん音が約 10ms 遅れる。
/// @note Init() / Term() / CreateEffect() はメインスレッドから呼ぶこと。
///       エフェクトの Process はオーディオスレッドで動き、SetParams はどのスレッドからでも呼べる。
namespace AudioHrtf {

/// @brief phonon.dll を読み込み、Steam Audio を初期化する
/// @return 使えるようになったら true（DLL が無い・バージョン不一致などなら false。理由は StatusText()）
bool Init();

/// @brief Steam Audio を終了する（すべてのエフェクトを破棄した後に呼ぶこと）
void Term();

/// @brief HRTF が使えるか
/// @return Init() に成功していれば true
bool IsAvailable();

/// @brief 状態の説明（Inspector 表示用）
/// @return 「有効 (Steam Audio 4.8.1)」「phonon.dll が見つかりません」など
std::string StatusText();

/// @brief 指定のサンプリングレート用の HRTF を先に作っておく（初回再生時のヒッチを避ける）
/// @param sampleRate サンプリングレート（Hz）
void Prewarm(uint32_t sampleRate);

/// @brief XAudio2 のソースボイスに挿す HRTF エフェクト（XAPO）。参照カウントで管理する
class Effect;

/// @brief Effect の参照を返すデリータ
struct EffectReleaser {
  void operator()(Effect *e) const noexcept;
};

/// @brief Effect を持つスマートポインタ（参照 1 つ分を持つ）
using EffectPtr = std::unique_ptr<Effect, EffectReleaser>;

/// @brief エフェクトを作る
/// @return 参照 1 つを持ったエフェクト（使えなければ null）
EffectPtr CreateEffect();

/// @brief XAUDIO2_EFFECT_DESCRIPTOR::pEffect に渡すポインタ
/// @param e エフェクト
/// @return IXAPO（IUnknown）としてのポインタ
IUnknown *AsXapo(Effect *e);

/// @brief 音源の方向と立体感の強さを設定する（毎フレーム呼ぶ。スレッドセーフ）
/// @param e            エフェクト
/// @param x            リスナーから見た音源方向（右が +）
/// @param y            同（上が +）
/// @param z            同（正面が +）。x, y, z は正規化済みであること
/// @param spatialBlend 0 = 立体化しない（両耳に同じ音）、1 = 完全に HRTF で立体化
void SetParams(Effect *e, float x, float y, float z, float spatialBlend);

} // namespace AudioHrtf
