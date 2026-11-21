#include "Fade.h"
#include "RenderCommon.h"
#include <algorithm>

using namespace RC;

void Fade::Init(SceneContext &ctx, float width, float height) {
  width_ = width;
  height_ = height;

  // 既に作っていたら一旦破棄する（Init の再呼び出しに備える）
  Term();

  // RenderCommon 管理の Sprite を使う（白 1x1 を黒に乗算してフェード）
  fadeSpriteHandle_ = RC::LoadSprite("Resources/white1x1.png", ctx, true);
  if (fadeSpriteHandle_ < 0) {
    status_ = Status::None;
    return;
  }

  // 1x1 のスプライトを画面サイズまで拡大し、透明な黒で待機させる
  Transform t{};
  t.scale = {width_, height_, 1.0f};
  t.rotation = {0.0f, 0.0f, 0.0f};
  t.translation = {0.0f, 0.0f, 0.0f};

  RC::SetSpriteTransform(fadeSpriteHandle_, t);
  RC::SetSpriteColor(fadeSpriteHandle_, Vector4(0, 0, 0, 0));
}

void Fade::Term() {
  if (fadeSpriteHandle_ >= 0) {
    RC::UnloadSprite(fadeSpriteHandle_);
    fadeSpriteHandle_ = -1;
  }
}

void Fade::Start(Status status, float duration, float alpha) {
  status_ = status;
  duration_ = duration;
  counter_ = 0.0f;
  overlayAlpha_ = alpha;

  if (fadeSpriteHandle_ < 0)
    return;

  // 開始時点の不透明度をフェードの種類に合わせて設定する
  switch (status_) {
  case Status::FadeIn:
    RC::SetSpriteColor(fadeSpriteHandle_, Vector4(0, 0, 0, 1));
    break;
  case Status::FadeOut:
    RC::SetSpriteColor(fadeSpriteHandle_, Vector4(0, 0, 0, 0));
    break;
  case Status::kOverlay:
    RC::SetSpriteColor(fadeSpriteHandle_, Vector4(0, 0, 0, overlayAlpha_));
    break;
  default:
    break;
  }
}

void Fade::Stop() { status_ = Status::None; }

void Fade::Update() {
  if (fadeSpriteHandle_ < 0)
    return;

  // duration_ == 0 対策（ゼロ割り防止）
  const float dur = (duration_ > 0.0f) ? duration_ : 0.0001f;

  switch (status_) {
  case Status::None:
    break;

  case Status::FadeIn:
    // deltaTime を使わず 60FPS 固定で経過時間を進める
    counter_ += 1.0f / 60.0f;
    if (counter_ >= dur)
      counter_ = dur;

    // 1 -> 0 に落ちていく（黒が薄くなる）
    RC::SetSpriteColor(
        fadeSpriteHandle_,
        Vector4(0, 0, 0, std::clamp(1.0f - counter_ / dur, 0.0f, 1.0f)));
    break;

  case Status::FadeOut:
    counter_ += 1.0f / 60.0f;
    if (counter_ >= dur)
      counter_ = dur;

    // 0 -> 1 に上がる（黒が濃くなる）
    RC::SetSpriteColor(
        fadeSpriteHandle_,
        Vector4(0, 0, 0, std::clamp(counter_ / dur, 0.0f, 1.0f)));
    break;

  case Status::kOverlay:
    RC::SetSpriteColor(fadeSpriteHandle_, Vector4(0, 0, 0, overlayAlpha_));
    break;
  }
}

void Fade::Draw(ID3D12GraphicsCommandList *cl) {
  (void)cl; // 互換のため残しているだけ

  if (fadeSpriteHandle_ < 0)
    return;

  // ※ PreDraw2D が呼ばれて gCL がセットされている前提
  RC::DrawSprite(fadeSpriteHandle_);
}

bool Fade::IsFinished() const {
  switch (status_) {
  case Status::FadeIn:
  case Status::FadeOut:
    return counter_ >= duration_;
  default:
    return true;
  }
}
