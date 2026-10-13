# -*- coding: utf-8 -*-
"""Stage1〜Stage5 のシーン JSON をグレーボックスで生成する。

構成（ステージセレクトで順に進む）:
  Stage1 水上 外洋        … 洞窟の入口（岩壁のアーチ）で終わる
  Stage2 水上 洞窟（岩窟水道）… 外洋から崖の口をくぐって洞窟へ入っていく
  Stage3 水中 洞窟（沈み洞） … 洞窟の広間（水上）から、天井が下がって水没した通路へもぐる
  Stage4 水上 洞窟（地底湖） … 水没した通路（水中）から浮上して、天井の高い地底湖へ上がる
  Stage5 水上 外洋（黎明の海）… 洞窟の中から始まり、崖の口を抜けて外洋へ出る最終面
屋外 ⇔ 洞窟を行き来する面（2・5）は CaveLightZoneScript で太陽光と環境光をカメラの位置に合わせて切り替える。

Resources/Scenes/Game.json を雛形（カメラ・レール・ポーズ・ウェーブ管理・敵のパラメータ）として読み、
地形・レール・ウェーブ・照明だけ差し替えて書き出す。エディタで調整したあとに再実行すると
手調整が上書きされるので、初回の叩き台として使うこと（--force を付けないと既存ファイルは上書きしない）。

使い方（リポジトリのルートで）:
  python scripts/generate_stages.py          # 無いステージだけ作る
  python scripts/generate_stages.py --force  # 5 面とも作り直す
  python scripts/generate_stages.py --force Stage2 Stage4  # 指定した面だけ作り直す
"""
import copy
import json
import math
import os
import random
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "project")
SCENE_DIR = os.path.join(ROOT, "Resources", "Scenes")
TEMPLATE = os.path.join(SCENE_DIR, "Game.json")

rng = random.Random(20260927)
_used_guids = set()


def guid():
    while True:
        g = rng.randrange(10**17, 2**63 - 1)
        if g not in _used_guids:
            _used_guids.add(g)
            return g


def tr(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"type": "TransformComponent", "enabled": True,
            "data": {"position": list(pos), "rotation": list(rot), "scale": list(scale)}}


def entity(name, components, parent=0, tags=None, folder=False):
    return {"active": True, "components": components, "guid": guid(), "isFolder": folder,
            "locked": False, "name": name, "parentGuid": parent, "tags": tags or {}, "visible": True}


def prim(shape, color, lighting=2, env=0.0):
    types = {"sphere": 0, "box": 1, "plane": 2, "cylinder": 3, "cone": 4, "torus": 5, "capsule": 6}
    return {"type": "PrimitiveMeshComponent", "enabled": True,
            "data": {"color": list(color), "environmentCoeff": env, "lightingMode": lighting,
                     "normalMapPath": "", "roughnessMapPath": "", "shininess": 32.0, "texturePath": "",
                     "type": types[shape], "uvOffset": [0.0, 0.0], "uvTiling": [1.0, 1.0], "visible": True}}


def collider():
    return {"type": "ColliderComponent", "enabled": True,
            "data": {"center": [0.0, 0.0, 0.0], "isTrigger": False, "layer": 4294967295, "radius": 1.0,
                     "shape": 0, "size": [1.0, 1.0, 1.0]}}


def point_light(color, intensity, radius, decay=1.5):
    return {"type": "PointLightComponent", "enabled": True,
            "data": {"visible": True, "color": list(color), "intensity": intensity, "radius": radius,
                     "decay": decay, "castShadow": False}}


# ---------------------------------------------------------------------------
# 雛形の読み込み
# ---------------------------------------------------------------------------
with open(TEMPLATE, encoding="utf-8") as f:
    TPL = {e["name"]: e for e in json.load(f)["entities"]}


def tpl_script_data(name):
    for c in TPL[name]["components"]:
        if c["type"] == "NativeScriptComponent":
            return c["data"]["scriptDataList"][0]
    raise KeyError(name)


SHARK = tpl_script_data("WaveSpawner_1")
OCTO = tpl_script_data("WaveSpawner_1_octopus")
SHIP = tpl_script_data("WaveSpawner_3_ship")


# ---------------------------------------------------------------------------
# 部品
# ---------------------------------------------------------------------------
class Stage:
    def __init__(self, name, title, underwater, cave):
        self.name = name
        self.title = title
        self.underwater = underwater
        self.cave = cave
        self.entities = []
        self.field = entity("Field", [], folder=True)
        self.entities.append(self.field)
        self.cave_folder = None
        if cave:
            self.cave_folder = entity("Cave", [], folder=True)
            self.entities.append(self.cave_folder)
        self.spawner_count = 0
        self.chest_count = 0
        # 共通の世界（World）から地形を取り込むときの z の範囲。None なら絞らない
        self.window = None

    def in_window(self, z_lo, z_hi):
        if self.window is None:
            return True
        return z_hi >= self.window[0] and z_lo <= self.window[1]

    def add(self, e):
        self.entities.append(e)
        return e

    # --- 地形 ---
    def block(self, name, pos, scale, color, rot=(0.0, 0.0, 0.0), shape="box", terrain=False,
              cave=False, solid=True, lighting=2):
        if not self.in_window(pos[2] - abs(scale[2]) * 0.5, pos[2] + abs(scale[2]) * 0.5):
            return None
        comps = []
        if solid:
            comps.append(collider())
        comps += [tr(pos, rot, scale), prim(shape, color, lighting)]
        parent = self.cave_folder["guid"] if (cave and self.cave_folder) else self.field["guid"]
        tags = {"is_terrain": 1} if terrain else {}
        return self.add(entity(name, comps, parent, tags))

    def light(self, name, pos, color, intensity, radius, decay=1.0):
        """点光源だけを置く（入口から差し込む光など）"""
        if not self.in_window(pos[2] - radius * 0.5, pos[2] + radius * 0.5):
            return
        parent = self.cave_folder["guid"] if self.cave_folder else self.field["guid"]
        self.add(entity(name, [tr(pos), point_light(color, intensity, radius, decay)], parent))

    def crystal(self, pos, color, light_intensity=1.6, radius=14.0, size=0.9):
        """発光する結晶（ライティング無しの小さな八面体風の球 ＋ 点光源）"""
        if not self.in_window(pos[2], pos[2]):
            return
        parent = self.cave_folder["guid"] if self.cave_folder else self.field["guid"]
        self.add(entity("Crystal", [tr(pos, (0.6, 0.3, 0.4), (size, size * 1.8, size)),
                                    prim("sphere", color, lighting=0)], parent))
        self.add(entity("CrystalLight", [tr((pos[0], pos[1], pos[2])),
                                         point_light(color, light_intensity, radius)], parent))

    # --- 敵 ---
    def spawner(self, kind, wave, pos, count=None, **over):
        base = {"shark": SHARK, "octopus": OCTO, "ship": SHIP}[kind]
        data = copy.deepcopy(base)
        data["waveId"] = wave
        if count is not None:
            data["count"] = count
        for k, v in over.items():
            if k == "ship_params":
                for s in data["scripts"]:
                    if s["name"] == "ShipEnemyScript":
                        s["params"].update(v)
            elif k == "enemy_params":
                data.setdefault("enemyParams", {}).update(v)
            else:
                data[k] = v
        self.spawner_count += 1
        nm = f"WaveSpawner_{wave}_{kind}_{self.spawner_count}"
        rot = (0.0, math.pi, 0.0) if kind == "ship" else (0.0, 0.0, 0.0)
        comps = [{"type": "NativeScriptComponent", "enabled": True,
                  "data": {"scriptDataList": [data], "scriptTypeName": "WaveSpawnerScript",
                           "scriptTypeNames": ["WaveSpawnerScript"]}},
                 tr(pos, rot)]
        self.add(entity(nm, comps, 0, {"spawner_wave_id": wave, "wave_spawner": 1}))

    def chest(self, pos, hp=3, points=100):
        self.chest_count += 1
        data = copy.deepcopy(tpl_script_data("TreasureChest_1"))
        data.update({"hp": hp, "maxHp": hp, "points": points})
        comps = [collider(),
                 {"type": "NativeScriptComponent", "enabled": True,
                  "data": {"scriptDataList": [data], "scriptTypeName": "TreasureChestScript",
                           "scriptTypeNames": ["TreasureChestScript"]}},
                 tr(pos, scale=(1.4, 1.0, 1.0)),
                 prim("box", (0.55, 0.35, 0.15, 1.0))]
        self.add(entity(f"TreasureChest_{self.chest_count}", comps, self.field["guid"],
                        {"current_hp": hp, "is_item": 1, "max_hp": hp}))

    def floater(self, name, pos, scale):
        e = copy.deepcopy(TPL[name])
        e["guid"] = guid()
        e["parentGuid"] = self.field["guid"]
        for c in e["components"]:
            if c["type"] == "TransformComponent":
                c["data"]["position"] = list(pos)
                c["data"]["scale"] = list(scale)
        self.add(e)

    # --- 共通エンティティ ---
    def camera(self, waypoints, rise_depth, start_text, underwater_look=None, far_z=100.0):
        e = copy.deepcopy(TPL["Camera"])
        e["guid"] = guid()
        nsc = next(c for c in e["components"] if c["type"] == "NativeScriptComponent")["data"]
        lst = nsc["scriptDataList"]
        # [0] DeepRiseIntroScript
        lst[0]["riseDepth"] = rise_depth
        lst[0]["startText"] = start_text
        lst[0]["showStartText"] = False  # ステージ名の帯演出は出さない
        # [1] RailShooterController（水中の見た目）
        if underwater_look:
            lst[1]["underwater"].update(underwater_look)
        # [2] RailMovementScript
        lst[2]["waypoints"] = waypoints
        lst[2]["isMoving"] = True
        # [3] DeathSinkScript は Result へ（既定のまま）
        for c in e["components"]:
            if c["type"] == "TransformComponent":
                c["data"]["position"] = list(waypoints[0]["pos"])
                c["data"]["rotation"] = [0.0, 0.0, 0.0]
            if c["type"] == "CameraComponent":
                c["data"]["farZ"] = far_z
        e["tags"] = {"has_rail": 1, "is_player": 1, "is_underwater": 1 if waypoints[0]["pos"][1] < 0.0 else 0,
                     "rail_branch_to": -1, "rail_wp": 1}
        self.add(e)

    def water(self, **over):
        e = copy.deepcopy(TPL["Water"])
        e["guid"] = guid()
        e["parentGuid"] = self.field["guid"]
        for c in e["components"]:
            if c["type"] == "WaterComponent":
                c["data"].update(over)
        self.add(e)

    def sky(self, visible=True, color=(1.0, 1.0, 1.0, 1.0)):
        e = copy.deepcopy(TPL["Skybox"])
        e["guid"] = guid()
        e["parentGuid"] = self.field["guid"]
        for c in e["components"]:
            if c["type"] == "SkyboxComponent":
                c["data"]["visible"] = visible
                c["data"]["color"] = list(color)
        self.add(e)

    def sun(self, direction, color, intensity, ambient_color=(1.0, 1.0, 1.0), ambient=0.0):
        e = copy.deepcopy(TPL["Directional Light"])
        e["guid"] = guid()
        e["parentGuid"] = self.field["guid"]
        for c in e["components"]:
            if c["type"] == "DirectionalLightComponent":
                c["data"].update({"direction": list(direction), "color": list(color), "intensity": intensity,
                                  "ambientColor": list(ambient_color), "ambientIntensity": ambient})
        self.add(e)
        return e

    def light_zone(self, sun_entity, from_z, to_z, a, b):
        """カメラの z に応じて太陽光・環境光・水面の映り込みを a → b へ補間する（CaveLightZoneScript）。
        屋外 ⇔ 洞窟を 1 面の中で行き来する面で使う（平行光源に影が無いので、洞窟の中を暗くするため）。
        a / b は {"intensity", "color", "ambientColor", "ambientIntensity", "waterEnvironment"}。"""
        sun_entity["components"].append({
            "type": "NativeScriptComponent", "enabled": True,
            "data": {"scriptDataList": [{"fromZ": from_z, "toZ": to_z, "a": a, "b": b}],
                     "scriptTypeName": "CaveLightZoneScript", "scriptTypeNames": ["CaveLightZoneScript"]}})

    def managers(self):
        wm = copy.deepcopy(TPL["WaveManager"])
        wm["guid"] = guid()
        wm["tags"] = {"wave_active": 0, "wave_busy": 0, "wave_cleared_id": 0, "waves_cleared": 0}
        self.add(wm)
        pm = copy.deepcopy(TPL["PauseMenu"])
        pm["guid"] = guid()
        for c in pm["components"]:
            if c["type"] == "NativeScriptComponent":
                d = c["data"]["scriptDataList"][0]
                d["retryScene"] = ""          # いまのステージをやり直す
                d["titleScene"] = "Select"
                d["titleLabel"] = "セレクトへ"
        self.add(pm)

    def save(self, force):
        path = os.path.join(SCENE_DIR, f"{self.name}.json")
        if os.path.exists(path) and not force:
            print(f"skip (exists): {path}")
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"sceneName": self.name, "entities": self.entities}, f, ensure_ascii=False, indent=2)
        print(f"wrote {path} ({len(self.entities)} entities)")


def wp(x, y, z, wave=0, end=False):
    d = {"pos": [float(x), float(y), float(z)], "waitTime": 0.0}
    if wave:
        d["waveId"] = wave
    if end:
        d["isEnd"] = True
    return d


# 色
ROCK = (0.55, 0.50, 0.45, 1.0)
CAVE_ROCK = (0.30, 0.27, 0.25, 1.0)
CAVE_ROCK_DARK = (0.20, 0.18, 0.17, 1.0)
SAND = (0.80, 0.70, 0.50, 1.0)
CAVE_SAND = (0.42, 0.38, 0.32, 1.0)
CYAN = (0.35, 0.95, 1.0, 1.0)
TEAL = (0.30, 1.0, 0.75, 1.0)
AMBER = (1.0, 0.70, 0.35, 1.0)
VIOLET = (0.75, 0.45, 1.0, 1.0)

# 洞窟の水中の見た目（外洋より暗く、光の筋は弱い）
CAVE_WATER_LOOK = {
    "shallowFogColor": [0.0, 0.12, 0.20, 1.0], "shallowFogStart": 4.0, "shallowFogEnd": 70.0,
    "deepDepth": 40.0, "deepBias": 1.2,
    "tintColor": [0.25, 0.60, 0.85, 1.0],
    "causticsIntensity": 0.35, "lightShaftIntensity": 0.25, "lightShaftDensity": 0.12,
}
# 旧 Stage4（深淵水路）で使っていた、より暗い水中の見た目。今は未使用（水中面を増やすときの候補）
ABYSS_WATER_LOOK = {
    "shallowFogColor": [0.0, 0.07, 0.12, 1.0], "shallowFogStart": 3.0, "shallowFogEnd": 55.0,
    "deepDepth": 55.0, "deepBias": 1.1,
    "tintColor": [0.20, 0.65, 0.80, 1.0],
    "causticsIntensity": 0.0, "lightShaftIntensity": 0.6, "lightShaftDensity": 0.05,
}
CAVE_WATER_SURFACE = {
    "deepColor": [0.01, 0.04, 0.08, 0.97], "shallowColor": [0.04, 0.20, 0.26, 0.9],
    "environmentCoeff": 0.12, "waveHeight": 0.06, "waveHeight2": 0.05, "waveSpeed": 1.2, "waveSpeed2": 1.0,
}


def cave_shell(st, x_half, z0, z1, ceil_y, floor_y, ceil_thick=8.0, wall_color=CAVE_ROCK, seg=30.0,
               open_ceiling_from=None, ceil_fn=None, back_wall=True, front_wall=True, name_prefix="Cave"):
    """洞窟の外殻（左右の壁・天井・床・前後の壁）を区間ごとの箱で並べる。
    区間ごとに幅と天井をわずかに揺らして、まっすぐな箱のトンネルに見えないようにする。
    ceil_fn(z) を渡すと区間ごとに天井の高さを変えられる（水上の広間 → 水没した通路など）。
    back_wall / front_wall を False にすると、その端は壁を置かずに開けておく（崖の穴から出入りする面）。"""
    if ceil_fn is None:
        ceil_fn = lambda _z: ceil_y  # noqa: E731
    top = max(ceil_fn(z0 + k * seg + seg * 0.5) for k in range(int((z1 - z0) / seg) + 1))
    top = max(top, ceil_y)
    wall_h = top + ceil_thick - floor_y + 10.0
    wall_cy = (top + ceil_thick + floor_y - 10.0) * 0.5
    z = z0
    i = 0
    while z < z1:
        zl = min(seg, z1 - z)
        zc = z + zl * 0.5
        wob = 2.5 * math.sin(i * 1.7)
        cw = 2.0 * math.sin(i * 2.3 + 0.5)
        for side in (-1, 1):
            st.block(f"{name_prefix}Wall_{'L' if side < 0 else 'R'}{i}",
                     (side * (x_half + 4.0 + side * wob * 0.5), wall_cy, zc), (8.0, wall_h, zl + 0.5),
                     wall_color if i % 2 == 0 else CAVE_ROCK_DARK, cave=True)
        if open_ceiling_from is None or zc < open_ceiling_from:
            st.block(f"{name_prefix}Ceiling_{i}", (0.0, ceil_fn(zc) + ceil_thick * 0.5 + cw * 0.3, zc),
                     (x_half * 2.0 + 10.0, ceil_thick, zl + 0.5), CAVE_ROCK_DARK, cave=True)
        st.block(f"{name_prefix}Floor_{i}", (0.0, floor_y - 2.5, zc), (x_half * 2.0 + 10.0, 5.0, zl + 0.5), CAVE_SAND,
                 cave=True)
        z += zl
        i += 1
    # 前後の壁（入口・出口を塞ぐ。farZ の外に置くので見えるのは暗がりだけ）
    ends = []
    if back_wall:
        ends.append(("Back", z0 - 2.0))
    if front_wall:
        ends.append(("Front", z1 + 2.0))
    for tag, zz in ends:
        st.block(f"{name_prefix}End_{tag}", (0.0, wall_cy, zz), (x_half * 2.0 + 16.0, wall_h, 4.0), CAVE_ROCK_DARK,
                 cave=True)


def stalactites(st, x_half, z0, z1, ceil_y, n, max_len, seed, ceil_fn=None, min_tip_y=None):
    """天井から垂れる鍾乳石。ceil_fn(z) で天井の高さ、min_tip_y で先端の下限（水面やレールに刺さらないように）"""
    r = random.Random(seed)
    for k in range(n):
        x = r.uniform(-x_half + 2.0, x_half - 2.0)
        z = r.uniform(z0, z1)
        cy = ceil_fn(z) if ceil_fn else ceil_y
        ln = r.uniform(max_len * 0.4, max_len)
        if min_tip_y is not None:
            ln = min(ln, cy - min_tip_y)
            if ln < 0.8:
                continue
        ceil_y_here = cy
        w = r.uniform(0.8, 2.0)
        # 円錐を上下反転（x 軸まわりに π）して天井から垂らす
        st.block(f"Stalactite{seed}_{k}", (x, ceil_y_here - ln * 0.5 + 0.3, z), (w, ln, w),
                 CAVE_ROCK if k % 2 else CAVE_ROCK_DARK, rot=(math.pi, 0.0, 0.0), shape="cone",
                 cave=True, solid=False)


def pillars(st, x_half, z0, z1, floor_y, n, top_y, seed, terrain=False, avoid=None, name="RockPillar"):
    """床から立つ岩の柱。avoid=(x, 半径) のリストでレールの近くを避ける"""
    r = random.Random(seed)
    k = 0
    tries = 0
    while k < n and tries < n * 20:
        tries += 1
        x = r.uniform(-x_half + 2.5, x_half - 2.5)
        z = r.uniform(z0, z1)
        if avoid and any(abs(x - ax(z)) < rad for ax, rad in avoid):
            continue
        w = r.uniform(2.0, 4.5)
        h = top_y - floor_y + r.uniform(-3.0, 2.0)
        st.block(f"{name}_{k}", (x, floor_y + h * 0.5, z), (w, h, w),
                 CAVE_ROCK if k % 2 else ROCK, rot=(0.0, r.uniform(0, 3.14), 0.0), cave=True, terrain=terrain)
        k += 1


def rail_x(waypoints):
    """z から、そのあたりのレールの x を線形補間で返す（柱の配置で避けるため）"""
    pts = [(w["pos"][2], w["pos"][0]) for w in waypoints]

    def f(z):
        if z <= pts[0][0]:
            return pts[0][1]
        for (z0, x0), (z1, x1) in zip(pts, pts[1:]):
            if z0 <= z <= z1:
                t = (z - z0) / max(z1 - z0, 1e-3)
                return x0 + (x1 - x0) * t
        return pts[-1][1]
    return f


# ===========================================================================
# 共通の世界（5 面は 1 本の航路。各面のスタート＝ひとつ前の面のゴール）
# ===========================================================================
# すべての面が同じワールド座標を使う。z 方向に進み、区間はこう並ぶ:
#
#   z -100 ─ 175  外洋 A（Stage1 全体と Stage2 の冒頭）
#   z  175 ─ 185  崖 A と洞窟の口
#   z  185 ─ 330  岩窟水道（Stage2 の後半）
#   z  330 ─ 530  広間 → 天井が下がって水没した通路（Stage3）
#   z  530 ─ 730  水没した通路 → 天井が上がって地底湖（Stage4）
#   z  730 ─ 790  出口の水路（Stage5 の冒頭）
#   z  790 ─ 800  崖 B と洞窟の口
#   z  800 ─      外洋 B（Stage5）
#
# 各面は自分の区間の前後（WINDOW_BEHIND / WINDOW_AHEAD）にある地形だけを取り込む。
# そのため Stage1 のゴールから見える崖と洞窟の口は、Stage2 のスタートで見えるものと同じ位置・同じ形になる。
# レールは面の最後の 2 点と次の面の最初の 2 点をまっすぐ z 方向にそろえ、向きもつながるようにしている。
WINDOW_BEHIND = 40.0
WINDOW_AHEAD = 260.0

CLIFF_A_Z = 180.0
CLIFF_B_Z = 795.0
CAVE_FLOOR_Y = -22.0

# 屋外 ⇔ 洞窟の明るさ（CaveLightZoneScript の a / b）と水面の色
SEA_WATER = {"waterShallow": [0.10, 0.50, 0.60, 0.85], "waterDeep": [0.02, 0.10, 0.25, 0.95]}
DAWN_WATER = {"waterShallow": [0.18, 0.45, 0.55, 0.85], "waterDeep": [0.04, 0.10, 0.22, 0.95]}
CAVE_WATER = {"waterShallow": CAVE_WATER_SURFACE["shallowColor"], "waterDeep": CAVE_WATER_SURFACE["deepColor"]}
SUN_DIR = (-0.5, -0.8, 0.5)
DAWN_DIR = (-0.7, -0.35, 0.6)
OUTSIDE_LIGHT = dict({"intensity": 1.0, "color": [1.0, 1.0, 1.0, 1.0], "ambientColor": [1.0, 1.0, 1.0],
                      "ambientIntensity": 0.0, "waterEnvironment": 0.6}, **SEA_WATER)
DAWN_LIGHT = dict({"intensity": 1.0, "color": [1.0, 0.78, 0.55, 1.0], "ambientColor": [1.0, 0.8, 0.7],
                   "ambientIntensity": 0.08, "waterEnvironment": 0.6}, **DAWN_WATER)
CAVE_LIGHT = dict({"intensity": 0.3, "color": [0.7, 0.72, 0.8, 1.0], "ambientColor": [0.5, 0.5, 0.58],
                   "ambientIntensity": 0.25, "waterEnvironment": 0.12}, **CAVE_WATER)

# --- 各面のレール（ワールド座標）---
RAIL = {
    "Stage1": [wp(0, 2, -50), wp(4, 2, -25, wave=1), wp(-3, 2, 5), wp(2, 2, 30, wave=2), wp(8, 2, 60),
               wp(0, 2, 90, wave=3), wp(0, 2, 110), wp(0, 2, 125, end=True)],
    "Stage2": [wp(0, 2, 125), wp(4, 2, 145, wave=1), wp(0, 2, 168), wp(0, 2, 195), wp(-4, 2, 225, wave=2),
               wp(4, 2, 260), wp(0, 2, 290, wave=3), wp(0, 2, 310), wp(0, 2, 325, end=True)],
    "Stage3": [wp(0, 2, 325), wp(3, 2, 350, wave=1), wp(0, 2, 378), wp(0, 1.5, 398), wp(0, -6, 416),
               wp(-4, -10, 440, wave=2), wp(4, -12, 470), wp(0, -8, 495, wave=3), wp(0, -8, 515),
               wp(0, -8, 530, end=True)],
    "Stage4": [wp(0, -8, 530), wp(3, -10, 555, wave=1), wp(-3, -12, 580), wp(0, -10, 605, wave=2),
               wp(2, -5, 625), wp(0, 1.5, 642), wp(4, 2, 670, wave=3), wp(-3, 2, 700), wp(0, 2, 712),
               wp(0, 2, 725, end=True)],
    "Stage5": [wp(0, 2, 725), wp(0, 2, 755), wp(0, 2, 785), wp(0, 2, 810), wp(5, 2, 835, wave=1),
               wp(-4, 2, 865), wp(2, 2, 895, wave=2), wp(-6, 2, 925), wp(0, 2, 955, wave=3), wp(8, 2, 985),
               wp(0, 2, 1015, wave=4), wp(0, 2, 1050, end=True)],
}
ALL_RAIL = [w for k in ("Stage1", "Stage2", "Stage3", "Stage4", "Stage5") for w in RAIL[k]]


def hall_ceiling(z):
    """Stage3：広間（高い天井）→ 下がり始め → 水没した通路（天井が水面のすぐ上）"""
    if z < 390.0:
        return 14.0
    if z < 420.0:
        return 8.0
    return 2.5


def lake_ceiling(z):
    """Stage4：水没した通路 → 上がり始め → 地底湖の広間"""
    if z < 590.0:
        return 2.5
    if z < 620.0:
        return 8.0
    return 15.0


def cliff_with_arch(st, z, prefix):
    """崖の壁と、その真ん中の洞窟の口（幅 16m・高さ 11m）。z は崖の中心（厚さ 10m）"""
    st.block(f"{prefix}_L", (-26, 8, z), (36, 30, 10), CAVE_ROCK)
    st.block(f"{prefix}_R", (26, 8, z), (36, 30, 10), CAVE_ROCK)
    st.block(f"{prefix}_Arch", (0, 17, z), (16, 12, 10), CAVE_ROCK_DARK)


def build_world(st):
    """共通の世界の地形・結晶・光を、st.window の範囲だけ取り込む"""
    avoid = [(rail_x(ALL_RAIL), 6.0)]

    # --- 外洋 A ---
    for k, (p, s_) in enumerate([((-9, 0, -30), (5, 10, 5)), ((12, 0, -8), (6, 12, 6)),
                                 ((-12, 0, 20), (4, 8, 4)), ((16, 0, 45), (5, 9, 7)),
                                 ((-10, 0, 72), (6, 11, 5)), ((14, 0, 100), (4, 7, 4)),
                                 ((-13, 0, 140), (5, 9, 5)), ((14, 0, 158), (5, 11, 6))]):
        st.block(f"SeaRockA{k + 1}", p, s_, ROCK, terrain=True)
    st.block("SeabedA", (0, -30, 40), (80, 5, 300), SAND, terrain=True)
    cliff_with_arch(st, CLIFF_A_Z, "CliffA")

    # --- 岩窟水道（Stage2）---
    cave_shell(st, x_half=16.0, z0=185.0, z1=330.0, ceil_y=12.0, floor_y=CAVE_FLOOR_Y,
               back_wall=False, front_wall=False, name_prefix="Channel")
    stalactites(st, 16.0, 190.0, 325.0, 12.0, 20, 5.0, seed=22, min_tip_y=5.0)
    pillars(st, 16.0, 195.0, 325.0, CAVE_FLOOR_Y, 7, 3.0, seed=23, terrain=True, avoid=avoid,
            name="ChannelRock")
    st.light("EntranceLight", (0, 6, 190), (1.0, 0.95, 0.85, 1.0), 2.5, 35.0)
    for k, (x, y, z) in enumerate([(-14, 6, 215), (14, 5, 240), (-14, 7, 262), (14, 4, 285), (-13, 6, 310)]):
        st.crystal((x, y, z), CYAN if k % 2 == 0 else AMBER, light_intensity=2.0, radius=18.0, size=1.0)

    # --- 広間 → 水没した通路（Stage3）---
    cave_shell(st, x_half=18.0, z0=330.0, z1=530.0, ceil_y=14.0, floor_y=CAVE_FLOOR_Y, ceil_fn=hall_ceiling,
               back_wall=False, front_wall=False, name_prefix="Hall")
    stalactites(st, 18.0, 335.0, 418.0, 14.0, 18, 6.0, seed=32, ceil_fn=hall_ceiling, min_tip_y=4.5)
    stalactites(st, 18.0, 422.0, 528.0, 2.5, 12, 1.8, seed=33, min_tip_y=0.6)
    pillars(st, 18.0, 335.0, 395.0, CAVE_FLOOR_Y, 5, 3.0, seed=34, terrain=True, avoid=avoid, name="HallRock")
    pillars(st, 18.0, 430.0, 528.0, CAVE_FLOOR_Y, 7, -9.0, seed=35, avoid=avoid, name="HallPillar")
    for k, (x, y, z) in enumerate([(-16, 8, 340), (16, 6, 362), (-16, 7, 385), (15, 4, 405),
                                   (-14, -12, 435), (14, -16, 460), (-13, -18, 485), (13, -10, 510)]):
        st.crystal((x, y, z), (AMBER, CYAN, TEAL)[k % 3], light_intensity=2.0, radius=18.0)

    # --- 水没した通路 → 地底湖（Stage4）---
    cave_shell(st, x_half=20.0, z0=530.0, z1=730.0, ceil_y=15.0, floor_y=CAVE_FLOOR_Y, ceil_fn=lake_ceiling,
               back_wall=False, front_wall=False, name_prefix="Lake")
    stalactites(st, 20.0, 532.0, 588.0, 2.5, 10, 1.8, seed=42, min_tip_y=0.6)
    stalactites(st, 20.0, 592.0, 728.0, 15.0, 26, 7.0, seed=43, ceil_fn=lake_ceiling, min_tip_y=5.0)
    pillars(st, 20.0, 535.0, 600.0, CAVE_FLOOR_Y, 6, -9.0, seed=44, avoid=avoid, name="LakePillar")
    pillars(st, 20.0, 650.0, 725.0, CAVE_FLOOR_Y, 6, 3.5, seed=45, terrain=True, avoid=avoid, name="LakeRock")
    for k, (x, y, z) in enumerate([(-18, -12, 545), (18, -15, 570), (-17, -10, 595), (18, -8, 615),
                                   (-18, 6, 640), (18, 10, 665), (-18, 8, 690), (18, 11, 715)]):
        st.crystal((x, y, z), (TEAL, VIOLET, AMBER)[k % 3], light_intensity=2.2, radius=20.0, size=1.1)

    # --- 出口の水路（Stage5 の冒頭）---
    cave_shell(st, x_half=16.0, z0=730.0, z1=790.0, ceil_y=12.0, floor_y=CAVE_FLOOR_Y,
               back_wall=False, front_wall=False, name_prefix="Exit")
    stalactites(st, 16.0, 732.0, 788.0, 12.0, 8, 5.0, seed=52, min_tip_y=5.0)
    for k, (x, y, z) in enumerate([(-14, 6, 740), (14, 5, 760), (-13, 7, 780)]):
        st.crystal((x, y, z), (AMBER, CYAN)[k % 2], light_intensity=2.0, radius=18.0)
    st.light("ExitGlow", (0, 6, 786), (1.0, 0.8, 0.6, 1.0), 2.5, 35.0)
    cliff_with_arch(st, CLIFF_B_Z, "CliffB")

    # --- 外洋 B（Stage5）---
    for k, (p, s_) in enumerate([((-12, 0, 835), (5, 9, 5)), ((13, 0, 865), (6, 12, 6)),
                                 ((-14, 0, 903), (5, 10, 6)), ((15, 0, 935), (4, 8, 4)),
                                 ((-12, 0, 965), (6, 11, 6)), ((16, 0, 995), (5, 9, 5)),
                                 ((-10, 0, 1025), (4, 7, 4))]):
        st.block(f"SeaRockB{k + 1}", p, s_, ROCK, terrain=True)
    st.block("SeabedB", (0, -30, 950), (80, 5, 320), SAND, terrain=True)


def begin_stage(name, title, underwater, cave):
    st = Stage(name, title, underwater=underwater, cave=True)  # どの面にも洞窟の地形が入り得る
    wps = RAIL[name]
    st.window = (wps[0]["pos"][2] - WINDOW_BEHIND, wps[-1]["pos"][2] + WINDOW_AHEAD)
    build_world(st)
    return st, wps


# ===========================================================================
# Stage1 外洋（水上）… 洞窟の口が見えるところまで
# ===========================================================================
def stage1():
    st, wps = begin_stage("Stage1", "外洋", underwater=False, cave=False)
    st.water()
    st.sky()
    st.sun(SUN_DIR, (1.0, 1.0, 1.0, 1.0), 1.0)
    st.camera(wps, rise_depth=26.0, start_text="第1面　外洋", far_z=260.0)
    st.spawner("shark", 1, (4, 0.5, -12), count=3)
    st.spawner("octopus", 1, (-8, -1.5, -4), count=1)
    st.spawner("shark", 2, (2, 0.5, 45), count=4, spawnInterval=1.6)
    st.spawner("octopus", 2, (10, -1.5, 40), count=1)
    st.spawner("ship", 3, (12, 0.5, 118), count=1)
    st.spawner("shark", 3, (-4, 0.5, 110), count=2, startDelay=3.0, aheadDistance=35.0)
    st.chest((-4, 1.2, -18), hp=3, points=100)
    st.chest((10, 1.2, 52), hp=5, points=300)
    st.floater("FloatCrate", (4, 2.8, -20), (2, 1, 3))
    st.floater("FloatBuoy", (-6, 2.8, 36), (1, 1, 1))
    st.managers()
    return st


# ===========================================================================
# Stage2 岩窟水道 … Stage1 のゴール（崖の手前）から、口をくぐって洞窟へ
# ===========================================================================
def stage2():
    st, wps = begin_stage("Stage2", "岩窟水道", underwater=False, cave=True)
    st.water()
    st.sky()
    sun = st.sun(SUN_DIR, (1.0, 1.0, 1.0, 1.0), 1.0)
    st.light_zone(sun, from_z=CLIFF_A_Z - 8.0, to_z=CLIFF_A_Z + 25.0, a=OUTSIDE_LIGHT, b=CAVE_LIGHT)
    st.camera(wps, rise_depth=26.0, start_text="第2面　岩窟水道", far_z=240.0)
    small_ship = {"maxHp": 20, "patrolRadius": 7.0, "detectDistance": 40.0, "preferredDistance": 18.0}
    # 外のウェーブは崖の裏（洞窟の中）に湧かないよう、前方の距離を縮める
    st.spawner("shark", 1, (4, 0.5, 158), count=3, aheadDistance=20.0)
    st.spawner("octopus", 1, (-8, -1.5, 160), count=1)
    st.spawner("shark", 2, (0, 0.5, 270), count=3, spawnInterval=1.8, aheadDistance=45.0)
    st.spawner("octopus", 2, (-6, -1.5, 262), count=2)
    st.spawner("ship", 3, (6, 0.5, 318), count=1, ship_params=small_ship, enemyScale=[2.4, 1.3, 7.0])
    st.spawner("shark", 3, (-3, 0.5, 312), count=2, startDelay=3.0, aheadDistance=30.0)
    st.chest((-6, 1.2, 150), hp=3, points=150)
    st.chest((9, 1.2, 248), hp=5, points=300)
    st.floater("FloatBuoy", (5, 2.8, 135), (1, 1, 1))
    st.floater("FloatCrate", (-6, 2.8, 236), (1.5, 1, 2))
    st.managers()
    return st


# ===========================================================================
# Stage3 沈み洞 … Stage2 のゴール（水道の奥）から広間を進み、水没した通路へもぐる
# ===========================================================================
def stage3():
    st, wps = begin_stage("Stage3", "沈み洞", underwater=True, cave=True)
    st.water(**CAVE_WATER_SURFACE)
    st.sky(visible=False)
    st.sun(SUN_DIR, tuple(CAVE_LIGHT["color"]), CAVE_LIGHT["intensity"],
           ambient_color=tuple(CAVE_LIGHT["ambientColor"]), ambient=CAVE_LIGHT["ambientIntensity"])
    st.camera(wps, rise_depth=13.0, start_text="第3面　沈み洞", underwater_look=CAVE_WATER_LOOK,
              far_z=240.0)  # 前半は水上（フォグが無い）なので遠くまで描く。水中に入ればフォグで隠れる
    st.spawner("shark", 1, (3, 0.5, 368), count=3, aheadDistance=30.0)
    st.spawner("octopus", 1, (-6, -1.5, 372), count=1)
    st.spawner("shark", 2, (0, -11, 470), count=4, spawnInterval=1.6)
    st.spawner("octopus", 2, (8, -10, 465), count=2)
    st.spawner("shark", 3, (0, -8, 525), count=5, spawnInterval=1.4, aheadDistance=35.0)
    st.spawner("octopus", 3, (-8, -8, 520), count=1, startDelay=2.5)
    st.chest((-8, 1.2, 342), hp=3, points=150)
    st.chest((9, -11, 480), hp=5, points=300)
    st.floater("FloatCrate", (5, 2.8, 360), (2, 1, 3))
    st.managers()
    return st


# ===========================================================================
# Stage4 地底湖 … Stage3 のゴール（水没した通路）から浮上して地底湖へ
# ===========================================================================
def stage4():
    st, wps = begin_stage("Stage4", "地底湖", underwater=False, cave=True)
    st.water(**CAVE_WATER_SURFACE)
    st.sky(visible=False)
    st.sun(SUN_DIR, tuple(CAVE_LIGHT["color"]), CAVE_LIGHT["intensity"],
           ambient_color=tuple(CAVE_LIGHT["ambientColor"]), ambient=CAVE_LIGHT["ambientIntensity"])
    st.camera(wps, rise_depth=10.0, start_text="第4面　地底湖", underwater_look=CAVE_WATER_LOOK,
              far_z=240.0)
    small_ship = {"maxHp": 25, "patrolRadius": 9.0, "detectDistance": 45.0, "preferredDistance": 22.0}
    st.spawner("shark", 1, (3, -9, 580), count=3)
    st.spawner("octopus", 1, (-6, -10, 585), count=1)
    st.spawner("shark", 2, (0, -10, 650), count=4, spawnInterval=1.6)
    st.spawner("octopus", 2, (8, -9, 640), count=2)
    st.spawner("ship", 3, (10, 0.5, 705), count=1, ship_params=small_ship, enemyScale=[2.4, 1.3, 7.0])
    st.spawner("shark", 3, (-4, 0.5, 700), count=3, startDelay=3.0, aheadDistance=45.0)
    st.spawner("octopus", 3, (6, -1.5, 695), count=1, startDelay=2.0)
    st.chest((-8, -12, 590), hp=3, points=150)
    st.chest((12, 1.2, 690), hp=5, points=300)
    st.floater("FloatCrate", (-4, 2.8, 680), (2, 1, 3))
    st.managers()
    return st


# ===========================================================================
# Stage5 黎明の海 … Stage4 のゴール（地底湖の奥）から出口の水路を抜けて外洋へ（最終面）
# ===========================================================================
def stage5():
    st, wps = begin_stage("Stage5", "黎明の海", underwater=False, cave=False)
    st.water(**{"shallowColor": CAVE_WATER["waterShallow"], "deepColor": CAVE_WATER["waterDeep"],
                "environmentCoeff": CAVE_LIGHT["waterEnvironment"]})
    st.sky(color=(1.0, 0.86, 0.75, 1.0))
    sun = st.sun(DAWN_DIR, tuple(CAVE_LIGHT["color"]), CAVE_LIGHT["intensity"],
                 ambient_color=tuple(CAVE_LIGHT["ambientColor"]), ambient=CAVE_LIGHT["ambientIntensity"])
    st.light_zone(sun, from_z=CLIFF_B_Z - 25.0, to_z=CLIFF_B_Z + 10.0, a=CAVE_LIGHT, b=DAWN_LIGHT)
    st.camera(wps, rise_depth=14.0, start_text="第5面　黎明の海", underwater_look=CAVE_WATER_LOOK,
              far_z=240.0)
    st.spawner("shark", 1, (5, 0.5, 850), count=4, spawnInterval=1.6)
    st.spawner("octopus", 1, (-8, -1.5, 855), count=2)
    st.spawner("ship", 2, (14, 0.5, 925), count=1)
    st.spawner("shark", 2, (-4, 0.5, 915), count=3, startDelay=3.0)
    st.spawner("octopus", 3, (0, -1.5, 965), count=3, spawnInterval=1.2)
    st.spawner("shark", 3, (6, 0.5, 970), count=4, startDelay=1.5)
    st.spawner("ship", 4, (16, 0.5, 1040), count=1)
    st.spawner("ship", 4, (-16, 0.5, 1045), count=1, startDelay=4.0)
    st.spawner("shark", 4, (0, 0.5, 1035), count=3, startDelay=6.0)
    st.chest((4, 1.2, 760), hp=3, points=150)  # 洞窟の中（抜ける前のご褒美）
    st.chest((-5, 1.2, 837), hp=3, points=150)
    st.chest((11, 1.2, 915), hp=5, points=300)
    st.chest((-9, 1.2, 995), hp=8, points=600)
    st.floater("FloatCrate", (6, 2.8, 875), (2, 1, 3))
    st.floater("FloatBuoy", (-5, 2.8, 955), (1, 1, 1))
    st.managers()
    return st


if __name__ == "__main__":
    force = "--force" in sys.argv
    only = {a for a in sys.argv[1:] if not a.startswith("--")}
    for build in (stage1, stage2, stage3, stage4, stage5):
        st = build()  # guid の乱数列を面ごとに変えないため、対象外の面も組み立てだけは行う
        if only and st.name not in only:
            continue
        st.save(force)
