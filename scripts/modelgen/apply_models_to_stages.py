# -*- coding: utf-8 -*-
"""
ステージの WaveSpawner（タコ・鳥・船）を、プリミティブからモデル（scripts/modelgen で作った .glb）へ切り替える。

  python scripts/modelgen/apply_models_to_stages.py            # Stage*.json をすべて
  python scripts/modelgen/apply_models_to_stages.py Stage2     # 指定した面だけ

当たり判定の大きさ（colliderRadius × enemyScale の最大成分）は変えずに、enemyScale をモデルの単位に合わせる。
  タコ : 球（半径 1）→ octopus_enemy.glb（胴の幅 ≒ 0.9）。scale を 1.2 倍にして見た目の大きさを近づける
  鳥   : 楕円球 (0.45, 0.4, 0.95) → bird.glb（胴の半長 = 1）。scale = 旧 z の一様スケール
  船   : 箱（scale = 船体の寸法 m）→ ship.glb（全長 9m の船体で作ってある）。scale = 旧 z / 9 の一様スケール
すでにモデルになっている（modelPath が入っている）スポナーは触らない。
"""
import glob
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCENES = os.path.join(ROOT, "project", "Resources", "Scenes")

MODELS = {
    "OctopusEnemyScript": "Resources/model/octopus/octopus_enemy.glb",
    "BirdEnemyScript": "Resources/model/bird/bird.glb",
    "ShipEnemyScript": "Resources/model/ship/ship.glb",
}


def enemy_kind(sd):
    names = [sd.get("enemyScript", "")]
    for s in sd.get("scripts") or []:
        names.append(s.get("name", ""))
    for n in names:
        if n in MODELS:
            return n
    return None


def convert(sd):
    kind = enemy_kind(sd)
    if not kind or sd.get("modelPath"):
        return False
    old = [float(x) for x in sd.get("enemyScale", [1, 1, 1])]
    old_r = float(sd.get("colliderRadius", 0.5))
    world_r = old_r * max(abs(x) for x in old)
    if kind == "OctopusEnemyScript":
        s = round(max(old) * 1.2, 3)
    elif kind == "BirdEnemyScript":
        s = round(old[2], 3)
    else:
        s = round(old[2] / 9.0, 3)
    sd["modelPath"] = MODELS[kind]
    sd["enemyScale"] = [s, s, s]
    sd["colliderRadius"] = round(world_r / s, 3)
    return True


def main():
    names = sys.argv[1:]
    files = sorted(glob.glob(os.path.join(SCENES, "Stage*.json")))
    if names:
        files = [f for f in files if os.path.splitext(os.path.basename(f))[0] in names]
    for f in files:
        d = json.load(open(f, encoding="utf-8"))
        n = 0
        for e in d.get("entities", []):
            for c in e.get("components", []):
                if c.get("type") != "NativeScriptComponent":
                    continue
                for sd in c.get("data", {}).get("scriptDataList", []):
                    if isinstance(sd, dict) and "enemyScript" in sd and convert(sd):
                        n += 1
        if n:
            with open(f, "w", encoding="utf-8", newline="\n") as fp:
                json.dump(d, fp, ensure_ascii=False, indent=2)
        print(os.path.basename(f), "converted", n)


if __name__ == "__main__":
    main()
