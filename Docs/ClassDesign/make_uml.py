# UML クラス図の生成スクリプト（Graphviz）。実行: python make_uml.py && dot -Tpng -Gdpi=110 UML_Enemy.dot -o UML_Enemy.png
import html
FONT="Noto Sans CJK JP"
def cls(name, stereo=None, attrs=(), ops=(), color="#ffffff", head="#dbe7f5"):
    rows=[]
    t=html.escape(name)
    if stereo: t=f'<FONT POINT-SIZE="10">&laquo;{html.escape(stereo)}&raquo;</FONT><BR/><B>{t}</B>'
    else: t=f'<B>{t}</B>'
    rows.append(f'<TR><TD BGCOLOR="{head}">{t}</TD></TR>')
    if attrs is not None:
        rows.append('<TR><TD ALIGN="LEFT" BALIGN="LEFT">'+('<BR/>'.join(html.escape(a) for a in attrs) if attrs else ' ')+'</TD></TR>')
    if ops is not None:
        rows.append('<TR><TD ALIGN="LEFT" BALIGN="LEFT">'+('<BR/>'.join(html.escape(o) for o in ops) if ops else ' ')+'</TD></TR>')
    return f'<<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="5" BGCOLOR="{color}">'+''.join(rows)+'</TABLE>>'
def graph(title, nodes, edges, out):
    L=[f'digraph G {{ graph [rankdir=BT, fontname="{FONT}", fontsize=18, label="{title}", labelloc=t, nodesep=0.5, ranksep=0.7, bgcolor="white", pad=0.4];',
       f'node [shape=plain, fontname="{FONT}", fontsize=11];',
       f'edge [fontname="{FONT}", fontsize=10];']
    for n,l in nodes: L.append(f'"{n}" [label={l}];')
    for e in edges: L.append(e)
    L.append('}')
    open(out,'w').write('\n'.join(L))
def inh(c,p): return f'"{c}" -> "{p}" [arrowhead=onormal, arrowsize=1.3];'
def comp(whole,part,label="",mult=""): return f'"{part}" -> "{whole}" [arrowhead=diamond, arrowsize=1.4, label="{label}", taillabel="{mult}"];'
def dep(a,b,label=""): return f'"{a}" -> "{b}" [style=dashed, arrowhead=vee, label="{label}"];'

# ---- 図1: 敵まわり ----
n=[("ScriptableEntity",cls("ScriptableEntity","engine",None,["#OnCreate()","#OnUpdate(dt)","+Serialize() / Deserialize()"],head="#eeeeee")),
 ("DamageableScript",cls("DamageableScript","abstract",["-hp_ : int","-maxHp_ : int","-defeated_ : bool","-flashTimer_ : float"],
   ["+TakeDamage(damage)","#OnUpdate(dt) final  ← Template Method","#OnUpdateAlive(dt) : virtual","#OnUpdateDefeated(dt) : virtual","#OnDefeated() : pure virtual","#OnDamaged(damage) : virtual","#DamageFlashColor() : virtual"])),
 ("EnemyBaseScript",cls("EnemyBaseScript",None,["-spawnDeathBubbles_ : bool"],["-OnDefeated() final","#OnEnemyDefeated() : virtual","#SetDefaultDeathDuration(s)"])),
 ("TreasureChestScript",cls("TreasureChestScript",None,["-points_ : int"],["#OnDefeated()  ← 開封","#OnUpdateDefeated(dt)","#ApplyColor(c)"])),
 ("SharkEnemyScript",cls("SharkEnemyScript",None,["-fsm_ : StateMachine","-sense_ : SharkSense"],["#OnUpdateAlive(dt)","#OnUpdateDefeated(dt)"])),
 ("ShipEnemyScript",cls("ShipEnemyScript",None,["-fsm_ : StateMachine","-shellPool_ : EntityPool"],["#OnUpdateAlive(dt)","#OnEnemyDefeated()"])),
 ("EnemyAI",cls("EnemyAI",None,["-bulletPool_ : EntityPool"],["#OnUpdateAlive(dt)","#OnUpdateDefeated(dt)"])),
 ("OctopusEnemyScript",cls("OctopusEnemyScript",None,None,["#OnDamaged(damage)"])),
 ("BirdEnemyScript",cls("BirdEnemyScript",None,None,["#OnDamaged(damage)","#OnEnemyDefeated()"])),
 ("StateMachine",cls("StateMachine<Owner>","template",["-current_ : unique_ptr<IState>","-pending_ : unique_ptr<IState>"],["+ChangeState<S>()","+Update(owner, dt)","+IsIn<S>() / CurrentName()"],head="#e3f1e0")),
 ("IState",cls("IState<Owner>","interface",None,["+OnEnter(owner, machine)","+OnUpdate(owner, machine, dt) : pure","+OnExit(owner)","+Name() : pure"],head="#e3f1e0")),
 ("WaitState",cls("WaitState",None,None,None,head="#e3f1e0")),
 ("ApproachState",cls("ApproachState",None,None,None,head="#e3f1e0")),
 ("AttackState",cls("AttackState",None,["-attackTimer_ / hasHit_"],None,head="#e3f1e0")),
 ("RetreatState",cls("RetreatState",None,["-cooldownTimer_ / retreatTimer_"],None,head="#e3f1e0")),
 ("EntityPool",cls("EntityPool","Object Pool",["-entityName_ : string"],["+Acquire(scene) : Lease","+Release(entity)"],head="#fbeedd")),
 ("GameEvent",cls("GameEvent<Args...>","Observer: Subject",["-listeners_"],["+Subscribe(handler) : EventConnection","+Emit(args)"],head="#f3e3f3")),
 ("EventConnection",cls("EventConnection","RAII",None,["+Disconnect()"],head="#f3e3f3")),
 ("RailShooterGameMode",cls("RailShooterGameMode","Observer: 購読側",["-enemyDefeatedConnection_ : EventConnection"],["+OnSceneEnter()","+EvaluateOutcome()"],head="#f3e3f3")),
]
e=[inh("DamageableScript","ScriptableEntity"),inh("EnemyBaseScript","DamageableScript"),inh("TreasureChestScript","DamageableScript"),
 *[inh(c,"EnemyBaseScript") for c in ["SharkEnemyScript","ShipEnemyScript","EnemyAI","OctopusEnemyScript","BirdEnemyScript"]],
 comp("SharkEnemyScript","StateMachine","","1"),comp("StateMachine","IState","current","0..1"),
 *[inh(s,"IState") for s in ["WaitState","ApproachState","AttackState","RetreatState"]],
 comp("ShipEnemyScript","EntityPool","","1"),comp("EnemyAI","EntityPool","","1"),
 dep("EnemyBaseScript","GameEvent","Emit(撃破)"),comp("RailShooterGameMode","EventConnection","","1"),dep("RailShooterGameMode","GameEvent","Subscribe"),
 dep("EventConnection","GameEvent","解除"),
]
graph("敵まわりのクラス設計（ポリモーフィズム / Template Method / State / Object Pool / Observer）",n,e,"UML_Enemy.dot")

# ---- 図2: 自機 ----
n=[("ScriptableEntity",cls("ScriptableEntity","engine",None,None,head="#eeeeee")),
 ("RailShooterController",cls("RailShooterController",None,["-weapons_ : WeaponSelector","-bulletPool_ : EntityPool","-waterFsm_ : StateMachine","-lifeFsm_ : StateMachine"],["#OnUpdate(dt)","-Fire()"])),
 ("WeaponSelector",cls("WeaponSelector",None,["-weapons_ : vector<unique_ptr<IWeapon>>","-index_ : size_t"],["+Current() : const IWeapon&","+SelectNext() / SelectPrevious()"],head="#fbeedd")),
 ("IWeapon",cls("IWeapon","Strategy",None,["+BulletTypeTag() : pure","+ShotCount() : virtual","+AimPoint(center, i) : virtual","+BulletScale() : pure","+Cooldown(base) : virtual","+HudColor(a) : pure"],head="#fbeedd")),
 ("NormalWeapon",cls("NormalWeapon",None,None,None,head="#fbeedd")),
 ("SpreadWeapon",cls("SpreadWeapon",None,None,["+ShotCount() → 3","+AimPoint() → ±80px"],head="#fbeedd")),
 ("HeavyWeapon",cls("HeavyWeapon",None,None,["+Cooldown() → 2倍"],head="#fbeedd")),
 ("EntityPool",cls("EntityPool","Object Pool",None,["+Acquire(scene) : Lease"],head="#fbeedd")),
 ("StateMachine",cls("StateMachine<Owner>","template",None,["+ChangeState<S>()","+Update(owner, dt)"],head="#e3f1e0")),
 ("IState",cls("IState<Owner>","interface",None,["+OnEnter / OnUpdate / OnExit"],head="#e3f1e0")),
 ("SurfaceState",cls("SurfaceState",None,None,None,head="#e3f1e0")),
 ("UnderwaterState",cls("UnderwaterState",None,None,None,head="#e3f1e0")),
 ("LifeState",cls("LifeState","abstract",None,["+CanAct() : pure","+TakeDamage(...) : pure","+DrawOverlayText(...) : pure","+DrawScreenBand(...) : pure"],head="#e3f1e0")),
 ("AliveState",cls("AliveState",None,None,["+TakeDamage(...)  → HP減少"],head="#e3f1e0")),
 ("PlayingState",cls("PlayingState",None,None,None,head="#e3f1e0")),
 ("ClearedState",cls("ClearedState",None,None,None,head="#e3f1e0")),
 ("DeadState",cls("DeadState",None,None,None,head="#e3f1e0")),
]
e=[inh("RailShooterController","ScriptableEntity"),
 comp("RailShooterController","WeaponSelector","","1"),comp("WeaponSelector","IWeapon","","3"),
 *[inh(w,"IWeapon") for w in ["NormalWeapon","SpreadWeapon","HeavyWeapon"]],
 comp("RailShooterController","EntityPool","","1"),comp("RailShooterController","StateMachine","waterFsm_ / lifeFsm_","2"),
 comp("StateMachine","IState","current","0..1"),
 inh("SurfaceState","IState"),inh("UnderwaterState","IState"),inh("LifeState","IState"),
 inh("AliveState","LifeState"),inh("DeadState","LifeState"),inh("PlayingState","AliveState"),inh("ClearedState","AliveState"),
]
graph("自機（RailShooterController）のクラス設計（Strategy / State / Object Pool）",n,e,"UML_Player.dot")
