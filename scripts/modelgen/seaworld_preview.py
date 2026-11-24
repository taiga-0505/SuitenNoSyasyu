# -*- coding: utf-8 -*-
"""
seaworld のモデル単体、またはステージ JSON をまるごと、ソフトウェアラスタライザで静止画にする（確認用）。

  python scripts/modelgen/seaworld_preview.py model <name.glb> <out.png> [yaw_deg] [pitch_deg] [dist]
  python scripts/modelgen/seaworld_preview.py scene <Stage1> <out.png> <cx,cy,cz> [yaw_deg] [pitch_deg] [fov_deg]

scene モードは ModelRenderer（.glb）と PrimitiveMesh（箱として近似）と水面（y の平面）を描く。
ライティングは Half Lambert（ステージの屋外と同じ）、影・フォグ・水の透過は無い。
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RES = os.path.join(ROOT, "project")

COMP = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
SUN = np.array([-0.5, -0.8, 0.5])
SUN = SUN / np.linalg.norm(SUN)
_cache = {}


def load_glb(path, tex_override=None):
    key = (path, tex_override)
    if key in _cache:
        return _cache[key]
    b = open(path, "rb").read()
    jl = struct.unpack("<I", b[12:16])[0]
    js = json.loads(b[20:20 + jl])
    bin_ = b[28 + jl:]

    def acc(i):
        a = js["accessors"][i]
        v = js["bufferViews"][a["bufferView"]]
        n = NCOMP[a["type"]]
        arr = np.frombuffer(bin_, dtype=COMP[a["componentType"]], count=a["count"] * n,
                            offset=v.get("byteOffset", 0) + a.get("byteOffset", 0))
        return arr.reshape(a["count"], n) if n > 1 else arr
    prim = js["meshes"][0]["primitives"][0]
    P = acc(prim["attributes"]["POSITION"]).astype(np.float64) * [1, 1, -1]
    N = acc(prim["attributes"]["NORMAL"]).astype(np.float64) * [1, 1, -1]
    UV = acc(prim["attributes"]["TEXCOORD_0"]).astype(np.float64)
    # z を反転して戻すと左右が入れ替わるので、巻き順も戻す（エンジンは MakeLeftHanded + FlipWindingOrder）
    I = acc(prim["indices"]).reshape(-1, 3).astype(np.int64)[:, ::-1]
    uri = js["images"][0]["uri"]
    tex_path = tex_override or os.path.join(os.path.dirname(path), uri)
    tex = np.asarray(Image.open(tex_path).convert("RGB")).astype(np.float64) / 255.0
    nmap = None
    npath = os.path.join(os.path.dirname(path), uri.replace("_albedo", "_normal"))
    if "_albedo" in uri and os.path.exists(npath):
        a = np.asarray(Image.open(npath).convert("RGB")).astype(np.float64) / 255.0
        # エンジンは sRGB として読む（書き出し側で sRGB にエンコードしてある）ので同じくデコードする
        a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
        nmap = a * 2.0 - 1.0
    _cache[key] = (P, N, UV, I, (tex, nmap))
    return _cache[key]


def rot_y(t):
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_x(t):
    c, s = math.cos(t), math.sin(t)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_z(t):
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


class Raster:
    def __init__(self, w, h, eye, yaw, pitch, fov, near=0.3, far=2000.0):
        self.w, self.h = w, h
        self.eye = np.asarray(eye, float)
        self.R = rot_y(yaw) @ rot_x(pitch)  # カメラ → ワールド（pitch > 0 で見下ろす）
        self.f = 1.0 / math.tan(fov / 2)
        self.near, self.far = near, far
        self.color = np.zeros((h, w, 3))
        self.depth = np.full((h, w), np.inf)
        # 空（上ほど濃い青）
        sky_top = np.array([0.33, 0.43, 0.62])
        sky_hz = np.array([0.42, 0.55, 0.71])
        for y in range(h):
            t = y / h
            self.color[y, :] = sky_top * (1 - t) + sky_hz * t

    def project(self, P):
        v = (P - self.eye) @ self.R  # ワールド → カメラ
        z = v[:, 2]
        aspect = self.w / self.h
        sx = (v[:, 0] * self.f / aspect / np.maximum(z, 1e-6) * 0.5 + 0.5) * self.w
        sy = (-v[:, 1] * self.f / np.maximum(z, 1e-6) * 0.5 + 0.5) * self.h
        return np.column_stack([sx, sy, z])

    def draw(self, P, N, UV, I, tex, flat_color=None, alpha=1.0):
        nmap = None
        if isinstance(tex, tuple):
            tex, nmap = tex
        S = self.project(P)
        tz = S[I][:, :, 2]
        vis = (tz > self.near).all(axis=1) & (tz.min(axis=1) < self.far)
        tri = I[vis]
        if len(tri) == 0:
            return
        a, b, c = S[tri[:, 0]], S[tri[:, 1]], S[tri[:, 2]]
        area = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
        keep = np.abs(area) > 1e-9
        tri, a, b, c, area = tri[keep], a[keep], b[keep], c[keep], area[keep]
        Pw = P[tri]
        fn = np.cross(Pw[:, 1] - Pw[:, 0], Pw[:, 2] - Pw[:, 0])
        fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
        view = Pw.mean(axis=1) - self.eye
        front = np.einsum("ij,ij->i", fn, view) < 0
        if alpha < 1.0:
            front[:] = True
        tri, a, b, c, area, fn = tri[front], a[front], b[front], c[front], area[front], fn[front]
        th, tw = (tex.shape[0], tex.shape[1]) if tex is not None else (1, 1)
        for k in range(len(tri)):
            xs = (a[k, 0], b[k, 0], c[k, 0])
            ys = (a[k, 1], b[k, 1], c[k, 1])
            x0 = max(int(math.floor(min(xs))), 0)
            x1 = min(int(math.ceil(max(xs))), self.w - 1)
            y0 = max(int(math.floor(min(ys))), 0)
            y1 = min(int(math.ceil(max(ys))), self.h - 1)
            if x0 > x1 or y0 > y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((b[k, 0] - gx) * (c[k, 1] - gy) - (b[k, 1] - gy) * (c[k, 0] - gx)) / area[k]
            w1 = ((c[k, 0] - gx) * (a[k, 1] - gy) - (c[k, 1] - gy) * (a[k, 0] - gx)) / area[k]
            w2 = 1 - w0 - w1
            m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not m.any():
                continue
            iz = w0 / a[k, 2] + w1 / b[k, 2] + w2 / c[k, 2]
            z = 1.0 / iz
            sub = self.depth[y0:y1 + 1, x0:x1 + 1]
            m &= z < sub
            if not m.any():
                continue
            # 透視補正した重み
            pw0, pw1, pw2 = w0 / a[k, 2] * z, w1 / b[k, 2] * z, w2 / c[k, 2] * z
            if N is not None:
                nv = N[tri[k]]
                Nn = pw0[..., None] * nv[0] + pw1[..., None] * nv[1] + pw2[..., None] * nv[2]
                Nn /= np.maximum(np.linalg.norm(Nn, axis=-1, keepdims=True), 1e-9)
            else:
                Nn = np.broadcast_to(fn[k], m.shape + (3,))
            if flat_color is not None:
                col = np.broadcast_to(np.asarray(flat_color), m.shape + (3,))
            else:
                uvs = UV[tri[k]]
                u = pw0 * uvs[0, 0] + pw1 * uvs[1, 0] + pw2 * uvs[2, 0]
                v = pw0 * uvs[0, 1] + pw1 * uvs[1, 1] + pw2 * uvs[2, 1]
                tx = np.clip((u * tw).astype(int), 0, tw - 1)
                ty = np.clip((v * th).astype(int), 0, th - 1)
                col = tex[ty, tx]
                if nmap is not None:
                    # シェーダと同じく T = dP/du, B = dP/dv（三角形ごとに一定）
                    p0, p1, p2 = P[tri[k]]
                    d1, d2 = p1 - p0, p2 - p0
                    t1, t2 = uvs[1] - uvs[0], uvs[2] - uvs[0]
                    det = t1[0] * t2[1] - t1[1] * t2[0]
                    if abs(det) > 1e-12:
                        Tg = (d1 * t2[1] - d2 * t1[1]) / det
                        Bg = (d2 * t1[0] - d1 * t2[0]) / det
                        Tg /= max(np.linalg.norm(Tg), 1e-12)
                        Bg /= max(np.linalg.norm(Bg), 1e-12)
                        ny_, nx_ = nmap.shape[0], nmap.shape[1]
                        nt = nmap[np.clip((v * ny_).astype(int), 0, ny_ - 1), np.clip((u * nx_).astype(int), 0, nx_ - 1)]
                        Nn = nt[..., 0:1] * Tg + nt[..., 1:2] * Bg + nt[..., 2:3] * Nn
                        Nn /= np.maximum(np.linalg.norm(Nn, axis=-1, keepdims=True), 1e-9)
            nd = np.clip(-(Nn @ SUN), -1, 1)
            hl = (nd * 0.5 + 0.5) ** 2
            shade = col * (0.15 + 0.95 * hl)[..., None]
            csub = self.color[y0:y1 + 1, x0:x1 + 1]
            if alpha < 1.0:
                csub[m] = csub[m] * (1 - alpha) + shade[m] * alpha
            else:
                csub[m] = shade[m]
                sub[m] = z[m]

    def save(self, path):
        Image.fromarray((np.clip(self.color, 0, 1) * 255).astype(np.uint8)).save(path)


BOX_P = np.array([[x, y, z] for x in (-.5, .5) for y in (-.5, .5) for z in (-.5, .5)])
BOX_I = np.array([[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5], [0, 4, 5], [0, 5, 1], [2, 3, 7], [2, 7, 6],
                  [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3]])


def water_grid(cx, cz, w, h, y, n):
    """水面（近くの三角形がカメラの後ろへ回って丸ごと消えないよう細かく割る）"""
    xs = np.linspace(-w / 2, w / 2, n + 1) + cx
    zs = np.linspace(-h / 2, h / 2, n + 1) + cz
    X, Z = np.meshgrid(xs, zs)
    P = np.stack([X, np.full_like(X, y), Z], axis=-1).reshape(-1, 3)
    I = []
    for j in range(n):
        for i in range(n):
            a, b, c, d = j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i
            I += [[a, c, b], [a, d, c]]
    return P, np.array(I)


def entity_matrix(tr):
    s = np.asarray(tr["scale"], float)
    r = tr["rotation"]
    R = rot_x(r[0]) @ rot_y(r[1]) @ rot_z(r[2])
    return R @ np.diag(s), np.asarray(tr["position"], float)


def render_scene(stage, out, eye, yaw, pitch, fov, w=960, h=540):
    path = os.path.join(RES, "Resources", "Scenes", stage + ".json")
    d = json.load(open(path, encoding="utf-8"))
    ras = Raster(w, h, eye, yaw, pitch, fov)
    water = []
    for e in d["entities"]:
        if not e.get("active", True) or not e.get("visible", True):
            continue
        comps = {c["type"]: c["data"] for c in e["components"]}
        tr = comps.get("TransformComponent")
        if not tr:
            continue
        M, t = entity_matrix(tr)
        if "ModelRendererComponent" in comps and comps["ModelRendererComponent"].get("visible", True):
            mr = comps["ModelRendererComponent"]
            mp = os.path.join(RES, mr["modelPath"])
            if not mp.endswith(".glb"):
                continue
            to = os.path.join(RES, mr["texturePath"]) if mr.get("texturePath") else None
            P, N, UV, I, tex = load_glb(mp, to)
            Ninv = np.linalg.inv(M).T
            Nw = N @ Ninv.T
            Nw /= np.maximum(np.linalg.norm(Nw, axis=1, keepdims=True), 1e-12)
            ras.draw(P @ M.T + t, Nw, UV, I, tex)
        elif "PrimitiveMeshComponent" in comps and comps["PrimitiveMeshComponent"].get("visible", True):
            pm = comps["PrimitiveMeshComponent"]
            ras.draw(BOX_P @ M.T + t, None, None, BOX_I, None, flat_color=pm["color"][:3])
        if "WaterComponent" in comps:
            water.append((comps["WaterComponent"], t))
    for wc, t in water:
        P, I = water_grid(t[0], t[2], wc["planeWidth"], wc["planeHeight"], t[1], 48)
        ras.draw(P, None, None, I, None, flat_color=[0.08, 0.36, 0.42], alpha=0.85)
    ras.save(out)


def render_model(glb, out, yaw=30, pitch=15, dist=None, w=720, h=480, water=True):
    P, N, UV, I, tex = load_glb(glb)
    lo, hi = P.min(axis=0), P.max(axis=0)
    c = (lo + hi) / 2
    size = np.linalg.norm(hi - lo)
    dist = dist or size * 1.15
    yawr, pr = math.radians(yaw), math.radians(pitch)
    fwd = np.array([math.sin(yawr) * math.cos(pr), -math.sin(pr), math.cos(yawr) * math.cos(pr)])
    eye = c - fwd * dist
    ras = Raster(w, h, eye, yawr, pr, math.radians(50))
    ras.draw(P, N, UV, I, tex)
    if water:
        Pw, Iw = water_grid(c[0], c[2], size * 2, size * 2, 0.0, 16)
        ras.draw(Pw, None, None, Iw, None, flat_color=[0.08, 0.36, 0.42], alpha=0.75)
    ras.save(out)


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "model":
        args = sys.argv[2:]
        render_model(args[0], args[1], *(float(x) for x in args[2:]))
    else:
        stage, out, eye = sys.argv[2], sys.argv[3], [float(x) for x in sys.argv[4].split(",")]
        yaw = math.radians(float(sys.argv[5])) if len(sys.argv) > 5 else 0.0
        pitch = math.radians(float(sys.argv[6])) if len(sys.argv) > 6 else 0.0
        fov = math.radians(float(sys.argv[7])) if len(sys.argv) > 7 else 0.45  # ゲームのカメラと同じ縦画角
        render_scene(stage, out, eye, yaw, pitch, fov)
