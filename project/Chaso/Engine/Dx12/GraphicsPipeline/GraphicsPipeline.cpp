#include "GraphicsPipeline.h"
#include "Common/Log/Log.h"
#include <d3d12sdklayers.h>

void GraphicsPipeline::Term() {
  // ====================
  // Resource Release
  // ====================
  // PSO 解放
  pso_.Reset();
  // ルートシグネチャ解放
  root_.Reset();
  // デバイス参照をクリア
  device_.Reset();
}

void GraphicsPipeline::BuildEx(const D3D12_INPUT_ELEMENT_DESC *inputElems,
                               UINT elemCount, D3D12_SHADER_BYTECODE vs,
                               D3D12_SHADER_BYTECODE ps, DXGI_FORMAT rtvFmt,
                               DXGI_FORMAT dsvFmt,
                               const GPipelineOptions &opt,
                               const D3D12_CACHED_PIPELINE_STATE &cachedPSO) {
  // ====================
  // Root Signature
  // ====================
  // ルートシグネチャ構築
  buildRootSignature_(opt.rootType);

  // ====================
  // Pipeline State
  // ====================
  // PSO 作成
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = root_.Get();
  d.VS = vs.BytecodeLength > 0 ? vs : D3D12_SHADER_BYTECODE{nullptr, 0};
  d.PS = ps.BytecodeLength > 0 ? ps : D3D12_SHADER_BYTECODE{nullptr, 0};

  // 入力レイアウト
  D3D12_INPUT_LAYOUT_DESC ild{};
  ild.pInputElementDescs = inputElems;
  ild.NumElements = elemCount;
  d.InputLayout = ild;

  // ラスタライザ
  D3D12_RASTERIZER_DESC rs{};
  rs.FillMode = opt.fill;
  rs.CullMode = opt.cull;
  rs.FrontCounterClockwise = FALSE;
  rs.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
  rs.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
  rs.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
  rs.DepthClipEnable = TRUE;
  rs.MultisampleEnable = FALSE;
  rs.AntialiasedLineEnable = FALSE;
  d.RasterizerState = rs;

  // ブレンド
  D3D12_BLEND_DESC b{};
  b.AlphaToCoverageEnable = FALSE;
  b.IndependentBlendEnable = FALSE;
  auto &rt = b.RenderTarget[0];
  rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  BlendMode m = opt.blendMode;
  if (opt.enableAlphaBlend && m == kBlendModeNone)
    m = kBlendModeNormal;

  if (m == kBlendModeNone) {
    rt.BlendEnable = FALSE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  } else {
    rt.BlendEnable = TRUE;
    switch (m) {
    case kBlendModeNormal: // Src * SrcA + Dest * (1 - SrcA)
      rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      break;
    case kBlendModeAdd: // Src * SrcA + Dest * 1
      rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      rt.DestBlend = D3D12_BLEND_ONE;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      break;
    case kBlendModeSubtract: // Dest * 1 - Src * SrcA
      rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      rt.DestBlend = D3D12_BLEND_ONE;
      rt.BlendOp = D3D12_BLEND_OP_REV_SUBTRACT; // D - S
      break;
    case kBlendModeMultiply: // Src * 0 + Dest * Src
      rt.SrcBlend = D3D12_BLEND_ZERO;
      rt.DestBlend = D3D12_BLEND_SRC_COLOR;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      break;
    case kBlendModeScreen: // Src * (1 - Dest) + Dest * 1
      rt.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
      rt.DestBlend = D3D12_BLEND_ONE;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      break;
    case kBlendModePremultiplied: // Src * 1 + Dest * (1 - SrcA)
      rt.SrcBlend = D3D12_BLEND_ONE;
      rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      break;
    default:
      break;
    }
    // アルファのブレンドは基本「保持」寄りに
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = (m == kBlendModeSubtract) ? D3D12_BLEND_OP_REV_SUBTRACT
                                                : D3D12_BLEND_OP_ADD;
  }
  d.BlendState = b;

  // 深度ステンシル
  D3D12_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = opt.enableDepth ? TRUE : FALSE;

  // 深度テストするかどうか と、書き込むかどうかを分ける
  if (opt.enableDepth && opt.enableDepthWrite) {
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  } else {
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  }

  ds.DepthFunc = opt.enableDepth ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                 : D3D12_COMPARISON_FUNC_ALWAYS;
  ds.StencilEnable = FALSE;

  d.DepthStencilState = ds;
  d.DSVFormat = opt.enableDepth ? (opt.dsvFormatOverride != DXGI_FORMAT_UNKNOWN ? opt.dsvFormatOverride : dsvFmt) : DXGI_FORMAT_UNKNOWN;

  // RT
  if (opt.disableRTV) {
      d.NumRenderTargets = 0;
      d.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
  } else {
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = rtvFmt;
  }

  d.PrimitiveTopologyType = opt.topologyType;
  d.SampleDesc.Count = 1;
  d.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

  // キャッシュPSO設定
  d.CachedPSO = cachedPSO;

  // -------------------------------------------------------------
  // D3D12 Debug Layer のエラーブレークを一時的に無効化する
  // (キャッシュ不一致による強制終了を回避してフォールバックさせるため)
  // -------------------------------------------------------------
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
  bool pushedFilter = false;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
    D3D12_MESSAGE_ID denyIds[] = {D3D12_MESSAGE_ID_CREATEPIPELINESTATE_CACHEDBLOBDESCMISMATCH};
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumIDs = 1;
    filter.DenyList.pIDList = denyIds;
    if (SUCCEEDED(infoQueue->PushStorageFilter(&filter))) {
      pushedFilter = true;
    }
  }

  HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_));

  // フィルター解除
  if (pushedFilter && infoQueue) {
    infoQueue->PopStorageFilter();
  }

  isCacheFallback_ = false;

  // キャッシュ付きで失敗した場合：キャッシュを破棄してリトライ
  if (FAILED(hr) && cachedPSO.pCachedBlob != nullptr) {
    static bool sAlreadyLogged = false;
    if (!sAlreadyLogged) {
      Log::Print(
          "[PipelineManager] PSOキャッシュ不一致を検出 → キャッシュを破棄して再構築します");
      Log::Print(
          "  ※ ルートシグネチャやシェーダーを変更した場合に発生します。以降の不一致ログは省略します。");
      sAlreadyLogged = true;
    }

    d.CachedPSO = {}; // キャッシュなし
    pso_.Reset();
    hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_));
    isCacheFallback_ = true;
  }

  assert(SUCCEEDED(hr));
}

Microsoft::WRL::ComPtr<ID3DBlob> GraphicsPipeline::GetSerializedBlob() const {
  if (!pso_)
    return nullptr;
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  pso_->GetCachedBlob(blob.GetAddressOf());
  return blob;
}

void GraphicsPipeline::buildRootSignature_(RootSignatureType type) {
  // ====================
  // Root Signature
  // ====================
  // 既存ルートシグネチャ解放
  root_.Reset();

  D3D12_ROOT_PARAMETER params[16] = {};
  D3D12_DESCRIPTOR_RANGE ranges[8] = {}; // t0(Tex), t1(EnvMap), t1(Depth), t1(SkinMat)等
  UINT paramCount = 0;

  switch (type) {
  case RootSignatureType::Object3D:
    // 0: CBV b0 (PS)  Material
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    // 1: CBV b0 (VS)  Transform
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].Descriptor.ShaderRegister = 0;

    // 2: SRV table t0 (PS)  Texture
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 3: CBV b1 (PS)  DirectionalLight
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].Descriptor.ShaderRegister = 1;

    // 4: CBV b2 (PS)  camera
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].Descriptor.ShaderRegister = 2;

    // 5: CBV b3 (PS) PointLight
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].Descriptor.ShaderRegister = 3;

    // 6: CBV b4 (PS) SpotLight
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].Descriptor.ShaderRegister = 4;

    // 7: CBV b5 (PS) AreaLight
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].Descriptor.ShaderRegister = 5;

    // 8: SRV table t1 (PS) EnvironmentMap (Cubemap)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].BaseShaderRegister = 1; // t1
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &ranges[1];

    // 9: SRV table t2 (PS) NormalMap
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 2; // t2
    ranges[2].NumDescriptors = 1;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &ranges[2];

    // 10: SRV table t3 (PS) RoughnessMap
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 3; // t3
    ranges[3].NumDescriptors = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &ranges[3];

    // 11: SRV table t4 (PS) ShadowMap
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].BaseShaderRegister = 4; // t4
    ranges[4].NumDescriptors = 1;
    ranges[4].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &ranges[4];

    // 12: CBV b6 (ALL) ShadowParams
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[12].Descriptor.ShaderRegister = 6; // b6

    // 13: SRV table t5 (PS) SpotShadowAtlas（スポットライト影のアトラス）
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].BaseShaderRegister = 5; // t5
    ranges[5].NumDescriptors = 1;
    ranges[5].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[13].DescriptorTable.NumDescriptorRanges = 1;
    params[13].DescriptorTable.pDescriptorRanges = &ranges[5];

    // 14: CBV b7 (PS) SpotShadowCB
    params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[14].Descriptor.ShaderRegister = 7; // b7

    paramCount = 15;
    break;

  case RootSignatureType::Object3DInstancing:
    // 0: CBV b0 (PS) Material
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    // 1: SRV t1 (VS) Instance buffer（StructuredBuffer）
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].Descriptor.ShaderRegister = 1;

    // 2: SRV table t0 (PS) Texture
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 3..7 は Object3D と同じ（Directional/Camera/Point/Spot/Area）
    // 3: CBV b1 (PS) DirectionalLight
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].Descriptor.ShaderRegister = 1;

    // 4: CBV b2 (PS) camera
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].Descriptor.ShaderRegister = 2;

    // 5: CBV b3 (PS) PointLight
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].Descriptor.ShaderRegister = 3;

    // 6: CBV b4 (PS) SpotLight
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].Descriptor.ShaderRegister = 4;

    // 7: CBV b5 (PS) AreaLight
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].Descriptor.ShaderRegister = 5;

    // 8: SRV table t1 (PS) EnvironmentMap (Cubemap)
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 1; // t1
    ranges[2].NumDescriptors = 1;
    ranges[2].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &ranges[2];

    // 9: SRV table t2 (PS) NormalMap
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 2; // t2
    ranges[3].NumDescriptors = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &ranges[3];

    // 10: SRV table t3 (PS) RoughnessMap
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].BaseShaderRegister = 3; // t3
    ranges[4].NumDescriptors = 1;
    ranges[4].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &ranges[4];

    // 11: SRV table t4 (PS) ShadowMap
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].BaseShaderRegister = 4; // t4
    ranges[5].NumDescriptors = 1;
    ranges[5].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &ranges[5];

    // 12: CBV b6 (ALL) ShadowParams
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[12].Descriptor.ShaderRegister = 6; // b6

    // 13: SRV table t5 (PS) SpotShadowAtlas（スポットライト影のアトラス）
    ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[6].BaseShaderRegister = 5; // t5
    ranges[6].NumDescriptors = 1;
    ranges[6].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[13].DescriptorTable.NumDescriptorRanges = 1;
    params[13].DescriptorTable.pDescriptorRanges = &ranges[6];

    // 14: CBV b7 (PS) SpotShadowCB
    params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[14].Descriptor.ShaderRegister = 7; // b7

    paramCount = 15;
    break;

  case RootSignatureType::Object3DSkin:
    // Object3D と同じ slot 0-8 に加え、VS用 SRV t1 (行列パレット) を追加
    // 0: CBV b0 (PS) Material
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    // 1: CBV b0 (VS) Transform
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].Descriptor.ShaderRegister = 0;

    // 2: SRV table t0 (PS) Texture
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 3: CBV b1 (PS) DirectionalLight
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].Descriptor.ShaderRegister = 1;

    // 4: CBV b2 (PS) camera
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].Descriptor.ShaderRegister = 2;

    // 5: CBV b3 (PS) PointLight
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].Descriptor.ShaderRegister = 3;

    // 6: CBV b4 (PS) SpotLight
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].Descriptor.ShaderRegister = 4;

    // 7: CBV b5 (PS) AreaLight
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].Descriptor.ShaderRegister = 5;

    // 8: SRV table t1 (PS) EnvironmentMap
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].BaseShaderRegister = 1; // t1
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &ranges[1];

    // 9: SRV table t2 (PS) NormalMap
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 2; // t2
    ranges[2].NumDescriptors = 1;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &ranges[2];

    // 10: SRV table t3 (PS) RoughnessMap
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 3; // t3
    ranges[3].NumDescriptors = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &ranges[3];

    // 11: SRV table t4 (PS) ShadowMap
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].BaseShaderRegister = 4; // t4
    ranges[4].NumDescriptors = 1;
    ranges[4].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &ranges[4];

    // 12: CBV b6 (ALL) ShadowParams
    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[12].Descriptor.ShaderRegister = 6; // b6

    // 13: SRV t1 (VS) SkinMatrices (StructuredBuffer<float4x4>)
    params[Object3DRootParam::kSkinMatrices].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[Object3DRootParam::kSkinMatrices].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[Object3DRootParam::kSkinMatrices].Descriptor.ShaderRegister = 1; // t1

    // 14: SRV table t5 (PS) SpotShadowAtlas（スポットライト影のアトラス）
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].BaseShaderRegister = 5; // t5
    ranges[5].NumDescriptors = 1;
    ranges[5].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[14].DescriptorTable.NumDescriptorRanges = 1;
    params[14].DescriptorTable.pDescriptorRanges = &ranges[5];

    // 15: CBV b7 (PS) SpotShadowCB
    params[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[15].Descriptor.ShaderRegister = 7; // b7

    paramCount = 16;
    break;

  case RootSignatureType::Sprite:
    // 0: CBV b0 (PS)  Material
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    // 1: CBV b0 (VS)  Transform
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].Descriptor.ShaderRegister = 0;

    // 2: SRV table t0 (PS)  Texture
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[0];

    paramCount = 3;
    break;

  case RootSignatureType::FogOverlay:
    // 0: CBV b0 (PS) Fog parameters
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    paramCount = 1;
    break;



  case RootSignatureType::Primitive3D:
    // 0: CBV b0 (VS) View/Proj
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].Descriptor.ShaderRegister = 0;
    paramCount = 1;
    break;
  case RootSignatureType::PostProcess:
    // 0: SRV table t0 (PS) Texture
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 1: Root32BitConstants b0 (PS) PostEffect params (4 x uint32)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Constants.ShaderRegister = 0; // b0
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 4;

    // 2: SRV table t1 (PS) Depth Texture
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 1; // t1
    ranges[3].NumDescriptors = 1;
    ranges[3].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[3];

    // 3: CBV b1 (PS) ProjectionInverse / Material
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].Descriptor.ShaderRegister = 1; // b1

    paramCount = 4;
    break;

  case RootSignatureType::GPUParticle:
    // GPU Particle 描画用ルートシグネチャ
    // 0: CBV b0 (VS) PerView (viewProjection + billboardMatrix)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].Descriptor.ShaderRegister = 0; // b0

    // 1: SRV table t0 (VS) Particles StructuredBuffer
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0; // t0
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 2: SRV table t0 (PS) Texture
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].BaseShaderRegister = 0; // t0 (PS)
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];

    paramCount = 3;
    break;

  case RootSignatureType::Water:
    // Object3D と同じ 0..10 に加え、b6 (ALL) に WaterParams を追加
    // 0: CBV b0 (PS)  Material
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    // 1: CBV b0 (VS)  Transform
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].Descriptor.ShaderRegister = 0;

    // 2: SRV table t0 (PS)  Texture (Normal Map)
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[0];

    // 3: CBV b1 (PS)  DirectionalLight
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].Descriptor.ShaderRegister = 1;

    // 4: CBV b2 (PS)  camera
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].Descriptor.ShaderRegister = 2;

    // 5: CBV b3 (PS) PointLight
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].Descriptor.ShaderRegister = 3;

    // 6: CBV b4 (PS) SpotLight
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].Descriptor.ShaderRegister = 4;

    // 7: CBV b5 (PS) AreaLight
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7].Descriptor.ShaderRegister = 5;

    // 8: SRV table t1 (PS) EnvironmentMap (Cubemap)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].BaseShaderRegister = 1; // t1
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &ranges[1];

    // 9: SRV table t2 (PS) NormalMap
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 2; // t2
    ranges[2].NumDescriptors = 1;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &ranges[2];

    // 10: SRV table t3 (PS) RoughnessMap
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 3; // t3
    ranges[3].NumDescriptors = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &ranges[3];

    // 11: CBV b6 (ALL)  WaterParams (VS+PS で共有)
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[11].Descriptor.ShaderRegister = 6;

    // 12: SRV table t4 (VS, PS) InteractiveWave HeightMap
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].BaseShaderRegister = 4; // t4
    ranges[4].NumDescriptors = 1;
    ranges[4].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[12].DescriptorTable.NumDescriptorRanges = 1;
    params[12].DescriptorTable.pDescriptorRanges = &ranges[4];

    // 13: SRV table t5 (PS) Depth Texture for Foam
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].BaseShaderRegister = 5; // t5
    ranges[5].NumDescriptors = 1;
    ranges[5].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[13].DescriptorTable.NumDescriptorRanges = 1;
    params[13].DescriptorTable.pDescriptorRanges = &ranges[5];

    // 14: SRV table t6 (PS) Scene Color（水を描く前の画面のコピー。屈折用）
    ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[6].BaseShaderRegister = 6; // t6
    ranges[6].NumDescriptors = 1;
    ranges[6].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[14].DescriptorTable.NumDescriptorRanges = 1;
    params[14].DescriptorTable.pDescriptorRanges = &ranges[6];

    paramCount = 15;
    break;

  default:
    // Compute 用のルートシグネチャは PipelineManager::CreateCompute で作成する
    assert(false && "Compute RootSignatureType is not supported by GraphicsPipeline");
    break;
  }

  // ====================
  // Static Sampler
  // ====================
  D3D12_STATIC_SAMPLER_DESC samplers[4] = {};

  // s0: Linear
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[0].ShaderRegister = 0;
  samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s1: Point (for depth sampling etc)
  samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[1].ShaderRegister = 1;
  samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s2: Linear Clamp
  samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[2].ShaderRegister = 2;
  samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // s3: Shadow Comparison (PCF用)
  // 深度比較つきバイリニアフィルタ。1タップで 2x2 の比較結果が補間されるため、
  // シェーダ側の 3x3 タップと合わせて実質 6x6 相当の滑らかさになる。
  samplers[3].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  samplers[3].AddressU = samplers[3].AddressV = samplers[3].AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[3].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[3].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
  samplers[3].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[3].ShaderRegister = 3;
  samplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // ====================
  // Serialize
  // ====================
  // ルートシグネチャ作成
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  desc.pParameters = params;
  desc.NumParameters = paramCount;
  desc.pStaticSamplers = samplers;
  desc.NumStaticSamplers = 4u;

  Microsoft::WRL::ComPtr<ID3DBlob> sig;
  Microsoft::WRL::ComPtr<ID3DBlob> err;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           sig.GetAddressOf(), err.GetAddressOf());

  if (FAILED(hr)) {
    if (err) {
      OutputDebugStringA((char *)err->GetBufferPointer());
    }
    return; // ここで抜ける（Release でもクラッシュしない）
  }

  hr = device_->CreateRootSignature(0, sig->GetBufferPointer(),
                                    sig->GetBufferSize(), IID_PPV_ARGS(&root_));
  assert(SUCCEEDED(hr));
  root_->SetName(L"GraphicsPipeline::RootSignature");
}
