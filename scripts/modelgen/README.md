# modelgen — タコ・鳥・船のモデル生成

プリミティブで代用していた敵・タイトルのタコを、スキン付き glTF（.glb）に置き換えるためのスクリプト。
形・骨・アニメーションをすべて Python で手続き的に作っている（Blender 不要）。

| ファイル | 役割 |
| --- | --- |
| `gltfgen.py` | 形の部品（楕円体・筒・箱・断面のロフト）、パレットテクスチャ、スキン付き .glb の書き出し |
| `models.py` | タコ / 鳥 / 船の定義。実行すると `project/Resources/model/{octopus,bird,ship}/` に .glb と palette.png を出す |
| `preview.py` | 書き出した .glb を glTF の仕様どおりに読み直してスキニングし、静止画で確認する |
| `apply_models_to_stages.py` | Stage*.json の WaveSpawner（タコ・鳥・船）を modelPath 付きに切り替える（当たり判定の大きさは維持） |

```
python scripts/modelgen/models.py
python scripts/modelgen/preview.py project/Resources/model/bird/bird.glb out.png Flap 0,0.1,0.2 front,iso,top
```

## 決まりごと

- 座標はエンジン空間（+X 右 / +Y 上 / +Z 前）で作り、書き出し時に z を反転して glTF にする
  （Assimp の MakeLeftHanded で戻るので、ここで作った座標がそのままエンジンのモデルローカルになる）。
- 色はパレットテクスチャ（1 行 = 1 色 or 2 色のグラデーション）。エンジンは頂点カラー・マテリアル色を読まないため。
- アニメーションは全ジョイントの T/R/S を毎キー書く（エンジンは空のキー列を扱えない）。
- クリップの並び順 = スクリプトの AnimationClip の index。並びを変えたらスクリプトも直すこと。

| モデル | 単位 / 原点 | 骨 | クリップ（index 順） | 使う所 |
| --- | --- | --- | --- | --- |
| `octopus/octopus.glb` | 胴の幅 ≒ 1 / 足の付け根 | head, mantle, eye_L/R, pupil_L/R, leg{t}_{k}（すべて root 直下） | Idle, Jet, Windup, Spit | TitleOctopusScript（骨を毎フレーム上書き） |
| `octopus/octopus_enemy.glb` | 同上 / 胴の中心（0.3 下げてある） | 同上 | 同上 | OctopusEnemyScript（enemyScale 2.4） |
| `bird/bird.glb` | 胴の半長 = 1 / 胴の中心 | body, head, tail, wing_R1/R2, wing_L1/L2 | Flap, Cruise, Windup, Throw, Fall, Float, Glide | BirdEnemyScript（enemyScale 0.95）/ TitleAmbientScript のカモメ |
| `ship/ship.glb` | m / 船体の中心（喫水 ≒ y 0） | hull, sail, flag_0..3 | Sail | ShipVisualScript（enemyScale 1）/ TitleAmbientScript の船（scale 0.53） |

スクリプトから骨を直接動かすときは `RC::SetModelJointOverride`（RenderCommon.h）を使う。
