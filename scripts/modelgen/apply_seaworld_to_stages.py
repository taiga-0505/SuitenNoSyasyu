# -*- coding: utf-8 -*-
"""
水上ステージ（屋外区間）の地形を、箱プリミティブから seaworld のモデルへ差し替える。

  python scripts/modelgen/seaworld.py                      # 先にモデルを作る（Resources/model/seaworld/）
  python scripts/modelgen/apply_seaworld_to_stages.py      # Stage1 / Stage2 / Stage5 に当てる
  python scripts/modelgen/apply_seaworld_to_stages.py Stage1

やること（何度実行しても同じ結果になる）:
  SeaRock* … PrimitiveMesh → ModelRenderer（searock_*.glb）。Transform・Collider・is_terrain はそのまま。
             モデルは ±0.5 の箱に正規化してあるので、旧プリミティブと同じ外接箱に収まる。
             水面の反射に使う半径は、箱の外接円ではなく岩の水際の実寸を water_obstacle_radius タグで渡す。
  Seabed*  … 同上（seabed.glb）。
  Cliff*_L / _R / _Arch … 見た目（PrimitiveMesh）だけ外し、当たり判定の箱として残す。
             代わりに Cliff*_Model（cliff_cave.glb、当たり判定なし）を 1 つ置く。
  島        … Island_* を SeaWorld フォルダの下に置く（当たり判定なし・背景）。レールから遠いものは palette_far.png で霞ませる。
  影        … 崖のモデル・島・海底は cast_shadow = 0（平行光源の影は原点の ±40m しか覆わないので、
              影キャスターの一覧に入れても描かれず、毎フレームの登録と判定の手間だけがかかる）。
  洞窟      … 洞窟に入らない面（Stage1）では、洞窟の中身の箱・円錐を 1 つのモデル（Cave_Merged）にまとめる。
              元のエンティティは当たり判定として残し、PrimitiveMesh の visible だけ false にする。
  カメラ    … farZ を FAR_Z へ広げる。
  水面      … 1000m 四方の水面が面の範囲を覆うよう、中心の z を面ごとに動かす（高さ y は変えない）。

generate_stages.py で面を作り直したときは、このスクリプトをもう一度実行すること。
"""
import json
import math
import os
import random
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
SCENES = os.path.join(ROOT, "project", "Resources", "Scenes")
META = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seaworld_meta.json")
MODEL_DIR = "Resources/model/seaworld/"
TAG = "seaworld"  # このスクリプトが足したエンティティの目印

CLIFF_A_Z = 180.0
CLIFF_B_Z = 795.0
FAR_Z = 520.0
FAR_DIST = 170.0  # レールからこれより遠い島は霞ませる（<name>_far.jpg）

# 面ごとの設定。water_z = 水面の中心 z（1000m 四方。面の始点〜終点＋farZ をほぼ覆う位置）
STAGES = {
    # merge_cave: 洞窟に入らない面。洞窟の中身（プリミティブ）を 1 つのモデルにまとめて描画コマンドを減らす
    "Stage1": {"water_z": 150.0, "islands": "A", "merge_cave": True},
    "Stage2": {"water_z": 150.0, "islands": "A"},
    "Stage5": {"water_z": 1000.0, "islands": "B"},
}

# 島の配置（ワールド座標）。(モデル, x, z, 向き rad, 一様スケール)
# 外洋 A：崖 A（z = 180、両端は奥へ回り込む）の手前。
# カメラの縦画角は 0.45rad（横 ≒ ±22°）と狭く、正面は崖がふさぐので、崖の手前にも小島を置いて
# 崖を背にした島影が正面に入るようにしている。左右の島は視点を振ったとき（maxYawDeg = 80）に見える。
ISLANDS_A = [
    ("island_stacks", 88.0, -8.0, 1.2, 0.9),
    ("island_large", -135.0, 35.0, 0.6, 1.0),
    ("island_low", -66.0, 112.0, 0.3, 1.0),
    ("island_medium", 98.0, 72.0, 2.0, 0.9),
    ("island_low", 54.0, 150.0, 0.8, 1.0),      # 崖の手前（正面の視界に入る）
    ("island_stacks", -60.0, 146.0, 4.4, 0.8),  # 同上
    ("island_stacks", -98.0, -62.0, 3.3, 0.8),
    ("island_large", 245.0, -30.0, 2.6, 0.9),
    ("island_medium", -225.0, 150.0, 4.0, 1.2),
    ("island_large", 340.0, 300.0, 1.1, 1.3),   # 崖の東端の向こうに覗く
    ("island_stacks", -310.0, 330.0, 5.2, 1.2),  # 崖の西端の向こうに覗く
]
# 外洋 B：崖 B（z = 795）を抜けた先
ISLANDS_B = [
    ("island_stacks", 78.0, 880.0, 0.4, 1.0),
    ("island_large", -150.0, 930.0, 2.2, 1.0),
    ("island_low", -72.0, 1012.0, 1.0, 1.0),
    ("island_medium", 125.0, 1045.0, 3.1, 1.1),
    ("island_large", 40.0, 1300.0, 0.9, 1.3),   # 最後に正面に見える大きな島
    ("island_stacks", -170.0, 1185.0, 5.0, 1.1),
    ("island_medium", 240.0, 1200.0, 1.7, 1.2),
    ("island_low", 205.0, 905.0, 2.4, 1.1),
]
# 当たり判定を持たない遠景なので、レールのどの点からも十分離れていることだけ確認する
RAIL_X_HALF = 10.0


def model_renderer(name, far=False):
    """seaworld のモデル。法線マップ・ラフネスはモデルごとの焼き込み。far なら縮小して霞ませた albedo に差し替える"""
    return {"type": "ModelRendererComponent", "enabled": True,
            "data": {"blendMode": 0, "color": [1.0, 1.0, 1.0, 1.0], "environmentCoeff": 0.0,
                     "lightingMode": 2,  # Half Lambert（旧プリミティブの屋外の岩と同じ）
                     "modelPath": MODEL_DIR + name + ".glb",
                     "normalMapPath": MODEL_DIR + name + "_normal.jpg",
                     "roughnessMapPath": MODEL_DIR + "rough_none.png",  # 全面ハイライト無し（共通の 4x4）
                     "texturePath": MODEL_DIR + name + "_far.jpg" if far else "", "visible": True}}


def transform(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return {"type": "TransformComponent", "enabled": True,
            "data": {"position": [float(x) for x in pos], "rotation": [float(x) for x in rot],
                     "scale": [float(x) for x in scale]}}


def comp(e, typ):
    return next((c for c in e["components"] if c["type"] == typ), None)


def rail_points(entities):
    cam = next((e for e in entities if e["name"] == "Camera"), None)
    if not cam:
        return []
    for c in cam["components"]:
        if c["type"] == "NativeScriptComponent":
            for sd in c["data"].get("scriptDataList", []):
                if isinstance(sd, dict) and "waypoints" in sd:
                    return [w["pos"] for w in sd["waypoints"]]
    return []


def searock_variant(k, scale):
    sx, sy, sz = (abs(v) for v in scale)
    aspect = sy / max(sx, sz)
    if aspect >= 1.95:
        return "searock_stack"
    return ("searock_twin", "searock_boulder", "searock_stack")[k % 3]


def waterline_radius(meta, model, scale, yaw):
    """岩の水際の外接半径（m）。モデルの水際の点を Transform で伸ばして測る"""
    pts = meta[model]["waterline"]
    sx, _, sz = (abs(v) for v in scale)
    c, s = math.cos(yaw), math.sin(yaw)
    r = 0.0
    for x, z in pts:
        x2, z2 = x * sx, z * sz
        # 向きを変えても原点からの距離は同じ（回転は Transform の scale の後）
        r = max(r, math.hypot(x2 * c + z2 * s, -x2 * s + z2 * c))
    return r


# ---------------------------------------------------------------------------
# 洞窟の中身を 1 つのモデルにまとめる（洞窟に入らない面だけ）
# ---------------------------------------------------------------------------
# Stage1 では洞窟の中身（箱・円錐のプリミティブ約 70 個）が崖の口越しに見えるだけだが、
# プリミティブは 1 個ずつ描画コマンドを積むため、毎フレームのコマンド記録がそのぶん重かった
# （Hierarchy で Cave を消すと CPU が 2〜3ms 下がる）。口越しの見え方は変えずに 1 ドローへまとめる。
# 当たり判定（ColliderComponent）は元のエンティティに残し、PrimitiveMesh の visible だけ false にする。
CAVE_TAG = "seaworld_cave_merged"


def _rot_rows(rx, ry, rz):
    """エンジンの MakeRotateMatrix(X/Y/Z) と同じ行ベクトル規約の回転（v' = v * Rx * Ry * Rz）"""
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    X = [[1, 0, 0], [0, cx, sx], [0, -sx, cx]]
    Y = [[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]]
    Z = [[cz, sz, 0], [-sz, cz, 0], [0, 0, 1]]
    import numpy as np
    return np.array(X) @ np.array(Y) @ np.array(Z)


def _prim_mesh(ptype):
    """MeshGenerator と同じ形（Box = ±0.5 の箱、Cone = 半径 1・高さ 1・中心原点・32 分割）"""
    import numpy as np
    if ptype == 1:
        faces = [((0, 0, 1), (-1, 0, 0), (0, 1, 0)), ((0, 0, -1), (1, 0, 0), (0, 1, 0)),
                 ((0, 1, 0), (1, 0, 0), (0, 0, 1)), ((0, -1, 0), (1, 0, 0), (0, 0, -1)),
                 ((1, 0, 0), (0, 0, 1), (0, 1, 0)), ((-1, 0, 0), (0, 0, -1), (0, 1, 0))]
        P, N, I = [], [], []
        for n, t, b in faces:
            base = len(P)
            for y in (0, 1):
                for x in (0, 1):
                    fx, fy = x * 2 - 1, y * 2 - 1
                    P.append([(n[k] + t[k] * fx + b[k] * fy) * 0.5 for k in range(3)])
                    N.append(n)
            I += [[base, base + 2, base + 3], [base, base + 3, base + 1]]
        return np.array(P, float), np.array(N, float), np.array(I)
    if ptype == 4:
        seg, r, h2 = 32, 1.0, 0.5
        P, N, I = [[0, h2, 0]], [[0, 1, 0]], []
        for i in range(seg + 1):
            a = i / seg * 2 * math.pi
            c, s_ = math.cos(a), math.sin(a)
            n = np.array([c, 0.5, s_])
            P.append([c * r, -h2, s_ * r])
            N.append(list(n / np.linalg.norm(n)))
        for i in range(seg):
            I.append([0, i + 2, i + 1])
        ci = len(P)
        P.append([0, -h2, 0])
        N.append([0, -1, 0])
        rs = len(P)
        for i in range(seg + 1):
            a = i / seg * 2 * math.pi
            P.append([math.cos(a) * r, -h2, math.sin(a) * r])
            N.append([0, -1, 0])
        for i in range(seg):
            I.append([ci, rs + i, rs + i + 1])
        return np.array(P, float), np.array(N, float), np.array(I)
    return None


def merge_cave(ents, stage, field_guid):
    import numpy as np
    from terrainbake import save_rgb, write_glb
    folder = next((e for e in ents if e["name"] == "Cave" and e.get("isFolder")), None)
    if folder is None:
        return 0
    colors, P_all, N_all, UV_all, I_all = [], [], [], [], []
    base = 0
    merged = 0
    items = []
    for e in ents:
        if e.get("parentGuid") != folder["guid"]:
            continue
        pm = comp(e, "PrimitiveMeshComponent")
        tr = comp(e, "TransformComponent")
        if not pm or not tr:
            continue
        mesh = _prim_mesh(pm["data"]["type"])
        if mesh is None or pm["data"].get("texturePath"):
            continue  # 箱と円錐以外・テクスチャ付きはそのまま残す
        if not pm["data"].get("visible", True) and not e.get("tags", {}).get(CAVE_TAG):
            continue  # もともと非表示の物
        items.append((e, pm, tr, mesh))
    for e, pm, tr, (P, N, I) in items:
        col = tuple(round(c, 4) for c in pm["data"]["color"][:3])
        if col not in colors:
            colors.append(col)
        row = colors.index(col)
        d = tr["data"]
        S = np.diag([float(v) for v in d["scale"]])
        R = _rot_rows(*[float(v) for v in d["rotation"]])
        M = S @ R
        Pw = P @ M + np.array(d["position"], float)
        Nw = N @ np.linalg.inv(M).T
        Nw /= np.linalg.norm(Nw, axis=1, keepdims=True)
        P_all.append(Pw)
        N_all.append(Nw)
        UV_all.append(np.tile([0.5, (row + 0.5) / 16.0], (len(P), 1)))
        I_all.append(I + base)
        base += len(P)
        pm["data"]["visible"] = False
        e.setdefault("tags", {})[CAVE_TAG] = 1
        merged += 1
    if not merged:
        return 0
    name = f"cave_backdrop_{stage}"
    out_dir = os.path.join(ROOT, "project", "Resources", "model", "seaworld")
    tex = np.zeros((16, 16, 3))
    for k, c in enumerate(colors):
        tex[k] = c  # 1 行 1 色（PrimitiveMesh の色はリニアなので sRGB で保存し、読み込みで元へ戻す）
    save_rgb(tex, os.path.join(out_dir, name + ".png"))
    write_glb(name, np.concatenate(P_all), np.concatenate(N_all), np.concatenate(UV_all), np.concatenate(I_all),
              os.path.join(out_dir, name + ".glb"), name + ".png")
    mr = model_renderer(name)
    mr["data"]["normalMapPath"] = ""
    mr["data"]["roughnessMapPath"] = ""
    mr["data"]["lightingMode"] = 1  # 洞窟の岩は Lambert（元のプリミティブと同じ）
    ents.append({"active": True, "guid": None, "isFolder": False, "locked": False, "name": "Cave_Merged",
                 "parentGuid": folder["guid"], "tags": {TAG: 1, "cast_shadow": 0}, "visible": True,
                 "components": [transform((0.0, 0.0, 0.0)), mr]})
    return merged


def apply(stage, meta):
    path = os.path.join(SCENES, stage + ".json")
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    ents = d["entities"]
    conf = STAGES[stage]
    # 以前の実行で足したものを消す（やり直せるように）
    ents[:] = [e for e in ents if not e.get("tags", {}).get(TAG)]
    rng = random.Random("seaworld-" + stage)
    used = {e["guid"] for e in ents}

    def new_guid():
        while True:
            g = rng.randrange(10 ** 17, 2 ** 63 - 1)
            if g not in used:
                used.add(g)
                return g

    field = next(e for e in ents if e["name"] == "Field" and e.get("isFolder"))
    # 足した物（崖のモデル・島）は専用のフォルダへ。Hierarchy の目のアイコン 1 つで消せるので、
    # 負荷の比較（水面や空は Field にあるので Field ごと消すと比較にならない）がしやすい
    world = {"active": True, "guid": new_guid(), "isFolder": True, "locked": False, "name": "SeaWorld",
             "parentGuid": 0, "tags": {TAG: 1}, "visible": True, "components": []}
    ents.append(world)
    log = []

    # --- 海の岩・海底 ---
    for e in ents:
        n = e["name"]
        if not (n.startswith("SeaRock") or n.startswith("Seabed")):
            continue
        tr = comp(e, "TransformComponent")["data"]
        if n.startswith("SeaRock"):
            # 名前の番号で決める（隣の面にも同じ岩が入るので、どの面でも同じ見た目になるように）
            k = int(re.search(r"(\d+)$", n).group(1)) + (0 if "SeaRockA" in n else 1)
            model = searock_variant(k, tr["scale"])
            # 箱の外接箱は y 軸まわりに π 回しても変わらないので、当たり判定はそのまま
            tr["rotation"] = [0.0, math.pi if k % 2 else 0.0, 0.0]
            r = waterline_radius(meta, model, tr["scale"], tr["rotation"][1])
            e["tags"]["water_obstacle_radius"] = int(round(r * 10.0))
        else:
            model = "seabed"
            e["tags"]["cast_shadow"] = 0  # 水深 30m の海底の影は見えない
        e["components"] = [c for c in e["components"]
                           if c["type"] not in ("PrimitiveMeshComponent", "ModelRendererComponent")]
        e["components"].append(model_renderer(model))
        log.append(f"{n} -> {model}")

    # --- 崖 ---
    cliffs = {}
    for e in ents:
        n = e["name"]
        if n.startswith("Cliff") and n.split("_")[-1] in ("L", "R", "Arch"):
            prefix = n.rsplit("_", 1)[0]
            e["components"] = [c for c in e["components"] if c["type"] != "PrimitiveMeshComponent"]
            cliffs[prefix] = comp(e, "TransformComponent")["data"]["position"][2]
    for prefix, z in sorted(cliffs.items()):
        yaw = 0.0 if prefix.endswith("A") else math.pi  # B は洞窟の内側から外へ出る向き
        ents.append({"active": True, "guid": new_guid(), "isFolder": False, "locked": False,
                     "name": prefix + "_Model", "parentGuid": world["guid"], "tags": {TAG: 1, "cast_shadow": 0},
                     "visible": True,
                     "components": [transform((0.0, 0.0, z), (0.0, yaw, 0.0)), model_renderer("cliff_cave")]})
        log.append(f"{prefix}_L/_R/_Arch: 見た目を外して当たり判定だけ残す / {prefix}_Model を追加")

    # --- 島 ---
    rail = rail_points(ents)
    layout = ISLANDS_A if conf["islands"] == "A" else ISLANDS_B
    if stage == "Stage2":
        layout = [i for i in layout if i[2] >= 60.0]  # 洞窟へ入る前に視界に入るものだけ
    for idx, (model, x, z, yaw, s) in enumerate(layout):
        reach = max(abs(v) for v in meta[model]["bounds"][0] + meta[model]["bounds"][1][::2]) * s
        dist = min(math.hypot(x - p[0], z - p[2]) for p in rail) if rail else 1e9
        near_x = min(abs(x - p[0]) for p in rail) if rail else 1e9
        assert near_x - reach > RAIL_X_HALF or dist - reach > RAIL_X_HALF, f"{model} がレールに近すぎる"
        far = dist > FAR_DIST
        ents.append({"active": True, "guid": new_guid(), "isFolder": False, "locked": False,
                     "name": f"Island_{idx + 1}_{model.split('_', 1)[1]}", "parentGuid": world["guid"],
                     "tags": {TAG: 1, "cast_shadow": 0}, "visible": True,
                     "components": [transform((x, 0.0, z), (0.0, yaw, 0.0), (s, s, s)), model_renderer(model, far)]})
        log.append(f"Island_{idx + 1}: {model} at ({x:.0f}, {z:.0f}) dist={dist:.0f}m{' far' if far else ''}")

    # --- 洞窟の中身をまとめる ---
    if conf.get("merge_cave"):
        n = merge_cave(ents, stage, field["guid"])
        for e in ents:
            if e["name"] == "Cave_Merged" and e["guid"] is None:
                e["guid"] = new_guid()
        log.append(f"Cave: プリミティブ {n} 個を Cave_Merged（1 ドロー）にまとめた")

    # --- カメラの描画距離・水面の位置 ---
    for e in ents:
        if e["name"] == "Camera":
            cc = comp(e, "CameraComponent")
            if cc:
                cc["data"]["farZ"] = FAR_Z
        if e["name"] == "Water" and comp(e, "WaterComponent"):
            comp(e, "TransformComponent")["data"]["position"][2] = conf["water_z"]
    log.append(f"Camera.farZ = {FAR_Z} / Water.z = {conf['water_z']}")

    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    print(f"== {stage}")
    for line in log:
        print("  " + line)


def main():
    with open(META, encoding="utf-8") as f:
        meta = json.load(f)
    names = sys.argv[1:] or list(STAGES)
    for n in names:
        apply(n, meta)


if __name__ == "__main__":
    main()
