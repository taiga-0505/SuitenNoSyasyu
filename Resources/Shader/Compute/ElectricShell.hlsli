// ============================================================================
// ElectricShell.hlsli
// ----------------------------------------------------------------------------
// 電撃（ParticleType::Electric）の Emit / Update CS で共有する「殻」の幾何。
//
// 殻 = モデルを包む「丸みのある箱」。
//   - Box 形状      : gShapeBoxSize を全長（直径）とする箱。gShellRoundness で角を丸める
//                     （0 = 角のある箱, 1 = 最短辺いっぱいまで丸める → カプセル / 球）
//   - それ以外の形状 : gShapeRadius の球
//   - gShellMargin  : 表面から外側へ浮かせる距離（m）。0 だとモデル表面と重なって半分隠れる
//   - gShellYaw     : 殻の Y 軸回転（rad）。Transform.rotation.y を渡すと回る箱にも殻が合う
//
// 前提: このファイルを include する前に PerFrame cbuffer が宣言されていること
//       （gEmitterShape, gShapeBoxSize, gShapeRadius, gShellRoundness, gShellMargin, gShellYaw）
// ============================================================================

/// @brief 簡易ハッシュ関数（EmitParticle.CS.hlsl と同じもの）。seed から 0〜1 の擬似乱数を作る
/// @param seed 乱数のシード
/// @return 0〜1 の擬似乱数
float Hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / 4294967295.0f;
}

/// @brief Hash を -1〜1 に広げた擬似乱数
/// @param seed 乱数のシード
/// @return -1〜1 の擬似乱数
float HashSigned(uint seed)
{
    return Hash(seed) * 2.0f - 1.0f;
}

/// @brief 単位球面上で一様な方向を返す
/// @details cos(phi) を -1〜1 で一様に取ると球面上で一様分布になる。
/// @param seed 乱数のシード（seed と seed + 1 を使う）
/// @return 単位方向ベクトル
float3 RandomDirection(uint seed)
{
    float z = HashSigned(seed);
    float theta = Hash(seed + 1u) * 6.28318530f;
    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(theta), r * sin(theta), z);
}

/// @brief 殻ローカル → ワールド（Y 軸回転）。エンジンの MakeRotateMatrix(Y) と同じ向き
/// @param v 殻ローカルのベクトル（平行移動は含めない）
/// @return ワールドの向きに回したベクトル
float3 ShellToWorld(float3 v)
{
    float c = cos(gShellYaw);
    float s = sin(gShellYaw);
    return float3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

/// @brief ワールド → 殻ローカル（ShellToWorld の逆回転）
/// @param v ワールドのベクトル（エミッタ位置からの相対）
/// @return 殻ローカルのベクトル
float3 WorldToShell(float3 v)
{
    float c = cos(gShellYaw);
    float s = sin(gShellYaw);
    return float3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
}

/// @brief 殻の半径（軸ごと）。Box は全長の半分、それ以外は球の半径
/// @return 軸ごとの半径（最小 0.005）
float3 ShellHalfExtents()
{
    if (gEmitterShape == 2) // Box
    {
        return max(gShapeBoxSize * 0.5f, 0.005f);
    }
    return max(gShapeRadius, 0.005f).xxx;
}

/// @brief 角の丸み半径。Box 以外（球）は常に完全な丸み
/// @param h 殻の半径（ShellHalfExtents の戻り値）
/// @return 角の丸み半径（最短辺の半径 × 丸み）
float ShellCornerRadius(float3 h)
{
    float minH = min(h.x, min(h.y, h.z));
    float roundness = (gEmitterShape == 2) ? saturate(gShellRoundness) : 1.0f;
    return minH * roundness;
}

/// @brief 殻ローカル座標 p を殻の表面へ射影し、表面点と外向き法線を返す
/// @details 丸みのある箱 = 「内側の箱（半径 h - r）」を r だけ太らせた形。
///          p を内側の箱へクランプした点 q との差が、そのまま表面の法線方向になる。
///          p が内側の箱の中にあるときは最も近い面へ押し出す。
/// @param p      殻ローカルの座標
/// @param h      殻の半径（ShellHalfExtents の戻り値）
/// @param r      角の丸み半径（ShellCornerRadius の戻り値）
/// @param normal 射影先の外向き法線（殻ローカル。出力）
/// @return 殻の表面上の点（殻ローカル）
float3 ProjectToShell(float3 p, float3 h, float r, out float3 normal)
{
    float3 hi = max(h - r, 0.0f);
    float3 q = clamp(p, -hi, hi);
    float3 d = p - q;
    float dl = length(d);
    if (dl > 1e-5f)
    {
        normal = d / dl;
        return q + normal * r;
    }

    // 内側の箱の中: 各面までの距離が最も小さい面へ押し出す
    float3 gap = hi - abs(p);
    float3 n = float3(0.0f, 0.0f, 0.0f);
    float3 onFace = q;
    if (gap.x <= gap.y && gap.x <= gap.z)
    {
        n.x = (p.x >= 0.0f) ? 1.0f : -1.0f;
        onFace.x = n.x * hi.x;
    }
    else if (gap.y <= gap.z)
    {
        n.y = (p.y >= 0.0f) ? 1.0f : -1.0f;
        onFace.y = n.y * hi.y;
    }
    else
    {
        n.z = (p.z >= 0.0f) ? 1.0f : -1.0f;
        onFace.z = n.z * hi.z;
    }
    normal = n;
    return onFace + n * r;
}

/// @brief 表面から浮かせる距離。固有乱数（0〜1）で 0.5〜1.5 倍に散らし、皮 1 枚に見えないようにする
/// @param rnd パーティクル固有の乱数（0〜1。Particle.pad0）
/// @return 浮かせる距離（m）
float ShellLift(float rnd)
{
    return gShellMargin * (0.5f + rnd);
}
