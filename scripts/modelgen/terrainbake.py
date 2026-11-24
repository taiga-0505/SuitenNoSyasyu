# -*- coding: utf-8 -*-
"""
地形モデル用の「1 モデル = 1 メッシュ = 1 マテリアル」で質感を出すための道具。

  - なめらかな法線（同じ位置の頂点をまとめて平均。ローポリの面がカクカク見えない）
  - モデルごとに重ならない UV（アトラス）を割り当て、
    UV 空間にメッシュをラスタライズして「その画素のワールド位置・法線」を求め、
    手続きノイズで albedo / 法線マップ / ラフネスを焼き込む。

描画は今までどおり 1 モデル 1 ドローなので CPU（コマンド記録）の負担は増えない。
質感はテクスチャで、形はポリゴン数で出す（GPU 側に余裕がある前提）。

エンジンの事情:
  - ModelRenderer の normalMapPath / roughnessMapPath は sRGB として読まれる（RC::LoadTex の既定）。
    そのまま線形の値を入れるとデコードで歪むので、書き出し時に sRGB へエンコードしておく（encode_for_srgb_load）。
  - 法線マップの接空間はシェーダが ddx/ddy から作る（T = dP/du, B = dP/dv）。接線は要らない。
"""
from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------
# ノイズ（3D の値ノイズ。焼き込みは画素ごとのワールド位置で評価するので、UV の継ぎ目で模様が切れない）
# ---------------------------------------------------------------------------


def _hash3(ix, iy, iz, seed):
    h = (ix * 374761393 + iy * 668265263 + iz * 2147483647 + seed * 1442695041) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return h.astype(np.float64) * (1.0 / 4294967296.0)


def vnoise3(x, y, z, seed=0):
    """-1..1"""
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    z = np.asarray(z, np.float64)
    x0 = np.floor(x)
    y0 = np.floor(y)
    z0 = np.floor(z)
    fx, fy, fz = x - x0, y - y0, z - z0
    ux = fx * fx * (3 - 2 * fx)
    uy = fy * fy * (3 - 2 * fy)
    uz = fz * fz * (3 - 2 * fz)
    ix = x0.astype(np.int64)
    iy = y0.astype(np.int64)
    iz = z0.astype(np.int64)

    def h(a, b, c):
        return _hash3(ix + a, iy + b, iz + c, seed)
    x00 = h(0, 0, 0) + (h(1, 0, 0) - h(0, 0, 0)) * ux
    x10 = h(0, 1, 0) + (h(1, 1, 0) - h(0, 1, 0)) * ux
    x01 = h(0, 0, 1) + (h(1, 0, 1) - h(0, 0, 1)) * ux
    x11 = h(0, 1, 1) + (h(1, 1, 1) - h(0, 1, 1)) * ux
    y0_ = x00 + (x10 - x00) * uy
    y1_ = x01 + (x11 - x01) * uy
    return (y0_ + (y1_ - y0_) * uz) * 2.0 - 1.0


def fbm3(P, freq, seed=0, octaves=4, stretch=(1.0, 1.0, 1.0)):
    """P: (K,3)。stretch で軸ごとに周波数を変える（縦の筋など）"""
    s = np.zeros(len(P))
    amp, norm, f = 1.0, 0.0, freq
    for k in range(octaves):
        s += amp * vnoise3(P[:, 0] * f * stretch[0], P[:, 1] * f * stretch[1], P[:, 2] * f * stretch[2], seed + k * 131)
        norm += amp
        amp *= 0.5
        f *= 2.07
    return s / norm


def ridged3(P, freq, seed=0, octaves=3, stretch=(1.0, 1.0, 1.0)):
    """尾根状（0..1、1 が谷の線＝割れ目）"""
    return 1.0 - np.abs(fbm3(P, freq, seed, octaves, stretch))


def sstep(e0, e1, x):
    t = np.clip((np.asarray(x, float) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    t = np.asarray(t)[..., None] if np.ndim(t) == 1 else t
    return np.asarray(a) * (1 - t) + np.asarray(b) * t


# ---------------------------------------------------------------------------
# パーツ
# ---------------------------------------------------------------------------


@dataclass
class BPart:
    """三角形メッシュ 1 つ。uvp は UV の「m 単位の展開座標」（アトラス上の置き場所は後で決める）"""
    pos: np.ndarray                    # (N,3) エンジン空間（+X 右 / +Y 上 / +Z 前）
    tris: np.ndarray                   # (M,3) 外向きが右手の外積の向き
    uvp: Optional[np.ndarray] = None   # (N,2) m 単位。None なら swatch を使う
    cls: str = "rock"                  # 焼き込みの材質クラス
    swatch: Optional[str] = None       # 木などの共有見本（"pine" / "leaf" / "trunk"）
    smooth: bool = True
    bake_pos: Optional[np.ndarray] = None  # ノイズを評価する位置（正規化モデルで実寸に近い座標を使う）
    name: str = ""
    nrm: Optional[np.ndarray] = None   # 法線を先に決めてある場合（分割したパーツの継ぎ目で法線をそろえる）

    def copy(self) -> "BPart":
        return BPart(self.pos.copy(), self.tris.copy(), None if self.uvp is None else self.uvp.copy(), self.cls,
                     self.swatch, self.smooth, None if self.bake_pos is None else self.bake_pos.copy(), self.name)


def split_part(part: BPart, side: Callable[[np.ndarray], np.ndarray]) -> List[BPart]:
    """三角形の重心で 2 つに分ける（UV の置き場所を分けて解像度を稼ぐ用）。法線は分ける前に決めておく"""
    P, N, T, uvp, bp = vertex_normals(part)
    cen = P[T].mean(axis=1)
    out = []
    for m in (side(cen), ~side(cen)):
        tt = T[m]
        used = np.unique(tt)
        remap = np.full(len(P), -1)
        remap[used] = np.arange(len(used))
        out.append(BPart(P[used], remap[tt], None if uvp is None else uvp[used], part.cls, part.swatch, True,
                         None if bp is None else bp[used], part.name, N[used]))
    return out


def orient_by(part: BPart, desired: Callable[[np.ndarray], np.ndarray]) -> BPart:
    P, T = part.pos, part.tris
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    n = np.cross(b - a, c - a)
    d = desired((a + b + c) / 3.0)
    bad = np.einsum("ij,ij->i", n, d) < 0
    T = T.copy()
    T[bad] = T[bad][:, ::-1]
    part.tris = T
    return part


def orient_outward(part: BPart, center) -> BPart:
    P, T = part.pos, part.tris
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    n = np.cross(b - a, c - a)
    if np.einsum("ij,ij->i", n, (a + b + c) / 3 - np.asarray(center)).sum() < 0:
        part.tris = part.tris[:, ::-1].copy()
    return part


def vertex_normals(part: BPart) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Optional[np.ndarray], Optional[np.ndarray]]:
    """(pos, nrm, tris, uvp, bake_pos)。smooth は同じ位置の頂点をまとめて平均、flat は面ごとに頂点を分ける"""
    if part.nrm is not None:
        return part.pos, part.nrm, part.tris, part.uvp, part.bake_pos
    P, T = part.pos, part.tris
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    fn = np.cross(b - a, c - a)
    keep = np.linalg.norm(fn, axis=1) > 1e-10
    T = T[keep]
    fn = fn[keep]
    if not part.smooth:
        idx = T.reshape(-1)
        pos = P[idx]
        nrm = np.repeat(fn / np.linalg.norm(fn, axis=1, keepdims=True), 3, axis=0)
        uvp = None if part.uvp is None else part.uvp[idx]
        bp = None if part.bake_pos is None else part.bake_pos[idx]
        return pos, nrm, np.arange(len(idx)).reshape(-1, 3), uvp, bp
    key = np.round(P * 1e4).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    acc = np.zeros((inv.max() + 1, 3))
    for k in range(3):
        np.add.at(acc, inv[T[:, k]], fn)
    nrm = acc[inv]
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
    return P, nrm, T, part.uvp, part.bake_pos


# ---------------------------------------------------------------------------
# アトラス
# ---------------------------------------------------------------------------
SWATCH_PX = 128
PAD = 4


def pack(sizes: List[Tuple[float, float]], tex: int, swatches: int) -> Tuple[float, List[Tuple[int, int]]]:
    """m 単位の矩形を tex×tex に棚詰めする。入る最大の密度（px/m）と各矩形の左上（px）を返す。
    見本（swatch）は右下に SWATCH_PX 四方で並べる（棚詰めの前に場所を取っておく）"""
    sw_h = SWATCH_PX + PAD * 2 if swatches else 0

    def try_pack(d):
        rects = [(int(math.ceil(w * d)) + PAD * 2, int(math.ceil(h * d)) + PAD * 2) for w, h in sizes]
        order = sorted(range(len(rects)), key=lambda i: -rects[i][1])
        x = y = shelf = 0
        out = [None] * len(rects)
        for i in order:
            rw, rh = rects[i]
            if rw > tex:
                return None
            if x + rw > tex:
                x = 0
                y += shelf
                shelf = 0
            if y + rh > tex - sw_h:
                return None
            out[i] = (x, y)
            x += rw
            shelf = max(shelf, rh)
        return out
    lo, hi = 0.01, 400.0
    best = None
    for _ in range(40):
        mid = (lo + hi) / 2
        r = try_pack(mid)
        if r is None:
            hi = mid
        else:
            lo = mid
            best = (mid, r)
    if best is None:
        raise RuntimeError("atlas packing failed")
    return best


# ---------------------------------------------------------------------------
# モデル
# ---------------------------------------------------------------------------


@dataclass
class BakedModel:
    name: str
    tex: int = 1024
    parts: List[BPart] = field(default_factory=list)
    params: Dict = field(default_factory=dict)   # 焼き込みの調整（beach の高さなど）

    def add(self, p: BPart) -> BPart:
        self.parts.append(p)
        return p


SWATCH_ORDER = ["pine", "leaf", "trunk"]


def assemble(model: BakedModel):
    """法線・UV を決めて最終の頂点列と、焼き込み用のラスタ情報を返す"""
    built = []
    sizes = []
    for p in model.parts:
        pos, nrm, tris, uvp, bp = vertex_normals(p)
        if p.swatch is None:
            lo = uvp.min(axis=0)
            uvp = uvp - lo
            sizes.append(tuple(np.maximum(uvp.max(axis=0), 0.05)))
        built.append([p, pos, nrm, tris, uvp, bp])
    has_sw = any(p.swatch for p in model.parts)
    T = model.tex
    density, origins = pack(sizes, T, has_sw)
    sw_origin = {name: (PAD + k * (SWATCH_PX + PAD * 2), T - SWATCH_PX - PAD) for k, name in enumerate(SWATCH_ORDER)}
    P_all, N_all, UV_all, I_all, raster = [], [], [], [], []
    base = 0
    k = 0
    for p, pos, nrm, tris, uvp, bp in built:
        if p.swatch is None:
            ox, oy = origins[k]
            k += 1
            px = np.column_stack([ox + PAD + uvp[:, 0] * density, oy + PAD + uvp[:, 1] * density])
            raster.append((p, pos if bp is None else bp, nrm, tris, px))
        else:
            ox, oy = sw_origin[p.swatch]
            c = pos.mean(axis=0)
            ang = np.arctan2(pos[:, 2] - c[2], pos[:, 0] - c[0]) / (2 * math.pi) + 0.5
            hgt = (pos[:, 1] - pos[:, 1].min()) / max(np.ptp(pos[:, 1]), 1e-6)
            px = np.column_stack([ox + 2 + ang * (SWATCH_PX - 4), oy + 2 + (1 - hgt) * (SWATCH_PX - 4)])
        uv = px / T
        P_all.append(pos)
        N_all.append(nrm)
        UV_all.append(uv)
        I_all.append(tris + base)
        base += len(pos)
    return (np.concatenate(P_all), np.concatenate(N_all), np.concatenate(UV_all), np.concatenate(I_all),
            raster, density, sw_origin if has_sw else {})


# ---------------------------------------------------------------------------
# UV 空間のラスタライズ（G バッファ：ワールド位置・法線・材質クラス）
# ---------------------------------------------------------------------------
CLASSES = ["rock", "terrain", "rockcol", "cave", "seabed", "seabedrock", "plateau"]


def rasterize(raster, T):
    G_P = np.zeros((T, T, 3), np.float32)
    G_N = np.zeros((T, T, 3), np.float32)
    G_C = np.full((T, T), -1, np.int16)
    for part, pos, nrm, tris, px in raster:
        cid = CLASSES.index(part.cls)
        A, B, C = px[tris[:, 0]], px[tris[:, 1]], px[tris[:, 2]]
        area = (B[:, 0] - A[:, 0]) * (C[:, 1] - A[:, 1]) - (B[:, 1] - A[:, 1]) * (C[:, 0] - A[:, 0])
        for f in range(len(tris)):
            if abs(area[f]) < 1e-12:
                continue
            xs = (A[f, 0], B[f, 0], C[f, 0])
            ys = (A[f, 1], B[f, 1], C[f, 1])
            x0 = max(int(math.floor(min(xs))) - 1, 0)
            x1 = min(int(math.ceil(max(xs))) + 1, T - 1)
            y0 = max(int(math.floor(min(ys))) - 1, 0)
            y1 = min(int(math.ceil(max(ys))) + 1, T - 1)
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((B[f, 0] - gx) * (C[f, 1] - gy) - (B[f, 1] - gy) * (C[f, 0] - gx)) / area[f]
            w1 = ((C[f, 0] - gx) * (A[f, 1] - gy) - (C[f, 1] - gy) * (A[f, 0] - gx)) / area[f]
            w2 = 1.0 - w0 - w1
            # 縁の画素も拾う（少し外側まで）
            m = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
            if not m.any():
                continue
            ia, ib, ic = tris[f]
            wp = w0[m, None] * pos[ia] + w1[m, None] * pos[ib] + w2[m, None] * pos[ic]
            wn = w0[m, None] * nrm[ia] + w1[m, None] * nrm[ib] + w2[m, None] * nrm[ic]
            yy = gy[m].astype(int)
            xx = gx[m].astype(int)
            G_P[yy, xx] = wp
            G_N[yy, xx] = wn
            G_C[yy, xx] = cid
    n = np.linalg.norm(G_N, axis=-1, keepdims=True)
    G_N = np.where(n > 1e-6, G_N / np.maximum(n, 1e-6), 0)
    return G_P, G_N, G_C


def dilate(img: np.ndarray, mask: np.ndarray, iters: int = 6) -> np.ndarray:
    """空の画素を隣の値で埋める（ミップで隣の島の色が混ざらないように）"""
    img = img.copy()
    mask = mask.copy()
    for _ in range(iters):
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            src = np.roll(np.roll(img, dy, axis=0), dx, axis=1)
            sm = np.roll(np.roll(mask, dy, axis=0), dx, axis=1)
            fill = (~mask) & sm
            img[fill] = src[fill]
            mask = mask | fill
    return img


# ---------------------------------------------------------------------------
# 書き出し
# ---------------------------------------------------------------------------


def lin_to_srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def save_rgb(arr_lin_or_raw: np.ndarray, path: str, encode_srgb: bool = True, quality: int = 92):
    a = lin_to_srgb(arr_lin_or_raw) if encode_srgb else np.clip(arr_lin_or_raw, 0, 1)
    img = Image.fromarray((a * 255 + 0.5).astype(np.uint8), "RGB")
    if path.lower().endswith(".jpg"):
        img.save(path, quality=quality, subsampling=0)
    else:
        img.save(path)


def write_glb(name: str, P, N, UV, I, path_glb: str, texture_name: str) -> dict:
    Pg = (P * np.array([1, 1, -1])).astype(np.float32)
    Ng = (N * np.array([1, 1, -1])).astype(np.float32)
    I = I.astype(np.uint32).copy()
    a, b, c = Pg[I[:, 0]], Pg[I[:, 1]], Pg[I[:, 2]]
    fn = np.cross(b - a, c - a)
    vn = Ng[I[:, 0]] + Ng[I[:, 1]] + Ng[I[:, 2]]
    bad = np.einsum("ij,ij->i", fn, vn) < 0
    I[bad] = I[bad][:, ::-1]
    views, accessors = [], []
    data = bytearray()

    def add(arr, comp, typ, target, minmax=False):
        nonlocal data
        while len(data) % 4:
            data += b"\0"
        raw = arr.tobytes()
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(raw), "target": target})
        data += raw
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            acc["min"] = [float(x) for x in arr.min(axis=0)]
            acc["max"] = [float(x) for x in arr.max(axis=0)]
        accessors.append(acc)
        return len(accessors) - 1
    a_pos = add(Pg, 5126, "VEC3", 34962, True)
    a_nrm = add(Ng, 5126, "VEC3", 34962)
    a_uv = add(UV.astype(np.float32), 5126, "VEC2", 34962)
    a_idx = add(I.reshape(-1), 5125, "SCALAR", 34963)
    gltf = {
        "asset": {"version": "2.0", "generator": "Chaso modelgen (scripts/modelgen/seaworld.py)"},
        "scene": 0, "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm,
                                                                 "TEXCOORD_0": a_uv},
                                                  "indices": a_idx, "material": 0}]}],
        "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                             "baseColorFactor": [1, 1, 1, 1],
                                                             "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
        "images": [{"uri": texture_name}],
        "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(data)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    bin_ = bytes(data)
    while len(bin_) % 4:
        bin_ += b"\0"
    with open(path_glb, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942))
        f.write(bin_)
    return {"vertices": int(len(P)), "triangles": int(len(I)),
            "bounds": [[round(float(x), 3) for x in P.min(axis=0)], [round(float(x), 3) for x in P.max(axis=0)]]}
