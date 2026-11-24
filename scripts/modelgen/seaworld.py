# -*- coding: utf-8 -*-
"""
水上ステージ（外洋）の地形モデルを手続き的に作る（Blender 不要）。

  python scripts/modelgen/seaworld.py            # 全部作る
  python scripts/modelgen/seaworld.py cliff_cave # 名前を並べるとそれだけ

出力先: project/Resources/model/seaworld/
  <name>.glb          … メッシュ（なめらかな法線・モデル専用の UV）
  <name>_albedo.jpg   … 焼き込んだ色（岩の割れ目・地層・濡れた水際・草・砂）
  <name>_normal.jpg   … 焼き込んだ細かい凹凸（ModelRenderer の normalMapPath に指定）
  rough_none.png      … 全モデル共通のラフネス（4x4 の白 = ハイライト無し。エンジンのハイライトは白一色で強いため）
  <name>_far.jpg      … 遠景用（縮小して空気の色へ寄せた albedo。texturePath で差し替える）

  searock_stack / searock_twin / searock_boulder … レール脇の岩（当たり判定あり）。**±0.5 の箱に正規化**
  seabed                                         … 海底。同じく ±0.5 に正規化
  cliff_cave                                     … 洞窟の口が開いた断崖。m 単位・原点 = 旧 Cliff の中心（水面）
  island_large / island_medium / island_low / island_stacks … 遠景の島。m 単位・原点 = 島の中心（水面）

1 モデル = 1 メッシュ = 1 マテリアル なので、描画コマンド（CPU）は前の版と同じ数。
質感はテクスチャ、形はポリゴン数（GPU）で出す。詳しくは terrainbake.py。
"""
from __future__ import annotations

import json
import math
import os
import sys
import time
from typing import List

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terrainbake import (BPart, BakedModel, assemble, split_part, dilate, fbm3, lin_to_srgb, mix, orient_by,  # noqa: E402
                         orient_outward, rasterize, ridged3, save_rgb, sstep, write_glb, CLASSES, SWATCH_PX,
                         vnoise3)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.environ.get("MODELGEN_OUT") or os.path.join(ROOT, "project", "Resources", "model", "seaworld")
META = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seaworld_meta.json")

# 遠景用：空の水平線付近の色（Resources/SkyBox/front.png の下半分の平均を sRGB→リニア）
HAZE = np.array([0.14, 0.25, 0.45])
HAZE_AMOUNT = 0.42


# ---------------------------------------------------------------------------
# 2D ノイズ（形を作る側。焼き込みは terrainbake の 3D ノイズ）
# ---------------------------------------------------------------------------
def _hash2(ix, iy, seed):
    h = (np.asarray(ix, np.int64) * 374761393 + np.asarray(iy, np.int64) * 668265263 + seed * 1442695041) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return h.astype(np.float64) / 4294967296.0


def vnoise(x, y, seed=0):
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = x - x0, y - y0
    ux, uy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = _hash2(x0, y0, seed), _hash2(x0 + 1, y0, seed)
    c, d = _hash2(x0, y0 + 1, seed), _hash2(x0 + 1, y0 + 1, seed)
    return (a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy) * 2.0 - 1.0


def fbm(x, y, seed=0, octaves=4):
    s, amp, norm, f = 0.0, 1.0, 0.0, 1.0
    for k in range(octaves):
        s = s + amp * vnoise(np.asarray(x) * f, np.asarray(y) * f, seed + k * 101)
        norm += amp
        amp *= 0.5
        f *= 2.03
    return s / norm


def h01(i, seed=0):
    return float(_hash2(np.array(i), np.array(7), seed))


def smoothstep_arr(e0, e1, x):
    t = np.clip((np.asarray(x, float) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def grid_tris(nu, nv, skip=None):
    tris = []
    for j in range(nv):
        for i in range(nu):
            if skip and skip(i, j):
                continue
            a, b, c, d = j * (nu + 1) + i, j * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i
            if (i + j) % 2 == 0:
                tris += [[a, b, c], [a, c, d]]
            else:
                tris += [[a, b, d], [b, c, d]]
    return np.array(tris, dtype=np.int64)


def strip(rows):
    m = len(rows[0])
    pos = np.concatenate(rows, axis=0)
    tris = []
    for k in range(len(rows) - 1):
        for i in range(m - 1):
            a, b, c, d = k * m + i, k * m + i + 1, (k + 1) * m + i + 1, (k + 1) * m + i
            tris += [[a, b, c], [a, c, d]]
    return pos, np.array(tris, dtype=np.int64)


def resample(poly, fracs):
    seg = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    cum /= cum[-1]
    return np.column_stack([np.interp(fracs, cum, poly[:, k]) for k in range(poly.shape[1])])


def arc_fracs(poly):
    seg = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    return cum / cum[-1]


def arc_len(poly):
    seg = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    return np.concatenate([[0.0], np.cumsum(seg)])


# ---------------------------------------------------------------------------
# 岩の柱（側面は継ぎ目を持つ開いた帯 ＋ 天端の蓋）
# ---------------------------------------------------------------------------
def facet_r(th, seed, k):
    """角度 th の半径の倍率。k 個の頂点を直線でつないだ多角形（平らな面と角）"""
    angs = np.sort(np.array([(i + 0.6 * (h01(i, seed) - 0.5)) / k for i in range(k)]) * 2 * math.pi)
    rads = np.array([0.88 + 0.24 * h01(i + 50, seed) for i in range(k)])
    t = (th % (2 * math.pi))
    a = np.concatenate([angs, [angs[0] + 2 * math.pi]])
    r = np.concatenate([rads, [rads[0]]])
    if t < a[0]:
        t += 2 * math.pi
    j = np.searchsorted(a, t) - 1
    j = min(max(j, 0), k - 1)
    u = (t - a[j]) / (a[j + 1] - a[j])
    # 弦（まっすぐな辺）になるよう、半径ではなく点を直線補間する
    p0 = np.array([math.cos(a[j]), math.sin(a[j])]) * r[j]
    p1 = np.array([math.cos(a[j + 1]), math.sin(a[j + 1])]) * r[j + 1]
    p = p0 + (p1 - p0) * u
    return float(np.linalg.norm(p))


def rock_column(center, y0, y1, R, seed, nseg=36, ny=None, lean=0.25, jag=0.2, ledge=0.08, squat=False,
                top_raise=0.18, cls="rockcol", facets=7) -> List[BPart]:
    cx0, cz0 = center[0], center[2]
    H = y1 - y0
    if ny is None:
        ny = int(np.clip(H / max(R * 0.22, 0.4), 10, 48))
    rings = []
    for j in range(ny):
        yn = j / (ny - 1)
        y = y0 + H * yn
        if squat:
            base = R * (0.55 + 0.55 * math.sin(math.pi * min(0.5 + yn * 0.62, 1.0)))
        else:
            base = R * (1.15 - 0.28 * yn) * (1.0 - 0.38 * smoothstep_arr(0.78, 1.0, yn))
        # 地層の張り出し（段）。tanh で角を少し丸める
        step = 1.0 + ledge * math.tanh(3.0 * math.sin(y / max(R, 1e-3) * 2.2 + seed))
        lx = lean * R * math.sin(yn * 2.2 + seed * 0.7)
        lz = lean * R * math.cos(yn * 1.9 + seed * 1.3)
        pts = []
        # 層ごとに少しずれた多角形の断面（節理で割れた平らな面）
        layer = int(math.floor(y / max(R * 0.9, 0.5) + seed))
        for i in range(nseg):
            th = 2 * math.pi * i / nseg
            fr_ = facet_r(th, seed * 7 + layer, facets) * 0.7 + facet_r(th, seed * 7 + layer + 1, facets) * 0.3
            r = base * step * fr_ * (1.0 + jag * 0.5 * float(fbm(math.cos(th) * 1.4 + seed * 3.1,
                                                                 y / max(R, 1e-3) * 0.45 + math.sin(th), seed))
                                     + 0.04 * float(fbm(math.cos(th) * 5 + seed, y / max(R, 1e-3) * 2.0, seed + 9)))
            yy = y0 if j == 0 else y
            if j == ny - 1:
                yy += R * (h01(i + 900, seed) - 0.6) * 0.35
            pts.append([cx0 + lx + r * math.sin(th), yy, cz0 + lz + r * math.cos(th)])
        pts.append(pts[0])  # 継ぎ目（UV を切るため同じ位置の頂点を 2 つ持つ）
        rings.append(np.array(pts))
    pos, tris = strip(rings)
    circ = np.mean([arc_len(r[:, [0, 2]])[-1] for r in rings])
    u = np.tile(np.linspace(0, circ, nseg + 1), ny)
    v = np.concatenate([np.full(nseg + 1, arc) for arc in arc_len(np.array([r.mean(axis=0) for r in rings]))])
    side = BPart(pos, tris, np.column_stack([u, v]), cls=cls, name="side")
    orient_outward(side, (cx0, (y0 + y1) * 0.5, cz0))
    # 天端の蓋
    top = rings[-1][:-1]
    c = top.mean(axis=0) + np.array([0, R * top_raise, 0])
    cp = np.concatenate([top, c[None]], axis=0)
    ct = np.array([[i, (i + 1) % nseg, nseg] for i in range(nseg)])
    cap = BPart(cp, ct, cp[:, [0, 2]].copy(), cls=cls, name="cap")
    orient_by(cap, lambda q: np.tile([0, 1.0, 0], (len(q), 1)))
    return [side, cap]


def normalize_unit(parts: List[BPart], meters):
    """全パーツの外接箱を ±0.5 の立方体へ。ノイズ用の位置（bake_pos）と UV の展開は実寸に近い meters で測る"""
    P = np.concatenate([p.pos for p in parts], axis=0)
    lo, hi = P.min(axis=0), P.max(axis=0)
    c = (lo + hi) * 0.5
    s = np.maximum(hi - lo, 1e-9)
    k = np.asarray(meters, float) / s  # 作った座標 → m
    for p in parts:
        if p.uvp is not None:
            if p.name == "side":
                p.uvp = p.uvp * np.array([(k[0] + k[2]) * 0.5, k[1]])
            elif p.name == "cap":
                p.uvp = p.uvp * np.array([k[0], k[2]])
        p.pos = (p.pos - c) / s
        p.bake_pos = p.pos * np.asarray(meters, float)


def waterline_points(parts: List[BPart], y=0.0):
    pts = []
    for p in parts:
        P, T = p.pos, p.tris
        for e0, e1 in ((0, 1), (1, 2), (2, 0)):
            a, b = P[T[:, e0]], P[T[:, e1]]
            m = (a[:, 1] - y) * (b[:, 1] - y) < 0
            if np.any(m):
                t = (y - a[m, 1]) / (b[m, 1] - a[m, 1])
                pts.append((a[m] + (b[m] - a[m]) * t[:, None])[:, [0, 2]])
    return np.concatenate(pts, axis=0) if pts else np.zeros((0, 2))


# ---------------------------------------------------------------------------
# 木（ローポリのまま。色は共有見本 swatch から）
# ---------------------------------------------------------------------------
def cone(p0, p1, r0, n=9):
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    ring = np.array([p0 + [r0 * math.cos(2 * math.pi * i / n), 0.0, r0 * math.sin(2 * math.pi * i / n)]
                     for i in range(n)])
    pos = np.concatenate([ring, p1[None], p0[None]], axis=0)
    tris = [[i, (i + 1) % n, n] for i in range(n)] + [[i, n + 1, (i + 1) % n] for i in range(n)]
    return orient_outward(BPart(pos, np.array(tris), swatch="pine", smooth=True), (p0 + p1) * 0.5)


def trunk(base, h, r, n=6):
    base = np.asarray(base, float)
    r0 = np.array([[base[0] + r * math.cos(2 * math.pi * i / n), base[1] - 0.6, base[2] + r * math.sin(2 * math.pi * i / n)]
                   for i in range(n)])
    r1 = r0.copy()
    r1[:, 1] = base[1] + h
    r1[:, [0, 2]] = base[[0, 2]] + (r0[:, [0, 2]] - base[[0, 2]]) * 0.65
    pos = np.concatenate([r0, r1])
    tris = []
    for i in range(n):
        a, b, c, d = i, (i + 1) % n, n + (i + 1) % n, n + i
        tris += [[a, b, c], [a, c, d]]
    return orient_outward(BPart(pos, np.array(tris), swatch="trunk", smooth=False), base + [0, h * 0.5, 0])


def blob(center, radii, seed, nu=9, nv=6, jitter=0.16):
    pts = [[0, -1, 0]]
    for j in range(1, nv):
        phi = -math.pi / 2 + math.pi * j / nv
        for i in range(nu):
            th = 2 * math.pi * (i + 0.5 * (j % 2)) / nu
            pts.append([math.cos(phi) * math.sin(th), math.sin(phi), math.cos(phi) * math.cos(th)])
    pts.append([0, 1, 0])
    P = np.array(pts)
    for k in range(len(P)):
        P[k] *= 1.0 + jitter * (h01(k, seed) - 0.5) * 2
    P = P * np.asarray(radii) + np.asarray(center)
    tris = []
    top = len(pts) - 1

    def idx(j, i):
        return 1 + (j - 1) * nu + (i % nu)
    for i in range(nu):
        tris.append([0, idx(1, i + 1), idx(1, i)])
    for j in range(1, nv - 1):
        for i in range(nu):
            a, b, c, d = idx(j, i), idx(j, i + 1), idx(j + 1, i + 1), idx(j + 1, i)
            tris += [[a, b, c], [a, c, d]]
    for i in range(nu):
        tris.append([top, idx(nv - 1, i), idx(nv - 1, i + 1)])
    return orient_outward(BPart(P, np.array(tris), swatch="leaf", smooth=True), center)


def tree(base, seed, size=1.0, kind=None) -> List[BPart]:
    base = np.asarray(base, float)
    if kind is None:
        kind = "pine" if h01(seed, 77) < 0.6 else "leaf"
    s = size * (0.75 + 0.6 * h01(seed, 78))
    parts = []
    if kind == "pine":
        parts.append(trunk(base, 2.4 * s, 0.32 * s))
        y = base[1] + 1.4 * s
        for rr, hh in ((2.5, 3.8), (2.0, 3.3), (1.4, 2.9), (0.8, 2.2)):
            parts.append(cone([base[0], y, base[2]], [base[0], y + hh * s, base[2]], rr * s))
            y += hh * s * 0.5
    else:
        parts.append(trunk(base, 2.8 * s, 0.38 * s))
        for k in range(3):
            off = np.array([(h01(seed * 3 + k, 5) - 0.5) * 2.2 * s, 3.4 * s + k * 0.9 * s,
                            (h01(seed * 7 + k, 6) - 0.5) * 2.2 * s])
            parts.append(blob(base + off, np.array([2.2, 1.7, 2.2]) * s * (1.0 - 0.2 * k), seed * 13 + k))
    return parts


# ===========================================================================
# 焼き込み（材質ごとの手続き的な色・凹凸・ラフネス）
# ===========================================================================
ROCK_LIGHT = np.array([0.50, 0.465, 0.41])
ROCK_MID = np.array([0.30, 0.28, 0.255])
ROCK_WARM = np.array([0.42, 0.33, 0.24])
ROCK_COOL = np.array([0.30, 0.31, 0.31])
LICHEN = np.array([0.55, 0.52, 0.36])
WET = np.array([0.045, 0.05, 0.05])
ALGAE = np.array([0.06, 0.085, 0.03])
GRASS_D = np.array([0.035, 0.075, 0.018])
GRASS_L = np.array([0.13, 0.21, 0.045])
GRASS_DRY = np.array([0.26, 0.24, 0.10])
SOIL = np.array([0.17, 0.13, 0.08])
SAND_L = np.array([0.62, 0.54, 0.37])
SAND_D = np.array([0.43, 0.36, 0.24])
SAND_WET = np.array([0.24, 0.20, 0.13])
SEA_SAND = np.array([0.36, 0.34, 0.26])


def shade_rock(P, N, seed=0, mul=1.0):
    """岩肌。戻り値 (albedo, height[m], roughness)"""
    y = P[:, 1]
    ny = N[:, 1]
    n1 = fbm3(P, 0.05, seed + 1, 4)
    n2 = fbm3(P, 0.22, seed + 2, 3)
    n3 = fbm3(P, 1.1, seed + 3, 3)
    col = mix(ROCK_MID, ROCK_LIGHT, np.clip(0.55 + 0.55 * n1 + 0.2 * n2, 0, 1))
    col = mix(col, ROCK_WARM, np.clip(0.35 + 0.6 * fbm3(P, 0.025, seed + 4, 3), 0, 1) * 0.6)
    col = mix(col, ROCK_COOL, np.clip(0.5 * fbm3(P, 0.07, seed + 14, 2), 0, 1))
    # 地層：ゆがんだ水平の縞と、層の境目の細い暗い線
    warp = 2.4 * fbm3(P, 0.02, seed + 5, 3)
    band = np.sin(y * 0.8 + warp)
    col = mix(col, col * np.array([0.92, 0.84, 0.74]), sstep(0.35, 0.85, band) * 0.7)
    ph = y / 3.3 + 0.6 * fbm3(P, 0.04, seed + 6, 2)
    seam = np.exp(-(((ph - np.floor(ph)) - 0.5) / 0.035) ** 2) * sstep(0.3, 0.6, 1 - np.abs(ny))
    col = col * (1 - 0.35 * seam[:, None])
    # 節理（縦に走るほぼまっすぐな割れ目が 2 方向）＋ 細かいひび
    wj = 0.9 * fbm3(P, 0.03, seed + 7, 2)  # 曲げすぎると小さな岩で輪になる
    j1 = np.abs(np.sin((P[:, 0] * 0.83 + P[:, 2] * 0.56) * 0.55 + wj))
    j2 = np.abs(np.sin((P[:, 0] * -0.42 + P[:, 2] * 0.91) * 0.37 + wj * 0.7 + 1.3))
    gate = sstep(-0.2, 0.3, fbm3(P, 0.08, seed + 15, 2, stretch=(1.0, 0.3, 1.0)))  # 途切れさせる
    crack = np.maximum(sstep(0.06, 0.0, j1) * gate, sstep(0.05, 0.0, j2) * (1 - gate) * 0.8)
    # 細かいひびは尾根ノイズの等値線が輪になって見えるのでやめ、粒の陰影（下の n3）と法線マップに任せる
    crack2 = np.zeros(len(P))
    col = col * (1 - 0.6 * crack[:, None])
    # 割れ目のまわりはわずかに暗く（汚れ）
    col = col * (1 - 0.15 * (sstep(0.25, 0.0, j1) * gate))[:, None]
    # 雨だれの縦筋（切り立った面だけ）
    streak = sstep(0.05, 0.55, fbm3(P, 0.35, seed + 9, 3, stretch=(1.0, 0.08, 1.0)))
    col = col * (1 - 0.28 * (streak * sstep(0.2, 0.8, 1 - np.abs(ny)))[:, None])
    # 上向きの面の地衣類（白〜黄の斑）
    li = sstep(0.35, 0.8, ny) * sstep(0.15, 0.55, fbm3(P, 0.35, seed + 10, 3)) * sstep(2.0, 4.0, y)
    col = mix(col, LICHEN, li * 0.55)
    # 庇の下は暗く（擬似 AO）
    col = col * (1 - 0.35 * sstep(-0.1, -0.7, ny))[:, None]
    # 細かいざらつき
    col = col * (0.9 + 0.2 * n3)[:, None]
    # 水際：濡れた黒 → 海藻の緑 → しぶきで湿った帯
    wl = 0.25 + 0.45 * fbm3(P, 0.35, seed + 11, 2)
    wet = sstep(wl + 0.15, wl - 0.15, y)
    splash = sstep(wl + 2.2, wl, y) * (1 - wet)
    algae = sstep(-2.5, -0.6, y) * wet
    col = col * (1 - 0.35 * splash)[:, None]
    under = mix(WET * 1.6, ALGAE, np.clip(algae * (0.6 + 0.6 * n2), 0, 1))
    under = under * (0.75 + 0.5 * n3)[:, None]
    col = mix(col, under, wet)
    # フジツボの白い点（水際の少し上）
    barn = sstep(0.55, 0.75, fbm3(P, 3.0, seed + 12, 2)) * sstep(-0.8, 0.2, y) * sstep(1.2, 0.4, y)
    col = mix(col, np.array([0.38, 0.36, 0.32]), barn * 0.7)
    col = col * mul
    height = 0.05 * n3 + 0.12 * n2 + 0.03 * fbm3(P, 3.5, seed + 13, 2) - 0.12 * crack - 0.04 * crack2 + 0.05 * band
    # ラフネスは全面 1（ハイライト無し）。エンジンのハイライトは白一色で強く、
    # 濡れた水際を少し照らそうとすると岩の下半分が白く飛んで見えた
    rough = np.ones(len(P))
    return col, height, rough


def shade_grass(P, N, seed=0):
    n1 = fbm3(P, 0.06, seed + 21, 3)
    n2 = fbm3(P, 0.5, seed + 22, 3)
    n3 = fbm3(P, 2.5, seed + 23, 2)
    col = mix(GRASS_D, GRASS_L, np.clip(0.5 + 0.7 * n1 + 0.3 * n2, 0, 1))
    col = mix(col, GRASS_DRY, sstep(0.25, 0.6, fbm3(P, 0.09, seed + 24, 3)) * 0.6)
    col = col * (0.8 + 0.4 * (0.5 + 0.5 * n3))[:, None]
    height = 0.03 * n2 + 0.02 * n3
    return col, height, np.ones(len(P))


def shade_sand(P, N, seed=0, beach=1.2):
    y = P[:, 1]
    n1 = fbm3(P, 0.1, seed + 31, 3)
    n3 = fbm3(P, 3.0, seed + 32, 2)
    col = mix(SAND_D, SAND_L, np.clip(0.6 + 0.6 * n1, 0, 1))
    col = col * (0.92 + 0.16 * n3)[:, None]
    wetline = 0.35 + 0.25 * fbm3(P, 0.2, seed + 33, 2)
    w = sstep(wetline + 0.3, wetline - 0.1, y)
    col = mix(col, SAND_WET, w)
    ripple = np.sin(P[:, 0] * 1.7 + P[:, 2] * 0.9 + 2.0 * fbm3(P, 0.3, seed + 34, 2))
    height = 0.015 * ripple + 0.01 * n3
    return col, height, np.ones(len(P))


def shade(model: BakedModel, G_P, G_N, G_C):
    T = G_C.shape[0]
    filled = G_C >= 0
    P = G_P[filled].astype(np.float64)
    N = G_N[filled].astype(np.float64)
    C = G_C[filled]
    K = len(P)
    alb = np.zeros((K, 3))
    hgt = np.zeros(K)
    rgh = np.ones(K)
    seed = model.params.get("seed", 0)
    beach = model.params.get("beach", 1.2)
    rock_mul = model.params.get("rock_mul", 1.0)

    def put(mask, res):
        a, h, r = res
        alb[mask] = a
        hgt[mask] = h
        rgh[mask] = r

    for cid, cname in enumerate(CLASSES):
        m = C == cid
        if not m.any():
            continue
        Pm, Nm = P[m], N[m]
        if cname in ("rock", "rockcol", "terrain", "plateau"):
            ra, rh, rr = shade_rock(Pm, Nm, seed, rock_mul)
            if cname == "rock":
                put(m, (ra, rh, rr))
                continue
            y, ny = Pm[:, 1], Nm[:, 1]
            ga, gh, gr = shade_grass(Pm, Nm, seed)
            noise = fbm3(Pm, 0.15, seed + 41, 3)
            if cname == "rockcol":
                g = sstep(0.68, 0.85, ny + 0.12 * noise) * sstep(0.8, 1.6, y)
            else:
                g = sstep(0.60, 0.78, ny + 0.12 * noise) * sstep(beach + 0.2, beach + 0.9, y + 0.5 * noise)
            # 草と岩の境目に土
            edge = g * (1 - g) * 4
            a = mix(ra, ga, g)
            a = mix(a, SOIL * (0.8 + 0.4 * (0.5 + 0.5 * noise))[:, None], edge * 0.6)
            h = rh * (1 - g) + gh * g
            r = rr * (1 - g) + gr * g
            if cname in ("terrain",):
                sa, sh, sr = shade_sand(Pm, Nm, seed, beach)
                s = sstep(0.35, 0.6, ny) * sstep(beach + 0.6, beach + 0.1, y + 0.3 * noise) * sstep(-0.6, 0.1, y)
                a = mix(a, sa, s)
                h = h * (1 - s) + sh * s
                r = r * (1 - s) + sr * s
            put(m, (a, h, r))
        elif cname == "cave":
            ra, rh, rr = shade_rock(Pm, Nm, seed + 50, 0.42)
            put(m, (ra, rh, rr))
        elif cname in ("seabed", "seabedrock"):
            if cname == "seabed":
                n1 = fbm3(Pm, 0.08, seed + 61, 3)
                n3 = fbm3(Pm, 2.0, seed + 62, 2)
                a = SEA_SAND * (0.75 + 0.35 * (0.5 + 0.5 * n1))[:, None] * (0.9 + 0.2 * n3)[:, None]
                ripple = np.sin(Pm[:, 2] * 1.2 + 2.5 * fbm3(Pm, 0.1, seed + 63, 2))
                put(m, (a, 0.03 * ripple + 0.01 * n3, np.ones(m.sum())))
            else:
                ra, rh, rr = shade_rock(Pm - np.array([0, 40, 0]), Nm, seed + 70, 0.55)
                put(m, (mix(ra, ALGAE * 2, 0.35), rh, rr))
    A = np.zeros((T, T, 3))
    H = np.zeros((T, T))
    Rg = np.ones((T, T))
    A[filled] = alb
    H[filled] = hgt
    Rg[filled] = rgh
    return A, H, Rg, filled


def shade_swatches(A, H, Rg, sw_origin, seed=0):
    """木の見本（針葉・広葉・幹）を 2D ノイズで描く"""
    S = SWATCH_PX
    yy, xx = np.meshgrid(np.arange(S), np.arange(S), indexing="ij")
    P = np.column_stack([xx.reshape(-1) * 0.06, yy.reshape(-1) * 0.06, np.zeros(S * S)])
    for name, (ox, oy) in sw_origin.items():
        n1 = fbm3(P, 1.0, seed + 81, 3)
        n2 = fbm3(P, 4.0, seed + 82, 2)
        t = (yy.reshape(-1) / S)  # 0 = 上（梢）
        if name == "pine":
            a = mix(np.array([0.018, 0.045, 0.025]), np.array([0.05, 0.11, 0.05]), np.clip(0.5 + 0.8 * n1 + 0.5 * n2, 0, 1))
            a = a * (1.15 - 0.4 * t)[:, None]
        elif name == "leaf":
            a = mix(np.array([0.03, 0.07, 0.015]), np.array([0.14, 0.24, 0.05]), np.clip(0.5 + 0.8 * n1 + 0.5 * n2, 0, 1))
            a = a * (1.15 - 0.35 * t)[:, None]
        else:
            a = mix(np.array([0.07, 0.05, 0.03]), np.array([0.17, 0.12, 0.07]),
                    np.clip(0.5 + 0.7 * np.sin(xx.reshape(-1) * 0.6 + 2 * n1), 0, 1))
        h = 0.03 * n2
        A[oy - 4:oy + S + 4, ox - 4:ox + S + 4] = np.pad(a.reshape(S, S, 3), ((4, 4), (4, 4), (0, 0)), mode="edge")
        H[oy - 4:oy + S + 4, ox - 4:ox + S + 4] = np.pad(h.reshape(S, S), 4, mode="edge")
        Rg[oy - 4:oy + S + 4, ox - 4:ox + S + 4] = 1.0


def height_to_normal(H, density):
    """高さ（m）→ 接空間の法線（x = +u、y = +v、z = 面の外向き）"""
    gy, gx = np.gradient(H)
    gx *= density  # 1 画素 = 1/density m
    gy *= density
    n = np.stack([-gx, -gy, np.ones_like(H)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def bake_and_write(model: BakedModel, out_dir: str) -> dict:
    t0 = time.time()
    P, N, UV, I, raster, density, sw_origin = assemble(model)
    T = model.tex
    G_P, G_N, G_C = rasterize(raster, T)
    A, H, Rg, filled = shade(model, G_P, G_N, G_C)
    if sw_origin:
        shade_swatches(A, H, Rg, sw_origin)
        for (ox, oy) in sw_origin.values():
            filled[oy - 4:oy + SWATCH_PX + 4, ox - 4:ox + SWATCH_PX + 4] = True
    A = dilate(A, filled, 8)
    H = dilate(H, filled, 8)
    Rg = dilate(Rg, filled, 8)
    Nm = height_to_normal(H, density)
    name = model.name
    save_rgb(A, os.path.join(out_dir, name + "_albedo.jpg"))
    # 法線・ラフネスは sRGB として読まれるので、読んだ後に元の値へ戻るよう sRGB でエンコードして保存する
    if T >= 2048:
        # 2048 の法線マップは読み込み（展開＋ミップ生成）が albedo より重い（Debug で 1.6 秒）。
        # 凹凸は遠目では効きが小さいので 1/2 に縮める（2x2 の平均を取り直して正規化）
        v = Nm * 2.0 - 1.0
        v = v.reshape(T // 2, 2, T // 2, 2, 3).mean(axis=(1, 3))
        v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
        Nm = v * 0.5 + 0.5
    save_rgb(Nm, os.path.join(out_dir, name + "_normal.jpg"), encode_srgb=True, quality=95)
    # ラフネスは全モデル共通の 4x4 の白（rough_none.png）。モデルごとに持つとロードの手間だけ増える
    save_rgb(np.ones((4, 4, 3)), os.path.join(out_dir, "rough_none.png"), encode_srgb=False)
    # 遠景：1/4 に縮小して空気の色へ寄せる
    small = np.asarray(Image.fromarray((lin_to_srgb(A) * 255).astype(np.uint8)).resize((T // 4, T // 4),
                                                                                    Image.LANCZOS)) / 255.0
    lin = np.where(small <= 0.04045, small / 12.92, ((small + 0.055) / 1.055) ** 2.4)
    save_rgb(lin * (1 - HAZE_AMOUNT) + HAZE * HAZE_AMOUNT, os.path.join(out_dir, name + "_far.jpg"))
    info = write_glb(name, P, N, UV, I, os.path.join(out_dir, name + ".glb"), name + "_albedo.jpg")
    info["texture"] = T
    info["px_per_m"] = round(float(density), 2)
    info["bake_sec"] = round(time.time() - t0, 1)
    return info


# ===========================================================================
# 海の岩（正規化）
# ===========================================================================
def build_searock(kind: str):
    m = BakedModel("searock_" + kind, tex=1024, params={"seed": 3})
    parts: List[BPart] = []
    if kind == "stack":
        parts += rock_column((0, 0, 0), -2.0, 2.0, 1.0, seed=11, lean=0.3, jag=0.22)
        parts += rock_column((0.75, 0, -0.55), -2.0, 0.6, 0.55, seed=12, lean=0.2)
        parts += rock_column((-0.6, 0, 0.6), -2.0, -0.2, 0.5, seed=13, squat=True)
        meters = (5.0, 10.0, 5.0)
    elif kind == "twin":
        parts += rock_column((-0.42, 0, 0.12), -2.0, 2.0, 0.78, seed=21, lean=0.25)
        parts += rock_column((0.5, 0, -0.18), -2.0, 1.15, 0.72, seed=22, lean=0.35)
        parts += rock_column((0.1, 0, 0.55), -2.0, -0.3, 0.6, seed=23, squat=True)
        meters = (5.0, 9.0, 5.0)
    else:
        parts += rock_column((0, 0, 0), -1.2, 1.2, 1.25, seed=31, squat=True, jag=0.18, top_raise=0.3, ny=14)
        parts += rock_column((0.95, 0, 0.45), -1.2, 0.2, 0.55, seed=32, squat=True, ny=9)
        meters = (6.0, 9.0, 6.0)
    normalize_unit(parts, meters)
    for p in parts:
        m.add(p)
    wl = waterline_points(parts, 0.0)
    return m, {"waterline": wl[:: max(1, len(wl) // 64)].round(4).tolist()}


# ===========================================================================
# 海底（正規化）
# ===========================================================================
def build_seabed():
    m = BakedModel("seabed", tex=1024, params={"seed": 5})
    W, L = 80.0, 300.0
    nx, nz = 40, 140
    xs = np.linspace(-W / 2, W / 2, nx + 1)
    zs = np.linspace(-L / 2, L / 2, nz + 1)
    X, Z = np.meshgrid(xs, zs)
    Y = (0.9 * np.sin(Z * 0.35 + 1.5 * fbm(X * 0.04, Z * 0.04, 5)) + 1.6 * fbm(X * 0.025, Z * 0.025, 9)
         + 0.4 * fbm(X * 0.2, Z * 0.2, 2))
    Y -= 1.2 * (np.abs(X) / (W / 2)) ** 4
    pos = np.stack([X, Y, Z], axis=-1).reshape(-1, 3)
    top = BPart(pos, grid_tris(nx, nz), pos[:, [0, 2]].copy(), cls="seabed")
    orient_by(top, lambda c: np.tile([0, 1.0, 0], (len(c), 1)))
    parts = [top]
    for k in range(18):
        x = (h01(k, 41) - 0.5) * (W - 10)
        z = (h01(k, 42) - 0.5) * (L - 10)
        r = 1.2 + 2.8 * h01(k, 43)
        for p in rock_column((x, 0, z), -1.5, r * 0.9, r, seed=400 + k, squat=True, ny=8, nseg=12, cls="seabedrock"):
            parts.append(p)
    # 箱の下半分（側面）。見えないので UV は小さく
    for xi, zi, xj, zj in ((-W / 2, -L / 2, W / 2, -L / 2), (W / 2, -L / 2, W / 2, L / 2),
                           (W / 2, L / 2, -W / 2, L / 2), (-W / 2, L / 2, -W / 2, -L / 2)):
        q = np.array([[xi, -5, zi], [xj, -5, zj], [xj, 0, zj], [xi, 0, zi]], float)
        sp = BPart(q, np.array([[0, 1, 2], [0, 2, 3]]), np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float),
                   cls="seabedrock", smooth=False)
        orient_outward(sp, (0, -2.5, 0))
        parts.append(sp)
    P = np.concatenate([p.pos for p in parts])
    lo, hi = P.min(axis=0), P.max(axis=0)
    c, s = (lo + hi) / 2, hi - lo
    for p in parts:
        p.bake_pos = p.pos.copy() + np.array([0, -28.0, 0])  # 実際の深さ（y ≒ -28）でノイズを取る
        p.pos = (p.pos - c) / s
        m.add(p)
    return m, {}


# ===========================================================================
# 洞窟の口が開いた断崖（m 単位）
# ===========================================================================
# 原点 = 旧 CliffA/B の中心（x = 0, 水面 y = 0, 崖の厚み 10m の中央）。前面は -z を向く。
# 内側の洞窟（generate_stages.py の cave_shell）は崖の裏 z = +5 から始まり、
#   壁の内側 |x| ≒ 16〜18、天井の下面 y ≒ 12、壁の外側 |x| ≦ 26、天井の上面 y ≦ 21
# に収まっている。崖の裏面（z = +5）はこの断面を覆うように |x| ≦ 34, y ≦ 26 まで張る。
CLIFF_FRONT = -5.0
CLIFF_BACK = 5.0
CLIFF_HALF = 150.0
CLIFF_BOTTOM = -30.0
ARCH_HW, ARCH_SPRING, ARCH_TOP = 11.0, 6.0, 17.5     # 前面の口（外側を大きく、漏斗状に）
BACK_HW, BACK_SPRING, BACK_TOP = 8.6, 4.0, 11.5      # 裏面の口（洞窟の断面に合わせる）
WIN_X, WIN_Y = 24.0, 24.0


def cliff_bend(x):
    ax = np.abs(np.asarray(x, float))
    t = np.clip((ax - 38.0) / (CLIFF_HALF - 38.0), 0.0, 1.0)
    return 95.0 * t * t


def cliff_top(x):
    x = np.asarray(x, float)
    ax = np.abs(x)
    h = 52.0 + 7.0 * fbm(x * 0.018, 3.7, 61) + 2.5 * fbm(x * 0.09, 1.1, 62)
    return h * (1.0 - 0.93 * np.clip((ax - 80.0) / (CLIFF_HALF - 80.0), 0.0, 1.0) ** 1.4)


def cliff_disp(x, y):
    """前面の凹凸（-z 方向への張り出し m）。柱状の筋・地層の段・岩塊・波食ノッチ・天端のひさし"""
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    streak = 1.6 * fbm(x * 0.12, y * 0.022, 71) + 0.5 * fbm(x * 0.4, y * 0.06, 74)
    bump = 1.1 * fbm(x * 0.05, y * 0.05, 72) + 0.35 * fbm(x * 0.25, y * 0.25, 75)
    ph = y / 7.5 + 0.4 * fbm(x * 0.02, 0.5, 73)
    ledge = 1.0 * smoothstep_arr(0.55, 0.8, ph - np.floor(ph))
    notch = -1.3 * np.exp(-((y - 0.6) / 1.4) ** 2)
    top = cliff_top(x)
    lip = 1.4 * np.exp(-((y - top) / 2.0) ** 2)
    d = streak + bump + ledge + notch + lip
    r = np.sqrt((x / ARCH_HW) ** 2 + (np.maximum(y - ARCH_SPRING, 0) / (ARCH_TOP - ARCH_SPRING)) ** 2)
    calm = 0.2 + 0.8 * smoothstep_arr(1.0, 2.6, r)
    return d * np.where(y > -6.0, calm, calm * 0.6)


def cliff_z(x, y):
    return CLIFF_FRONT + cliff_bend(x) - cliff_disp(x, y)


# 崖の面の横方向の展開（曲がった両端で模様が横に伸びないよう、根元の曲線の弧長を使う）
_SX = np.linspace(-CLIFF_HALF, CLIFF_HALF, 3001)
_SC = np.concatenate([[0.0], np.cumsum(np.hypot(np.diff(_SX), np.diff(cliff_bend(_SX))))])


def cliff_s(x):
    return np.interp(x, _SX, _SC)


def arch_from_outer(outer2d, hw0, spring, top):
    out = np.zeros_like(outer2d)
    for i, (x, y) in enumerate(outer2d):
        if y <= spring:
            hw = hw0 + 0.6 * max(-y, 0.0) / 30.0
            out[i] = [math.copysign(hw, x if abs(x) > 1e-9 else -1.0), y]
        else:
            th = math.atan2(y - spring, x)
            k = max(math.sin(th), 0.0) ** 0.8
            out[i] = [hw0 * math.cos(th), spring + (top - spring) * k]
    return out


def roughen_outline(poly, amp, seed):
    out = poly.copy()
    for i in range(len(out)):
        x, y = out[i]
        d = np.array([x, y - ARCH_SPRING]) if y > ARCH_SPRING else np.array([math.copysign(1.0, x), 0.0])
        d /= max(np.linalg.norm(d), 1e-9)
        out[i] += d * amp * (0.6 * h01(i, seed) + 0.4 * h01(i // 3, seed + 1))
    return out


def build_cliff():
    m = BakedModel("cliff_cave", tex=2048, params={"seed": 7, "beach": 1.0})
    parts: List[BPart] = []
    # 縦の展開：水面下（ほとんど見えない）は 1/4 に詰めて、水面より上に解像度を回す
    def face_v(y):
        y = np.asarray(y, float)
        return np.where(y >= -2.0, 70.0 - y, 72.0 + (-2.0 - y) * 0.25)
    face_uv = lambda P: np.column_stack([cliff_s(P[:, 0]), face_v(P[:, 1])])  # noqa: E731

    # --- 前面（格子 + 口のまわりの窓）---
    xs = np.concatenate([np.linspace(-CLIFF_HALF, -WIN_X, 64), np.linspace(-WIN_X, WIN_X, 25)[1:-1],
                         np.linspace(WIN_X, CLIFF_HALF, 64)])
    nu = len(xs) - 1
    J1, J2 = 27, 16
    nv = J1 + J2
    il = int(np.argmin(np.abs(xs + WIN_X)))
    ir = int(np.argmin(np.abs(xs - WIN_X)))
    top = cliff_top(xs)
    ymid = np.minimum(WIN_Y, 0.8 * top)
    Yg = np.zeros((nv + 1, nu + 1))
    for i in range(nu + 1):
        Yg[:, i] = np.concatenate([np.linspace(CLIFF_BOTTOM, ymid[i], J1 + 1), np.linspace(ymid[i], top[i], J2 + 1)[1:]])
    Xg = np.tile(xs, (nv + 1, 1))
    Zg = cliff_z(Xg, Yg)
    front_pos = np.stack([Xg, Yg, Zg], axis=-1).reshape(-1, 3)
    front = BPart(front_pos, grid_tris(nu, nv, skip=lambda i, j: il <= i < ir and j < J1), face_uv(front_pos),
                  cls="rockcol")
    parts.append(front)

    def gp(i, j):
        return front_pos[j * (nu + 1) + i]
    outer = np.array([gp(il, j) for j in range(0, J1 + 1)] + [gp(i, J1) for i in range(il + 1, ir)] +
                     [gp(ir, j) for j in range(J1, -1, -1)])
    fr = arc_fracs(outer[:, :2])
    inner2d = roughen_outline(arch_from_outer(outer[:, :2], ARCH_HW, ARCH_SPRING, ARCH_TOP), 0.9, 91)
    K = 9
    rows = []
    for k in range(K + 1):
        t = k / K
        if k == K:
            rows.append(outer)
            continue
        xy = inner2d * (1 - t) + outer[:, :2] * t
        rows.append(np.column_stack([xy, cliff_z(xy[:, 0], xy[:, 1])]))
    wp, wt = strip(rows)
    window = BPart(wp, wt, face_uv(wp), cls="rock")
    parts.append(window)
    for p in (front, window):
        orient_by(p, lambda c: np.tile([0.0, 0.0, -1.0], (len(c), 1)) + np.column_stack(
            [np.sign(c[:, 0]) * (np.abs(c[:, 0]) > 40) * 1.2, np.zeros(len(c)), np.zeros(len(c))]))
    # 前面と窓の境目で法線がつながるよう、両方を 1 つのパーツにまとめる
    merged = BPart(np.concatenate([front.pos, window.pos]), np.concatenate([front.tris, window.tris + len(front.pos)]),
                   np.concatenate([front.uvp, window.uvp]), cls="rockcol")
    # 横 330m の面を 1 枚で置くとアトラスの幅で解像度が決まってしまうので、左右に分けて 2 段に積む
    parts = split_part(merged, lambda c: c[:, 0] < 0.0)

    # --- トンネル ---
    front_ring = rows[0]
    BX, BY = 34.0, 26.0
    rect = np.array([[-BX, CLIFF_BOTTOM], [-BX, BY], [BX, BY], [BX, CLIFF_BOTTOM]])
    dense = np.concatenate([np.linspace(rect[k], rect[k + 1], 60, endpoint=False) for k in range(3)] + [rect[-1:]])
    outer_b = resample(dense, fr)
    back2d = roughen_outline(arch_from_outer(outer_b, BACK_HW, BACK_SPRING, BACK_TOP), 0.6, 92)
    back_ring = np.column_stack([back2d, np.full(len(back2d), CLIFF_BACK)])
    M = 6
    trows = []
    for k in range(M + 1):
        t = k / M
        r = front_ring * (1 - t) + back_ring * t
        if 0 < k < M:
            r = r.copy()
            for i in range(len(r)):
                x, y = r[i, 0], r[i, 1]
                d = np.array([math.copysign(1.0, x), 0.0]) if y < ARCH_SPRING else \
                    np.array([x, y - ARCH_SPRING]) / max(math.hypot(x, y - ARCH_SPRING), 1e-6)
                r[i, :2] += d * 0.8 * h01(i * 17 + k, 93)
        trows.append(r)
    tp, tt = strip(trows)
    n_out = len(front_ring)
    s_ring = arc_len(front_ring[:, :2])
    tuv = np.column_stack([np.tile(s_ring, M + 1), np.repeat(np.linspace(0, 10.0, M + 1), n_out)])
    tunnel = BPart(tp, tt, tuv, cls="cave")
    orient_by(tunnel, lambda c: np.column_stack([-c[:, 0], -np.maximum(c[:, 1] - ARCH_SPRING, 0.0), np.zeros(len(c))]))
    parts.append(tunnel)

    # --- 裏面 ---
    brows = []
    for k in range(4):
        t = k / 3
        xy = back2d * (1 - t) + outer_b * t
        brows.append(np.column_stack([xy, CLIFF_BACK + 0.6 * fbm(xy[:, 0] * 0.15, xy[:, 1] * 0.15, 95) * t]))
    bp, bt = strip(brows)
    back = BPart(bp, bt, np.column_stack([bp[:, 0] + BX, BY - bp[:, 1]]), cls="cave", smooth=True)
    orient_by(back, lambda c: np.tile([0.0, 0.0, 1.0], (len(c), 1)))
    # 裏面は洞窟の中からしか見えない暗がりなので解像度はいらない（展開を縮めて場所を空ける）
    back.uvp *= 0.35
    parts.append(back)

    # --- 天端（草の台地）と奥への垂れ壁 ---
    tdepth = 34.0 * (1.0 - smoothstep_arr(95.0, CLIFF_HALF, np.abs(xs))) + 2.0
    ts = np.array([0.0, 0.03, 0.08, 0.16, 0.28, 0.45, 0.7, 1.0])
    rim = np.stack([Xg[-1], Yg[-1], Zg[-1]], axis=-1)
    plat = []
    for t in ts:
        y = rim[:, 1] + (1.5 * fbm(xs * 0.06, t * 4.0, 97) + 2.0) * min(t * 4, 1.0) * (top / 44.0)
        plat.append(np.column_stack([rim[:, 0], y, rim[:, 2] + t * tdepth]))
    pp, pt = strip(plat)
    puv = np.column_stack([cliff_s(pp[:, 0]), np.repeat(ts, len(xs)) * np.tile(tdepth, len(ts))])
    plateau = BPart(pp, pt, puv * np.array([1.0, 1.0]), cls="plateau")
    orient_by(plateau, lambda c: np.tile([0.0, 1.0, 0.0], (len(c), 1)))
    plateau.uvp *= np.array([0.45, 0.6])  # 台地は海面の高さからほぼ見えないので解像度を落とす
    parts.append(plateau)
    back_edge = plat[-1]
    skirt_bottom = back_edge.copy()
    skirt_bottom[:, 1] = np.minimum(18.0, back_edge[:, 1] - 1.0)
    sp_, st_ = strip([back_edge, skirt_bottom])
    skirt = BPart(sp_, st_, np.column_stack([cliff_s(sp_[:, 0]) * 0.1, np.r_[np.zeros(len(xs)), np.ones(len(xs))]]),
                  cls="rock", smooth=False)
    orient_by(skirt, lambda c: np.tile([0.0, 0.0, 1.0], (len(c), 1)))
    parts.append(skirt)

    # --- 天端の木 ---
    k = 0
    for i in range(1, nu):
        if top[i] < 9.0 or tdepth[i] < 6.0:
            continue
        for rep in range(3):
            if h01(i * 7 + rep, 101) > 0.55:
                continue
            t = 0.03 + 0.55 * h01(i * 11 + rep, 102)
            xx = xs[i] + (h01(i * 13 + rep, 103) - 0.5) * 2.0
            yy = np.interp(t, ts, [pl[i, 1] for pl in plat]) - 0.3
            zz = np.interp(t, ts, [pl[i, 2] for pl in plat])
            parts += tree((xx, yy, zz), seed=500 + k, size=1.3)
            k += 1

    # --- 崖の根元の岩 ---
    for b in range(16):
        side = -1 if b % 2 == 0 else 1
        x = side * (30.0 + 105.0 * h01(b, 111))
        R = 2.0 + 4.0 * h01(b, 112)
        z = float(cliff_z(x, 0.0)) - R * (0.2 + 0.6 * h01(b, 113))
        H = R * (0.6 + 1.2 * h01(b, 114))
        parts += rock_column((x, 0, z), CLIFF_BOTTOM * 0.4, H, R, seed=600 + b, squat=h01(b, 115) > 0.5, nseg=14)

    for p in parts:
        m.add(p)
    return m, {"front_z": CLIFF_FRONT, "back_z": CLIFF_BACK, "half_width": CLIFF_HALF}


# ===========================================================================
# 島（m 単位・遠景）
# ===========================================================================
def heightfield_island(R, H, seed, cliff=0.5, beach=1.2, res=40, peak=0.35, shelf=0.9) -> BPart:
    S = R * 1.35
    n = res
    xs = np.linspace(-S, S, n + 1)
    X, Z = np.meshgrid(xs, xs)
    step = xs[1] - xs[0]
    rs = np.random.RandomState(seed)
    X = X + (rs.rand(*X.shape) - 0.5) * step * 0.3
    Z = Z + (rs.rand(*Z.shape) - 0.5) * step * 0.3
    ang = np.arctan2(Z, X)
    Reff = R * (1.0 + 0.24 * fbm(np.cos(ang) * 1.6 + 3.0, np.sin(ang) * 1.6, seed))
    r = np.sqrt(X * X + Z * Z) / Reff
    cl = np.clip(cliff + 0.55 * fbm(np.cos(ang) * 1.2 - 2.0, np.sin(ang) * 1.2 + 5.0, seed + 9), 0.0, 1.0)
    wall = smoothstep_arr(shelf, shelf - 0.07, r)
    dome = smoothstep_arr(shelf, 0.15, r)
    prof = cl * wall * (0.72 + 0.28 * dome) + (1.0 - cl) * dome
    nz = fbm(X * 0.045, Z * 0.045, seed + 1)
    land = (H * prof * (1.0 + peak * np.maximum(nz, -0.3) * dome) + 0.6 * fbm(X * 0.2, Z * 0.2, seed + 2) * prof
            + 1.2 * fbm(X * 0.09, Z * 0.09, seed + 3) * wall * cl)
    beach_h = beach * (1.0 - smoothstep_arr(shelf, 1.0, r))
    Y = np.maximum(land, beach_h)
    Y = np.where(r > 1.0, -np.clip((r - 1.0) * 70.0, 0.0, 30.0), Y)
    pos = np.stack([X, Y, Z], axis=-1).reshape(-1, 3)
    tris = grid_tris(n, n)
    keep = ~(pos[tris][:, :, 1] < -29.9).all(axis=1)
    part = BPart(pos, tris[keep], pos[:, [0, 2]].copy(), cls="terrain")
    return orient_by(part, lambda c: np.tile([0.0, 1.0, 0.0], (len(c), 1)))


def scatter_trees(ground: BPart, seed, density=1.0, min_y=2.0, spacing=6.0, size=1.0, max_n=80, kinds=None):
    P, T = ground.pos, ground.tris
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    n = np.cross(b - a, c - a)
    area = np.linalg.norm(n, axis=1) * 0.5
    nn = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    cen = (a + b + c) / 3
    idx = np.where((nn[:, 1] > 0.8) & (cen[:, 1] > min_y))[0]
    rs = np.random.RandomState(seed)
    rs.shuffle(idx)
    placed, parts = [], []
    for f in idx:
        if len(placed) >= max_n:
            break
        if rs.rand() > density * min(1.0, area[f] / 5.0):
            continue
        u, v = rs.rand(), rs.rand()
        if u + v > 1:
            u, v = 1 - u, 1 - v
        p = a[f] + (b[f] - a[f]) * u + (c[f] - a[f]) * v
        if any(np.hypot(p[0] - q[0], p[2] - q[2]) < spacing for q in placed):
            continue
        placed.append(p)
        kind = None if kinds is None else kinds[len(placed) % len(kinds)]
        parts += tree((p[0], p[1] - 0.2, p[2]), seed=seed * 1000 + len(placed), size=size, kind=kind)
    return parts


def build_island(kind: str):
    parts: List[BPart] = []
    if kind == "large":
        # レールから 135m 以上離れた遠景なので 1024 で足りる（2048 だとロードが 1 秒以上かかっていた）
        m = BakedModel("island_large", tex=1024, params={"seed": 9, "beach": 0.9})
        g = heightfield_island(R=72.0, H=32.0, seed=201, cliff=0.75, beach=0.9, res=96, peak=0.45, shelf=0.96)
        parts.append(g)
        parts += scatter_trees(g, 2011, density=0.9, min_y=4.0, spacing=5.5, size=1.4, max_n=130)
        for k in range(4):
            th = 2 * math.pi * (k / 4 + 0.13 * h01(k, 202))
            R = 4.0 + 3.0 * h01(k, 203)
            parts += rock_column((math.cos(th) * 74, 0, math.sin(th) * 74), -30, 10 + 14 * h01(k, 204), R,
                                 seed=2020 + k, nseg=16)
    elif kind == "medium":
        m = BakedModel("island_medium", tex=1024, params={"seed": 19, "beach": 1.3})
        g = heightfield_island(R=42.0, H=18.0, seed=211, cliff=0.45, beach=1.3, res=68, peak=0.5, shelf=0.9)
        parts.append(g)
        parts += scatter_trees(g, 2111, density=1.0, min_y=3.0, spacing=4.5, size=1.2, max_n=70)
    elif kind == "low":
        m = BakedModel("island_low", tex=1024, params={"seed": 29, "beach": 1.4})
        g = heightfield_island(R=26.0, H=4.5, seed=221, cliff=0.1, beach=1.4, res=48, peak=0.3, shelf=0.7)
        parts.append(g)
        parts += scatter_trees(g, 2211, density=1.0, min_y=1.8, spacing=4.0, size=1.0, max_n=18, kinds=["leaf"])
    else:
        m = BakedModel("island_stacks", tex=1024, params={"seed": 39, "beach": 0.8})
        g = heightfield_island(R=30.0, H=3.0, seed=231, cliff=0.3, beach=0.8, res=40, peak=0.2, shelf=0.75)
        parts.append(g)
        specs = [((0, 0, 0), 9.0, 42.0), ((-17, 0, 9), 6.5, 28.0), ((15, 0, -8), 7.0, 33.0), ((6, 0, 18), 5.0, 20.0),
                 ((-10, 0, -16), 4.5, 15.0)]
        for k, (c, R, H) in enumerate(specs):
            col = rock_column(c, -30.0, H, R, seed=2300 + k, nseg=20, lean=0.15, jag=0.22, ledge=0.09)
            parts += col
            if R >= 5.0:
                topc = col[1].pos[-1]
                for t in range(int(R / 2.0)):
                    off = np.array([(h01(k * 10 + t, 232) - 0.5) * R * 0.8, 0, (h01(k * 10 + t, 233) - 0.5) * R * 0.8])
                    parts += tree(topc + off + np.array([0, -R * 0.25, 0]), seed=2400 + k * 10 + t, size=1.1,
                                  kind="pine")
    for p in parts:
        m.add(p)
    return m, {}


# ===========================================================================
BUILDERS = {
    "searock_stack": lambda: build_searock("stack"),
    "searock_twin": lambda: build_searock("twin"),
    "searock_boulder": lambda: build_searock("boulder"),
    "seabed": build_seabed,
    "cliff_cave": build_cliff,
    "island_large": lambda: build_island("large"),
    "island_medium": lambda: build_island("medium"),
    "island_low": lambda: build_island("low"),
    "island_stacks": lambda: build_island("stacks"),
}


def main(argv):
    names = [a for a in argv if not a.startswith("-")] or list(BUILDERS)
    os.makedirs(OUT, exist_ok=True)
    meta = {}
    if os.path.exists(META):
        with open(META, encoding="utf-8") as f:
            meta = json.load(f)
    for name in names:
        model, extra = BUILDERS[name]()
        info = bake_and_write(model, OUT)
        info.update(extra)
        meta[name] = info
        print(f"{name}: {info['vertices']} verts / {info['triangles']} tris / tex {info['texture']} "
              f"({info['px_per_m']} px/m) {info['bake_sec']}s")
    # 旧版（パレット）の残りを消す
    for old in ("palette.png", "palette_far.png"):
        p = os.path.join(OUT, old)
        if os.path.exists(p) and not os.environ.get("MODELGEN_OUT"):
            os.remove(p)
    with open(META, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
