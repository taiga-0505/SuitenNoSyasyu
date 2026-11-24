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
python scripts/modelgen/models.py                        # 全部
python scripts/modelgen/models.py octopus octopus_enemy  # 名前を並べるとそれだけ（MODELGEN_OUT=<フォルダ> で出力先を変えられる）
python scripts/modelgen/preview.py project/Resources/model/bird/bird.glb out.png Flap 0,0.1,0.2 front,iso,top
```

## 決まりごと

- 座標はエンジン空間（+X 右 / +Y 上 / +Z 前）で作り、書き出し時に z を反転して glTF にする
  （Assimp の MakeLeftHanded で戻るので、ここで作った座標がそのままエンジンのモデルローカルになる）。
- 色はパレットテクスチャ（1 行 = 1 色 or 2〜3 色のグラデーション。`Palette.add(name, a, b, mid=...)`）。
  エンジンは頂点カラー・マテリアル色を読まないため。タコのまだら模様は、頂点ごとにノイズで帯の位置
  （濃い斑 0 … 地の色 0.5 … 淡い腹 1）を決めて出している。
- タコはセミリアル（外套膜のいぼ・しわ、金色の目と横長の瞳、目の上の突起、先細りの長い足、足の裏の 2 列の吸盤、
  足の付け根の傘膜）。形を変えても骨の名前・rest の位置は変えないこと（3 つのスクリプトが骨を上書きしている）。
- アニメーションは全ジョイントの T/R/S を毎キー書く（エンジンは空のキー列を扱えない）。
- クリップの並び順 = スクリプトの AnimationClip の index。並びを変えたらスクリプトも直すこと。

| モデル | 単位 / 原点 | 骨 | クリップ（index 順） | 使う所 |
| --- | --- | --- | --- | --- |
| `octopus/octopus.glb` | 胴の幅 ≒ 1 / 足の付け根 | head, mantle, eye_L/R, pupil_L/R, leg{t}_{k}（すべて root 直下） | Idle, Jet, Windup, Spit | TitleOctopusScript / SharkChaseCutscene（骨を毎フレーム上書き） |
| `octopus/octopus_enemy.glb` | 同上 / 胴の中心（0.3 下げてある） | 同上 | 同上 | OctopusEnemyScript（enemyScale 2.4） |
| `bird/bird.glb` | 胴の半長 = 1 / 胴の中心 | body, head, tail, wing_R1/R2, wing_L1/L2 | Flap, Cruise, Windup, Throw, Fall, Float, Glide | BirdEnemyScript（enemyScale 0.95）/ TitleAmbientScript のカモメ |
| `ship/ship.glb` | m / 船体の中心（喫水 ≒ y 0） | hull, sail, flag_0..3 | Sail | ShipVisualScript（enemyScale 1）/ TitleAmbientScript の船（scale 0.53） |

スクリプトから骨を直接動かすときは `RC::SetModelJointOverride`（RenderCommon.h）を使う。

## seaworld — 水上ステージ（外洋）の地形

箱プリミティブだった屋外の地形（海の岩・海底・洞窟の口の崖）と遠景の島を、手続き生成のモデルにしたもの。
**1 モデル = 1 メッシュ = 1 マテリアル**（描画コマンドの数＝CPU 負荷は箱のときと同程度）で、
質感はモデルごとに焼き込んだテクスチャ（albedo / 法線 / ラフネス）、形はポリゴン数（GPU）で出す。

| ファイル | 役割 |
| --- | --- |
| `seaworld.py` | 地形モデルの形と、焼き込みの材質（岩の節理・地層・濡れた水際・海藻・地衣類・草・砂）。`project/Resources/model/seaworld/` に書き出す |
| `terrainbake.py` | なめらかな法線、モデル専用 UV のアトラス詰め、UV 空間へのラスタライズ（画素ごとのワールド位置・法線）、glb 書き出し |
| `apply_seaworld_to_stages.py` | Stage1 / Stage2 / Stage5 の屋外区間へ当てる（何度実行しても同じ結果。`generate_stages.py` で作り直したら再実行） |
| `seaworld_preview.py` | モデル単体、またはステージ JSON を指定したカメラ位置から静止画にする（法線マップ込み。エンジンを起動せずに確かめる用） |

```
python scripts/modelgen/seaworld.py                 # 全部で 2〜3 分
python scripts/modelgen/apply_seaworld_to_stages.py
python scripts/modelgen/seaworld_preview.py scene Stage1 out.png 0,2,-50 0 -2   # x,y,z  yaw(度)  pitch(度)
```

| モデル | 単位 / 原点 | テクスチャ | 使う所 |
| --- | --- | --- | --- |
| `searock_stack / _twin / _boulder` | **±0.5 の箱に正規化** / 箱の中心（= 水面） | 1024 | SeaRock*（旧プリミティブの Transform・Collider をそのまま使う） |
| `seabed` | 同上 | 1024 | SeabedA / SeabedB |
| `cliff_cave` | m / 旧 Cliff の中心（x 0・水面・厚み 10m の中央）。前面は -z | 2048（約 10px/m） | CliffA_Model（向き 0）/ CliffB_Model（向き π） |
| `island_large` | m / 島の中心（水面） | 2048 | Island_*（当たり判定なし） |
| `island_medium / _low / _stacks` | 同上 | 1024 | 同上 |

モデルごとに `<name>.glb`・`<name>_albedo.jpg`（glb が参照）・`<name>_normal.jpg`（normalMapPath）・
共通の `rough_none.png`（roughnessMapPath。4x4 の白＝ハイライト無し）・`<name>_far.jpg`（遠景用に縮小して霞ませた albedo。texturePath で差し替え）を出す。

- **法線マップとラフネスは sRGB でエンコードして保存している。** ModelRenderer の normalMapPath / roughnessMapPath は
  `RC::LoadTex(path)`（既定 srgb = true）で読まれ、読み込み時に sRGB → リニアへ変換されるため。
  エンジン側を `srgb = false` で読むように直したら、`terrainbake.py` / `seaworld.py` の `save_rgb(..., encode_srgb=True)` を
  法線・ラフネスだけ `False` に戻すこと。
- 法線マップの接空間はシェーダが ddx/ddy から作る（T = dP/du, B = dP/dv）。接線は glb に入れていない。
- 崖の口は外側が幅 22m・高さ 17.5m、奥（z = +5）で洞窟の断面（幅 17m・高さ 11.5m）へ絞る漏斗状。
  崖の裏面は洞窟の外殻（`cave_shell`、|x| ≦ 26・y ≦ 21）を覆う |x| ≦ 34・y ≦ 26 まで張ってある。
  洞窟の幅や天井を変えたら `seaworld.py` の BACK_* と裏面の範囲も合わせること。
- 旧 CliffA_L / _R / _Arch は見た目だけ外し、当たり判定の箱として残している。
- 岩は当たり判定の箱を変えずに見た目だけ替えるので、水面の反射に使う半径は `water_obstacle_radius` タグ（10 倍の整数）で
  岩の水際の実寸を渡している。
- 水面（1000m 四方）は面の範囲を覆うよう中心の z を動かしている（Stage1/2 = 150、Stage5 = 1000）。カメラの farZ は 520。
