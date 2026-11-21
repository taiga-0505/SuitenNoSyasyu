// ============================================================================
// Skinning.CS.hlsl
// ----------------------------------------------------------------------------
// Compute Shader でスキニング（Linear Blend Skinning）を実行する。
//
// 入力:
//   t0 : gMatrixPalette  — スキニング行列パレット（IBP * SSM）
//   t1 : gInputVertices  — 元の頂点データ（position, texcoord, normal, boneIndex, boneWeight）
//   b0 : gSkinningInfo   — 頂点数
//   ※ ボーンインデックスとウェイトは t1 の Vertex に含まれる。t2 は宣言していない
//     （VertexInfluence 構造体は残っているが未使用）
//
// 出力:
//   u0 : gOutputVertices — スキニング適用後の頂点データ
//
// スキニング済み頂点は position と normal のみ変換される。
// texcoord はそのままコピーされる。
// ============================================================================

// --- 頂点構造体（入出力共通） ---
// C++ 側の VertexData と同じレイアウト:
//   float4 position   (16 bytes)
//   float2 texcoord   ( 8 bytes)
//   float3 normal     (12 bytes)
//   int4   boneIndex  (16 bytes)  ← 入力でのみ意味を持つ
//   float4 boneWeight (16 bytes)  ← 入力でのみ意味を持つ
// 出力はスキニング済みなので boneIndex / boneWeight は不要だが、
// VBV のストライドを合わせるために同じ構造体で出力する。

struct Vertex
{
    float4 position;   ///< 頂点位置（モデルローカル）
    float2 texcoord;   ///< テクスチャ座標
    float3 normal;     ///< 法線
    int4   boneIndex;  ///< 影響するボーンのインデックス（最大 4 本）
    float4 boneWeight; ///< 各ボーンのウェイト
};

/// @brief 頂点ごとのボーン影響（現在は未使用。Vertex に同じ情報を持たせている）
struct VertexInfluence
{
    int4   boneIndex;  ///< 影響するボーンのインデックス（最大 4 本）
    float4 boneWeight; ///< 各ボーンのウェイト
};

/// @brief スキニングのディスパッチ情報
struct SkinningInformation
{
    uint numVertices; ///< 頂点数
};

// --- リソースバインド ---
StructuredBuffer<float4x4>        gMatrixPalette   : register(t0);
StructuredBuffer<Vertex>          gInputVertices   : register(t1);

RWStructuredBuffer<Vertex>        gOutputVertices  : register(u0);

ConstantBuffer<SkinningInformation> gSkinningInfo  : register(b0);

// --- エントリポイント ---
/// @brief 1 スレッドが 1 頂点をスキニングして出力バッファへ書く
/// @param DTid ディスパッチスレッド ID（x が頂点番号）
[numthreads(1024, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint vertexIndex = DTid.x;
    if (vertexIndex >= gSkinningInfo.numVertices)
    {
        return;
    }

    // 入力頂点の取得
    Vertex input = gInputVertices[vertexIndex];

    // 線形ブレンドスキニング: skinMat = Σ w_i * T_i
    float4x4 skinMat = (float4x4)0;
    skinMat += input.boneWeight.x * gMatrixPalette[input.boneIndex.x];
    skinMat += input.boneWeight.y * gMatrixPalette[input.boneIndex.y];
    skinMat += input.boneWeight.z * gMatrixPalette[input.boneIndex.z];
    skinMat += input.boneWeight.w * gMatrixPalette[input.boneIndex.w];

    // 出力を構築
    Vertex skinned;

    // スキニング適用（モデルローカル空間 → スケルトン空間）
    skinned.position = mul(input.position, skinMat);
    skinned.position.w = 1.0f; // w を保証

    // 法線にもスキニングを適用（回転のみ、正規化）
    // NOTE: 非等方スケーリング（X / Y / Z で異なるスケール）が含まれる場合、
    //       法線が正しく変換されない可能性がある。
    //       その場合は逆転置行列（inverse-transpose）を使うこと。
    skinned.normal = normalize(mul(input.normal, (float3x3)skinMat));

    // texcoord はそのままコピー
    skinned.texcoord = input.texcoord;

    // boneIndex / boneWeight はスキニング済みなので 0 クリア
    // （描画 VS では使わないが、ストライド合わせのために出力）
    skinned.boneIndex  = int4(0, 0, 0, 0);
    skinned.boneWeight = float4(0, 0, 0, 0);

    // 出力バッファに書き込み
    gOutputVertices[vertexIndex] = skinned;
}
