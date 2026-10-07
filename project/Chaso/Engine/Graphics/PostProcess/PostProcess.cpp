#include "PostProcess.h"
#include "Common/ResourcePath.h"
#include "Dx12/Dx12Core.h"
#include "Dx12/GraphicsPipeline/GraphicsPipeline.h"
#include "Dx12/PipelineManager.h"
#include "Graphics/Texture/RenderTexture/RenderTexture.h"
#include "Render/RenderContext.h"
#include "Common/Log/Log.h"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <utility> // std::swap (MoveEffect)

namespace {
const char* ToString(PostEffectType type) {
  switch (type) {
  case PostEffectType::Grayscale: return "Grayscale";
  case PostEffectType::Sepia:     return "Sepia";
  case PostEffectType::Vignette:  return "Vignette";
  case PostEffectType::BoxFilter: return "BoxFilter";
  case PostEffectType::GaussianFilter: return "GaussianFilter";
  case PostEffectType::DepthBasedOutline: return "DepthBasedOutline";
  case PostEffectType::RadialBlur: return "RadialBlur";
  case PostEffectType::Dissolve:   return "Dissolve";
  case PostEffectType::RandomNoise:return "RandomNoise";
  case PostEffectType::MaskOutline:return "MaskOutline";
  case PostEffectType::Ssao:      return "Ssao";
  case PostEffectType::Bloom:     return "Bloom";
  case PostEffectType::ColorGrade:return "ColorGrade";
  case PostEffectType::Fxaa:      return "Fxaa";
  case PostEffectType::Underwater: return "Underwater";
  case PostEffectType::Caustics:  return "Caustics";
  case PostEffectType::LightShaft:return "LightShaft";
  case PostEffectType::ScreenDroplets:return "ScreenDroplets";
  case PostEffectType::BloodOverlay:return "BloodOverlay";
  case PostEffectType::InkOverlay:  return "InkOverlay";
  case PostEffectType::None:      return "None";
  default:                        return "Unknown";
  }
}

std::string ActiveEffectsToString(const std::vector<PostEffectType>& effects) {
  if (effects.empty()) return "None";
  std::string result;
  for (size_t i = 0; i < effects.size(); ++i) {
    if (i > 0) result += " -> ";
    result += ToString(effects[i]);
  }
  return result;
}
} // namespace

#if RC_ENABLE_IMGUI
#include "imgui/imgui.h"
#endif

void PostProcess::Initialize(Dx12Core *dxCore,
                             PipelineManager *pipelineManager,
                             uint32_t width, uint32_t height) {
  dxCore_ = dxCore;
  pipelineManager_ = pipelineManager;
  width_ = width;
  height_ = height;

  // 各パイプラインを取得
  pipelineCopy_ = pipelineManager_->Get("copyimage.none");
  assert(pipelineCopy_ && "Failed to get copyimage pipeline");

  pipelineGrayscale_ = pipelineManager_->Get("grayscale.none");
  assert(pipelineGrayscale_ && "Failed to get grayscale pipeline");

  pipelineSepia_ = pipelineManager_->Get("sepia.none");
  assert(pipelineSepia_ && "Failed to get sepia pipeline");

  pipelineVignette_ = pipelineManager_->Get("vignette.none");
  assert(pipelineVignette_ && "Failed to get vignette pipeline");

  pipelineBoxFilter_ = pipelineManager_->Get("boxfilter.none");
  assert(pipelineBoxFilter_ && "Failed to get boxfilter pipeline");

  pipelineGaussianFilter_ = pipelineManager_->Get("gaussianfilter.none");
  assert(pipelineGaussianFilter_ && "Failed to get gaussianfilter pipeline");

  pipelineDepthBasedOutline_ = pipelineManager_->Get("depthbasedoutline.none");
  assert(pipelineDepthBasedOutline_ && "Failed to get depthbasedoutline pipeline");

  pipelineRadialBlur_ = pipelineManager_->Get("radialblur.none");
  assert(pipelineRadialBlur_ && "Failed to get radialblur pipeline");

  pipelineDissolve_ = pipelineManager_->Get("dissolve.none");
  assert(pipelineDissolve_ && "Failed to get dissolve pipeline");

  pipelineRandom_ = pipelineManager_->Get("random.none");
  assert(pipelineRandom_ && "Failed to get random pipeline");

  pipelineMaskOutline_ = pipelineManager_->Get("maskoutline.none");
  assert(pipelineMaskOutline_ && "Failed to get maskoutline pipeline");

  pipelineSsao_ = pipelineManager_->Get("ssao.none");
  assert(pipelineSsao_ && "Failed to get ssao pipeline");

  pipelineBloom_ = pipelineManager_->Get("bloom.none");
  assert(pipelineBloom_ && "Failed to get bloom pipeline");

  pipelineColorGrade_ = pipelineManager_->Get("colorgrade.none");
  assert(pipelineColorGrade_ && "Failed to get colorgrade pipeline");

  pipelineFxaa_ = pipelineManager_->Get("fxaa.none");
  assert(pipelineFxaa_ && "Failed to get fxaa pipeline");

  pipelineUnderwater_ = pipelineManager_->Get("underwater.none");
  assert(pipelineUnderwater_ && "Failed to get underwater pipeline");

  pipelineCaustics_ = pipelineManager_->Get("caustics.none");
  assert(pipelineCaustics_ && "Failed to get caustics pipeline");

  pipelineLightShaft_ = pipelineManager_->Get("lightshaft.none");
  assert(pipelineLightShaft_ && "Failed to get lightshaft pipeline");

  pipelineScreenDroplets_ = pipelineManager_->Get("screendroplets.none");
  assert(pipelineScreenDroplets_ && "Failed to get screendroplets pipeline");

  pipelineBloodOverlay_ = pipelineManager_->Get("bloodoverlay.none");
  assert(pipelineBloodOverlay_ && "Failed to get bloodoverlay pipeline");

  pipelineInkOverlay_ = pipelineManager_->Get("inkoverlay.none");
  assert(pipelineInkOverlay_ && "Failed to get inkoverlay pipeline");

  // CBuffer 初期化
  D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC cbDesc{};
  cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  cbDesc.Width = (sizeof(MaterialData) + 255) & ~255;
  cbDesc.Height = 1;
  cbDesc.DepthOrArraySize = 1;
  cbDesc.MipLevels = 1;
  cbDesc.Format = DXGI_FORMAT_UNKNOWN;
  cbDesc.SampleDesc.Count = 1;
  cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  cbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  HRESULT hr = dxCore_->GetDevice()->CreateCommittedResource(
      &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&cbufferMaterial_));
  assert(SUCCEEDED(hr));
  mappedMaterial_ = dynMaterial_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

  if (mappedMaterial_) {
    memcpy(mappedMaterial_->outlineColor, outlineColor_, sizeof(float) * 4);
    mappedMaterial_->outlineWeight = outlineWeight_;
    mappedMaterial_->outlineThickness = outlineThickness_;
    mappedMaterial_->outlineMode = outlineMode_;
    mappedMaterial_->excludeCount = 0; // 除外矩形は毎フレーム UI 側から申告される
  }

  // Dissolve CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescDissolve{};
    cbDescDissolve.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescDissolve.Width = (sizeof(DissolveData) + 255) & ~255;
    cbDescDissolve.Height = 1;
    cbDescDissolve.DepthOrArraySize = 1;
    cbDescDissolve.MipLevels = 1;
    cbDescDissolve.Format = DXGI_FORMAT_UNKNOWN;
    cbDescDissolve.SampleDesc.Count = 1;
    cbDescDissolve.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescDissolve.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescDissolve,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferDissolve_));
    assert(SUCCEEDED(hr));
    mappedDissolve_ = dynDissolve_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedDissolve_) {
      mappedDissolve_->edgeColor[0] = dissolveEdgeColor_[0];
      mappedDissolve_->edgeColor[1] = dissolveEdgeColor_[1];
      mappedDissolve_->edgeColor[2] = dissolveEdgeColor_[2];
      mappedDissolve_->edgeColor[3] = 1.0f;
      mappedDissolve_->baseColor[0] = dissolveBaseColor_[0];
      mappedDissolve_->baseColor[1] = dissolveBaseColor_[1];
      mappedDissolve_->baseColor[2] = dissolveBaseColor_[2];
      mappedDissolve_->baseColor[3] = dissolveBaseColor_[3];
      mappedDissolve_->threshold = dissolveThreshold_;
      mappedDissolve_->edgeRange = dissolveEdgeRange_;
    }
  }

  // ノイズテクスチャはRC::Init後に遅延初期化される (InitDissolveNoiseTextures)

  // RandomNoise CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescRandom{};
    cbDescRandom.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescRandom.Width = (sizeof(RandomNoiseData) + 255) & ~255;
    cbDescRandom.Height = 1;
    cbDescRandom.DepthOrArraySize = 1;
    cbDescRandom.MipLevels = 1;
    cbDescRandom.Format = DXGI_FORMAT_UNKNOWN;
    cbDescRandom.SampleDesc.Count = 1;
    cbDescRandom.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescRandom.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescRandom,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferRandom_));
    assert(SUCCEEDED(hr));
    mappedRandom_ = dynRandom_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedRandom_) {
      mappedRandom_->color[0] = randomColor_[0];
      mappedRandom_->color[1] = randomColor_[1];
      mappedRandom_->color[2] = randomColor_[2];
      mappedRandom_->color[3] = 1.0f;
      mappedRandom_->time = randomTime_;
      mappedRandom_->intensity = randomIntensity_;
    }
  }

  // MaskOutline CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescMask{};
    cbDescMask.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescMask.Width = (sizeof(MaskOutlineData) + 255) & ~255;
    cbDescMask.Height = 1;
    cbDescMask.DepthOrArraySize = 1;
    cbDescMask.MipLevels = 1;
    cbDescMask.Format = DXGI_FORMAT_UNKNOWN;
    cbDescMask.SampleDesc.Count = 1;
    cbDescMask.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescMask.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescMask,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferMaskOutline_));
    assert(SUCCEEDED(hr));
    mappedMaskOutline_ = dynMaskOutline_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedMaskOutline_) {
      mappedMaskOutline_->color[0] = maskOutlineColor_[0];
      mappedMaskOutline_->color[1] = maskOutlineColor_[1];
      mappedMaskOutline_->color[2] = maskOutlineColor_[2];
      mappedMaskOutline_->color[3] = maskOutlineColor_[3];
      mappedMaskOutline_->thickness = maskOutlineThickness_;
      mappedMaskOutline_->strength = maskOutlineStrength_;
      mappedMaskOutline_->excludeCount = 0; // 除外矩形は毎フレーム UI 側から申告される
    }
  }

  // SSAO CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescSsao{};
    cbDescSsao.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescSsao.Width = (sizeof(SsaoData) + 255) & ~255;
    cbDescSsao.Height = 1;
    cbDescSsao.DepthOrArraySize = 1;
    cbDescSsao.MipLevels = 1;
    cbDescSsao.Format = DXGI_FORMAT_UNKNOWN;
    cbDescSsao.SampleDesc.Count = 1;
    cbDescSsao.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescSsao.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescSsao,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferSsao_));
    assert(SUCCEEDED(hr));
    mappedSsao_ = dynSsao_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedSsao_) {
      // 行列は SetProjectionInverse が来るまで単位行列にしておく
      // （ゼロ行列だと w = 0 で除算して NaN になる）
      const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f};
      memcpy(mappedSsao_->projectionInverse, identity, sizeof(identity));

      mappedSsao_->radius = ssaoRadius_;
      mappedSsao_->intensity = ssaoIntensity_;
      mappedSsao_->bias = ssaoBias_;
      mappedSsao_->power = ssaoPower_;
    }
  }

  // ColorGrade CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescGrade{};
    cbDescGrade.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescGrade.Width = (sizeof(ColorGradeData) + 255) & ~255;
    cbDescGrade.Height = 1;
    cbDescGrade.DepthOrArraySize = 1;
    cbDescGrade.MipLevels = 1;
    cbDescGrade.Format = DXGI_FORMAT_UNKNOWN;
    cbDescGrade.SampleDesc.Count = 1;
    cbDescGrade.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescGrade.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescGrade,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferColorGrade_));
    assert(SUCCEEDED(hr));
    mappedColorGrade_ = dynColorGrade_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedColorGrade_) {
      memcpy(mappedColorGrade_->colorFilter, gradeColorFilter_, sizeof(float) * 4);
      mappedColorGrade_->exposure = gradeExposure_;
      mappedColorGrade_->contrast = gradeContrast_;
      mappedColorGrade_->saturation = gradeSaturation_;
      mappedColorGrade_->temperature = gradeTemperature_;
      mappedColorGrade_->tint = gradeTint_;
      mappedColorGrade_->lerpFactor = gradeLerpFactor_;
    }
  }

  // Underwater CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescUnderwater{};
    cbDescUnderwater.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescUnderwater.Width = (sizeof(UnderwaterData) + 255) & ~255;
    cbDescUnderwater.Height = 1;
    cbDescUnderwater.DepthOrArraySize = 1;
    cbDescUnderwater.MipLevels = 1;
    cbDescUnderwater.Format = DXGI_FORMAT_UNKNOWN;
    cbDescUnderwater.SampleDesc.Count = 1;
    cbDescUnderwater.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescUnderwater.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescUnderwater,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferUnderwater_));
    assert(SUCCEEDED(hr));
    mappedUnderwater_ = dynUnderwater_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedUnderwater_) {
      mappedUnderwater_->tintColor[0] = underwaterTintColor_[0];
      mappedUnderwater_->tintColor[1] = underwaterTintColor_[1];
      mappedUnderwater_->tintColor[2] = underwaterTintColor_[2];
      mappedUnderwater_->tintColor[3] = underwaterTintColor_[3];
      mappedUnderwater_->fogColor[0] = underwaterFogColor_[0];
      mappedUnderwater_->fogColor[1] = underwaterFogColor_[1];
      mappedUnderwater_->fogColor[2] = underwaterFogColor_[2];
      mappedUnderwater_->fogColor[3] = underwaterFogColor_[3];
      mappedUnderwater_->time = 0.0f;
      mappedUnderwater_->distortionForce = underwaterDistortionForce_;
      mappedUnderwater_->fogStart = underwaterFogStart_;
      mappedUnderwater_->fogEnd = underwaterFogEnd_;
      mappedUnderwater_->lerpFactor = underwaterLerpFactor_;
    }
  }

  // Caustics CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescCaustics{};
    cbDescCaustics.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescCaustics.Width = (sizeof(CausticsData) + 255) & ~255;
    cbDescCaustics.Height = 1;
    cbDescCaustics.DepthOrArraySize = 1;
    cbDescCaustics.MipLevels = 1;
    cbDescCaustics.Format = DXGI_FORMAT_UNKNOWN;
    cbDescCaustics.SampleDesc.Count = 1;
    cbDescCaustics.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescCaustics.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescCaustics,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferCaustics_));
    assert(SUCCEEDED(hr));
    mappedCaustics_ = dynCaustics_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedCaustics_) {
      // 行列は SetProjectionInverse / SetViewInverse が来るまで単位行列にしておく
      // （初フレームで壊れた値を読まないようにするため）
      const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f};
      memcpy(mappedCaustics_->projectionInverse, identity, sizeof(identity));
      memcpy(mappedCaustics_->viewInverse, identity, sizeof(identity));

      mappedCaustics_->causticsColor[0] = causticsColor_[0];
      mappedCaustics_->causticsColor[1] = causticsColor_[1];
      mappedCaustics_->causticsColor[2] = causticsColor_[2];
      mappedCaustics_->causticsColor[3] = 1.0f;
      mappedCaustics_->time = 0.0f;
      mappedCaustics_->intensity = causticsIntensity_;
      mappedCaustics_->scale = causticsScale_;
      mappedCaustics_->speed = causticsSpeed_;
      mappedCaustics_->contrast = causticsContrast_;
      mappedCaustics_->chromaticOffset = causticsChromaticOffset_;
      mappedCaustics_->waterHeight = causticsWaterHeight_;
      mappedCaustics_->depthFadeDistance = causticsDepthFadeDistance_;
      mappedCaustics_->upwardBias = causticsUpwardBias_;
      mappedCaustics_->distanceFadeStart = causticsDistanceFadeStart_;
      mappedCaustics_->distanceFadeEnd = causticsDistanceFadeEnd_;
      mappedCaustics_->lerpFactor = causticsLerpFactor_;
    }
  }

  // LightShaft CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescLightShaft{};
    cbDescLightShaft.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescLightShaft.Width = (sizeof(LightShaftData) + 255) & ~255;
    cbDescLightShaft.Height = 1;
    cbDescLightShaft.DepthOrArraySize = 1;
    cbDescLightShaft.MipLevels = 1;
    cbDescLightShaft.Format = DXGI_FORMAT_UNKNOWN;
    cbDescLightShaft.SampleDesc.Count = 1;
    cbDescLightShaft.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescLightShaft.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescLightShaft,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferLightShaft_));
    assert(SUCCEEDED(hr));
    mappedLightShaft_ = dynLightShaft_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedLightShaft_) {
      const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f};
      memcpy(mappedLightShaft_->projectionInverse, identity, sizeof(identity));
      memcpy(mappedLightShaft_->viewInverse, identity, sizeof(identity));

      mappedLightShaft_->shaftColor[0] = lightShaftColor_[0];
      mappedLightShaft_->shaftColor[1] = lightShaftColor_[1];
      mappedLightShaft_->shaftColor[2] = lightShaftColor_[2];
      mappedLightShaft_->shaftColor[3] = 1.0f;
      mappedLightShaft_->time = 0.0f;
      mappedLightShaft_->intensity = lightShaftIntensity_;
      mappedLightShaft_->contrast = lightShaftContrast_;
      mappedLightShaft_->density = lightShaftDensity_;
      mappedLightShaft_->maxDistance = lightShaftMaxDistance_;
      mappedLightShaft_->sampleCount = lightShaftSampleCount_;
      mappedLightShaft_->ditherStrength = lightShaftDitherStrength_;
      mappedLightShaft_->lerpFactor = lightShaftLerpFactor_;
      mappedLightShaft_->_padding = 0.0f;
      mappedLightShaft_->sunDir[0] = lightShaftSunDir_[0];
      mappedLightShaft_->sunDir[1] = lightShaftSunDir_[1];
      mappedLightShaft_->sunDir[2] = lightShaftSunDir_[2];
      mappedLightShaft_->sunDir[3] = 0.0f;

      // scale / speed / waterHeight は Caustics と共有する必要がある
      mappedLightShaft_->scale = causticsScale_;
      mappedLightShaft_->speed = causticsSpeed_;
      mappedLightShaft_->waterHeight = causticsWaterHeight_;
    }
  }

  // ScreenDroplets CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescScreenDroplets{};
    cbDescScreenDroplets.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescScreenDroplets.Width = (sizeof(ScreenDropletsData) + 255) & ~255;
    cbDescScreenDroplets.Height = 1;
    cbDescScreenDroplets.DepthOrArraySize = 1;
    cbDescScreenDroplets.MipLevels = 1;
    cbDescScreenDroplets.Format = DXGI_FORMAT_UNKNOWN;
    cbDescScreenDroplets.SampleDesc.Count = 1;
    cbDescScreenDroplets.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescScreenDroplets.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescScreenDroplets,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferScreenDroplets_));
    assert(SUCCEEDED(hr));
    mappedScreenDroplets_ = dynScreenDroplets_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedScreenDroplets_) {
      mappedScreenDroplets_->time = 0.0f;
      mappedScreenDroplets_->intensity = screenDropletsIntensity_;
      mappedScreenDroplets_->speed = screenDropletsSpeed_;
      mappedScreenDroplets_->distortion = screenDropletsDistortion_;
      mappedScreenDroplets_->scale = screenDropletsScale_;
      screenDropletsAspectRatio_ = (height_ > 0) ? (static_cast<float>(width_) / static_cast<float>(height_)) : 1.777f;
      mappedScreenDroplets_->aspectRatio = screenDropletsAspectRatio_;
      mappedScreenDroplets_->padding[0] = 0.0f;
      mappedScreenDroplets_->padding[1] = 0.0f;
    }
  }

  // BloodOverlay CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescBlood{};
    cbDescBlood.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescBlood.Width = (sizeof(BloodOverlayData) + 255) & ~255;
    cbDescBlood.Height = 1;
    cbDescBlood.DepthOrArraySize = 1;
    cbDescBlood.MipLevels = 1;
    cbDescBlood.Format = DXGI_FORMAT_UNKNOWN;
    cbDescBlood.SampleDesc.Count = 1;
    cbDescBlood.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescBlood.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescBlood,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferBloodOverlay_));
    assert(SUCCEEDED(hr));
    mappedBloodOverlay_ = dynBloodOverlay_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedBloodOverlay_) {
      mappedBloodOverlay_->time = 0.0f;
      mappedBloodOverlay_->hitFlash = bloodHitFlash_;
      mappedBloodOverlay_->lowHealth = bloodLowHealth_;
      mappedBloodOverlay_->pulseSpeed = bloodPulseSpeed_;
      mappedBloodOverlay_->bloodColor[0] = bloodColor_[0];
      mappedBloodOverlay_->bloodColor[1] = bloodColor_[1];
      mappedBloodOverlay_->bloodColor[2] = bloodColor_[2];
      mappedBloodOverlay_->bloodColor[3] = 1.0f;
      mappedBloodOverlay_->coverage = bloodCoverage_;
      mappedBloodOverlay_->splatterScale = bloodSplatterScale_;
      mappedBloodOverlay_->desaturate = bloodDesaturate_;
      mappedBloodOverlay_->aspectRatio =
          (height_ > 0) ? (static_cast<float>(width_) / static_cast<float>(height_)) : 1.777f;
      mappedBloodOverlay_->seed = bloodSeed_;
      mappedBloodOverlay_->padding[0] = 0.0f;
      mappedBloodOverlay_->padding[1] = 0.0f;
      mappedBloodOverlay_->padding[2] = 0.0f;
    }
  }

  // InkOverlay CBuffer 初期化
  {
    D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC cbDescInk{};
    cbDescInk.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDescInk.Width = (sizeof(InkOverlayData) + 255) & ~255;
    cbDescInk.Height = 1;
    cbDescInk.DepthOrArraySize = 1;
    cbDescInk.MipLevels = 1;
    cbDescInk.Format = DXGI_FORMAT_UNKNOWN;
    cbDescInk.SampleDesc.Count = 1;
    cbDescInk.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDescInk.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = dxCore_->GetDevice()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDescInk,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&cbufferInkOverlay_));
    assert(SUCCEEDED(hr));
    mappedInkOverlay_ = dynInkOverlay_.Ptr(); // DynamicCB: CPU 側に書き、バインド時に今フレームの領域へ送る

    if (mappedInkOverlay_) {
      *mappedInkOverlay_ = InkOverlayData{};
      mappedInkOverlay_->aspectRatio =
          (height_ > 0) ? (static_cast<float>(width_) / static_cast<float>(height_)) : 1.777f;
      for (int i = 0; i < 4; ++i) mappedInkOverlay_->inkColor[i] = inkColor_[i];
    }
  }
}

void PostProcess::Resize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  screenDropletsAspectRatio_ = (height_ > 0) ? (static_cast<float>(width_) / static_cast<float>(height_)) : 1.777f;
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->aspectRatio = screenDropletsAspectRatio_;
  }
  if (mappedBloodOverlay_) {
    // 血の形が縦横に潰れないよう、こちらも同じアスペクト比を流す
    mappedBloodOverlay_->aspectRatio = screenDropletsAspectRatio_;
  }
  if (mappedInkOverlay_) {
    // 墨が縦横に潰れないよう、同じアスペクト比を流す
    mappedInkOverlay_->aspectRatio = screenDropletsAspectRatio_;
  }

  // DepthStencil::Resize は深度リソース自体を作り直すため、
  // キャッシュ済みの深度SRVは破棄しないと解放済みリソースを指したままになる。
  // (DepthBasedOutline / Underwater / Caustics が共有している)
  if (depthSrv_.IsValid()) {
    dxCore_->SRVMan().Free(depthSrv_);
    depthSrv_ = {};
  }

  if (pingPongInitialized_) {
    pingPongA_->Initialize(dxCore_, width_, height_);
    pingPongB_->Initialize(dxCore_, width_, height_);
  }
}

void PostProcess::UpdateTime(float deltaTime) {
  randomTime_ += deltaTime;
  if (mappedRandom_) {
    mappedRandom_->time = randomTime_;
  }
  if (mappedUnderwater_) {
    mappedUnderwater_->time = randomTime_;
    mappedUnderwater_->lerpFactor = underwaterLerpFactor_;
  }
  if (mappedCaustics_) {
    mappedCaustics_->time = randomTime_;
    mappedCaustics_->lerpFactor = causticsLerpFactor_;
  }
  if (mappedLightShaft_) {
    // Caustics と同じ time を渡すことで床の網目と光柱の位相が揃う
    mappedLightShaft_->time = randomTime_;
    mappedLightShaft_->lerpFactor = lightShaftLerpFactor_;
  }
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->time = randomTime_;
    mappedScreenDroplets_->intensity = screenDropletsIntensity_;
  }
  if (mappedBloodOverlay_) {
    // time は心拍の位相にしか使わない（飛沫パターンは seed 固定でチラつかせない）
    mappedBloodOverlay_->time = randomTime_;
  }
  if (mappedInkOverlay_) {
    // time は艶の揺らぎにしか使わない（墨の形は seed 固定でチラつかせない）
    mappedInkOverlay_->time = randomTime_;
  }
}

void PostProcess::SetProjectionInverse(const float* projInv16) {
  if (mappedMaterial_) {
    memcpy(mappedMaterial_->projectionInverse, projInv16, sizeof(float) * 16);
  }
  if (mappedUnderwater_) {
    memcpy(mappedUnderwater_->projectionInverse, projInv16, sizeof(float) * 16);
  }
  if (mappedCaustics_) {
    memcpy(mappedCaustics_->projectionInverse, projInv16, sizeof(float) * 16);
  }
  if (mappedLightShaft_) {
    memcpy(mappedLightShaft_->projectionInverse, projInv16, sizeof(float) * 16);
  }
  if (mappedSsao_) {
    memcpy(mappedSsao_->projectionInverse, projInv16, sizeof(float) * 16);
  }
}

void PostProcess::SetViewInverse(const float* viewInv16) {
  if (mappedCaustics_) {
    memcpy(mappedCaustics_->viewInverse, viewInv16, sizeof(float) * 16);
  }
  if (mappedLightShaft_) {
    memcpy(mappedLightShaft_->viewInverse, viewInv16, sizeof(float) * 16);
  }
}

void PostProcess::SetOutlineColor(const float color[4]) {
  memcpy(outlineColor_, color, sizeof(float) * 4);
  if (mappedMaterial_) {
    memcpy(mappedMaterial_->outlineColor, color, sizeof(float) * 4);
  }
}

void PostProcess::SetOutlineWeight(float weight) {
  outlineWeight_ = weight;
  if (mappedMaterial_) {
    mappedMaterial_->outlineWeight = weight;
  }
}

void PostProcess::SetOutlineThickness(float thickness) {
  outlineThickness_ = thickness;
  if (mappedMaterial_) {
    mappedMaterial_->outlineThickness = thickness;
  }
}

void PostProcess::SetOutlineMode(int mode) {
  outlineMode_ = mode;
  if (mappedMaterial_) {
    mappedMaterial_->outlineMode = mode;
  }
}

// ============================================================================
// Dissolve パラメータ
// ============================================================================

void PostProcess::SetDissolveThreshold(float threshold) {
  dissolveThreshold_ = threshold;
  if (mappedDissolve_) {
    mappedDissolve_->threshold = threshold;
  }
}

void PostProcess::SetDissolveEdgeColor(float r, float g, float b) {
  dissolveEdgeColor_[0] = r;
  dissolveEdgeColor_[1] = g;
  dissolveEdgeColor_[2] = b;
  if (mappedDissolve_) {
    mappedDissolve_->edgeColor[0] = r;
    mappedDissolve_->edgeColor[1] = g;
    mappedDissolve_->edgeColor[2] = b;
  }
}

void PostProcess::SetDissolveBaseColor(float r, float g, float b, float a) {
  dissolveBaseColor_[0] = r;
  dissolveBaseColor_[1] = g;
  dissolveBaseColor_[2] = b;
  dissolveBaseColor_[3] = a;
  if (mappedDissolve_) {
    mappedDissolve_->baseColor[0] = r;
    mappedDissolve_->baseColor[1] = g;
    mappedDissolve_->baseColor[2] = b;
    mappedDissolve_->baseColor[3] = a;
  }
}

void PostProcess::SetDissolveEdgeRange(float range) {
  dissolveEdgeRange_ = range;
  if (mappedDissolve_) {
    mappedDissolve_->edgeRange = range;
  }
}

void PostProcess::SetDissolveNoiseIndex(int index) {
  if (!dissolveNoiseTextures_.empty()) {
    dissolveNoiseIndex_ = index % static_cast<int>(dissolveNoiseTextures_.size());
  }
}

void PostProcess::SetRandomNoiseIntensity(float intensity) {
  randomIntensity_ = intensity;
  if (mappedRandom_) mappedRandom_->intensity = intensity;
}

// ---- Bloom（b0 のルート定数で渡すので CBuffer は無い）----

void PostProcess::SetBloomThreshold(float threshold) { bloomThreshold_ = threshold; }
void PostProcess::SetBloomIntensity(float intensity) { bloomIntensity_ = intensity; }
void PostProcess::SetBloomRadius(float radius) { bloomRadius_ = radius; }
void PostProcess::SetBloomKnee(float knee) { bloomKnee_ = knee; }

// ---- SSAO ----

void PostProcess::SetSsaoRadius(float radius) {
  ssaoRadius_ = radius;
  if (mappedSsao_) mappedSsao_->radius = radius;
}

void PostProcess::SetSsaoIntensity(float intensity) {
  ssaoIntensity_ = intensity;
  if (mappedSsao_) mappedSsao_->intensity = intensity;
}

void PostProcess::SetSsaoBias(float bias) {
  ssaoBias_ = bias;
  if (mappedSsao_) mappedSsao_->bias = bias;
}

void PostProcess::SetSsaoPower(float power) {
  ssaoPower_ = power;
  if (mappedSsao_) mappedSsao_->power = power;
}

// ---- ColorGrade ----

void PostProcess::SetGradeExposure(float ev) {
  gradeExposure_ = ev;
  if (mappedColorGrade_) mappedColorGrade_->exposure = ev;
}

void PostProcess::SetGradeContrast(float contrast) {
  gradeContrast_ = contrast;
  if (mappedColorGrade_) mappedColorGrade_->contrast = contrast;
}

void PostProcess::SetGradeSaturation(float saturation) {
  gradeSaturation_ = saturation;
  if (mappedColorGrade_) mappedColorGrade_->saturation = saturation;
}

void PostProcess::SetGradeTemperature(float temperature) {
  gradeTemperature_ = temperature;
  if (mappedColorGrade_) mappedColorGrade_->temperature = temperature;
}

void PostProcess::SetGradeTint(float tint) {
  gradeTint_ = tint;
  if (mappedColorGrade_) mappedColorGrade_->tint = tint;
}

void PostProcess::SetGradeColorFilter(float r, float g, float b) {
  gradeColorFilter_[0] = r;
  gradeColorFilter_[1] = g;
  gradeColorFilter_[2] = b;
  gradeColorFilter_[3] = 1.0f;
  if (mappedColorGrade_) {
    memcpy(mappedColorGrade_->colorFilter, gradeColorFilter_, sizeof(float) * 4);
  }
}

void PostProcess::SetGradeLerpFactor(float lerpFactor) {
  gradeLerpFactor_ = lerpFactor;
  if (mappedColorGrade_) mappedColorGrade_->lerpFactor = lerpFactor;
}

void PostProcess::SetMaskOutlineColor(float r, float g, float b, float a) {
  maskOutlineColor_[0] = r;
  maskOutlineColor_[1] = g;
  maskOutlineColor_[2] = b;
  maskOutlineColor_[3] = a;
  if (mappedMaskOutline_) {
    mappedMaskOutline_->color[0] = r;
    mappedMaskOutline_->color[1] = g;
    mappedMaskOutline_->color[2] = b;
    mappedMaskOutline_->color[3] = a;
  }
}

void PostProcess::SetMaskOutlineThickness(float thickness) {
  maskOutlineThickness_ = thickness;
  if (mappedMaskOutline_) mappedMaskOutline_->thickness = thickness;
}

void PostProcess::SetOutlineExclusions(const float *rectsPixels, int count) {
  // 画面ピクセル → UV。解像度が未確定（初期化前・リサイズ直後）なら除外しない。
  int n = (rectsPixels == nullptr) ? 0 : std::clamp(count, 0, kMaxOutlineExclusions);
  if (width_ == 0 || height_ == 0) n = 0;

  if (mappedMaterial_) mappedMaterial_->excludeCount = n;
  if (mappedMaskOutline_) mappedMaskOutline_->excludeCount = n;
  if (n <= 0) return;

  const float invW = 1.0f / static_cast<float>(width_);
  const float invH = 1.0f / static_cast<float>(height_);

  for (int i = 0; i < n; ++i) {
    const float *r = rectsPixels + i * 4;
    const float uv[4] = {r[0] * invW, r[1] * invH, r[2] * invW, r[3] * invH};
    for (int c = 0; c < 4; ++c) {
      if (mappedMaterial_) mappedMaterial_->excludeRects[i][c] = uv[c];
      if (mappedMaskOutline_) mappedMaskOutline_->excludeRects[i][c] = uv[c];
    }
  }
}

void PostProcess::SetMaskOutlineStrength(float strength) {
  maskOutlineStrength_ = strength;
  if (mappedMaskOutline_) mappedMaskOutline_->strength = strength;
}

void PostProcess::SetRandomNoiseColor(float r, float g, float b) {
  randomColor_[0] = r;
  randomColor_[1] = g;
  randomColor_[2] = b;
  if (mappedRandom_) {
    mappedRandom_->color[0] = r;
    mappedRandom_->color[1] = g;
    mappedRandom_->color[2] = b;
  }
}

// ============================================================================
// Underwater パラメータ
// ============================================================================

void PostProcess::SetUnderwaterTintColor(float r, float g, float b, float a) {
  underwaterTintColor_[0] = r;
  underwaterTintColor_[1] = g;
  underwaterTintColor_[2] = b;
  underwaterTintColor_[3] = a;
  if (mappedUnderwater_) {
    mappedUnderwater_->tintColor[0] = r;
    mappedUnderwater_->tintColor[1] = g;
    mappedUnderwater_->tintColor[2] = b;
    mappedUnderwater_->tintColor[3] = a;
  }
}

void PostProcess::SetUnderwaterDistortionForce(float force) {
  underwaterDistortionForce_ = force;
  if (mappedUnderwater_) {
    mappedUnderwater_->distortionForce = force;
  }
}

// ============================================================================
// Caustics パラメータ
// ============================================================================

void PostProcess::SetCausticsColor(float r, float g, float b) {
  causticsColor_[0] = r;
  causticsColor_[1] = g;
  causticsColor_[2] = b;
  if (mappedCaustics_) {
    mappedCaustics_->causticsColor[0] = r;
    mappedCaustics_->causticsColor[1] = g;
    mappedCaustics_->causticsColor[2] = b;
  }
}

void PostProcess::SetCausticsIntensity(float intensity) {
  causticsIntensity_ = intensity;
  if (mappedCaustics_) {
    mappedCaustics_->intensity = intensity;
  }
}

void PostProcess::SetCausticsScale(float scale) {
  causticsScale_ = scale;
  if (mappedCaustics_) {
    mappedCaustics_->scale = scale;
  }
  // 光柱の断面と床の網目は同じパターンなので密度を揃える
  SyncLightShaftWithCaustics();
}

void PostProcess::SetCausticsSpeed(float speed) {
  causticsSpeed_ = speed;
  if (mappedCaustics_) {
    mappedCaustics_->speed = speed;
  }
  SyncLightShaftWithCaustics();
}

void PostProcess::SetCausticsWater(float waterHeight, float fadeDistance) {
  causticsWaterHeight_ = waterHeight;
  causticsDepthFadeDistance_ = fadeDistance;
  if (mappedCaustics_) {
    mappedCaustics_->waterHeight = waterHeight;
    mappedCaustics_->depthFadeDistance = fadeDistance;
  }
  SyncLightShaftWithCaustics();
}

void PostProcess::SetCausticsContrast(float contrast) {
  causticsContrast_ = contrast;
  if (mappedCaustics_) {
    mappedCaustics_->contrast = contrast;
  }
}

void PostProcess::SetCausticsChromaticOffset(float offset) {
  causticsChromaticOffset_ = offset;
  if (mappedCaustics_) {
    mappedCaustics_->chromaticOffset = offset;
  }
}

void PostProcess::SetCausticsUpwardBias(float bias) {
  causticsUpwardBias_ = bias;
  if (mappedCaustics_) {
    mappedCaustics_->upwardBias = bias;
  }
}

void PostProcess::SetCausticsDistanceFade(float start, float end) {
  causticsDistanceFadeStart_ = start;
  causticsDistanceFadeEnd_ = end;
  if (mappedCaustics_) {
    mappedCaustics_->distanceFadeStart = start;
    mappedCaustics_->distanceFadeEnd = end;
  }
}

void PostProcess::SetCausticsLerpFactor(float lerpFactor) {
  causticsLerpFactor_ = lerpFactor;
  if (mappedCaustics_) {
    mappedCaustics_->lerpFactor = lerpFactor;
  }
}

// ============================================================================
// LightShaft パラメータ
// ============================================================================

void PostProcess::SetLightShaftColor(float r, float g, float b) {
  lightShaftColor_[0] = r;
  lightShaftColor_[1] = g;
  lightShaftColor_[2] = b;
  if (mappedLightShaft_) {
    mappedLightShaft_->shaftColor[0] = r;
    mappedLightShaft_->shaftColor[1] = g;
    mappedLightShaft_->shaftColor[2] = b;
  }
}

void PostProcess::SetLightShaftIntensity(float intensity) {
  lightShaftIntensity_ = intensity;
  if (mappedLightShaft_) {
    mappedLightShaft_->intensity = intensity;
  }
}

void PostProcess::SetLightShaftDensity(float density) {
  lightShaftDensity_ = density;
  if (mappedLightShaft_) {
    mappedLightShaft_->density = density;
  }
}

void PostProcess::SetLightShaftContrast(float contrast) {
  lightShaftContrast_ = contrast;
  if (mappedLightShaft_) {
    mappedLightShaft_->contrast = contrast;
  }
}

void PostProcess::SetLightShaftDitherStrength(float strength) {
  lightShaftDitherStrength_ = strength;
  if (mappedLightShaft_) {
    mappedLightShaft_->ditherStrength = strength;
  }
}

void PostProcess::SetLightShaftMaxDistance(float maxDistance) {
  lightShaftMaxDistance_ = maxDistance;
  if (mappedLightShaft_) {
    mappedLightShaft_->maxDistance = maxDistance;
  }
}

void PostProcess::SetLightShaftSampleCount(int sampleCount) {
  // 0 以下だとシェーダー側で除算が壊れるためクランプする
  lightShaftSampleCount_ = (sampleCount < 1) ? 1 : sampleCount;
  if (mappedLightShaft_) {
    mappedLightShaft_->sampleCount = lightShaftSampleCount_;
  }
}

void PostProcess::SetLightShaftLerpFactor(float lerpFactor) {
  lightShaftLerpFactor_ = lerpFactor;
  if (mappedLightShaft_) {
    mappedLightShaft_->lerpFactor = lerpFactor;
  }
}

void PostProcess::SetLightShaftSunDirection(float x, float y, float z) {
  // 上向き・ほぼ水平だと水面へ戻す距離が発散するので、下向きに最低限の傾きを持たせる
  if (y > -0.2f) y = -0.2f;
  float len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-6f) { x = 0.0f; y = -1.0f; z = 0.0f; len = 1.0f; }
  lightShaftSunDir_[0] = x / len;
  lightShaftSunDir_[1] = y / len;
  lightShaftSunDir_[2] = z / len;
  if (mappedLightShaft_) {
    mappedLightShaft_->sunDir[0] = lightShaftSunDir_[0];
    mappedLightShaft_->sunDir[1] = lightShaftSunDir_[1];
    mappedLightShaft_->sunDir[2] = lightShaftSunDir_[2];
    mappedLightShaft_->sunDir[3] = 0.0f;
  }
}

void PostProcess::SyncLightShaftWithCaustics() {
  if (mappedLightShaft_) {
    mappedLightShaft_->scale = causticsScale_;
    mappedLightShaft_->speed = causticsSpeed_;
    mappedLightShaft_->waterHeight = causticsWaterHeight_;
  }
}

// ============================================================================
// ScreenDroplets パラメータ
// ============================================================================

void PostProcess::SetScreenDropletsIntensity(float intensity) {
  screenDropletsIntensity_ = intensity;
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->intensity = intensity;
  }
}

void PostProcess::SetScreenDropletsSpeed(float speed) {
  screenDropletsSpeed_ = speed;
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->speed = speed;
  }
}

void PostProcess::SetScreenDropletsDistortion(float distortion) {
  screenDropletsDistortion_ = distortion;
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->distortion = distortion;
  }
}

void PostProcess::SetScreenDropletsScale(float scale) {
  screenDropletsScale_ = scale;
  if (mappedScreenDroplets_) {
    mappedScreenDroplets_->scale = scale;
  }
}

// ============================================================================
// BloodOverlay パラメータ
// ============================================================================

void PostProcess::SetBloodOverlayHitFlash(float flash) {
  bloodHitFlash_ = std::clamp(flash, 0.0f, 1.0f);
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->hitFlash = bloodHitFlash_;
  }
}

void PostProcess::SetBloodOverlayLowHealth(float level) {
  bloodLowHealth_ = std::clamp(level, 0.0f, 1.0f);
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->lowHealth = bloodLowHealth_;
  }
}

void PostProcess::SetBloodOverlayColor(float r, float g, float b) {
  bloodColor_[0] = r;
  bloodColor_[1] = g;
  bloodColor_[2] = b;
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->bloodColor[0] = r;
    mappedBloodOverlay_->bloodColor[1] = g;
    mappedBloodOverlay_->bloodColor[2] = b;
    mappedBloodOverlay_->bloodColor[3] = 1.0f;
  }
}

void PostProcess::SetBloodOverlayCoverage(float coverage) {
  bloodCoverage_ = std::clamp(coverage, 0.0f, 1.0f);
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->coverage = bloodCoverage_;
  }
}

void PostProcess::SetBloodOverlaySplatterScale(float scale) {
  bloodSplatterScale_ = scale;
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->splatterScale = scale;
  }
}

void PostProcess::SetBloodOverlayDesaturate(float desaturate) {
  bloodDesaturate_ = std::clamp(desaturate, 0.0f, 1.0f);
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->desaturate = bloodDesaturate_;
  }
}

void PostProcess::SetBloodOverlayPulseSpeed(float speed) {
  bloodPulseSpeed_ = speed;
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->pulseSpeed = speed;
  }
}

void PostProcess::SetBloodOverlaySeed(float seed) {
  // [0, 1) に折り返す。大きな値を渡すとシェーダー側のハッシュの刻みが粗くなり、
  // 飛沫パターンの種類が目に見えて減る。
  bloodSeed_ = seed - std::floor(seed);
  if (mappedBloodOverlay_) {
    mappedBloodOverlay_->seed = bloodSeed_;
  }
}

void PostProcess::RerollBloodOverlaySplatter() {
  // 黄金比を足して小数部を取る（低食い違い列。連続で呼んでも似た値が並ばない）
  SetBloodOverlaySeed(bloodSeed_ + 0.6180339887f);
}

// ============================================================================
// InkOverlay パラメータ
// ============================================================================

void PostProcess::SetInkOverlaySplats(const InkSplat *splats, int count) {
  inkSplatCount_ = std::clamp(count, 0, kMaxInkSplats);
  if (!splats) inkSplatCount_ = 0;
  if (!mappedInkOverlay_) return;
  mappedInkOverlay_->splatCount = inkSplatCount_;
  for (int i = 0; i < inkSplatCount_; ++i) {
    const InkSplat &s = splats[i];
    mappedInkOverlay_->splatA[i][0] = s.centerX;
    mappedInkOverlay_->splatA[i][1] = s.centerY;
    mappedInkOverlay_->splatA[i][2] = (std::max)(s.radius, 0.001f);
    mappedInkOverlay_->splatA[i][3] = std::clamp(s.strength, 0.0f, 1.0f);
    mappedInkOverlay_->splatB[i][0] = (std::max)(s.age, 0.0f);
    // [0, 1) に折り返す（大きな値だとシェーダーのハッシュの刻みが粗くなる）
    mappedInkOverlay_->splatB[i][1] = s.seed - std::floor(s.seed);
    mappedInkOverlay_->splatB[i][2] = 0.0f;
    mappedInkOverlay_->splatB[i][3] = 0.0f;
  }
}

void PostProcess::SetInkOverlayMurk(float murk) {
  inkMurk_ = std::clamp(murk, 0.0f, 1.0f);
  if (mappedInkOverlay_) {
    mappedInkOverlay_->murk = inkMurk_;
  }
}

void PostProcess::SetInkOverlayColor(float r, float g, float b, float opacity) {
  inkColor_[0] = r;
  inkColor_[1] = g;
  inkColor_[2] = b;
  inkColor_[3] = std::clamp(opacity, 0.0f, 1.0f);
  if (mappedInkOverlay_) {
    for (int i = 0; i < 4; ++i) mappedInkOverlay_->inkColor[i] = inkColor_[i];
  }
}

// ============================================================================
void PostProcess::InitDissolveNoiseTextures() {
  if (dissolveNoiseInitialized_) return;
  dissolveNoiseInitialized_ = true;

  // Resources/noise/ フォルダからPNGをリストアップ
  const std::string noiseDir = Chaso::ResolvePath("Resources/noise");
  if (!std::filesystem::exists(noiseDir)) return;

  auto& rc = RC::RenderContext::GetInstance();
  auto& texMan = rc.Textures();

  for (const auto& entry : std::filesystem::directory_iterator(noiseDir)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".png" && ext != ".PNG") continue;

    NoiseEntry ne;
    ne.path = entry.path().string();
    ne.name = entry.path().stem().string();
    // srgb=false: マスクはリニア値として扱う
    ne.srv = texMan.Load(ne.path, false);
    dissolveNoiseTextures_.push_back(ne);
    Log::Print(std::format("[PostProcess] Dissolve noise loaded: {}", ne.name));
  }

  if (dissolveNoiseTextures_.empty()) {
    Log::Print("[PostProcess] Warning: No noise textures found in Resources/noise/");
  }
}

// ============================================================================
// エフェクト操作
// ============================================================================

void PostProcess::SetEffect(PostEffectType type) {
  activeEffects_.clear();
  if (type != PostEffectType::None) {
    activeEffects_.push_back(type);
    Log::Print(std::format("[PostProcess] SetEffect: {} (Active: {})", ToString(type), ActiveEffectsToString(activeEffects_)));
  } else {
    Log::Print(std::format("[PostProcess] SetEffect: None (Active: {})", ActiveEffectsToString(activeEffects_)));
  }
}

PostEffectType PostProcess::GetEffect() const {
  return activeEffects_.empty() ? PostEffectType::None : activeEffects_.front();
}

void PostProcess::AddEffect(PostEffectType type) {
  if (type == PostEffectType::None) {
    return;
  }
  if (!HasEffect(type)) {
    activeEffects_.push_back(type);
    Log::Print(std::format("[PostProcess] AddEffect: {} (Active: {})", ToString(type), ActiveEffectsToString(activeEffects_)));
  }
}

void PostProcess::RemoveEffect(PostEffectType type) {
  auto it = std::find(activeEffects_.begin(), activeEffects_.end(), type);
  if (it != activeEffects_.end()) {
    activeEffects_.erase(it);
    Log::Print(std::format("[PostProcess] RemoveEffect: {} (Active: {})", ToString(type), ActiveEffectsToString(activeEffects_)));
  }
}

void PostProcess::ClearEffects() {
  if (!activeEffects_.empty()) {
    activeEffects_.clear();
    Log::Print(std::format("[PostProcess] ClearEffects (Active: {})", ActiveEffectsToString(activeEffects_)));
  }
}

bool PostProcess::MoveEffect(size_t index, int offset) {
  if (index >= activeEffects_.size()) {
    return false;
  }
  const int target = static_cast<int>(index) + offset;
  if (target < 0 || target >= static_cast<int>(activeEffects_.size())) {
    return false;
  }
  std::swap(activeEffects_[index], activeEffects_[static_cast<size_t>(target)]);
  Log::Print(std::format("[PostProcess] MoveEffect (Active: {})",
                         ActiveEffectsToString(activeEffects_)));
  return true;
}

bool PostProcess::HasEffect(PostEffectType type) const {
  return std::find(activeEffects_.begin(), activeEffects_.end(), type) !=
         activeEffects_.end();
}

// ============================================================================
// パイプライン選択
// ============================================================================

GraphicsPipeline *PostProcess::GetPipelineForEffect(PostEffectType type) {
  switch (type) {
  case PostEffectType::Grayscale:
    return pipelineGrayscale_;
  case PostEffectType::Sepia:
    return pipelineSepia_;
  case PostEffectType::Vignette:
    return pipelineVignette_;
  case PostEffectType::BoxFilter:
    return pipelineBoxFilter_;
  case PostEffectType::GaussianFilter:
    return pipelineGaussianFilter_;
  case PostEffectType::DepthBasedOutline:
    return pipelineDepthBasedOutline_;
  case PostEffectType::RadialBlur:
    return pipelineRadialBlur_;
  case PostEffectType::Dissolve:
    return pipelineDissolve_;
  case PostEffectType::RandomNoise:
    return pipelineRandom_;
  case PostEffectType::MaskOutline:
    // マスクがまだ描かれていないフレームは素通し（未初期化ディスクリプタを読ませない）
    return (maskSrv_.ptr != 0) ? pipelineMaskOutline_ : pipelineCopy_;
  case PostEffectType::Ssao:
    return pipelineSsao_;
  case PostEffectType::Bloom:
    return pipelineBloom_;
  case PostEffectType::ColorGrade:
    return pipelineColorGrade_;
  case PostEffectType::Fxaa:
    return pipelineFxaa_;
  case PostEffectType::Underwater:
    return pipelineUnderwater_;
  case PostEffectType::Caustics:
    return pipelineCaustics_;
  case PostEffectType::LightShaft:
    return pipelineLightShaft_;
  case PostEffectType::ScreenDroplets:
    return pipelineScreenDroplets_;
  case PostEffectType::BloodOverlay:
    return pipelineBloodOverlay_;
  case PostEffectType::InkOverlay:
    return pipelineInkOverlay_;
  case PostEffectType::None:
  default:
    return pipelineCopy_;
  }
}

// ============================================================================
// ピンポンテクスチャ（マルチパス用・遅延初期化）
// ============================================================================

void PostProcess::EnsurePingPongTextures() {
  if (pingPongInitialized_) {
    return;
  }
  pingPongA_ = std::make_unique<RenderTexture>();
  pingPongA_->Initialize(dxCore_, width_, height_);

  pingPongB_ = std::make_unique<RenderTexture>();
  pingPongB_->Initialize(dxCore_, width_, height_);

  pingPongInitialized_ = true;
}

// ============================================================================
// 1パス描画
// ============================================================================

void PostProcess::DrawSinglePass(ID3D12GraphicsCommandList *cmdList,
                                 D3D12_GPU_DESCRIPTOR_HANDLE srcSRV,
                                 GraphicsPipeline *pipeline,
                                 PostEffectType effectType) {
  // NOTE: null を D3D12 に渡すと D3D12Core.dll の奥でアクセス違反になり、
  //       呼び出し元が分からないスタックになる。ここで弾いて原因を残す。
  if (!pipeline || !pipeline->Root() || !pipeline->PSO()) {
    // エフェクト種別ごとに1回だけ出す（毎フレーム呼ばれるためログが埋まる）
    static std::set<PostEffectType> logged;
    if (logged.insert(effectType).second) {
      Log::Print(std::format(
          "[PostProcess] 無効なパイプラインのため描画をスキップします "
          "(effectType={}, pipeline={}, root={}, pso={})",
          static_cast<int>(effectType), pipeline != nullptr,
          pipeline && pipeline->Root() != nullptr,
          pipeline && pipeline->PSO() != nullptr));
    }
    return;
  }

  cmdList->SetGraphicsRootSignature(pipeline->Root());
  cmdList->SetPipelineState(pipeline->PSO());
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // RootSignatureType::PostProcess → params[0] が SRV table (t0)
  cmdList->SetGraphicsRootDescriptorTable(0, srcSRV);

  // params[1] が RootConstants (b0) — エフェクト固有パラメータ
  struct PostEffectConstants {
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
  } constants = { 0, 0, 0, 0 };

  if (effectType == PostEffectType::Grayscale) {
    constants.param0 = *(uint32_t *)&grayscaleLerpFactor_;
  } else if (effectType == PostEffectType::BoxFilter) {
    constants.param0 = static_cast<uint32_t>(boxFilterK_);
  } else if (effectType == PostEffectType::GaussianFilter) {
    constants.param0 = static_cast<uint32_t>(gaussianFilterK_);
    constants.param1 = *(uint32_t *)&gaussianSigma_;
  } else if (effectType == PostEffectType::RadialBlur) {
    constants.param0 = *(uint32_t *)&radialBlurCenter_.x;
    constants.param1 = *(uint32_t *)&radialBlurCenter_.y;
    constants.param2 = *(uint32_t *)&radialBlurWidth_;
    constants.param3 = static_cast<uint32_t>(radialBlurSamples_);
  } else if (effectType == PostEffectType::Bloom) {
    constants.param0 = *(uint32_t *)&bloomThreshold_;
    constants.param1 = *(uint32_t *)&bloomIntensity_;
    constants.param2 = *(uint32_t *)&bloomRadius_;
    constants.param3 = *(uint32_t *)&bloomKnee_;
  }

  cmdList->SetGraphicsRoot32BitConstants(1, 4, &constants, 0);

  if (effectType == PostEffectType::DepthBasedOutline) {
    if (!depthSrv_.IsValid()) {
      depthSrv_ = dxCore_->SRVMan().CreateTexture2D(
          dxCore_->GetDepthResource(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
    }
    // params[2]: t1
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrv_.gpu);
    // params[3]: b1
    cmdList->SetGraphicsRootConstantBufferView(3, dynMaterial_.Address());
  }

  if (effectType == PostEffectType::Dissolve) {
    // params[2]: t1 (ノイズマスクテクスチャ)
    if (!dissolveNoiseTextures_.empty()) {
      int idx = dissolveNoiseIndex_ % static_cast<int>(dissolveNoiseTextures_.size());
      cmdList->SetGraphicsRootDescriptorTable(2, dissolveNoiseTextures_[idx].srv);
    }
    // params[3]: b1 (DissolveParams CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynDissolve_.Address());
  }

  if (effectType == PostEffectType::RandomNoise) {
    // params[3]: b1 (RandomNoise CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynRandom_.Address());
  }

  if (effectType == PostEffectType::Ssao) {
    if (!depthSrv_.IsValid()) {
      depthSrv_ = dxCore_->SRVMan().CreateTexture2D(
          dxCore_->GetDepthResource(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
    }
    // params[2]: t1 (Depth SRV)
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrv_.gpu);
    // params[3]: b1 (SSAO CBuffer。projectionInverse を含む)
    cmdList->SetGraphicsRootConstantBufferView(3, dynSsao_.Address());
  }

  if (effectType == PostEffectType::ColorGrade) {
    // params[3]: b1 (ColorGrade CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynColorGrade_.Address());
  }

  // Bloom と Fxaa は t0 と b0 だけで足りるので追加バインドは無い

  if (effectType == PostEffectType::MaskOutline && maskSrv_.ptr != 0) {
    // params[2]: t1 (マスクRT)
    cmdList->SetGraphicsRootDescriptorTable(2, maskSrv_);
    // params[3]: b1 (MaskOutline CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynMaskOutline_.Address());
  }

  if (effectType == PostEffectType::Underwater) {
    if (!depthSrv_.IsValid()) {
      depthSrv_ = dxCore_->SRVMan().CreateTexture2D(
          dxCore_->GetDepthResource(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
    }
    // params[2]: t1 (Depth SRV)
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrv_.gpu);
    // params[3]: b1 (Underwater CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynUnderwater_.Address());
  }

  if (effectType == PostEffectType::Caustics) {
    if (!depthSrv_.IsValid()) {
      depthSrv_ = dxCore_->SRVMan().CreateTexture2D(
          dxCore_->GetDepthResource(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
    }
    // params[2]: t1 (Depth SRV)
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrv_.gpu);
    // params[3]: b1 (Caustics CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynCaustics_.Address());
  }

  if (effectType == PostEffectType::LightShaft) {
    if (!depthSrv_.IsValid()) {
      depthSrv_ = dxCore_->SRVMan().CreateTexture2D(
          dxCore_->GetDepthResource(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);
    }
    // params[2]: t1 (Depth SRV)
    cmdList->SetGraphicsRootDescriptorTable(2, depthSrv_.gpu);
    // params[3]: b1 (LightShaft CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynLightShaft_.Address());
  }

  if (effectType == PostEffectType::ScreenDroplets) {
    // params[3]: b1 (ScreenDroplets CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynScreenDroplets_.Address());
  }

  if (effectType == PostEffectType::BloodOverlay) {
    // params[3]: b1 (BloodOverlay CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynBloodOverlay_.Address());
  }

  if (effectType == PostEffectType::InkOverlay) {
    // params[3]: b1 (InkOverlay CBuffer)
    cmdList->SetGraphicsRootConstantBufferView(3, dynInkOverlay_.Address());
  }

  // 全画面三角形（頂点バッファなし、SV_VertexID 使用）
  cmdList->DrawInstanced(3, 1, 0, 0);
}

// ============================================================================
// メイン描画（マルチパス対応）
// ============================================================================

void PostProcess::Draw(ID3D12GraphicsCommandList *cmdList,
                       const RenderTexture &renderTexture,
                       RenderTexture *dstTexture) {
  // ビューポート・シザー（全パス共通サイズ）
  D3D12_VIEWPORT vp{};
  vp.Width = static_cast<float>(width_);
  vp.Height = static_cast<float>(height_);
  vp.MaxDepth = 1.0f;
  D3D12_RECT sr{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};

  // 最終描画先を設定するラムダ
  auto SetFinalRenderTarget = [&]() {
    if (dstTexture) {
      dstTexture->TransitionToRenderTarget(cmdList);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = dstTexture->GetRTV();
      cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      cmdList->RSSetViewports(1, &vp);
      cmdList->RSSetScissorRects(1, &sr);
    } else {
      D3D12_CPU_DESCRIPTOR_HANDLE backRtv = dxCore_->CurrentRTV();
      cmdList->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
      dxCore_->ResetViewportScissorToBackbuffer(width_, height_);
    }
  };

  // --- エフェクトなし：そのままコピー ---
  if (activeEffects_.empty()) {
    SetFinalRenderTarget();
    DrawSinglePass(cmdList, renderTexture.GetSRVGPU(), pipelineCopy_, PostEffectType::None);
    return;
  }

  // --- 1エフェクト：シングルパス ---
  if (activeEffects_.size() == 1) {
    SetFinalRenderTarget();
    DrawSinglePass(cmdList, renderTexture.GetSRVGPU(),
                   GetPipelineForEffect(activeEffects_[0]),
                   activeEffects_[0]);
    return;
  }

  // --- 2エフェクト以上：マルチパス（ピンポン） ---
  EnsurePingPongTextures();

  RenderTexture *textures[2] = {pingPongA_.get(), pingPongB_.get()};
  const size_t count = activeEffects_.size();

  for (size_t i = 0; i < count; ++i) {
    const bool isFirst = (i == 0);
    const bool isLast = (i == count - 1);

    // ── ソースSRV ──
    D3D12_GPU_DESCRIPTOR_HANDLE srcSRV;
    if (isFirst) {
      // 最初のパス：入力テクスチャ（App の RenderTexture、すでに SRV 状態）
      srcSRV = renderTexture.GetSRVGPU();
    } else {
      // 前パスの出力をソースにする
      RenderTexture *prevDst = textures[(i - 1) % 2];
      prevDst->TransitionToShaderResource(cmdList);
      srcSRV = prevDst->GetSRVGPU();
    }

    // ── デスティネーション ──
    if (!isLast) {
      // 中間パス：ピンポンテクスチャに書き込む
      RenderTexture *dst = textures[i % 2];
      dst->TransitionToRenderTarget(cmdList);

      D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst->GetRTV();
      cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      cmdList->RSSetViewports(1, &vp);
      cmdList->RSSetScissorRects(1, &sr);
    } else {
      // 最終パス：バックバッファまたは指定されたテクスチャに書き込む
      SetFinalRenderTarget();
    }

    // ── 描画 ──
    DrawSinglePass(cmdList, srcSRV,
                   GetPipelineForEffect(activeEffects_[i]),
                   activeEffects_[i]);
  }
}

// ============================================================================
// ImGui
// ============================================================================

void PostProcess::DrawImGui([[maybe_unused]] const char *label) {
#if RC_ENABLE_IMGUI
  if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
    // 各エフェクトのチェックボックス
    bool grayscale = HasEffect(PostEffectType::Grayscale);
    bool sepia = HasEffect(PostEffectType::Sepia);
    bool vignette = HasEffect(PostEffectType::Vignette);

    if (ImGui::Checkbox("Grayscale", &grayscale)) {
      if (grayscale) {
        AddEffect(PostEffectType::Grayscale);
      } else {
        RemoveEffect(PostEffectType::Grayscale);
      }
    }

    if (grayscale) {
      ImGui::Indent();
      ImGui::SliderFloat("Grayscale Lerp Factor", &grayscaleLerpFactor_, 0.0f, 1.0f);
      ImGui::Unindent();
    }

    if (ImGui::Checkbox("Sepia", &sepia)) {
      if (sepia) {
        AddEffect(PostEffectType::Sepia);
      } else {
        RemoveEffect(PostEffectType::Sepia);
      }
    }

    if (ImGui::Checkbox("Vignette", &vignette)) {
      if (vignette) {
        AddEffect(PostEffectType::Vignette);
      } else {
        RemoveEffect(PostEffectType::Vignette);
      }
    }

    bool boxFilter = HasEffect(PostEffectType::BoxFilter);
    if (ImGui::Checkbox("BoxFilter", &boxFilter)) {
      if (boxFilter) {
        AddEffect(PostEffectType::BoxFilter);
      } else {
        RemoveEffect(PostEffectType::BoxFilter);
      }
    }

    // BoxFilter が有効なとき K スライダーを表示
    if (boxFilter) {
      ImGui::Indent();
      ImGui::SliderInt("K", &boxFilterK_, 1, 10);
      ImGui::Unindent();
    }

    bool gaussianFilter = HasEffect(PostEffectType::GaussianFilter);
    if (ImGui::Checkbox("GaussianFilter", &gaussianFilter)) {
      if (gaussianFilter) {
        AddEffect(PostEffectType::GaussianFilter);
      } else {
        RemoveEffect(PostEffectType::GaussianFilter);
      }
    }

    if (gaussianFilter) {
      ImGui::Indent();
      ImGui::SliderInt("Gaussian K", &gaussianFilterK_, 1, 10);
      ImGui::SliderFloat("Gaussian Sigma", &gaussianSigma_, 0.1f, 10.0f);
      ImGui::Unindent();
    }

    bool depthBasedOutline = HasEffect(PostEffectType::DepthBasedOutline);
    if (ImGui::Checkbox("DepthBasedOutline", &depthBasedOutline)) {
      if (depthBasedOutline) {
        AddEffect(PostEffectType::DepthBasedOutline);
      } else {
        RemoveEffect(PostEffectType::DepthBasedOutline);
      }
    }

    if (depthBasedOutline) {
      ImGui::Indent();
      bool changed = false;
      if (ImGui::ColorEdit4("Outline Color", outlineColor_)) changed = true;
      if (ImGui::SliderFloat("Outline Weight", &outlineWeight_, 0.0f, 20.0f)) changed = true;
      if (ImGui::SliderFloat("Outline Thickness", &outlineThickness_, 0.1f, 10.0f)) changed = true;

      const char* modes[] = { "Both (両側)", "Outside (外側)", "Inside (内側)" };
      if (ImGui::Combo("Outline Mode", &outlineMode_, modes, 3)) changed = true;

      if (changed) {
        SetOutlineColor(outlineColor_);
        SetOutlineWeight(outlineWeight_);
        SetOutlineThickness(outlineThickness_);
        SetOutlineMode(outlineMode_);
      }
      ImGui::Unindent();
    }

    bool radialBlur = HasEffect(PostEffectType::RadialBlur);
    if (ImGui::Checkbox("RadialBlur", &radialBlur)) {
      if (radialBlur) {
        AddEffect(PostEffectType::RadialBlur);
      } else {
        RemoveEffect(PostEffectType::RadialBlur);
      }
    }

    if (radialBlur) {
      ImGui::Indent();
      ImGui::SliderFloat2("Center", &radialBlurCenter_.x, 0.0f, 1.0f);
      ImGui::SliderFloat("BlurWidth", &radialBlurWidth_, -0.1f, 0.1f);
      ImGui::SliderInt("Samples", &radialBlurSamples_, 1, 50);
      ImGui::Unindent();
    }

    bool dissolve = HasEffect(PostEffectType::Dissolve);
    if (ImGui::Checkbox("Dissolve", &dissolve)) {
      if (dissolve) {
        AddEffect(PostEffectType::Dissolve);
      } else {
        RemoveEffect(PostEffectType::Dissolve);
      }
    }

    if (dissolve) {
      ImGui::Indent();
      bool changed = false;
      if (ImGui::ColorEdit3("EdgeColor", dissolveEdgeColor_)) changed = true;
      if (ImGui::ColorEdit4("BaseColor", dissolveBaseColor_)) changed = true;
      if (ImGui::SliderFloat("Threshold", &dissolveThreshold_, 0.0f, 1.0f)) changed = true;
      if (ImGui::SliderFloat("EdgeRange", &dissolveEdgeRange_, 0.001f, 0.2f)) changed = true;

      if (changed) {
        SetDissolveEdgeColor(dissolveEdgeColor_[0], dissolveEdgeColor_[1], dissolveEdgeColor_[2]);
        SetDissolveBaseColor(dissolveBaseColor_[0], dissolveBaseColor_[1], dissolveBaseColor_[2], dissolveBaseColor_[3]);
        SetDissolveThreshold(dissolveThreshold_);
        SetDissolveEdgeRange(dissolveEdgeRange_);
      }

      // ノイズ選択コンボボックス
      if (!dissolveNoiseTextures_.empty()) {
        const char* currentName = dissolveNoiseTextures_[dissolveNoiseIndex_].name.c_str();
        if (ImGui::BeginCombo("Noise", currentName)) {
          for (int i = 0; i < static_cast<int>(dissolveNoiseTextures_.size()); ++i) {
            bool selected = (i == dissolveNoiseIndex_);
            if (ImGui::Selectable(dissolveNoiseTextures_[i].name.c_str(), selected)) {
              dissolveNoiseIndex_ = i;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
      }
      ImGui::Unindent();
    }

    bool randomNoise = HasEffect(PostEffectType::RandomNoise);
    if (ImGui::Checkbox("RandomNoise", &randomNoise)) {
      if (randomNoise) {
        AddEffect(PostEffectType::RandomNoise);
      } else {
        RemoveEffect(PostEffectType::RandomNoise);
      }
    }
    if (randomNoise) {
      ImGui::Indent();
      ImGui::Text("Time: %.3f", randomTime_);
      if (ImGui::SliderFloat("Intensity", &randomIntensity_, 0.0f, 1.0f)) {
        if (mappedRandom_) mappedRandom_->intensity = randomIntensity_;
      }
      if (ImGui::ColorEdit3("Noise Color", randomColor_)) {
        if (mappedRandom_) {
          mappedRandom_->color[0] = randomColor_[0];
          mappedRandom_->color[1] = randomColor_[1];
          mappedRandom_->color[2] = randomColor_[2];
        }
      }
      ImGui::Unindent();
    }

    bool ssao = HasEffect(PostEffectType::Ssao);
    if (ImGui::Checkbox("SSAO", &ssao)) {
      if (ssao) {
        AddEffect(PostEffectType::Ssao);
      } else {
        RemoveEffect(PostEffectType::Ssao);
      }
    }
    if (ssao) {
      ImGui::Indent();
      ImGui::TextDisabled("積む順は前のほう（にじみや色調整より前）");
      if (ImGui::SliderFloat("AO Radius", &ssaoRadius_, 0.05f, 3.0f, "%.2f m")) {
        SetSsaoRadius(ssaoRadius_);
      }
      if (ImGui::SliderFloat("AO Intensity", &ssaoIntensity_, 0.0f, 2.0f)) {
        SetSsaoIntensity(ssaoIntensity_);
      }
      if (ImGui::SliderFloat("AO Bias", &ssaoBias_, 0.0f, 0.2f, "%.3f m")) {
        SetSsaoBias(ssaoBias_);
      }
      if (ImGui::SliderFloat("AO Power", &ssaoPower_, 0.1f, 4.0f)) {
        SetSsaoPower(ssaoPower_);
      }
      ImGui::Unindent();
    }

    bool bloom = HasEffect(PostEffectType::Bloom);
    if (ImGui::Checkbox("Bloom", &bloom)) {
      if (bloom) {
        AddEffect(PostEffectType::Bloom);
      } else {
        RemoveEffect(PostEffectType::Bloom);
      }
    }
    if (bloom) {
      ImGui::Indent();
      if (ImGui::SliderFloat("Bloom Threshold", &bloomThreshold_, 0.0f, 1.0f)) {
        SetBloomThreshold(bloomThreshold_);
      }
      if (ImGui::SliderFloat("Bloom Intensity", &bloomIntensity_, 0.0f, 3.0f)) {
        SetBloomIntensity(bloomIntensity_);
      }
      if (ImGui::SliderFloat("Bloom Radius", &bloomRadius_, 1.0f, 16.0f, "%.1f px")) {
        SetBloomRadius(bloomRadius_);
      }
      if (ImGui::SliderFloat("Bloom Knee", &bloomKnee_, 0.01f, 1.0f)) {
        SetBloomKnee(bloomKnee_);
      }
      ImGui::Unindent();
    }

    bool colorGrade = HasEffect(PostEffectType::ColorGrade);
    if (ImGui::Checkbox("ColorGrade", &colorGrade)) {
      if (colorGrade) {
        AddEffect(PostEffectType::ColorGrade);
      } else {
        RemoveEffect(PostEffectType::ColorGrade);
      }
    }
    if (colorGrade) {
      ImGui::Indent();
      if (ImGui::SliderFloat("Exposure (EV)", &gradeExposure_, -3.0f, 3.0f)) {
        SetGradeExposure(gradeExposure_);
      }
      if (ImGui::SliderFloat("Contrast", &gradeContrast_, 0.5f, 2.0f)) {
        SetGradeContrast(gradeContrast_);
      }
      if (ImGui::SliderFloat("Saturation", &gradeSaturation_, 0.0f, 2.0f)) {
        SetGradeSaturation(gradeSaturation_);
      }
      if (ImGui::SliderFloat("Temperature", &gradeTemperature_, -1.0f, 1.0f)) {
        SetGradeTemperature(gradeTemperature_);
      }
      if (ImGui::SliderFloat("Tint", &gradeTint_, -1.0f, 1.0f)) {
        SetGradeTint(gradeTint_);
      }
      if (ImGui::ColorEdit3("Color Filter", gradeColorFilter_)) {
        SetGradeColorFilter(gradeColorFilter_[0], gradeColorFilter_[1], gradeColorFilter_[2]);
      }
      if (ImGui::SliderFloat("Grade Amount", &gradeLerpFactor_, 0.0f, 1.0f)) {
        SetGradeLerpFactor(gradeLerpFactor_);
      }
      ImGui::Unindent();
    }

    bool fxaa = HasEffect(PostEffectType::Fxaa);
    if (ImGui::Checkbox("FXAA", &fxaa)) {
      if (fxaa) {
        AddEffect(PostEffectType::Fxaa);
      } else {
        RemoveEffect(PostEffectType::Fxaa);
      }
    }
    if (fxaa) {
      ImGui::Indent();
      ImGui::TextDisabled("パラメータなし。積む順は一番最後が正しい");
      ImGui::Unindent();
    }

    bool maskOutline = HasEffect(PostEffectType::MaskOutline);
    if (ImGui::Checkbox("MaskOutline", &maskOutline)) {
      if (maskOutline) {
        AddEffect(PostEffectType::MaskOutline);
      } else {
        RemoveEffect(PostEffectType::MaskOutline);
      }
    }
    if (maskOutline) {
      ImGui::Indent();
      ImGui::TextDisabled(maskSrv_.ptr != 0 ? "Mask: ready" : "Mask: not drawn (pass-through)");
      if (ImGui::ColorEdit4("Mask Outline Color", maskOutlineColor_)) {
        SetMaskOutlineColor(maskOutlineColor_[0], maskOutlineColor_[1],
                            maskOutlineColor_[2], maskOutlineColor_[3]);
      }
      if (ImGui::SliderFloat("Mask Outline Thickness", &maskOutlineThickness_, 1.0f, 4.0f)) {
        SetMaskOutlineThickness(maskOutlineThickness_);
      }
      if (ImGui::SliderFloat("Mask Outline Strength", &maskOutlineStrength_, 0.0f, 1.0f)) {
        SetMaskOutlineStrength(maskOutlineStrength_);
      }
      ImGui::Unindent();
    }

    bool underwater = HasEffect(PostEffectType::Underwater);
    if (ImGui::Checkbox("Underwater", &underwater)) {
      if (underwater) {
        AddEffect(PostEffectType::Underwater);
      } else {
        RemoveEffect(PostEffectType::Underwater);
      }
    }
    if (underwater) {
      ImGui::Indent();
      if (ImGui::ColorEdit4("Tint Color", underwaterTintColor_)) {
        SetUnderwaterTintColor(underwaterTintColor_[0], underwaterTintColor_[1], underwaterTintColor_[2], underwaterTintColor_[3]);
      }
      if (ImGui::ColorEdit4("Fog Color", underwaterFogColor_)) {
        SetUnderwaterFogColor(underwaterFogColor_[0], underwaterFogColor_[1], underwaterFogColor_[2], underwaterFogColor_[3]);
      }
      if (ImGui::SliderFloat("Fog Start", &underwaterFogStart_, 0.0f, 50.0f)) {
        SetUnderwaterFogRange(underwaterFogStart_, underwaterFogEnd_);
      }
      if (ImGui::SliderFloat("Fog End", &underwaterFogEnd_, 10.0f, 300.0f)) {
        SetUnderwaterFogRange(underwaterFogStart_, underwaterFogEnd_);
      }
      if (ImGui::SliderFloat("Distortion Force", &underwaterDistortionForce_, 0.0f, 0.1f)) {
        SetUnderwaterDistortionForce(underwaterDistortionForce_);
      }
      if (ImGui::SliderFloat("Lerp Factor (Debug)", &underwaterLerpFactor_, 0.0f, 1.0f)) {
        SetUnderwaterLerpFactor(underwaterLerpFactor_);
      }
      ImGui::Unindent();
    }

    bool caustics = HasEffect(PostEffectType::Caustics);
    if (ImGui::Checkbox("Caustics", &caustics)) {
      if (caustics) {
        AddEffect(PostEffectType::Caustics);
      } else {
        RemoveEffect(PostEffectType::Caustics);
      }
    }
    if (caustics) {
      ImGui::Indent();

      if (ImGui::ColorEdit3("Caustics Color", causticsColor_)) {
        SetCausticsColor(causticsColor_[0], causticsColor_[1], causticsColor_[2]);
      }
      if (ImGui::SliderFloat("Caustics Intensity", &causticsIntensity_, 0.0f, 3.0f)) {
        SetCausticsIntensity(causticsIntensity_);
      }
      if (ImGui::SliderFloat("Caustics Scale", &causticsScale_, 0.02f, 2.0f)) {
        SetCausticsScale(causticsScale_);
      }
      if (ImGui::SliderFloat("Caustics Speed", &causticsSpeed_, 0.0f, 3.0f)) {
        SetCausticsSpeed(causticsSpeed_);
      }
      if (ImGui::SliderFloat("Caustics Contrast", &causticsContrast_, 1.0f, 12.0f)) {
        SetCausticsContrast(causticsContrast_);
      }
      if (ImGui::SliderFloat("Chromatic Offset", &causticsChromaticOffset_, 0.0f, 2.0f)) {
        SetCausticsChromaticOffset(causticsChromaticOffset_);
      }

      ImGui::Separator();

      if (ImGui::DragFloat("Water Height (Y)", &causticsWaterHeight_, 0.1f)) {
        SetCausticsWater(causticsWaterHeight_, causticsDepthFadeDistance_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "水面のワールドY座標。\n"
            "これより上のピクセルには caustics が落ちない。\n"
            "※何も見えない場合はまずこの値を疑うこと\n"
            "  （床が Water Height より上にあると全く描かれない）");
      }
      if (ImGui::SliderFloat("Depth Fade Distance", &causticsDepthFadeDistance_, 1.0f, 500.0f)) {
        SetCausticsWater(causticsWaterHeight_, causticsDepthFadeDistance_);
      }
      if (ImGui::SliderFloat("Upward Bias", &causticsUpwardBias_, 0.0f, 1.0f)) {
        SetCausticsUpwardBias(causticsUpwardBias_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "1.0 = 上向きの面にだけ落ちる（床）\n"
            "0.0 = 面の向きを無視して全面に落ちる\n"
            "壁面で模様が伸びる場合は上げる");
      }
      if (ImGui::SliderFloat("Dist Fade Start", &causticsDistanceFadeStart_, 0.0f, 600.0f)) {
        SetCausticsDistanceFade(causticsDistanceFadeStart_, causticsDistanceFadeEnd_);
      }
      if (ImGui::SliderFloat("Dist Fade End", &causticsDistanceFadeEnd_, 10.0f, 600.0f)) {
        SetCausticsDistanceFade(causticsDistanceFadeStart_, causticsDistanceFadeEnd_);
      }
      if (ImGui::SliderFloat("Caustics Lerp Factor (Debug)", &causticsLerpFactor_, 0.0f, 1.0f)) {
        SetCausticsLerpFactor(causticsLerpFactor_);
      }

      ImGui::Unindent();
    }

    bool lightShaft = HasEffect(PostEffectType::LightShaft);
    if (ImGui::Checkbox("LightShaft", &lightShaft)) {
      if (lightShaft) {
        AddEffect(PostEffectType::LightShaft);
      } else {
        RemoveEffect(PostEffectType::LightShaft);
      }
    }
    if (lightShaft) {
      ImGui::Indent();

      if (ImGui::ColorEdit3("Shaft Color", lightShaftColor_)) {
        SetLightShaftColor(lightShaftColor_[0], lightShaftColor_[1], lightShaftColor_[2]);
      }
      if (ImGui::SliderFloat("Shaft Intensity", &lightShaftIntensity_, 0.0f, 3.0f)) {
        SetLightShaftIntensity(lightShaftIntensity_);
      }
      if (ImGui::SliderFloat("Shaft Density", &lightShaftDensity_, 0.0f, 0.3f)) {
        SetLightShaftDensity(lightShaftDensity_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "深さによる指数減衰。\n"
            "大きくすると浅い層だけが光り、光柱が短くなる。");
      }
      if (ImGui::SliderFloat("Shaft Contrast", &lightShaftContrast_, 1.0f, 8.0f)) {
        SetLightShaftContrast(lightShaftContrast_);
      }

      ImGui::Separator();

      // Caustics と共有するパラメータ。LightShaft 単体で有効にしたときにも
      // ここから触れるようにしておく（触れないと水面高さを変更できない）。
      ImGui::TextDisabled("以下は Caustics と共有");
      if (ImGui::DragFloat("Water Height (Y)##shaft", &causticsWaterHeight_, 0.1f)) {
        SetCausticsWater(causticsWaterHeight_, causticsDepthFadeDistance_);
      }
      if (ImGui::SliderFloat("Pattern Scale##shaft", &causticsScale_, 0.02f, 2.0f)) {
        SetCausticsScale(causticsScale_);
      }
      if (ImGui::SliderFloat("Pattern Speed##shaft", &causticsSpeed_, 0.0f, 3.0f)) {
        SetCausticsSpeed(causticsSpeed_);
      }

      ImGui::Separator();

      if (ImGui::SliderInt("Sample Count", &lightShaftSampleCount_, 4, 64)) {
        SetLightShaftSampleCount(lightShaftSampleCount_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(!)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "レイマーチのステップ数。\n"
            "このエフェクトのコストはほぼこの値に比例する。\n"
            "FPS が落ちる場合はまずここを下げること。");
      }
      if (ImGui::SliderFloat("Shaft Max Distance", &lightShaftMaxDistance_, 10.0f, 400.0f)) {
        SetLightShaftMaxDistance(lightShaftMaxDistance_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "レイマーチの打ち切り距離。明るさには影響しない（コストだけ）。\n"
            "カメラの Far Clip より大きくしても効果は伸びない。");
      }
      if (ImGui::SliderFloat("Dither Strength", &lightShaftDitherStrength_, 0.0f, 1.0f)) {
        SetLightShaftDitherStrength(lightShaftDitherStrength_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 にするとステップの縞（バンディング）が見える。\n"
            "Sample Count を下げたときは 1.0 のままにしておくこと。");
      }
      if (ImGui::SliderFloat("Shaft Lerp Factor (Debug)", &lightShaftLerpFactor_, 0.0f, 1.0f)) {
        SetLightShaftLerpFactor(lightShaftLerpFactor_);
      }

      ImGui::Unindent();
    }

    bool screenDroplets = HasEffect(PostEffectType::ScreenDroplets);
    if (ImGui::Checkbox("ScreenDroplets (レンズ水滴)", &screenDroplets)) {
      if (screenDroplets) {
        AddEffect(PostEffectType::ScreenDroplets);
      } else {
        RemoveEffect(PostEffectType::ScreenDroplets);
      }
    }
    if (screenDroplets) {
      ImGui::Indent();
      if (ImGui::SliderFloat("Intensity (水滴の強さ)", &screenDropletsIntensity_, 0.0f, 1.0f)) {
        SetScreenDropletsIntensity(screenDropletsIntensity_);
      }
      if (ImGui::SliderFloat("Speed (流れる速度)", &screenDropletsSpeed_, 0.0f, 5.0f)) {
        SetScreenDropletsSpeed(screenDropletsSpeed_);
      }
      if (ImGui::SliderFloat("Distortion (屈折の強さ)", &screenDropletsDistortion_, 0.0f, 0.2f)) {
        SetScreenDropletsDistortion(screenDropletsDistortion_);
      }
      if (ImGui::SliderFloat("Scale (水滴の密度)", &screenDropletsScale_, 0.5f, 5.0f)) {
        SetScreenDropletsScale(screenDropletsScale_);
      }
      ImGui::Unindent();
    }

    bool bloodOverlay = HasEffect(PostEffectType::BloodOverlay);
    if (ImGui::Checkbox("BloodOverlay (被弾の血)", &bloodOverlay)) {
      if (bloodOverlay) {
        AddEffect(PostEffectType::BloodOverlay);
      } else {
        RemoveEffect(PostEffectType::BloodOverlay);
      }
    }
    if (bloodOverlay) {
      ImGui::Indent();
      ImGui::TextDisabled("ゲーム中は ScreenFx が毎フレーム上書きします");
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Hit Flash / Low Health は Player の被弾処理から駆動される。\n"
            "ここで動かして見た目を確認できるのは、ステージ外（ScreenFx が\n"
            "触っていない場面）か、値を掴んで離すまでの一瞬だけ。\n"
            "色・侵食量・飛沫密度は上書きされないので、調整はそちらで行うこと。");
      }
      if (ImGui::SliderFloat("Hit Flash (被弾フラッシュ)", &bloodHitFlash_, 0.0f, 1.0f)) {
        SetBloodOverlayHitFlash(bloodHitFlash_);
      }
      if (ImGui::SliderFloat("Low Health (低HP持続)", &bloodLowHealth_, 0.0f, 1.0f)) {
        SetBloodOverlayLowHealth(bloodLowHealth_);
      }
      if (ImGui::ColorEdit3("Blood Color (血の色)", bloodColor_)) {
        SetBloodOverlayColor(bloodColor_[0], bloodColor_[1], bloodColor_[2]);
      }
      if (ImGui::SliderFloat("Coverage (侵食する割合)", &bloodCoverage_, 0.0f, 1.0f)) {
        SetBloodOverlayCoverage(bloodCoverage_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "最大時に画面の何割まで血が内側へ入ってくるか。\n"
            "0.8 を超えると視界が塞がってプレイに支障が出る。");
      }
      if (ImGui::SliderFloat("Splatter Scale (飛沫の密度)", &bloodSplatterScale_, 0.4f, 4.0f)) {
        SetBloodOverlaySplatterScale(bloodSplatterScale_);
      }
      if (ImGui::SliderFloat("Desaturate (彩度の低下)", &bloodDesaturate_, 0.0f, 1.0f)) {
        SetBloodOverlayDesaturate(bloodDesaturate_);
      }
      if (ImGui::SliderFloat("Pulse Speed (脈動の速さ)", &bloodPulseSpeed_, 0.3f, 3.0f)) {
        SetBloodOverlayPulseSpeed(bloodPulseSpeed_);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("1.0 で毎秒1拍。瀕死の焦りを出すなら 1.2〜1.6 あたり。");
      }
      if (ImGui::Button("Reroll Splatter (飛沫を振り直す)")) {
        RerollBloodOverlaySplatter();
      }
      ImGui::Unindent();
    }

    bool inkOverlay = HasEffect(PostEffectType::InkOverlay);
    if (ImGui::Checkbox("InkOverlay (タコの墨)", &inkOverlay)) {
      if (inkOverlay) {
        AddEffect(PostEffectType::InkOverlay);
      } else {
        RemoveEffect(PostEffectType::InkOverlay);
      }
    }
    if (inkOverlay) {
      ImGui::Indent();
      ImGui::TextDisabled("ゲーム側のスクリプトが毎フレーム上書きする場合があります");
      ImGui::Text("Splats: %d / %d", inkSplatCount_, kMaxInkSplats);
      bool dirty = false;
      dirty |= ImGui::SliderFloat("Test Strength (強さ)", &inkDebugStrength_, 0.0f, 1.0f);
      dirty |= ImGui::SliderFloat("Test Age (経過秒・垂れ)", &inkDebugAge_, 0.0f, 6.0f);
      dirty |= ImGui::SliderFloat("Test Radius (大きさ)", &inkDebugRadius_, 0.05f, 0.8f);
      dirty |= ImGui::SliderFloat("Test Seed (形)", &inkDebugSeed_, 0.0f, 1.0f);
      if (ImGui::Button("Show Test Splat (中央に1つ貼る)") || (dirty && inkSplatCount_ == 1)) {
        InkSplat s{};
        s.radius = inkDebugRadius_;
        s.strength = inkDebugStrength_;
        s.age = inkDebugAge_;
        s.seed = inkDebugSeed_;
        SetInkOverlaySplats(&s, 1);
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear")) {
        SetInkOverlaySplats(nullptr, 0);
      }
      if (ImGui::SliderFloat("Murk (画面の濁り)", &inkMurk_, 0.0f, 1.0f)) {
        SetInkOverlayMurk(inkMurk_);
      }
      if (ImGui::ColorEdit3("Ink Color (墨の色)", inkColor_)) {
        SetInkOverlayColor(inkColor_[0], inkColor_[1], inkColor_[2], inkColor_[3]);
      }
      if (ImGui::SliderFloat("Ink Opacity (不透明度)", &inkColor_[3], 0.0f, 1.0f)) {
        SetInkOverlayColor(inkColor_[0], inkColor_[1], inkColor_[2], inkColor_[3]);
      }
      ImGui::Unindent();
    }

    // ------------------------------------------------------------------
    // 適用順（結果に影響するので並び替えできるようにしておく）
    // ------------------------------------------------------------------
    ImGui::Separator();

    const auto &effects = GetEffects();
    if (effects.empty()) {
      ImGui::TextDisabled("Active: None");
    } else {
      ImGui::Text("Apply Order (%zu effect(s))", effects.size());
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "上から順に適用される。\n"
            "Underwater は UV を歪めるため、Caustics / LightShaft より\n"
            "後ろ（下）に置かないと模様の位置がジオメトリとズレる。");
      }

      ImGui::Indent();
      // 並び替えはリストを書き換えるので、この描画フレーム中は1回だけ行う
      int moveFrom = -1;
      int moveOffset = 0;
      for (size_t i = 0; i < effects.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
          moveFrom = static_cast<int>(i);
          moveOffset = -1;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
          moveFrom = static_cast<int>(i);
          moveOffset = 1;
        }
        ImGui::SameLine();
        ImGui::Text("%zu. %s", i + 1, ToString(effects[i]));

        ImGui::PopID();
      }
      if (moveFrom >= 0) {
        MoveEffect(static_cast<size_t>(moveFrom), moveOffset);
      }
      ImGui::Unindent();

      // 順序ミスの警告
      auto indexOf = [&effects](PostEffectType type) -> int {
        for (size_t i = 0; i < effects.size(); ++i) {
          if (effects[i] == type) return static_cast<int>(i);
        }
        return -1;
      };
      const int idxUnderwater = indexOf(PostEffectType::Underwater);
      const int idxCaustics = indexOf(PostEffectType::Caustics);
      const int idxLightShaft = indexOf(PostEffectType::LightShaft);
      if (idxUnderwater >= 0 &&
          ((idxCaustics >= 0 && idxCaustics > idxUnderwater) ||
           (idxLightShaft >= 0 && idxLightShaft > idxUnderwater))) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "警告: Underwater を Caustics / LightShaft より後ろに\n"
                           "移動してください（模様の位置がズレています）");
      }
    }
  }
#endif
}
