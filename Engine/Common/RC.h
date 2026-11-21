#pragma once

/// @file RC.h
/// @brief エンジンの主要な機能を一括でインクルードするためのマスターヘッダー

#include "EngineConfig.h"

// --- 描画・レンダリング ---
#include "RenderCommon.h"

// --- オーディオ ---
#include "Audio/Sound.h"

// --- カメラ ---
#include "Camera/CameraController.h"
#include "CameraMath.h"

// --- 数学 ---
#include "Math/MathTypes.h"
#include "Math/MathUtils.h"
#include "Math/Math.h"

// --- ライト ---
#include "Light/Directional/DirectionalLightSource.h"
#include "Light/Spot/SpotLightSource.h"
#include "Light/Point/PointLightSource.h"
#include "Light/Area/AreaLightSource.h"

// --- パーティクル ---
#include "GPUParticle.h"
