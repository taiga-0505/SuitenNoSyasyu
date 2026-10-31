# -*- coding: utf-8 -*-
"""
スキン付き glTF (.glb) を手続き的に組み立てる小さなライブラリ。

  - 形状はすべて「エンジン空間」で作る（+X 右 / +Y 上 / +Z 前。PartPose と同じ数式の回転）。
    Assimp は glTF を読むときに MakeLeftHanded で z を反転するので、書き出し時に
    z を反転して glTF（右手系）へ直しておく。こうすると、ここで作った座標が
    そのままエンジンのモデルローカル座標になる。
  - 色は頂点カラーではなく「パレットテクスチャ」で付ける（エンジンは baseColor テクスチャしか読まない）。
    1 行 = 1 色（または 2 色のグラデーション）。三角形ごとに行を決めるので色の境目がにじまない。
  - アニメーションは全ジョイントの T/R/S を毎キー書き出す（エンジンの CalculateValue は空のキー列を許さないため）。
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
# 回転（標準の数式。エンジンの PartPose::RotX/Y/Z と同じ向き）
# ---------------------------------------------------------------------------


def rot_x(t: float) -> np.ndarray:
    c, s = math.cos(t), math.sin(t)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=np.float64)


def rot_y(t: float) -> np.ndarray:
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=np.float64)


def rot_z(t: float) -> np.ndarray:
    c, s = math.cos(t), math.sin(t)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=np.float64)


def quat_from_matrix(R: np.ndarray) -> np.ndarray:
    """回転行列（列ベクトル規約）→ クォータニオン (x, y, z, w)"""
    m = R
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s
        x = (m[2, 1] - m[1, 2]) / s
        y = (m[0, 2] - m[2, 0]) / s
        z = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        w = (m[2, 1] - m[1, 2]) / s
        x = 0.25 * s
        y = (m[0, 1] + m[1, 0]) / s
        z = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        w = (m[0, 2] - m[2, 0]) / s
        x = (m[0, 1] + m[1, 0]) / s
        y = 0.25 * s
        z = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        w = (m[1, 0] - m[0, 1]) / s
        x = (m[0, 2] + m[2, 0]) / s
        y = (m[1, 2] + m[2, 1]) / s
        z = 0.25 * s
    q = np.array([x, y, z, w], dtype=np.float64)
    q /= np.linalg.norm(q)
    return q


def matrix_from_quat(q: Sequence[float]) -> np.ndarray:
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def shortest_arc(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """単位ベクトル a を b へ回す最短の回転行列"""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c < -0.999999:
        # 真逆：a に直交する軸で 180°
        axis = np.cross(a, [1, 0, 0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0, 1, 0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1.0 / (1.0 + c))


def smoothstep(e0: float, e1: float, x: float) -> float:
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------------------
# パレット
# ---------------------------------------------------------------------------


class Palette:
    """横方向グラデーションの帯を縦に積んだテクスチャ（帯 1 本 = 1 色）"""

    ROWS = 16
    SIZE = 256

    def __init__(self) -> None:
        self.rows: Dict[str, Tuple[int, Tuple[float, ...], Tuple[float, ...]]] = {}

    def add(self, name: str, a: Sequence[float], b: Optional[Sequence[float]] = None) -> None:
        if len(self.rows) >= self.ROWS:
            raise ValueError("palette is full")
        self.rows[name] = (len(self.rows), tuple(a), tuple(b if b is not None else a))

    def uv(self, name: str, t: float) -> Tuple[float, float]:
        row = self.rows[name][0]
        t = min(max(t, 0.0), 1.0)
        # 端は少し内側に（ミップで隣の帯が混ざらないように縦は帯の中央）
        u = 0.06 + 0.88 * t
        v = (row + 0.5) / self.ROWS
        return u, v

    def image(self) -> Image.Image:
        h = self.SIZE // self.ROWS
        img = np.zeros((self.SIZE, self.SIZE, 3), dtype=np.float64)
        for name, (row, a, b) in self.rows.items():
            for x in range(self.SIZE):
                u = (x + 0.5) / self.SIZE
                t = min(max((u - 0.06) / 0.88, 0.0), 1.0)
                c = [a[i] + (b[i] - a[i]) * t for i in range(3)]
                img[row * h:(row + 1) * h, x] = c
        # 色はリニアで指定しているので sRGB へ（テクスチャは srgb=true で読まれる）
        srgb = np.where(img <= 0.0031308, img * 12.92, 1.055 * np.power(np.clip(img, 0, 1), 1 / 2.4) - 0.055)
        return Image.fromarray((np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


# ---------------------------------------------------------------------------
# メッシュ部品
# ---------------------------------------------------------------------------

SkinFn = Callable[[np.ndarray], List[Tuple[str, float]]]
PaintFn = Callable[[np.ndarray, np.ndarray], str]
GradFn = Callable[[np.ndarray, np.ndarray], float]


@dataclass
class Part:
    """インデックス付きの三角形メッシュ 1 つ（色と骨の重みは後で決める）"""
    pos: np.ndarray                      # (N,3)
    tris: np.ndarray                     # (M,3) 外向きが右手の外積の向き
    smooth: bool = True                  # False ならフラットシェーディング
    paint: Optional[PaintFn] = None      # 面ごとの色の帯名（重心で評価）
    grad: Optional[GradFn] = None        # 頂点ごとのグラデーション位置 0..1
    skin: Optional[SkinFn] = None        # 頂点ごとの (ジョイント名, 重み)
    name: str = ""

    def transformed(self, R: np.ndarray, t: Sequence[float]) -> "Part":
        p = Part(self.pos @ R.T + np.asarray(t, dtype=np.float64), self.tris.copy(), self.smooth,
                 self.paint, self.grad, self.skin, self.name)
        return p

    def flip(self) -> "Part":
        self.tris = self.tris[:, ::-1].copy()
        return self

    def orient_outward(self, center: Optional[Sequence[float]] = None) -> "Part":
        """重心から外向きになっていない面が多ければ全体を裏返す"""
        c = np.asarray(center if center is not None else self.pos.mean(axis=0))
        a, b, d = self.pos[self.tris[:, 0]], self.pos[self.tris[:, 1]], self.pos[self.tris[:, 2]]
        n = np.cross(b - a, d - a)
        cen = (a + b + d) / 3
        s = np.einsum("ij,ij->i", n, cen - c).sum()
        if s < 0:
            self.flip()
        return self


def ellipsoid(center, radii, R=None, nu=16, nv=10, **kw) -> Part:
    """楕円体（緯度経度グリッド）。極は 1 点"""
    R = np.eye(3) if R is None else R
    pts = []
    pts.append([0, -1, 0])
    for j in range(1, nv):
        phi = -math.pi / 2 + math.pi * j / nv
        for i in range(nu):
            th = 2 * math.pi * i / nu
            pts.append([math.cos(phi) * math.sin(th), math.sin(phi), math.cos(phi) * math.cos(th)])
    pts.append([0, 1, 0])
    P = np.array(pts) * np.asarray(radii)
    tris = []
    bottom, top = 0, len(pts) - 1

    def idx(j, i):
        return 1 + (j - 1) * nu + (i % nu)

    for i in range(nu):
        tris.append([bottom, idx(1, i + 1), idx(1, i)])
    for j in range(1, nv - 1):
        for i in range(nu):
            a, b, c, d = idx(j, i), idx(j, i + 1), idx(j + 1, i + 1), idx(j + 1, i)
            tris += [[a, b, c], [a, c, d]]
    for i in range(nu):
        tris.append([top, idx(nv - 1, i), idx(nv - 1, i + 1)])
    part = Part(P @ R.T + np.asarray(center), np.array(tris), **kw)
    return part.orient_outward(center)


def loft(rings: List[np.ndarray], closed: bool = True, cap_start: bool = False, cap_end: bool = False,
         **kw) -> Part:
    """同じ点数の断面（リング）を順につないだ筒。cap はリングの重心で扇形に閉じる"""
    m = len(rings[0])
    P = [np.asarray(r, dtype=np.float64) for r in rings]
    pos = np.concatenate(P, axis=0)
    tris = []
    seg = m if closed else m - 1
    for k in range(len(P) - 1):
        for i in range(seg):
            a = k * m + i
            b = k * m + (i + 1) % m
            c = (k + 1) * m + (i + 1) % m
            d = (k + 1) * m + i
            tris += [[a, b, c], [a, c, d]]
    extra = []
    if cap_start:
        ci = len(pos) + len(extra)
        extra.append(P[0].mean(axis=0))
        for i in range(seg):
            tris.append([ci, (i + 1) % m, i])
    if cap_end:
        ci = len(pos) + len(extra)
        extra.append(P[-1].mean(axis=0))
        base = (len(P) - 1) * m
        for i in range(seg):
            tris.append([ci, base + i, base + (i + 1) % m])
    if extra:
        pos = np.concatenate([pos, np.array(extra)], axis=0)
    return Part(pos, np.array(tris), **kw)


def box(center, size, R=None, **kw) -> Part:
    """直方体（フラット）"""
    R = np.eye(3) if R is None else R
    hx, hy, hz = np.asarray(size) / 2
    corners = np.array([[x, y, z] for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)])
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = []
    for a, b, c, d in faces:
        tris += [[a, b, c], [a, c, d]]
    kw.setdefault("smooth", False)
    part = Part(corners @ R.T + np.asarray(center), np.array(tris), **kw)
    return part.orient_outward(center)


def cylinder(p0, p1, r0, r1=None, n=12, cap0=True, cap1=True, **kw) -> Part:
    """p0 → p1 の円柱（円錐台）"""
    r1 = r0 if r1 is None else r1
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    axis = p1 - p0
    L = np.linalg.norm(axis)
    w = axis / L
    u = np.cross(w, [0, 1, 0]) if abs(w[1]) < 0.9 else np.cross(w, [1, 0, 0])
    u /= np.linalg.norm(u)
    v = np.cross(w, u)
    rings = []
    for p, r in ((p0, r0), (p1, r1)):
        rings.append(np.array([p + r * (math.cos(2 * math.pi * i / n) * u + math.sin(2 * math.pi * i / n) * v)
                               for i in range(n)]))
    part = loft(rings, closed=True, cap_start=cap0, cap_end=cap1, **kw)
    return part.orient_outward((p0 + p1) / 2)


def tube(points: np.ndarray, radii: Sequence[float], n: int = 10, squash: float = 1.0,
         up=(0, 1, 0), tip: bool = True, **kw) -> Part:
    """折れ線に沿った筒（断面は横 r・縦 r*squash の楕円）。先端は 1 点に閉じる"""
    pts = np.asarray(points, float)
    rings = []
    upv = np.asarray(up, float)
    for i, p in enumerate(pts):
        if i == 0:
            tdir = pts[1] - pts[0]
        elif i == len(pts) - 1:
            tdir = pts[-1] - pts[-2]
        else:
            tdir = pts[i + 1] - pts[i - 1]
        tdir /= np.linalg.norm(tdir)
        side = np.cross(upv, tdir)
        if np.linalg.norm(side) < 1e-6:
            side = np.array([1.0, 0, 0])
        side /= np.linalg.norm(side)
        vup = np.cross(tdir, side)
        r = radii[i]
        rings.append(np.array([p + r * math.cos(2 * math.pi * k / n) * side +
                               r * squash * math.sin(2 * math.pi * k / n) * vup for k in range(n)]))
    part = loft(rings, closed=True, cap_start=True, cap_end=False, **kw)
    if tip:
        # 先端を 1 点に
        base = len(part.pos)
        tip_p = pts[-1] + (pts[-1] - pts[-2]) / np.linalg.norm(pts[-1] - pts[-2]) * radii[-1] * 1.2
        part.pos = np.concatenate([part.pos, tip_p[None]], axis=0)
        last = (len(rings) - 1) * n
        extra = [[base, last + (k + 1) % n, last + k] for k in range(n)]
        part.tris = np.concatenate([part.tris, np.array(extra)], axis=0)
    return part


# ---------------------------------------------------------------------------
# スケルトン・アニメーション
# ---------------------------------------------------------------------------


@dataclass
class Joint:
    name: str
    parent: Optional[str]
    t: np.ndarray            # 親基準の位置（rest）
    r: np.ndarray = field(default_factory=lambda: np.eye(3))  # 親基準の回転（rest）


@dataclass
class Clip:
    name: str
    duration: float
    pose: Callable[[float], Dict[str, Tuple[np.ndarray, np.ndarray, np.ndarray]]]
    fps: float = 30.0
    loop: bool = True


class Model:
    def __init__(self, name: str) -> None:
        self.name = name
        self.parts: List[Part] = []
        self.joints: List[Joint] = []
        self.clips: List[Clip] = []
        self.palette = Palette()

    # --- skeleton ---
    def joint(self, name, parent, t, r=None) -> None:
        self.joints.append(Joint(name, parent, np.asarray(t, float), np.eye(3) if r is None else r))

    def jindex(self, name: str) -> int:
        for i, j in enumerate(self.joints):
            if j.name == name:
                return i
        raise KeyError(name)

    def rest_global(self) -> Dict[str, Tuple[np.ndarray, np.ndarray]]:
        """ジョイント名 → (回転, 位置)（モデル空間・rest）"""
        out = {}
        for j in self.joints:
            if j.parent is None:
                out[j.name] = (j.r, j.t)
            else:
                pr, pt = out[j.parent]
                out[j.name] = (pr @ j.r, pr @ j.t + pt)
        return out

    def rest_local(self) -> Dict[str, Tuple[np.ndarray, np.ndarray, np.ndarray]]:
        return {j.name: (j.t.copy(), j.r.copy(), np.ones(3)) for j in self.joints}

    def add(self, part: Part) -> Part:
        self.parts.append(part)
        return part

    # --- build final vertex streams ---
    def build(self):
        P_all, N_all, UV_all, J_all, W_all, I_all = [], [], [], [], [], []
        base = 0
        for part in self.parts:
            pos, tris = part.pos, part.tris
            a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
            fn = np.cross(b - a, c - a)
            # 面積 0 の三角形（船首で断面がつぶれた所など）は捨てる
            keep = np.linalg.norm(fn, axis=1) > 1e-9
            tris, a, b, c, fn = tris[keep], a[keep], b[keep], c[keep], fn[keep]
            if part.smooth:
                vn = np.zeros_like(pos)
                for k in range(3):
                    np.add.at(vn, tris[:, k], fn)
                vn /= np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)
            fnn = fn / np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
            cen = (a + b + c) / 3
            verts: Dict[Tuple[int, str, int], int] = {}
            out_p, out_n, out_uv, out_j, out_w, out_i = [], [], [], [], [], []
            for f, tri in enumerate(tris):
                row = part.paint(cen[f], fnn[f]) if part.paint else next(iter(self.palette.rows))
                for vi in tri:
                    key = (int(vi), row, -1 if part.smooth else f)
                    if key not in verts:
                        p = pos[vi]
                        n = vn[vi] if part.smooth else fnn[f]
                        g = part.grad(p, n) if part.grad else 0.5
                        verts[key] = len(out_p)
                        out_p.append(p)
                        out_n.append(n)
                        out_uv.append(self.palette.uv(row, g))
                        sk = part.skin(p) if part.skin else [(self.joints[0].name, 1.0)]
                        sk = sorted([s for s in sk if s[1] > 1e-4], key=lambda s: -s[1])[:4]
                        tot = sum(w for _, w in sk)
                        js = [self.jindex(n_) for n_, _ in sk] + [0] * (4 - len(sk))
                        ws = [w / tot for _, w in sk] + [0.0] * (4 - len(sk))
                        out_j.append(js)
                        out_w.append(ws)
                    out_i.append(verts[key] + base)
            base += len(out_p)
            P_all += out_p
            N_all += out_n
            UV_all += out_uv
            J_all += out_j
            W_all += out_w
            I_all += out_i
        return (np.array(P_all), np.array(N_all), np.array(UV_all), np.array(J_all, dtype=np.uint16),
                np.array(W_all), np.array(I_all, dtype=np.uint32).reshape(-1, 3))


# ---------------------------------------------------------------------------
# glTF 書き出し（エンジン空間 → glTF：z を反転）
# ---------------------------------------------------------------------------

_MIRROR = np.diag([1.0, 1.0, -1.0])


def _g_vec(v):
    v = np.asarray(v, float)
    return v * np.array([1.0, 1.0, -1.0])


def _g_rot(R):
    return _MIRROR @ R @ _MIRROR


def _mat4(R, t, s=(1, 1, 1)):
    M = np.eye(4)
    M[:3, :3] = R @ np.diag(s)
    M[:3, 3] = t
    return M


class _Buf:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, arr: np.ndarray, comp: int, typ: str, target=None, minmax=False) -> int:
        while len(self.data) % 4:
            self.data += b"\0"
        raw = arr.tobytes()
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(raw)}
        if target:
            view["target"] = target
        self.data += raw
        self.views.append(view)
        acc = {"bufferView": len(self.views) - 1, "componentType": comp, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a2 = arr.reshape(arr.shape[0], -1)
            acc["min"] = [float(x) for x in a2.min(axis=0)]
            acc["max"] = [float(x) for x in a2.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1


def write_glb(model: Model, path_glb: str, texture_name: str) -> dict:
    P, N, UV, J, W, I = model.build()
    # glTF 空間へ
    Pg = P * np.array([1, 1, -1])
    Ng = N * np.array([1, 1, -1])
    # 三角形を外向き（右手の外積）に揃える
    a, b, c = Pg[I[:, 0]], Pg[I[:, 1]], Pg[I[:, 2]]
    fn = np.cross(b - a, c - a)
    vn = Ng[I[:, 0]] + Ng[I[:, 1]] + Ng[I[:, 2]]
    bad = np.einsum("ij,ij->i", fn, vn) < 0
    I = I.copy()
    I[bad] = I[bad][:, ::-1]

    buf = _Buf()
    a_pos = buf.add(Pg.astype(np.float32), 5126, "VEC3", 34962, minmax=True)
    a_nrm = buf.add(Ng.astype(np.float32), 5126, "VEC3", 34962)
    a_uv = buf.add(UV.astype(np.float32), 5126, "VEC2", 34962)
    a_j = buf.add(J.astype(np.uint16), 5123, "VEC4", 34962)
    a_w = buf.add(W.astype(np.float32), 5126, "VEC4", 34962)
    a_idx = buf.add(I.reshape(-1).astype(np.uint32), 5125, "SCALAR", 34963)

    # ノード：0 = Armature（ルート）、1.. = ジョイント、最後 = メッシュ
    nodes = [{"name": "Armature", "children": []}]
    jnode = {}
    for j in model.joints:
        jnode[j.name] = len(nodes)
        q = quat_from_matrix(_g_rot(j.r))
        nodes.append({"name": j.name, "translation": [float(x) for x in _g_vec(j.t)],
                      "rotation": [float(x) for x in q]})
    for j in model.joints:
        if j.parent is None:
            nodes[0]["children"].append(jnode[j.name])
        else:
            nodes[jnode[j.parent]].setdefault("children", []).append(jnode[j.name])
    # スキン付きメッシュのノードはシーン直下に置く（親の Transform は効かないため）
    mesh_node = len(nodes)
    nodes.append({"name": model.name + "_mesh", "mesh": 0, "skin": 0})

    # 逆バインド行列（glTF 空間の rest のグローバル）
    rg = model.rest_global()
    ibm = []
    for j in model.joints:
        R, t = rg[j.name]
        M = _mat4(_g_rot(R), _g_vec(t))
        ibm.append(np.linalg.inv(M).T.reshape(-1))  # 列優先
    a_ibm = buf.add(np.array(ibm, dtype=np.float32), 5126, "MAT4")

    # アニメーション
    animations = []
    rest = model.rest_local()
    for clip in model.clips:
        nkeys = max(2, int(round(clip.duration * clip.fps)) + 1)
        times = np.linspace(0.0, clip.duration, nkeys)
        tr = {j.name: [] for j in model.joints}
        ro = {j.name: [] for j in model.joints}
        sc = {j.name: [] for j in model.joints}
        for tm in times:
            pose = dict(rest)
            pose.update(clip.pose(float(tm)))
            for j in model.joints:
                t_, r_, s_ = pose[j.name]
                tr[j.name].append(_g_vec(t_))
                ro[j.name].append(quat_from_matrix(_g_rot(r_)))
                sc[j.name].append(np.asarray(s_, float))
        a_time = buf.add(times.astype(np.float32), 5126, "SCALAR", minmax=True)
        samplers, channels = [], []
        for j in model.joints:
            qs = np.array(ro[j.name])
            # 隣のキーと符号をそろえる（補間で遠回りしないように）
            for k in range(1, len(qs)):
                if np.dot(qs[k], qs[k - 1]) < 0:
                    qs[k] = -qs[k]
            for path, arr, typ in (("translation", np.array(tr[j.name]), "VEC3"),
                                   ("rotation", qs, "VEC4"),
                                   ("scale", np.array(sc[j.name]), "VEC3")):
                a_out = buf.add(arr.astype(np.float32), 5126, typ)
                samplers.append({"input": a_time, "output": a_out, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": jnode[j.name], "path": path}})
        animations.append({"name": clip.name, "samplers": samplers, "channels": channels})

    gltf = {
        "asset": {"version": "2.0", "generator": "Chaso modelgen (scripts/modelgen)"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0, mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": model.name, "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv, "JOINTS_0": a_j, "WEIGHTS_0": a_w},
            "indices": a_idx, "material": 0}]}],
        "skins": [{"name": model.name + "_skin", "joints": [jnode[j.name] for j in model.joints],
                   "inverseBindMatrices": a_ibm, "skeleton": 0}],
        "materials": [{"name": model.name, "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0}, "baseColorFactor": [1, 1, 1, 1],
            "metallicFactor": 0.0, "roughnessFactor": 0.85}}],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
        "images": [{"uri": texture_name}],
        "accessors": buf.accessors,
        "bufferViews": buf.views,
        "buffers": [{"byteLength": len(buf.data)}],
    }
    if animations:
        gltf["animations"] = animations

    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    bin_ = bytes(buf.data)
    while len(bin_) % 4:
        bin_ += b"\0"
    total = 12 + 8 + len(js) + 8 + len(bin_)
    with open(path_glb, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942))
        f.write(bin_)
    return {"vertices": int(len(P)), "triangles": int(len(I)), "joints": len(model.joints),
            "clips": [c.name for c in model.clips]}
