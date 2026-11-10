# -*- coding: utf-8 -*-
"""
タコ・鳥・船のモデル（形・骨・アニメーション）を定義して .glb を書き出す。

  使い方:  python scripts/modelgen/models.py
  出力先:  project/Resources/model/{octopus,bird,ship}/

座標はすべてエンジン空間（+X 右 / +Y 上 / +Z 前）。各モデルの寸法・骨の名前・クリップ名は
ゲーム側のスクリプトと対応しているので、変えるときはスクリプトのコメントも合わせて直すこと。
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gltfgen import (Clip, Model, box, cylinder, ellipsoid, loft, matrix_from_quat, quat_from_matrix,  # noqa: E402
                     rot_x, rot_y, rot_z, shortest_arc, smoothstep, tube, write_glb)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "project", "Resources", "model")
TAU = 2 * math.pi


def lerp(a, b, t):
    return a + (b - a) * t


def shade(c, k):
    return tuple(min(1.0, x * k) for x in c)


def ground(c):
    """地の色（明るいアニメ調の色を、赤茶に沈めた本物寄りの色にする）"""
    return (c[0] * 0.74, c[1] * 0.52, c[2] * 0.48)


def mottle_dark(c):
    """まだらの濃い斑の色（地の色を暗く、赤茶へ寄せる）"""
    return (c[0] * 0.34, c[1] * 0.20, c[2] * 0.20)


def pose_of(model: Model):
    """クリップの pose 関数で使う：rest のローカル姿勢をコピーした辞書"""
    return {j.name: (j.t.copy(), j.r.copy(), np.ones(3)) for j in model.joints}


# ===========================================================================
# タコ（タイトルのプレイヤー / 敵の共用。色だけ違う 2 つの .glb を出す）
#
#   単位は TitleOctopusScript の scale = 1（胴の幅 ≒ 1m）。原点は足の付け根の中心。
#   +Z が顔の前。骨はすべて root 直下（平ら）なので、スクリプトから上書きするときは
#   「モデル空間の位置・向き」をそのまま入れればよい。
#     head / mantle / eye_L / eye_R / pupil_L / pupil_R / leg{t}_{k}（t = 0..7, k = 0..5）
#   rest の足は水平に放射状（足 t の向きは +Z から (t + 0.5) × 45° 右回り。節の間隔 0.17）。
#   クリップ（敵用）: Idle / Jet / Windup / Spit
# ===========================================================================

OCTO_TENTACLES = 8
OCTO_SEGMENTS = 6
OCTO_ROOT_R = 0.28
OCTO_SPACING = 0.17
OCTO_HEAD = np.array([0.0, 0.14, 0.12])
OCTO_MANTLE = np.array([0.0, 0.32, -0.22])
OCTO_EYE = [np.array([-0.17, 0.36, 0.27]), np.array([0.17, 0.36, 0.27])]
OCTO_PUPIL_OFS = np.array([0.0, 0.045, 0.085])  # 目の中心から前やや上（敵は横から見るので前を向かせる）


def octo_leg_dir(t: int) -> np.ndarray:
    a = (t + 0.5) * TAU / OCTO_TENTACLES
    return np.array([math.sin(a), 0.0, math.cos(a)])


def octo_leg_s(k: int) -> float:
    return OCTO_ROOT_R + OCTO_SPACING * k


def octo_leg_radius(s: float) -> float:
    if s >= 1.13:
        return lerp(0.062, 0.03, smoothstep(1.13, 1.24, s))
    return lerp(0.135, 0.062, max(0.0, (s - 0.2) / 0.93))


def build_octopus(name: str, body, legs_dark, under, sucker, root_offset=(0.0, 0.0, 0.0)) -> Model:
    """root_offset: モデル全体をずらす量（敵は胴の中心を原点＝当たり判定の中心にしたいので下げる）"""
    m = Model(name)
    pal = m.palette
    # 濃い斑 → 地の色 → 淡い腹（skin_t で位置を決める）
    pal.add("skin", mottle_dark(body), under, mid=ground(body))
    pal.add("leg", mottle_dark(legs_dark), under, mid=ground(legs_dark))
    pal.add("sucker", sucker, shade(sucker, 0.62))     # 縁は淡く、くぼみは濃い
    pal.add("iris", (0.30, 0.20, 0.04), (0.86, 0.68, 0.30))  # 金色の虹彩（縁が暗い）
    pal.add("pupil", (0.02, 0.02, 0.03))
    pal.add("shine", (1.0, 1.0, 1.0))
    pal.add("dark", shade(body, 0.3))

    # --- 骨 ---
    m.joint("root", None, root_offset)
    m.joint("head", "root", OCTO_HEAD)
    m.joint("mantle", "root", OCTO_MANTLE)
    m.joint("eye_L", "root", OCTO_EYE[0])
    m.joint("eye_R", "root", OCTO_EYE[1])
    m.joint("pupil_L", "root", OCTO_EYE[0] + OCTO_PUPIL_OFS)
    m.joint("pupil_R", "root", OCTO_EYE[1] + OCTO_PUPIL_OFS)
    for t in range(OCTO_TENTACLES):
        d = octo_leg_dir(t)
        for k in range(OCTO_SEGMENTS):
            m.joint(f"leg{t}_{k}", "root", d * octo_leg_s(k))

    # --- 皮膚の模様・凹凸（セミリアル）---
    # 本物のマダコ：背中側は赤褐色のまだら（濃い斑点）＋細かいいぼ（乳頭突起）、腹側・足の裏は淡い。
    # パレットの帯は「濃い斑 → 地の色 → 淡い腹」の 3 色。頂点ごとに t を決めて塗り分ける
    # （エンジンは頂点カラーを読まないので、模様はパレットの位置で出す）。
    rng = np.random.default_rng(7)

    def make_noise(n_waves, kmin, kmax):
        dirs = rng.normal(size=(n_waves, 3))
        dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
        ks = dirs * rng.uniform(kmin, kmax, size=(n_waves, 1))
        ph = rng.uniform(0, TAU, size=n_waves)

        def f(p):
            return float(np.sum(np.sin(ks @ np.asarray(p, float) + ph)) / math.sqrt(n_waves * 0.5))
        return f

    mottle = make_noise(12, 15.0, 24.0)   # 斑（10〜20 cm くらいの濃い模様）
    fleck = make_noise(14, 28.0, 38.0)    # 細かい点々
    bump = make_noise(16, 16.0, 24.0)     # いぼ（乳頭突起）の凹凸（頂点の間隔で潰れない細かさ）

    def skin_t(p, n, under_bias=0.0):
        """濃い斑 0 … 地の色 0.5 … 淡い腹 1"""
        under = smoothstep(-0.15, -0.75, float(n[1]) - under_bias)  # 下を向くほど 1
        m = smoothstep(0.0, 0.9, mottle(p))                          # 斑の濃さ
        f = smoothstep(0.7, 1.3, fleck(p))
        t = 0.56 - 0.46 * m - 0.14 * f
        return min(1.0, max(0.0, lerp(t, 0.97, under)))

    def displace(part, center, amount, axis_scale=(1.0, 1.0, 1.0)):
        """中心から外向きに、いぼ・しわの凹凸を付ける"""
        c = np.asarray(center, float)
        for i, p in enumerate(part.pos):
            d = p - c
            ln = np.linalg.norm(d)
            if ln < 1e-6:
                continue
            # いぼ：ノイズの山だけを丸く盛り上げる（値の上限を決めて、とげにならないように）
            h = amount * smoothstep(0.7, 1.9, bump(p)) + amount * 0.3 * float(np.clip(mottle(p * 0.5), -1.5, 1.5))
            part.pos[i] = p + d / ln * h * np.asarray(axis_scale)
        return part

    # --- 外套膜（頭の後ろの大きな袋。後ろほど膨らむ卵形。表面にいぼとしわ）---
    mant = ellipsoid([0, 0, 0], [0.45, 0.40, 0.64], nu=36, nv=24)
    z = mant.pos[:, 2]
    k = (1.0 + 0.16 * np.clip(-z / 0.64, 0, 1)) * (1.0 - 0.10 * np.clip(z / 0.64, 0, 1))
    mant.pos[:, 0] *= k
    mant.pos[:, 1] *= k
    # 背中にゆるい縦じわ（外套膜の筋肉の起伏）
    ang = np.arctan2(mant.pos[:, 0], mant.pos[:, 1])
    wr = 1.0 + 0.018 * np.sin(ang * 7.0) * np.clip(-z / 0.64 + 0.3, 0, 1)
    mant.pos[:, 0] *= wr
    mant.pos[:, 1] *= wr
    mant = mant.transformed(rot_x(0.55), OCTO_MANTLE)
    displace(mant, OCTO_MANTLE, 0.02)
    mant.paint = lambda p, n: "skin"
    mant.grad = lambda p, n: skin_t(p, n)
    mant.skin = lambda p: [("mantle", 1.0)]
    m.add(mant)

    # --- 頭（目のある部分。外套膜より小さく、下は足の付け根へすぼまる）---
    face = ellipsoid(OCTO_HEAD, [0.37, 0.31, 0.35], nu=32, nv=20)
    yb = face.pos[:, 1] - OCTO_HEAD[1]
    s_ = 1.0 - 0.10 * np.clip(-yb / 0.31, 0, 1)
    face.pos[:, 0] = OCTO_HEAD[0] + (face.pos[:, 0] - OCTO_HEAD[0]) * s_
    face.pos[:, 2] = OCTO_HEAD[2] + (face.pos[:, 2] - OCTO_HEAD[2]) * s_
    displace(face, OCTO_HEAD, 0.012)
    face.paint = lambda p, n: "skin"
    face.grad = lambda p, n: skin_t(p, n)
    face.skin = lambda p: [("head", 1.0)]
    m.add(face)

    # 首（頭と外套膜のあいだを埋める）
    neck_c = (OCTO_HEAD + OCTO_MANTLE) * 0.5 + np.array([0.0, -0.02, 0.0])
    neck = ellipsoid(neck_c, [0.34, 0.29, 0.26], R=rot_x(0.6), nu=28, nv=14)
    displace(neck, neck_c, 0.008)
    neck.paint = lambda p, n: "skin"
    neck.grad = lambda p, n: skin_t(p, n)
    neck.skin = lambda p: [("head", 0.5), ("mantle", 0.5)]
    m.add(neck)

    # 漏斗（左の脇から後ろへ向いた噴射口。口の縁は少し開く）
    fun = cylinder([0.22, 0.05, 0.02], [0.33, 0.02, -0.13], 0.055, 0.04, n=14)
    fun.paint = lambda p, n: "skin"
    fun.grad = lambda p, n: 0.62
    fun.skin = lambda p: [("head", 1.0)]
    m.add(fun)
    fun_in = ellipsoid([0.33, 0.02, -0.13], [0.028, 0.028, 0.01], R=rot_y(-0.9), nu=10, nv=5)
    fun_in.paint = lambda p, n: "dark"
    fun_in.skin = lambda p: [("head", 1.0)]
    m.add(fun_in)

    # --- 目：皮膚の盛り上がりに半分埋まった目玉。虹彩は金色、瞳は横長のスリット ---
    pdir = OCTO_PUPIL_OFS / np.linalg.norm(OCTO_PUPIL_OFS)
    eye_r = float(np.linalg.norm(OCTO_PUPIL_OFS))  # 瞳の骨が目玉の表面に来る大きさ
    for side, ename, pname in ((0, "eye_L", "pupil_L"), (1, "eye_R", "pupil_R")):
        c = OCTO_EYE[side]
        sx = -1.0 if side == 0 else 1.0
        # まぶた（目のまわりの皮膚の盛り上がり。目玉の上半分を少し覆う）
        lid = ellipsoid(c + np.array([0.01 * sx, 0.0, -0.05]), [0.118, 0.108, 0.11], nu=20, nv=12)
        displace(lid, c, 0.006)
        lid.paint = lambda p, n: "skin"
        lid.grad = lambda p, n: skin_t(p, n)
        lid.skin = lambda p: [("head", 1.0)]
        m.add(lid)
        # 目の上の小さな角（乳頭突起。マダコの特徴）
        horn = ellipsoid(c + np.array([0.02 * sx, 0.098, -0.05]), [0.018, 0.032, 0.018],
                         R=rot_z(-0.3 * sx), nu=10, nv=6)
        horn.paint = lambda p, n: "skin"
        horn.grad = lambda p, n: skin_t(p, n, under_bias=0.3)
        horn.skin = lambda p: [("head", 1.0)]
        m.add(horn)
        # 目玉（虹彩の色。前上を向いた面だけがまぶたから出る）
        eye = ellipsoid(c, [eye_r, eye_r * 0.94, eye_r], nu=20, nv=12)
        eye.paint = lambda p, n: "iris"
        eye.grad = (lambda cc: (lambda p, n: 0.15 + 0.7 * smoothstep(-0.2, 0.9, float(np.dot(n, pdir)))))(c)
        eye.skin = (lambda en: (lambda p: [(en, 1.0)]))(ename)
        m.add(eye)
        # 瞳：横長のスリット（目玉の表面に貼る平たい楕円体）
        pc = c + OCTO_PUPIL_OFS
        ex = np.array([1.0, 0.0, 0.0])
        ey = np.cross(pdir, ex)
        Rp = np.stack([ex, ey, pdir], axis=1)
        pup = ellipsoid(pc, [0.052, 0.017, 0.012], R=Rp, nu=14, nv=6)
        pup.paint = lambda p, n: "pupil"
        pup.skin = (lambda pn: (lambda p: [(pn, 1.0)]))(pname)
        m.add(pup)
        sh = ellipsoid(pc + ey * 0.03 + ex * 0.02 * sx + pdir * 0.004, [0.012, 0.009, 0.006], R=Rp, nu=8, nv=4)
        sh.paint = lambda p, n: "shine"
        sh.skin = (lambda pn: (lambda p: [(pn, 1.0)]))(pname)
        m.add(sh)

    # --- 足 ---
    joint_s = [octo_leg_s(k) for k in range(OCTO_SEGMENTS)]
    S_TIP = 1.42      # 先端（最後の骨 1.13 より先は最後の骨に付いて動く）

    def leg_weights(t: int, s: float):
        if s <= joint_s[0]:
            w0 = max(0.0, min(1.0, (s - 0.04) / (joint_s[0] - 0.04)))
            return [(f"leg{t}_0", max(w0, 1e-3)), ("head", 1.0 - w0)]
        for k in range(OCTO_SEGMENTS - 1):
            if s <= joint_s[k + 1]:
                f = (s - joint_s[k]) / (joint_s[k + 1] - joint_s[k])
                return [(f"leg{t}_{k}", 1.0 - f), (f"leg{t}_{k + 1}", f)]
        return [(f"leg{t}_{OCTO_SEGMENTS - 1}", 1.0)]

    def leg_r(s: float) -> float:
        """根元は太く、先へ向かってなめらかに細くなる（先端はほぼ点）"""
        u = min(max(s / S_TIP, 0.0), 1.0)
        return 0.006 + 0.122 * (1.0 - u) ** 1.3

    def leg_point(d: np.ndarray, s: float) -> np.ndarray:
        """足の中心線。先端だけ背中側へ少し巻く"""
        p = d * s
        if s > 1.1:
            u = (s - 1.1) / (S_TIP - 1.1)
            p = p + np.array([0.0, 0.10 * u * u, 0.0]) - d * 0.03 * u * u
        return p

    SQUASH = 0.8
    s_samples = list(np.arange(0.04, S_TIP, 0.035)) + [S_TIP]
    for t in range(OCTO_TENTACLES):
        d = octo_leg_dir(t)
        side = np.cross([0, 1, 0], d)
        pts = np.array([leg_point(d, s) for s in s_samples])
        radii = [leg_r(s) for s in s_samples]
        leg = tube(pts, radii, n=12, squash=SQUASH, tip=True)
        leg.paint = lambda p, n: "leg"
        leg.grad = lambda p, n: skin_t(p, n, under_bias=0.1)
        leg.skin = (lambda tt: (lambda p: leg_weights(tt, float(np.hypot(p[0], p[2])))))(t)
        m.add(leg)
        # 吸盤：足の裏に 2 列で互い違い。根元は大きく先へ行くほど小さい（縁が淡く、中がくぼんで濃い）
        s = 0.10
        i = 0
        while s < S_TIP - 0.05:
            r = leg_r(s)
            rs = r * 0.38
            down = -r * SQUASH * 0.86
            c = leg_point(d, s) + np.array([0, down, 0]) + side * (r * 0.36 * (1 if i % 2 else -1))
            w = leg_weights(t, float(s))
            rim = ellipsoid(c, [rs, rs * 0.42, rs], nu=8, nv=4)
            rim.paint = lambda p, n: "sucker"
            rim.grad = lambda p, n: 0.2
            rim.skin = (lambda ww: (lambda p: ww))(w)
            m.add(rim)
            cup = ellipsoid(c + np.array([0, -rs * 0.18, 0]), [rs * 0.52, rs * 0.3, rs * 0.52], nu=6, nv=3)
            cup.paint = lambda p, n: "sucker"
            cup.grad = lambda p, n: 0.85
            cup.skin = (lambda ww: (lambda p: ww))(w)
            m.add(cup)
            s += max(0.02, r * 0.62)
            i += 1

    # --- 足のあいだの膜（傘膜）：足の付け根どうしをつなぐ薄い膜。足のあいだで縁がくぼむ ---
    N_A = 8 * 10
    U_N = 5
    leg_angle = lambda tt: (tt + 0.5) * TAU / OCTO_TENTACLES  # noqa: E731
    half = math.pi / OCTO_TENTACLES

    def web_edge(a: float) -> float:
        dl = abs(((a - leg_angle(0)) + half) % (2 * half) - half)  # 一番近い足からの角度 0..half
        return 0.66 - 0.26 * (dl / half) ** 1.4

    profiles = []
    for ia in range(N_A + 1):
        a = TAU * (ia % N_A) / N_A
        dv = np.array([math.sin(a), 0.0, math.cos(a)])
        se = web_edge(a)
        ring = []
        # 下面（中心 → 縁）→ 上面（縁 → 中心）
        for j in range(U_N + 1):
            u = j / U_N
            s = 0.10 + (se - 0.10) * u
            th = 0.026 * (1.0 - u) + 0.004
            ring.append(dv * s + np.array([0.0, -th - 0.01, 0.0]))
        for j in range(U_N, -1, -1):
            u = j / U_N
            s = 0.10 + (se - 0.10) * u
            th = 0.026 * (1.0 - u) + 0.004
            ring.append(dv * s + np.array([0.0, th - 0.01, 0.0]))
        profiles.append(np.array(ring))
    web = loft(profiles, closed=False)
    web.orient_outward(np.array([0.0, 0.0, 0.0]))  # 薄い板なので向きは下で個別に確かめる

    def web_weights(p):
        a = math.atan2(p[0], p[2]) % TAU
        s = float(math.hypot(p[0], p[2]))
        x = (a / (TAU / OCTO_TENTACLES)) - 0.5
        t0 = int(math.floor(x)) % OCTO_TENTACLES
        f = x - math.floor(x)
        t1 = (t0 + 1) % OCTO_TENTACLES
        out = {}
        for tt, ww in ((t0, 1.0 - f), (t1, f)):
            for jn, wj in leg_weights(tt, s):
                out[jn] = out.get(jn, 0.0) + ww * wj
        return list(out.items())

    web.paint = lambda p, n: "leg"
    web.grad = lambda p, n: skin_t(p, n, under_bias=0.1)
    web.skin = web_weights
    m.add(web)

    # --- クリップ（敵用。タイトルはスクリプトが骨を上書きする）---
    def legs_pose(pose, tm, theta0, kappa, amp, omega, root_shrink=1.0, fan=0.0, tremble=0.0):
        for t in range(OCTO_TENTACLES):
            d0 = octo_leg_dir(t)
            a = (t + 0.5) * TAU / OCTO_TENTACLES + fan * math.sin((t + 0.5) * TAU / OCTO_TENTACLES)
            d = np.array([math.sin(a), 0.0, math.cos(a)])
            ph = t * 0.85
            p = d * OCTO_ROOT_R * root_shrink + np.array([0, -0.02, 0])
            pts = [p]
            for k in range(1, OCTO_SEGMENTS + 1):
                s = octo_leg_s(k)
                th = theta0 + kappa * (s - OCTO_ROOT_R) + amp * math.sin(omega * tm - 3.2 * s + ph)
                th += tremble * math.sin(TAU * 8 * tm / 1.0 + t * 1.7 + k)
                p = p + OCTO_SPACING * (d * math.cos(th) + np.array([0, -math.sin(th), 0]))
                pts.append(p)
            for k in range(OCTO_SEGMENTS):
                tang = pts[k + 1] - (pts[k - 1] if k > 0 else pts[k])
                R = shortest_arc(d0, tang / np.linalg.norm(tang))
                pose[f"leg{t}_{k}"] = (pts[k], R, np.ones(3))

    IDLE = dict(theta0=1.15, kappa=-0.75, amp=0.22)

    def idle(tm):
        pose = pose_of(m)
        legs_pose(pose, tm, omega=TAU / 2.4, **IDLE)
        return pose

    def jet(tm):
        # 0〜0.2 秒で足を閉じて水を押し出し、そのあと開きながら Idle へ戻る
        pose = pose_of(m)
        u = tm / 0.7
        close = smoothstep(0.0, 0.22, u) * (1.0 - smoothstep(0.3, 1.0, u))
        th0 = lerp(IDLE["theta0"], 1.55, close)
        kap = lerp(IDLE["kappa"], 0.15, close)
        amp = lerp(IDLE["amp"], 0.05, close)
        legs_pose(pose, tm, th0, kap, amp, TAU / 2.4, root_shrink=lerp(1.0, 0.7, close))
        sq = close * 0.12
        pose["mantle"] = (OCTO_MANTLE, np.eye(3), np.array([1 - sq, 1 + sq * 1.4, 1 - sq]))
        return pose

    def windup(tm):
        # 足を大きく開いて先を反らせ、小刻みに震える（墨を吐く予兆）
        pose = pose_of(m)
        legs_pose(pose, tm, theta0=0.35, kappa=-1.4, amp=0.0, omega=0.0, tremble=0.07, fan=0.08)
        return pose

    def spit(tm):
        pose = pose_of(m)
        u = tm / 0.45
        k = smoothstep(0.0, 0.15, u) * (1.0 - smoothstep(0.35, 1.0, u))
        legs_pose(pose, tm, lerp(0.35, 1.5, k), lerp(-1.4, 0.2, k), 0.0, 0.0, root_shrink=lerp(1.0, 0.75, k))
        sq = k * 0.15
        pose["mantle"] = (OCTO_MANTLE, np.eye(3), np.array([1 - sq, 1 + sq * 1.5, 1 - sq]))
        return pose

    m.clips = [Clip("Idle", 2.4, idle), Clip("Jet", 0.7, jet, loop=False),
               Clip("Windup", 1.0, windup), Clip("Spit", 0.45, spit, loop=False)]
    # 形も root と同じだけずらす（骨は root 基準なのでクリップ・上書きの座標はそのまま）
    for part in m.parts:
        part.pos = part.pos + np.asarray(root_offset, dtype=np.float64)
    return m


# ===========================================================================
# 鳥（カモメ）
#
#   単位は「胴の半長 u = 1」。スポナーの enemyScale を (u, u, u) にする（既定 0.95）。
#   +Z が機首、+X が右翼。骨: root / body / head / tail / wing_R1 / wing_R2 / wing_L1 / wing_L2
#   （wing_*1 は肩、wing_*2 は肘で *1 の子。翼は Z 軸まわりに回すと羽ばたく）
#   クリップ: Flap / Cruise / Windup / Throw / Fall / Float / Glide
# ===========================================================================

BIRD_SHOULDER = np.array([0.28, 0.18, 0.1])
BIRD_INNER_SPAN = 1.35
BIRD_FLAP_HZ = 2.6


def build_bird() -> Model:
    m = Model("bird")
    pal = m.palette
    pal.add("white", (0.97, 0.97, 0.95), (0.86, 0.87, 0.88))
    pal.add("mantle", (0.62, 0.66, 0.72), (0.74, 0.77, 0.81))
    pal.add("wing", (0.60, 0.64, 0.70), (0.84, 0.86, 0.89))
    pal.add("tip", (0.60, 0.64, 0.70), (0.13, 0.14, 0.17))
    pal.add("beak", (0.98, 0.72, 0.16))
    pal.add("beak_tip", (0.85, 0.22, 0.12))
    pal.add("eye", (0.04, 0.04, 0.05))
    pal.add("feet", (0.95, 0.55, 0.25))

    m.joint("root", None, [0, 0, 0])
    m.joint("body", "root", [0, 0, 0])
    m.joint("head", "body", [0, 0.25, 0.8])
    m.joint("tail", "body", [0, 0.06, -0.8])
    for side, s in (("R", 1.0), ("L", -1.0)):
        m.joint(f"wing_{side}1", "body", BIRD_SHOULDER * np.array([s, 1, 1]))
        m.joint(f"wing_{side}2", f"wing_{side}1", [s * BIRD_INNER_SPAN, 0, 0])

    grad_down = lambda p, n: 0.5 - 0.5 * float(n[1])  # noqa: E731

    # --- 胴（後ろが細い流線形）---
    bodyp = ellipsoid([0, 0, 0], [0.47, 0.42, 1.0], nu=20, nv=14)
    z = bodyp.pos[:, 2]
    taper = np.where(z < 0, 1.0 + 0.38 * z, 1.0 + 0.05 * z)
    bodyp.pos[:, 0] *= taper
    bodyp.pos[:, 1] *= taper
    bodyp.pos[:, 1] += np.where(z < 0, -0.05 * z, 0.0)
    bodyp.paint = lambda p, n: "mantle" if (n[1] > 0.55 and p[2] < 0.55) else "white"
    bodyp.grad = lambda p, n: 0.0
    bodyp.skin = lambda p: [("body", 1.0)]
    m.add(bodyp)

    head = ellipsoid([0, 0.28, 0.92], [0.30, 0.29, 0.33], nu=18, nv=12)
    head.paint = lambda p, n: "white"
    head.grad = lambda p, n: 0.0
    head.skin = lambda p: [("head", 1.0)]
    m.add(head)
    for s in (1, -1):
        e = ellipsoid([s * 0.255, 0.37, 1.03], [0.045, 0.05, 0.045], nu=8, nv=6)
        e.paint = lambda p, n: "eye"
        e.skin = lambda p: [("head", 1.0)]
        m.add(e)

    # くちばし（先が下へ曲がる）
    rings = []
    for i, zz in enumerate(np.linspace(1.12, 1.56, 7)):
        u = i / 6
        w = lerp(0.085, 0.012, u ** 0.9)
        h = lerp(0.075, 0.02, u)
        cy = 0.235 - 0.06 * u ** 2
        rings.append(np.array([[w * math.sin(a), cy + h * math.cos(a), zz]
                               for a in np.linspace(0, TAU, 10, endpoint=False)]))
    beak = loft(rings, closed=True, cap_start=True, cap_end=True)
    beak.orient_outward([0, 0.2, 1.3])
    beak.paint = lambda p, n: "beak_tip" if (p[2] > 1.42 and p[1] < 0.19) else "beak"
    beak.skin = lambda p: [("head", 1.0)]
    m.add(beak)

    # 尾（扇形の板）
    rings = []
    for i, zz in enumerate(np.linspace(-0.62, -1.42, 6)):
        u = i / 5
        w = lerp(0.2, 0.42, u)
        th = lerp(0.07, 0.025, u)
        cy = lerp(0.1, 0.12, u)
        notch = 0.06 * u * u
        pts = []
        for a in np.linspace(0, TAU, 12, endpoint=False):
            x = w * math.cos(a)
            pts.append([x, cy + th * math.sin(a), zz + notch * (1 - abs(math.cos(a))) * (1 if i == 5 else 0)])
        rings.append(np.array(pts))
    tail = loft(rings, closed=True, cap_start=False, cap_end=True)
    tail.orient_outward([0, 0.1, -1.0])
    tail.paint = lambda p, n: "mantle" if n[1] > 0.3 else "white"
    tail.grad = lambda p, n: 0.5
    tail.skin = lambda p: [("tail", 1.0)] if p[2] < -0.75 else [("tail", 0.6), ("body", 0.4)]
    m.add(tail)

    # 足（尾の下にたたんだ水かき）
    for s in (1, -1):
        f = ellipsoid([s * 0.12, -0.27, -0.62], [0.055, 0.035, 0.16], nu=8, nv=6)
        f.paint = lambda p, n: "feet"
        f.skin = lambda p: [("body", 1.0)]
        m.add(f)

    # --- 翼（翼型の断面を翼幅方向につなぐ）---
    elbow_x = BIRD_SHOULDER[0] + BIRD_INNER_SPAN  # 1.63
    xs = [0.12, 0.3, 0.55, 0.85, 1.15, 1.45, elbow_x, 1.85, 2.1, 2.35, 2.6, 2.8, 2.95]

    def wing_params(x):
        if x <= elbow_x:
            u = (x - 0.12) / (elbow_x - 0.12)
            return lerp(0.48, 0.42, u), lerp(0.92, 0.82, u), lerp(0.1, 0.07, u), BIRD_SHOULDER[1]
        u = (x - elbow_x) / (2.95 - elbow_x)
        le = lerp(0.42, -0.05, u ** 1.4)
        chord = lerp(0.8, 0.14, u ** 1.15)
        th = lerp(0.065, 0.018, u)
        return le, chord, th, BIRD_SHOULDER[1]

    def wing_weights(side, x):
        x = abs(x)
        if x < 0.42:
            f = smoothstep(0.12, 0.42, x)
            return [(f"wing_{side}1", max(f, 1e-3)), ("body", 1 - f)]
        if x < 1.48:
            return [(f"wing_{side}1", 1.0)]
        if x < 1.78:
            f = smoothstep(1.48, 1.78, x)
            return [(f"wing_{side}1", 1 - f), (f"wing_{side}2", f)]
        return [(f"wing_{side}2", 1.0)]

    for side, s in (("R", 1.0), ("L", -1.0)):
        rings = []
        for x in xs:
            le, chord, th, y0 = wing_params(x)
            pts = []
            for a in np.linspace(0, TAU, 14, endpoint=False):
                q = 0.5 - 0.5 * math.cos(a)            # 0 = 前縁, 1 = 後縁
                zz = le - chord * q
                thick = th * (1 - 0.75 * q) * math.sin(a)
                camber = 0.035 * chord * math.sin(math.pi * q)
                pts.append([s * x, y0 + camber + thick * 0.5, zz])
            rings.append(np.array(pts))
        wing = loft(rings, closed=True, cap_start=True, cap_end=True)
        wing.orient_outward([s * 1.5, BIRD_SHOULDER[1], 0.0])

        def paint(p, n, s=s):
            x = abs(p[0])
            le, chord, _, _ = wing_params(min(x, 2.95))
            q = (le - p[2]) / max(chord, 1e-3)
            if x > 2.05:
                return "tip"
            if q > 0.82 and x < 1.95:
                return "white"   # 後縁の白い帯
            return "wing"

        def grad(p, n):
            x = abs(p[0])
            if x > 2.05:
                return smoothstep(2.05, 2.55, x)
            return 0.5 - 0.5 * float(n[1])

        wing.paint = paint
        wing.grad = grad
        wing.skin = (lambda sd: (lambda p: wing_weights(sd, p[0])))(side)
        m.add(wing)

    # --- クリップ ---
    deg = math.pi / 180

    def wings(pose, inner, outer, twist=0.0, sweep=0.0):
        for side, s in (("R", 1.0), ("L", -1.0)):
            t1, _, _ = pose[f"wing_{side}1"]
            pose[f"wing_{side}1"] = (t1, rot_z(s * inner) @ rot_y(-s * sweep) @ rot_x(twist), np.ones(3))
            t2, _, _ = pose[f"wing_{side}2"]
            pose[f"wing_{side}2"] = (t2, rot_z(s * outer) @ rot_y(-s * sweep * 0.6), np.ones(3))

    def flap_clip(amp_scale):
        period = 1.0 / BIRD_FLAP_HZ

        def f(tm):
            pose = pose_of(m)
            ph = TAU * tm / period
            amp = 34 * deg * amp_scale
            inner = 6 * deg + amp * math.sin(ph)
            outer = 0.6 * amp * math.sin(ph - 0.7)
            wings(pose, inner, outer, twist=0.12 * amp_scale * math.cos(ph), sweep=0.06 * amp_scale * math.cos(ph))
            pose["body"] = (np.array([0, -0.045 * amp_scale * math.sin(ph), 0]), np.eye(3), np.ones(3))
            pose["tail"] = (pose["tail"][0], rot_x(-0.1 + 0.04 * amp_scale * math.sin(ph + 1.0)), np.ones(3))
            pose["head"] = (pose["head"][0] + np.array([0, 0.02 * amp_scale * math.sin(ph), 0]), np.eye(3), np.ones(3))
            return pose
        return period, f

    p_flap, flap = flap_clip(1.0)
    p_cruise, cruise = flap_clip(0.42)

    trem_period = TAU / 40.0

    def windup(tm):
        pose = pose_of(m)
        tremble = 4 * deg * math.sin(TAU * tm / trem_period)
        wings(pose, 70 * deg + tremble, 25 * deg, twist=-0.15, sweep=-0.12)
        pose["tail"] = (pose["tail"][0], rot_x(-0.5) , np.array([1.15, 1, 1]))
        pose["head"] = (pose["head"][0] + np.array([0, 0.03, -0.04]), rot_x(0.12), np.ones(3))
        return pose

    def throw(tm):
        pose = pose_of(m)
        ti = min(tm / 0.18, 1.0) ** 2
        wings(pose, lerp(70 * deg, -40 * deg, ti), lerp(25 * deg, -20 * deg, ti), twist=lerp(-0.15, 0.25, ti),
              sweep=lerp(-0.12, 0.18, ti))
        pose["tail"] = (pose["tail"][0], rot_x(lerp(-0.5, -0.1, ti)), np.ones(3))
        pose["head"] = (pose["head"][0] + np.array([0, 0.0, 0.05 * ti]), rot_x(-0.1 * ti), np.ones(3))
        return pose

    fall_period = TAU / 25.0

    def fall(tm):
        pose = pose_of(m)
        wings(pose, 15 * deg + 10 * deg * math.sin(TAU * tm / fall_period), 35 * deg, twist=0.3)
        pose["head"] = (pose["head"][0], rot_x(0.35) @ rot_z(0.2), np.ones(3))
        return pose

    def float_(tm):
        pose = pose_of(m)
        wings(pose, -5 * deg, 0.0, sweep=-0.35)
        pose["head"] = (pose["head"][0] + np.array([0, -0.04, 0]), rot_x(0.3), np.ones(3))
        return pose

    def glide(tm):
        # 翼を少し持ち上げたまま広げて滑空（タイトルのカモメ用）。ゆっくり風を受けて揺れる
        pose = pose_of(m)
        ph = TAU * tm / 2.0
        wings(pose, 8 * deg + 1.5 * deg * math.sin(ph), -3 * deg + 1.0 * deg * math.sin(ph - 0.8), twist=0.03)
        pose["tail"] = (pose["tail"][0], rot_x(-0.1 + 0.03 * math.sin(ph + 0.5)), np.ones(3))
        return pose

    # 並び順 = スクリプトの clip index（BirdEnemyScript は 0〜5、TitleAmbientScript は 0 と 6 を使う）
    m.clips = [Clip("Flap", p_flap, flap), Clip("Cruise", p_cruise, cruise), Clip("Windup", trem_period * 2, windup),
               Clip("Throw", 0.18, throw, fps=60, loop=False), Clip("Fall", fall_period * 2, fall),
               Clip("Float", 1.0, float_, fps=4), Clip("Glide", 2.0, glide, fps=15)]
    return m


# ===========================================================================
# 船（帆船・敵）
#
#   単位は m。原点は船体の中心（喫水はおよそ y = 0。BuoyancyScript の halfHeight 0.8 と合わせてある）。
#   船体は幅 3・高さ 1.6・長さ 9 の箱の位置に収め、船首だけ前へ 1.5m 尖らせている。+Z が船首。
#   スポナーの enemyScale は (1, 1, 1)、colliderRadius は 3.6（以前の 0.4 × 9 と同じ大きさ）。
#   骨: hull / sail / flag_0..3（旗は鎖）。クリップ: Sail（旗のなびき＋帆のふくらみ）
# ===========================================================================


def build_ship() -> Model:
    m = Model("ship")
    pal = m.palette
    hull_c = (0.34, 0.21, 0.11)
    pal.add("hull", hull_c)
    pal.add("hull2", shade(hull_c, 0.82))
    pal.add("bottom", (0.36, 0.11, 0.09))
    pal.add("wale", (0.08, 0.06, 0.05))
    pal.add("ochre", (0.70, 0.52, 0.20))
    pal.add("deck", (0.62, 0.46, 0.28))
    pal.add("deck2", (0.55, 0.40, 0.24))
    pal.add("trim", (0.24, 0.15, 0.08))
    pal.add("cabin", (0.84, 0.80, 0.70))
    pal.add("roof", (0.55, 0.14, 0.12))
    pal.add("sail", (0.95, 0.92, 0.84), (0.82, 0.78, 0.68))
    pal.add("flag", (0.10, 0.10, 0.12))
    pal.add("iron", (0.16, 0.16, 0.18))
    pal.add("window", (0.06, 0.05, 0.05))
    pal.add("lamp", (1.0, 0.85, 0.35))
    pal.add("rope", (0.45, 0.36, 0.22))

    MAST_Z = 0.72
    MAST_TOP = 7.1
    SAIL_Y0, SAIL_Y1 = 2.75, 6.15
    m.joint("hull", None, [0, 0, 0])
    m.joint("sail", "hull", [0, (SAIL_Y0 + SAIL_Y1) / 2, MAST_Z])
    m.joint("flag_0", "hull", [0, 6.8, MAST_Z + 0.05])
    for i in range(1, 4):
        m.joint(f"flag_{i}", f"flag_{i - 1}", [0, 0, 0.25])

    hull_skin = lambda p: [("hull", 1.0)]  # noqa: E731

    # --- 船体の寸法 ---
    ZS = np.array([-4.6, -4.3, -3.6, -2.6, -1.4, 0.0, 1.4, 2.6, 3.5, 4.2, 4.8, 5.3, 5.7, 6.0])

    def half_w(z):
        if z <= 2.5:
            return lerp(1.32, 1.5, smoothstep(-4.6, -2.6, z))
        u = (z - 2.5) / 3.5
        return max(0.03, 1.5 * math.sqrt(max(0.0, 1 - u ** 2.2)))

    def deck_y(z):
        return 0.8 + 0.18 * smoothstep(-2.0, -4.6, z) + 0.38 * smoothstep(3.0, 6.0, z) ** 1.6

    def keel_y(z):
        return -0.8 + 1.5 * smoothstep(3.6, 6.0, z) ** 1.4 + 0.22 * smoothstep(-3.6, -4.6, z)

    LEVELS = [0.55, 0.3, 0.12, -0.05, -0.28, -0.48, -0.64, -0.75]

    def section(z):
        w, top, bot = half_w(z), deck_y(z), keel_y(z)
        ys = [top] + [min(max(y, bot), top) for y in LEVELS] + [bot]
        pts = []
        for y in ys:
            if y >= 0:
                x = w * (1 - 0.04 * y)
            else:
                f = min(1.0, y / bot) if bot < -1e-3 else 1.0
                x = w * max(0.0, 1 - f ** 2.6) ** 0.55
            pts.append((x, y))
        pts[-1] = (0.0, bot)
        # 左舷 → 竜骨 → 右舷（右舷側は逆順で鏡写し）
        ring = [[-x, y, z] for x, y in pts] + [[x, y, z] for x, y in reversed(pts[:-1])]
        return np.array(ring)

    hull = loft([section(z) for z in ZS], closed=False)
    # 船尾の板（transom）を閉じる
    st = section(ZS[0])
    cidx = len(hull.pos)
    hull.pos = np.concatenate([hull.pos, [[0.0, (deck_y(ZS[0]) + keel_y(ZS[0])) / 2, ZS[0]]]])
    hull.tris = np.concatenate([hull.tris, np.array([[cidx, i + 1, i] for i in range(len(st) - 1)])])
    hull.orient_outward([0, 0, 0])

    def hull_paint(p, n):
        y = p[1]
        if y < -0.06:
            return "bottom"
        if y < 0.12:
            return "wale"
        if p[2] < -4.55 and y > 0.12:
            return "hull2"
        band = int((y - 0.12) / 0.22)
        return "hull" if band % 2 == 0 else "hull2"

    hull.paint = hull_paint
    hull.skin = hull_skin
    hull.smooth = True
    m.add(hull)

    # --- 甲板（板目は X で交互に色を変える）---
    nd = 9
    rings = []
    for z in ZS[:-1]:
        w = half_w(z) * (1 - 0.04 * deck_y(z)) - 0.05
        rings.append(np.array([[lerp(-w, w, i / (nd - 1)), deck_y(z) - 0.02, z] for i in range(nd)]))
    deck = loft(rings, closed=False)
    deck.tris = deck.tris[:, ::-1].copy()  # 上向き
    deck.paint = lambda p, n: "deck" if int((p[0] + 2) / 0.37) % 2 == 0 else "deck2"
    deck.skin = hull_skin
    deck.smooth = False
    m.add(deck)

    # --- 舷墻（ブルワーク）---
    BW_H, BW_T = 0.45, 0.1
    for s in (1, -1):
        rings = []
        for z in ZS[:-2]:
            w = half_w(z) * (1 - 0.04 * deck_y(z))
            y0 = deck_y(z) - 0.05
            wi = max(0.0, w - BW_T)
            # 外側は下の帯（船体色）と上の帯（砲門の黄土色）の 2 段
            rings.append(np.array([[s * w, y0, z], [s * w * 1.005, y0 + 0.2, z], [s * w * 1.01, y0 + BW_H, z],
                                   [s * wi, y0 + BW_H, z], [s * wi, y0 + 0.1, z]]))
        bw = loft(rings, closed=True, cap_start=True, cap_end=True)
        bw.orient_outward([s * 0.2, 1.0, 0.0])

        def bw_paint(p, n, s=s):
            if n[1] > 0.6:
                return "trim"
            if n[0] * s > 0.2:
                return "ochre" if p[1] > deck_y(p[2]) + 0.15 else "hull"
            return "deck2"

        bw.paint = bw_paint
        bw.skin = hull_skin
        bw.smooth = False
        m.add(bw)

    # --- 大砲と砲門 ---
    for z in (-1.62, 0.18, 1.98):
        for s in (1, -1):
            w = half_w(z) * (1 - 0.04 * deck_y(z))
            y = deck_y(z) + 0.25
            port = box([s * (w + 0.015), y, z], [0.04, 0.34, 0.42])
            port.paint = lambda p, n: "window"
            port.skin = hull_skin
            m.add(port)
            barrel = cylinder([s * (w - 0.45), y, z], [s * (w + 0.45), y, z], 0.15, 0.12, n=10)
            barrel.paint = lambda p, n: "iron"
            barrel.skin = hull_skin
            m.add(barrel)
            ring = cylinder([s * (w + 0.36), y, z], [s * (w + 0.45), y, z], 0.155, 0.155, n=10)
            ring.paint = lambda p, n: "iron"
            ring.skin = hull_skin
            m.add(ring)
            car = box([s * (w - 0.45), deck_y(z) + 0.12, z], [0.5, 0.22, 0.45])
            car.paint = lambda p, n: "trim"
            car.skin = hull_skin
            m.add(car)

    # --- 船尾の船室 ---
    cz = -3.06
    cy0 = deck_y(cz)
    cabin = box([0, cy0 + 0.6, cz], [2.16, 1.2, 1.8])
    cabin.paint = lambda p, n: "cabin"
    cabin.skin = hull_skin
    m.add(cabin)
    roof = box([0, cy0 + 1.26, cz], [2.46, 0.12, 2.07])
    roof.paint = lambda p, n: "roof"
    roof.skin = hull_skin
    m.add(roof)
    rail = box([0, cy0 + 1.12, cz - 0.95], [2.2, 0.16, 0.08])
    rail.paint = lambda p, n: "trim"
    rail.skin = hull_skin
    m.add(rail)
    for x in (-0.6, 0.6):
        for zf, zs in ((cz + 0.905, 1), (cz - 0.905, -1)):
            win = box([x, cy0 + 0.78, zf], [0.36, 0.34, 0.03])
            win.paint = lambda p, n: "window" if abs(n[2]) > 0.5 else "trim"
            win.skin = hull_skin
            m.add(win)
    door = box([0, cy0 + 0.42, cz + 0.905], [0.46, 0.8, 0.03])
    door.paint = lambda p, n: "trim"
    door.skin = hull_skin
    m.add(door)
    for s in (1, -1):
        sw = box([s * 1.085, cy0 + 0.78, cz], [0.03, 0.3, 0.5])
        sw.paint = lambda p, n: "window"
        sw.skin = hull_skin
        m.add(sw)
    # 船尾の窓（transom）とランタン
    for x in (-0.7, 0.0, 0.7):
        tw = box([x, 0.45, -4.62], [0.32, 0.28, 0.04])
        tw.paint = lambda p, n: "lamp" if abs(n[2]) > 0.5 else "trim"
        tw.skin = hull_skin
        m.add(tw)
    pole = cylinder([0, cy0 + 1.3, -4.1], [0, cy0 + 2.1, -4.1], 0.04, n=6)
    pole.paint = lambda p, n: "trim"
    pole.skin = hull_skin
    m.add(pole)
    lamp = box([0, cy0 + 2.2, -4.1], [0.22, 0.28, 0.22])
    lamp.paint = lambda p, n: "lamp" if abs(n[1]) < 0.5 else "iron"
    lamp.skin = hull_skin
    m.add(lamp)

    # 舵
    rud = box([0, -0.25, -4.72], [0.12, 1.1, 0.42])
    rud.paint = lambda p, n: "trim"
    rud.skin = hull_skin
    m.add(rud)

    # --- マスト・見張り台・帆桁 ---
    mast = cylinder([0, 0.6, MAST_Z], [0, MAST_TOP, MAST_Z], 0.16, 0.1, n=12)
    mast.paint = lambda p, n: "trim"
    mast.skin = hull_skin
    m.add(mast)
    nest = cylinder([0, 5.45, MAST_Z], [0, 5.8, MAST_Z], 0.48, 0.52, n=14)
    nest.paint = lambda p, n: "trim" if n[1] < 0.6 else "deck2"
    nest.skin = hull_skin
    m.add(nest)
    for yy in (SAIL_Y0 - 0.03, SAIL_Y1 + 0.04):
        yard = cylinder([-2.4, yy, MAST_Z + 0.15], [2.4, yy, MAST_Z + 0.15], 0.075, n=8)
        yard.paint = lambda p, n: "trim"
        yard.skin = hull_skin
        m.add(yard)
    sprit = cylinder([0, 1.05, 5.4], [0, 1.85, 7.5], 0.09, 0.05, n=8)
    sprit.paint = lambda p, n: "trim"
    sprit.skin = hull_skin
    m.add(sprit)

    # --- 横帆（前へふくらんだ厚みのある布）---
    nx, ny = 11, 9
    W = 4.5
    grid = np.zeros((ny, nx, 3))
    for j in range(ny):
        v = j / (ny - 1)
        y = lerp(SAIL_Y0, SAIL_Y1, v)
        for i in range(nx):
            u = i / (nx - 1)
            x = lerp(-W / 2, W / 2, u)
            bulge = 0.65 * (1 - (2 * u - 1) ** 2) * math.sin(math.pi * lerp(0.15, 0.85, v))
            grid[j, i] = [x * (1 - 0.06 * (1 - v)), y, MAST_Z + 0.22 + bulge]
    sail = _sheet(grid, 0.035)
    sail.paint = lambda p, n: "sail"
    sail.grad = lambda p, n: (abs(p[0]) / (W / 2)) ** 2 * 0.6 + (1 - (p[1] - SAIL_Y0) / (SAIL_Y1 - SAIL_Y0)) * 0.4
    sail.skin = lambda p: [("sail", 1.0)]
    m.add(sail)

    # --- 三角帆（バウスプリットとマストの間）---
    A = np.array([0, 1.8, 7.2])
    B = np.array([0, 5.2, MAST_Z + 0.45])
    C = np.array([0, 1.6, 4.6])
    tri_grid = np.zeros((7, 7, 3))
    for j in range(7):
        for i in range(7):
            u, v = i / 6, j / 6
            # 三角形を四角い格子に写す（v = 1 の辺は B に集まる）
            p = C + (A - C) * u
            p = p + (B - p) * v
            p[0] += 0.28 * math.sin(math.pi * u) * math.sin(math.pi * min(v * 1.2, 1.0))
            tri_grid[j, i] = p
    jib = _sheet(tri_grid, 0.03)
    jib.paint = lambda p, n: "sail"
    jib.grad = lambda p, n: 0.2
    jib.skin = hull_skin
    m.add(jib)

    # --- 索具（ロープ）---
    ropes = [((0, MAST_TOP - 0.2, MAST_Z), (0, 1.85, 7.45))]
    for s in (1, -1):
        ropes += [((s * 0.35, 5.5, MAST_Z), (s * 1.48, 1.2, MAST_Z - 0.6)),
                  ((s * 0.35, 5.5, MAST_Z), (s * 1.48, 1.2, MAST_Z + 0.7)),
                  ((s * 0.1, MAST_TOP - 0.3, MAST_Z), (s * 1.3, 1.3, -4.2))]
    for a, b in ropes:
        r = cylinder(a, b, 0.022, n=5)
        r.paint = lambda p, n: "rope"
        r.skin = hull_skin
        m.add(r)

    # --- 旗（4 つの骨で波打つ）---
    fz0 = MAST_Z + 0.05
    grid = np.zeros((4, 9, 3))
    for j in range(4):
        for i in range(9):
            grid[j, i] = [0.0, lerp(6.55, 7.05, j / 3), fz0 + 1.0 * i / 8]
    flag = _sheet(grid, 0.02, axis=0)
    flag.paint = lambda p, n: "flag"

    def flag_w(p):
        d = (p[2] - fz0) / 0.25
        if d >= 3:
            return [("flag_3", 1.0)]
        k = int(max(d, 0))
        f = max(d, 0) - k
        return [(f"flag_{k}", 1 - f), (f"flag_{k + 1}", f)]

    flag.skin = flag_w
    m.add(flag)
    # 旗の白い印（どくろの代わりの丸）
    for s in (1, -1):
        mark = ellipsoid([s * 0.022, 6.8, fz0 + 0.5], [0.01, 0.11, 0.11], nu=10, nv=6)
        mark.paint = lambda p, n: "cabin"
        mark.skin = flag_w
        m.add(mark)

    # --- クリップ ---
    period = TAU / 5.0

    def sail_clip(tm):
        pose = pose_of(m)
        ph = TAU * tm / period
        for i in range(4):
            t_, _, _ = pose[f"flag_{i}"]
            a = 0.22 * math.sin(ph) if i == 0 else 0.3 * math.sin(ph - 0.9 * i)
            pose[f"flag_{i}"] = (t_, rot_y(a) @ rot_x(0.04 * math.sin(2 * ph + i)), np.ones(3))
        st, _, _ = pose["sail"]
        pose["sail"] = (st, np.eye(3), np.array([1.0, 1.0, 1.0 + 0.06 * math.sin(ph) + 0.03 * math.sin(2 * ph)]))
        return pose

    m.clips = [Clip("Sail", period, sail_clip)]
    return m


def _sheet(grid: np.ndarray, thickness: float, axis=None):
    """格子状の面を、表と裏の 2 枚（間に thickness）にした薄い板にする"""
    from gltfgen import Part
    ny, nx, _ = grid.shape
    # 面の法線（おおよそ）
    if axis is None:
        n = np.cross(grid[0, -1] - grid[0, 0], grid[-1, 0] - grid[0, 0])
    else:
        n = np.zeros(3)
        n[axis] = 1.0
    n = n / np.linalg.norm(n)
    front = grid + n * thickness / 2
    back = grid - n * thickness / 2
    pos = np.concatenate([front.reshape(-1, 3), back.reshape(-1, 3)])
    tris = []
    off = ny * nx
    for j in range(ny - 1):
        for i in range(nx - 1):
            a, b, c, d = j * nx + i, j * nx + i + 1, (j + 1) * nx + i + 1, (j + 1) * nx + i
            tris += [[a, b, c], [a, c, d]]
            tris += [[off + a, off + c, off + b], [off + a, off + d, off + c]]
    # 縁
    def edge(idx):
        for k in range(len(idx) - 1):
            a, b = idx[k], idx[k + 1]
            tris.extend([[a, b, off + b], [a, off + b, off + a]])
    edge([i for i in range(nx)])
    edge([(ny - 1) * nx + i for i in reversed(range(nx))])
    edge([j * nx for j in reversed(range(ny))])
    edge([j * nx + nx - 1 for j in range(ny)])
    p = Part(pos, np.array(tris), smooth=True)
    # 表側が n の向きを向くように
    a_, b_, c_ = p.pos[p.tris[0, 0]], p.pos[p.tris[0, 1]], p.pos[p.tris[0, 2]]
    if np.dot(np.cross(b_ - a_, c_ - a_), n) < 0:
        p.flip()
    return p


# ===========================================================================


def main():
    """引数にモデル名（octopus / octopus_enemy / bird / ship）を並べると、それだけ書き出す（無ければ全部）"""
    jobs = [
        ("octopus", "octopus", lambda: build_octopus("octopus", body=(0.93, 0.40, 0.33), legs_dark=(0.86, 0.36, 0.30),
                                                     under=(1.0, 0.70, 0.60), sucker=(1.0, 0.86, 0.78))),
        ("octopus", "octopus_enemy", lambda: build_octopus("octopus_enemy", body=(0.62, 0.28, 0.72),
                                                           legs_dark=(0.56, 0.25, 0.66), under=(0.86, 0.62, 0.90),
                                                           sucker=(0.96, 0.84, 0.96), root_offset=(0.0, -0.3, 0.0))),
        ("bird", "bird", build_bird),
        ("ship", "ship", build_ship),
    ]
    only = set(sys.argv[1:])
    out_root = os.environ.get("MODELGEN_OUT", OUT)  # 試しに書き出すときの出力先
    for folder, name, make in jobs:
        if only and name not in only:
            continue
        model = make()
        d = os.path.join(out_root, folder)
        os.makedirs(d, exist_ok=True)
        tex = f"{name}_palette.png"
        model.palette.image().save(os.path.join(d, tex))
        info = write_glb(model, os.path.join(d, f"{name}.glb"), tex)
        print(f"{folder}/{name}.glb", info)


if __name__ == "__main__":
    main()
