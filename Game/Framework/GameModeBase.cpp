#include "GameModeBase.h"
#include "../Scene/Scene.h"

GameModeBase::GameModeBase() {
    gameState_ = std::make_unique<GameStateBase>();
}

void GameModeBase::BeginPlay(SceneContext& ctx) {
    if (gameState_) {
        gameState_->BeginPlay();
    }
}

void GameModeBase::Tick(SceneContext& ctx) {
    if (gameState_) {
        gameState_->Tick(ctx.deltaTime);
    }
}

GameModeBase::Factory& GameModeBase::FactoryStorage() {
    static Factory factory;
    return factory;
}

void GameModeBase::SetFactory(Factory factory) {
    FactoryStorage() = std::move(factory);
}

std::unique_ptr<GameModeBase> GameModeBase::Create(const std::string& sceneName) {
    // ファクトリ未登録、またはファクトリが null を返した場合は既定の GameModeBase で埋める
    if (auto& factory = FactoryStorage()) {
        if (auto mode = factory(sceneName)) {
            return mode;
        }
    }
    return std::make_unique<GameModeBase>();
}
