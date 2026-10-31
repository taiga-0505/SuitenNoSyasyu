# -*- coding: utf-8 -*-
"""
書き出した .glb を glTF の仕様どおりに読み直し（スキニング・アニメーション込み）、
ソフトウェアラスタライザで静止画を描いて確認する。エンジンを起動せずに形と動きを確かめる用。

  python scripts/modelgen/preview.py <model.glb> <out.png> [clip=Idle] [times=0,0.5,...] [views=front,side,top,iso]
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

COMP = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def load(path):
    b = open(path, "rb").read()
    jl = struct.unpack("<I", b[12:16])[0]
    js = json.loads(b[20:20 + jl])
    bl = struct.unpack("<I", b[20 + jl:24 + jl])[0]
    bin_ = b[28 + jl:28 + jl + bl]

    def acc(i):
        a = js["accessors"][i]
        v = js["bufferViews"][a["bufferView"]]
        n = NCOMP[a["type"]]
        arr = np.frombuffer(bin_, dtype=COMP[a["componentType"]], count=a["count"] * n,
                            offset=v.get("byteOffset", 0) + a.get("byteOffset", 0))
        return arr.reshape(a["count"], n) if n > 1 else arr

    tex = None
    if js.get("images"):
        tex = np.asarray(Image.open(os.path.join(os.path.dirname(path), js["images"][0]["uri"])).convert("RGB")) / 255.0
    return js, acc, tex


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def trs(t, r, s):
    M = np.eye(4)
    M[:3, :3] = qmat(r) @ np.diag(s)
    M[:3, 3] = t
    return M


def sample(times, vals, t, is_quat):
    if t <= times[0]:
        return vals[0]
    if t >= times[-1]:
        return vals[-1]
    i = int(np.searchsorted(times, t)) - 1
    f = (t - times[i]) / (times[i + 1] - times[i])
    v = vals[i] * (1 - f) + vals[i + 1] * f
    if is_quat:
        v = v / np.linalg.norm(v)
    return v


def posed_vertices(js, acc, clip, t):
    nodes = js["nodes"]
    local = []
    for n in nodes:
        local.append([np.array(n.get("translation", [0, 0, 0]), float),
                      np.array(n.get("rotation", [0, 0, 0, 1]), float),
                      np.array(n.get("scale", [1, 1, 1]), float)])
    if clip is not None:
        anim = [a for a in js.get("animations", []) if a["name"] == clip]
        if not anim:
            raise SystemExit(f"clip {clip} not found: {[a['name'] for a in js.get('animations', [])]}")
        anim = anim[0]
        for ch in anim["channels"]:
            sm = anim["samplers"][ch["sampler"]]
            times = acc(sm["input"])
            vals = acc(sm["output"]).astype(float)
            idx = {"translation": 0, "rotation": 1, "scale": 2}[ch["target"]["path"]]
            local[ch["target"]["node"]][idx] = sample(times, vals, t, idx == 1)
    glob = [None] * len(nodes)

    def visit(i, parent):
        glob[i] = parent @ trs(*local[i])
        for c in nodes[i].get("children", []):
            visit(c, glob[i])

    for r in js["scenes"][0]["nodes"]:
        visit(r, np.eye(4))
    prim = js["meshes"][0]["primitives"][0]
    P = acc(prim["attributes"]["POSITION"]).astype(float)
    N = acc(prim["attributes"]["NORMAL"]).astype(float)
    UV = acc(prim["attributes"]["TEXCOORD_0"]).astype(float)
    J = acc(prim["attributes"]["JOINTS_0"]).astype(int)
    W = acc(prim["attributes"]["WEIGHTS_0"]).astype(float)
    I = acc(prim["indices"]).reshape(-1, 3)
    skin = js["skins"][0]
    ibm = acc(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    jm = np.array([glob[j] @ ibm[k] for k, j in enumerate(skin["joints"])])
    M = np.einsum("vk,vkab->vab", W, jm[J])
    Ph = np.concatenate([P, np.ones((len(P), 1))], axis=1)
    Pp = np.einsum("vab,vb->va", M, Ph)[:, :3]
    Np = np.einsum("vab,vb->va", M[:, :3, :3], N)
    Np /= np.maximum(np.linalg.norm(Np, axis=1, keepdims=True), 1e-9)
    # glTF → エンジン空間（z 反転）で扱う
    flip = np.array([1, 1, -1])
    return Pp * flip, Np * flip, UV, I


def render(P, N, UV, I, tex, view, size=360, center=None, extent=None):
    yaw, pitch = {"front": (0, 0), "side": (90, 0), "top": (0, 89.9), "iso": (35, 25),
                  "back": (180, 10), "below": (20, -35)}[view]
    yaw, pitch = math.radians(yaw), math.radians(pitch)
    # カメラはモデルの前方（+Z 側）から見る
    fwd = -np.array([math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)])
    up0 = np.array([0, 1, 0]) if abs(fwd[1]) < 0.99 else np.array([0, 0, 1])
    right = np.cross(up0, fwd)
    right /= np.linalg.norm(right)
    up = np.cross(fwd, right)
    c = center if center is not None else (P.min(0) + P.max(0)) / 2
    ext = extent if extent is not None else np.max(P.max(0) - P.min(0)) * 0.62
    sx = (P - c) @ right
    sy = (P - c) @ up
    sz = (P - c) @ fwd
    px = (sx / ext * 0.5 + 0.5) * size
    py = (0.5 - sy / ext * 0.5) * size
    img = np.ones((size, size, 3)) * np.array([0.30, 0.45, 0.58])
    zb = np.full((size, size), np.inf)
    light = np.array([0.4, 0.8, 0.45])
    light /= np.linalg.norm(light)
    th, tw = tex.shape[:2]
    for tri in I:
        x = px[tri]; y = py[tri]; z = sz[tri]
        x0, x1 = int(max(min(x.min(), size - 1), 0)), int(min(max(x.max(), 0), size - 1))
        y0, y1 = int(max(min(y.min(), size - 1), 0)), int(min(max(y.max(), 0), size - 1))
        if x1 < x0 or y1 < y0:
            continue
        d = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if abs(d) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w1 = ((gx - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (gy - y[0])) / d
        w2 = ((x[1] - x[0]) * (gy - y[0]) - (gx - x[0]) * (y[1] - y[0])) / d
        w0 = 1 - w1 - w2
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        m = inside & (zz < sub)
        if not m.any():
            continue
        sub[m] = zz[m]
        uv = UV[tri].mean(0)
        col = tex[min(int(uv[1] * th), th - 1), min(int(uv[0] * tw), tw - 1)]
        n = N[tri].mean(0)
        n /= max(np.linalg.norm(n), 1e-9)
        if np.dot(n, -fwd) < 0:
            n = -n  # 裏面（両面で描く）
        lam = 0.5 + 0.5 * np.dot(n, light)
        img[y0:y1 + 1, x0:x1 + 1][m] = col * (0.35 + 0.75 * lam)
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def main():
    path, out = sys.argv[1], sys.argv[2]
    clip = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "rest" else None
    times = [float(x) for x in sys.argv[4].split(",")] if len(sys.argv) > 4 else [0.0]
    views = sys.argv[5].split(",") if len(sys.argv) > 5 else ["front", "side", "top", "iso"]
    js, acc, tex = load(path)
    # 枠は rest で固定（コマ間で拡大率が変わらないように）
    P0, _, _, _ = posed_vertices(js, acc, None, 0)
    center = (P0.min(0) + P0.max(0)) / 2
    extent = np.max(P0.max(0) - P0.min(0)) * 0.68
    size = 300
    sheet = Image.new("RGB", (size * len(views), size * len(times) + 18 * len(times)), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    for r, t in enumerate(times):
        P, N, UV, I = posed_vertices(js, acc, clip, t)
        for c_, v in enumerate(views):
            im = render(P, N, UV, I, tex, v, size, center, extent)
            sheet.paste(im, (c_ * size, r * (size + 18) + 18))
            draw.text((c_ * size + 4, r * (size + 18) + 3), f"{os.path.basename(path)} {clip or 'rest'} t={t:.2f} {v}",
                      fill=(255, 255, 255))
    sheet.save(out)


if __name__ == "__main__":
    main()
