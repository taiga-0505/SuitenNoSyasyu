// =============================================================================
// FogOverlay.hlsl
//
// 画面全体に重ねる冷たい霧（fog PSO。VS / PS とも本ファイル、エントリは VS / PS）。
// fbm ノイズを風で流して「もや」を作り、αブレンドでシーンの上に重ねる。
// 深度は使わないので、距離フォグではなく画面に貼り付く霧として見える。
// =============================================================================

cbuffer FogCB : register(b0)
{
    float gTime;       ///< 経過時間（秒）
    float gIntensity;  ///< 霧の濃さ（0〜1）
    float gScale;      ///< ノイズの細かさ（例: 2.0〜8.0）
    float gSpeed;      ///< 流れる速さ（例: 0.02〜0.15）

    float2 gWind;      ///< 風向き（例: (0.05, 0.02)）
    float gFeather;    ///< 霧の柔らかさ（例: 0.2）
    float gBottomBias; ///< 画面下側を濃くする量（例: 0.4）

    float4 gColor;     ///< 霧の色（a は不透明度に掛ける）
};

/// @struct VSOut
/// @brief VS の出力
struct VSOut
{
    float4 pos : SV_POSITION; ///< クリップ空間位置
    float2 uv : TEXCOORD0;    ///< 画面 UV（下 0 〜 上 1。V は上向き）
};

/// @brief 頂点バッファ無しでフルスクリーン三角形を出力する
/// @param vid 頂点番号（0〜2）
/// @return クリップ空間位置と UV
VSOut VS(uint vid : SV_VertexID)
{
    // フルスクリーン三角形（頂点バッファ不要）
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? 3.0 : -1.0;

    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    // -1〜3 を 0〜2 に写す（画面内は 0〜1。はみ出す分は PS で saturate する）
    o.uv = p * 0.5 + 0.5;
    return o;
}

/// @brief 2D の値から 0〜1 のハッシュ値を作る（軽量）
/// @param p 入力座標
/// @return 0〜1 のハッシュ値
float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

/// @brief 格子点のハッシュを滑らかに補間したバリューノイズ
/// @param p 入力座標
/// @return 0〜1 のノイズ値
float noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float a = hash12(i);
    float b = hash12(i + float2(1, 0));
    float c = hash12(i + float2(0, 1));
    float d = hash12(i + float2(1, 1));
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

/// @brief noise を周波数を変えて重ねた fbm（非整数ブラウン運動）
/// @param p 入力座標
/// @return おおむね 0〜1 のノイズ値
float fbm(float2 p)
{
    float v = 0.0;
    float a = 0.5;
    // 4 オクターブくらいが軽くてちょうどいい
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        v += a * noise(p);
        p *= 2.02;
        a *= 0.5;
    }
    return v;
}

/// @brief 霧の色と、ノイズから求めた不透明度を出力する
/// @param i VS の出力
/// @return 出力色（α ブレンドで重ねる）
float4 PS(VSOut i) : SV_TARGET
{
    float2 uv = saturate(i.uv);

    // 風で流す
    float2 p = uv * gScale + gWind * (gTime * gSpeed);

    // 霧っぽい“もや”＝ fbm + 少しの歪み（簡易ドメインワープ）
    float n1 = fbm(p);
    float2 warp = float2(fbm(p + 7.1), fbm(p + 13.7)) - 0.5;
    float n2 = fbm(p + warp * 0.8);

    float mist = lerp(n1, n2, 0.65);

    // ふわっとさせる（閾値をぼかす）
    float alpha = smoothstep(0.45 - gFeather, 0.45 + gFeather, mist);

    // 下側ほど濃く（寒い霧が溜まる感じ）
    float bottom = smoothstep(0.0, 1.0, uv.y); // 上が 1
    float bottomFactor = lerp(1.0 + gBottomBias, 1.0, bottom);
    alpha *= bottomFactor;
    
    alpha = saturate(alpha * gIntensity * gColor.a);
    
    return float4(gColor.rgb, alpha);
}
