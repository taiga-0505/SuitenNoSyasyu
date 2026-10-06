// Water.VS.hlsl - Vertex Shader for ocean/water surface
// Uses Gerstner Waves for realistic vertex displacement

#include "Water.hlsli"

// b0: WVP 行列（VS用）
struct TransformationMatrix
{
    float4x4 WVP;
    float4x4 World;
    float4x4 worldInverseTranspose;
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

// b6: 水面パラメータ (PS と共有、VS でも波の計算に使用)
cbuffer WaterParams : register(b6)
{
    float  gTime;         // 経過時間 (秒)
    float  gWaveHeight;   // 波の高さスケール
    float  gWaveSpeed;    // 波の速度スケール
    float  gWaveFreq;     // 波の周波数

    float  gWaveHeight2;  // 第2波の高さ
    float  gWaveSpeed2;   // 第2波の速度
    float  gWaveFreq2;    // 第2波の周波数
    float  gWaveSteepness; // Gerstner 鋭さ (0..1)

    float4 gWaterShallowColor;  // 浅瀬の色
    float4 gWaterDeepColor;     // 深海の色

    float  gFresnelPower;       // フレネル指数
    float  gSpecularPower;      // スペキュラ指数
    float  gNormalScrollSpeed;  // 法線マップスクロール速度
    float  gNormalStrength;     // 法線マップ強度

    float4 gInvScreenSize;
    float4 gCameraNearFar; // x: near, y: far
    float4 gFoamParams;    // x: foamDepth, y: foamScale
    float4 gFoamColor;

    float4 gObstacles[4];  // xyz: pos, w: radius
    float4 gObstacleCount; // x: count, y: 反射の強さ, z: 反射の到達範囲(半径倍率)

    float4 gOceanParams;   // x: 詳細波の強さ, y: 尖り(choppy), z: 白波の強さ, w: 頂点で変位させる最短波長(m)
    float4 gOceanParams2;  // x: 透明度(m), y: 遠景フェード距離(m), z: 法線マップのタイルサイズ(m, 0=従来UV), w: 白波の量
    float4 gSssColor;      // rgb: 波頭を透ける光の色, a: 強さ
    float4 gRefractParams; // x: 屈折が有効(1/0), y: 歪みの強さ(画面UV), z: 交差部のフェード幅(m)
};

#include "OceanWaves.hlsli"

Texture2D<float4> gInteractiveWave : register(t4); // r: 高さ, g: 引き波の泡, b: 波頭の白波（VS では高さだけ使う）
SamplerState gSamplerClamp : register(s2);

struct VertexShaderInput
{
    float4 position : POSITION0;
    float2 texcoord : TEXCOORD0;
    float3 normal   : NORMAL0;
};

// =====================================================
// Gerstner Wave
// =====================================================
// 3方向の平面波を重ね合わせて海面を作り、
// さらに障害物ごとの「反射波」を鏡像法で足し込む。
struct GerstnerResult
{
    float3 offset;
    float3 tangent;
    float3 binormal;
};

// 1つの平面波の定義
//
// ※ 位相を dot(dir, x) * freq + timePhase と組み立てるとき、
//    位相が一定になる点は timePhase が減るほど dir 方向へ移動する。
//    したがって timePhase に -speed * time を入れると波は +dir へ進む。
//    以前は +speed * time だったため dir と実際の進行方向が逆で、
//    反射波が「波が来る側」ではなく反対側に出てしまっていた。
struct WaveDef
{
    float2 dir;       // 波が実際に進む向き（正規化済み）
    float  freq;      // 空間周波数
    float  timePhase; // 時間項 (-speed * time)
    float  steepness; // Gerstner の鋭さ
    float  amp;       // 振幅
};

static const int kWaveCount    = 3; // 重ね合わせる平面波の数
static const int kMaxObstacles = 4; // CB が保持できる障害物の最大数

WaveDef GetWaveDef(int index, float time)
{
    WaveDef w;
    if (index == 0) {
        // 主波 (X方向寄り)
        w.dir       = normalize(float2(0.7, 0.5));
        w.freq      = gWaveFreq;
        w.timePhase = -gWaveSpeed * time;
        w.steepness = gWaveSteepness;
        w.amp       = gWaveHeight;
    } else if (index == 1) {
        // 副波 (Z方向寄り, 位相をずらす)
        w.dir       = normalize(float2(0.3, 0.8));
        w.freq      = gWaveFreq2;
        w.timePhase = -gWaveSpeed2 * time;
        w.steepness = gWaveSteepness * 0.8;
        w.amp       = gWaveHeight2;
    } else {
        // 微細波（高周波のディテール）
        w.dir       = normalize(float2(-0.4, 0.6));
        w.freq      = gWaveFreq * 2.5;
        w.timePhase = -gWaveSpeed * 1.3 * time;
        w.steepness = gWaveSteepness * 0.3;
        w.amp       = gWaveHeight * 0.25;
    }
    return w;
}

// 平面波を1成分ぶん Gerstner として加算する
// 入射波・反射波のどちらも同じ式を通すため、反射波も横方向変位と法線への寄与を持つ
void AccumulateGerstner(inout GerstnerResult result, float2 dir, float freq,
                        float phase, float steepness, float amp)
{
    float s = sin(phase);
    float c = cos(phase);

    result.offset.x += steepness * amp * dir.x * c;
    result.offset.z += steepness * amp * dir.y * c;
    result.offset.y += amp * s;

    // 接線/従法線への影響（交差項 dx*dz も入れる。斜めの波で法線が正しく傾くように）
    float wa = steepness * freq * amp * s;
    result.tangent.x  -= wa * dir.x * dir.x;
    result.tangent.y  += dir.x * freq * amp * c;
    result.tangent.z  -= wa * dir.x * dir.y;
    result.binormal.x -= wa * dir.x * dir.y;
    result.binormal.z -= wa * dir.y * dir.y;
    result.binormal.y += dir.y * freq * amp * c;
}

// 詳細波（OceanWaves.hlsli）の変位だけを足す。
// 法線は PS でピクセルごとに求めるので、ここでは接線に入れない。
// 頂点間隔より短い波は表現できず折り返しノイズになるため、minWavelength 未満はフェードアウトさせる。
// ※ CPU 側 WaterSurface::Evaluate() と同じ式
float3 ComputeDetailOffset(float2 xz, float time)
{
    float3 offset = float3(0, 0, 0);
    float detail = gOceanParams.x;
    if (detail <= 0.0) {
        return offset;
    }
    float minLambda = max(gOceanParams.w, 1e-3);

    [unroll]
    for (int i = 0; i < kDetailWaveCount; ++i)
    {
        DetailWave w = GetDetailWave(i, gWaveFreq, gWaveSpeed, gWaveHeight, detail, gOceanParams.y);
        float lambda = kTwoPi / w.k;
        float fade = smoothstep(minLambda, minLambda * 1.5, lambda);
        float ph = DetailPhase(w, xz, time);
        float s = sin(ph);
        float c = cos(ph);
        offset.x += w.qa * w.dir.x * c * fade;
        offset.z += w.qa * w.dir.y * c * fade;
        offset.y += w.amp * s * fade;
    }
    return offset;
}

// =====================================================
// 障害物への反射（鏡像法）
// =====================================================
// 頂点 pos から見た障害物表面の外向き法線 n を求め、
//   ・波の進行方向を n で反射させた方向に進む波を作る
//   ・その波の位相は円周上の反射点における入射波の位相から接続する
// ことで、「入射波が跳ね返って外へ伝播する」波を作る。
// 独立した sin 波を足す旧実装と違い、周波数・位相・進行方向が入射波と連続する。
// 反射点では入射波と反射波が同位相になるので、壁際で振幅が倍になる（定在波の腹）。
void AccumulateReflection(inout GerstnerResult result, WaveDef w, float2 posXZ,
                          int obstacleCount, float reflectStrength, float reflectRange)
{
    [loop]
    for (int i = 0; i < obstacleCount; ++i)
    {
        float2 obsXZ  = gObstacles[i].xz;
        float  radius = max(gObstacles[i].w, 0.001f);

        float2 toP  = posXZ - obsXZ;
        float  dist = length(toP);
        if (dist < 1e-4f) {
            continue; // 中心では法線が定義できない
        }

        // 障害物表面の外向き法線（円柱なので中心からの方向がそのまま法線）
        float2 n = toP / dist;

        // 波がこの面に向かって進んでいるか（w.dir は実際の進行方向）
        float facing = -dot(w.dir, n);
        if (facing <= 0.0f) {
            continue; // 波が当たらない裏側には反射波を作らない
        }

        // 円周上の反射点と、反射後に波が進む向き
        float2 hitPoint = obsXZ + n * radius;
        float2 refDir   = reflect(w.dir, n);

        // 位相の接続：反射点での入射波の位相を引き継ぎ、そこから反射方向へ進ませる
        float hitPhase = dot(w.dir, hitPoint) * w.freq + w.timePhase;
        float refPhase = hitPhase + dot(refDir, posXZ - hitPoint) * w.freq;

        // 反射点から離れるほど減衰（円柱から広がるぶん振幅が落ちる）。
        // fade^2 だと岩の際 2〜3 頂点しか残らず、その範囲は岩自体に隠れて見えない。
        // smoothstep は外周で滑らかに 0 になりつつ中間域が fade^2 の約 2 倍残る。
        float travel = distance(posXZ, hitPoint);
        float fade   = saturate(1.0f - travel / (radius * reflectRange));
        fade = fade * fade * (3.0f - 2.0f * fade);

        // 斜め入射の弱まりは残しつつ、真正面付近だけに寄らないよう角度の効きを緩める
        // （sqrt なので接線方向では 0 のまま＝つなぎ目は連続）
        float angleTerm = sqrt(saturate(facing));

        float atten = fade * angleTerm * reflectStrength;

        AccumulateGerstner(result, refDir, w.freq, refPhase, w.steepness, w.amp * atten);
    }
}

GerstnerResult ComputeGerstnerWave(float3 pos, float time)
{
    GerstnerResult result;
    result.offset   = float3(0, 0, 0);
    result.tangent  = float3(1, 0, 0);
    result.binormal = float3(0, 0, 1);

    int   obstacleCount   = min((int)gObstacleCount.x, kMaxObstacles);
    float reflectStrength = (gObstacleCount.y > 0.0f) ? gObstacleCount.y : 1.0f;
    float reflectRange    = (gObstacleCount.z > 0.0f) ? gObstacleCount.z : 3.0f;

    [unroll]
    for (int k = 0; k < kWaveCount; ++k)
    {
        WaveDef w = GetWaveDef(k, time);

        // 入射波（減衰させない。反射波と干渉させることで跳ね返りに見せる）
        float phase = dot(w.dir, pos.xz) * w.freq + w.timePhase;
        AccumulateGerstner(result, w.dir, w.freq, phase, w.steepness, w.amp);

        // 反射波
        AccumulateReflection(result, w, pos.xz, obstacleCount, reflectStrength, reflectRange);
    }

    // 詳細波（変位のみ。法線は PS）
    result.offset += ComputeDetailOffset(pos.xz, time);

    // 障害物の内側では水面を平らに寄せ、メッシュが岩を突き抜けないようにする
    float insideMask = 1.0f;
    [loop]
    for (int j = 0; j < obstacleCount; ++j)
    {
        float r = max(gObstacles[j].w, 0.001f);
        float d = distance(pos.xz, gObstacles[j].xz);
        insideMask = min(insideMask, smoothstep(r * 0.6f, r, d));
    }
    result.offset   *= insideMask;
    result.tangent   = lerp(float3(1, 0, 0), result.tangent, insideMask);
    result.binormal  = lerp(float3(0, 0, 1), result.binormal, insideMask);

    return result;
}

VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;

    // ワールド空間での頂点位置（波計算用）
    float4 worldPos = mul(input.position, gTransformationMatrix.World);

    // =========================
    // 波と障害物の干渉計算
    // =========================
    // 入射波（Gerstner 3成分）と、障害物で反射した波を合わせて計算する。
    // 反射の詳細は ComputeGerstnerWave / AccumulateReflection を参照。
    GerstnerResult wave = ComputeGerstnerWave(worldPos.xyz, gTime);

    // インタラクティブ波紋のサンプリング
    // WaterPlaneのワールドサイズ(100x100)に合わせたUV変換
    float2 waveUV = (worldPos.xz / 100.0f) + 0.5f;
    float interactiveHeight = gInteractiveWave.SampleLevel(gSamplerClamp, waveUV, 0).r;

    // 変位適用
    worldPos.xyz += wave.offset;
    worldPos.y += interactiveHeight;

    // 法線を接線・従法線から計算（cross product）
    // ※ 波紋の法線はここでは足さない。PS がピクセルごとに求めて合成する
    //   （以前は VS と PS の両方で足していて、波紋の傾きが二重に入っていた）
    float3 N = normalize(cross(wave.binormal, wave.tangent));

    // 変位後のワールド位置を WVP で投影
    // World逆行列を使って元のローカルに戻してから WVP する代わりに、
    // 直接 View*Proj を適用するために、World の逆を避けて VP を使う
    float4x4 VP = mul(
        mul(float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1), gTransformationMatrix.WVP),
        float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
    );

    // 簡易的に: 元のローカル空間に offset を加えて WVP で変換
    float4 displacedLocal = input.position;
    displacedLocal.xyz += wave.offset;
    displacedLocal.y += interactiveHeight;
    output.position = mul(displacedLocal, gTransformationMatrix.WVP);

    output.texcoord = input.texcoord;
    output.normal = N;
    output.worldPosition = worldPos.xyz;
    output.instColor = float4(1, 1, 1, 1);
    output.waveHeight = wave.offset.y + interactiveHeight;

    // 変位前の XZ（グリッドは平面なので線形補間しても正確）と、主波の水平変位の偏微分
    output.restXZ = worldPos.xz - wave.offset.xz;
    output.mainJacobian = float3(wave.tangent.x - 1.0, wave.binormal.z - 1.0, wave.tangent.z);

    return output;
}
