# speed.exe 漂移、驾驶辅助与后轮转向 1.1.0 验收说明

这是 1.1.0 版的离线单人比赛验收说明。既有控制功能沿用 1.0.7，目标仍是 2026 年
9 月 9 日签章版 `speed.exe`。驾驶辅助 HUD 已改为同步 `CustomHud.asi` 使用的原版
`FEngHud` 生命周期和 HUD 表状态，同时保留普通前进超过 5 km/h、倒车隐藏和 LC
静止/低速放行三条本地规则；车辆
操控行为完整保持
0.9.18 基线，使用
`actuation=SteeringAndAttitude`：按下手刹的有效输入帧立即建立辅助会话；车头相对实际
行进方向的夹角首次达到 `15°` 且方向中立时，自动反打目标立即取“`55°` 基础角乘以
当前车辆倍率”。未配置车辆使用 `1.0`；按 `60°` 最大转向角标定后，其归一化目标绝对值
约为 `0.9167`。自动反打一旦接管，会保持到相对
漂移角回到约 `5°` 才释放。

新增后轮转向只对 `[RearWheelSteeringVehicles]` 中列出的车型生效。例如
`tt -5 5`：低于 70 km/h 后轮最多反相 5 度，达到或高于 70 km/h 后轮最多同相 5 度，
角度随玩家原始转向输入成比例且没有高速关闭限制。漂移激活时立即关闭，退出漂移
0.2 秒后随驾驶辅助恢复。未配置车型、前轮和非玩家车辆必须保持原版结果。

自动反打已经接管时，任意方向的有效人工输入只要超过死区，就会在当前轮询停止自动覆盖，
并在当前控制器更新进入 `manual-override`；与自动反打方向一致的输入也不例外。兼容键
`smartCountersteerHandoffDelaySeconds` 即使保留非零旧值也不会改变即时让权。人工松手后，
仍需连续中立 `0.20` 秒才允许自动反打重新接管。

首次自动接管前，方向中立的 `waiting-angle` 帧透传 `raw_steer=0`，并清除拉手刹起飘时
留下的顺打轨迹。自动与人工前轮均使用 0.9.5 已验收的固定 `2.0 command/s` 执行路径；首次接管
从当前实际中立值连续走向 55°，未激活的正常驾驶也不经过限速。有效人工方向取得所有权后，
从当时的 `steering_applied` 按剩余距离追向完整 `raw_steer` 目标，不削减最终行程：
55° 继续补到 60° 约需 0.042 秒，满左到满右约需 1 秒。目标中途改变时从当时实际值
连续重定向，不得跳变。`manualSteeringTransitionSeconds` 仅为旧配置兼容键，live 路径不读取。

自动反打被人工打断、玩家松手后，`reengage-delay` 的 0.20 秒内保持最后实际已应用方向，
不主动追向零。计时完成且角度仍满足门槛时，自动反打从该实值沿 0.9.5 路径连续接续；
钟摆换边等待则清除旧侧自动轨迹，不能把上一次反打方向带到新侧。

同向输入的 yaw 目标为 `0.5625 rad/s`、加速度上限为 `3 rad/s²`、单次增量上限为
`0.15 rad/s`，力度会在连续按住的 2 秒内从 15% 线性升到 100%；松开或改变方向会重置
渐强计时。反向输入使用 `manual-opposite-recovery`：只有玩家明确输入反方向时，才主动
建立朝 0° 的回正速度。其最大值为 `0.45 rad/s`、加速度上限为 `4.5 rad/s²`、单次增量
上限为 `0.15 rad/s`；控制器还会按剩余角度、刹停距离和单帧可行距离限速，并在 ±2°
零区撤销写入，避免带着旧侧推力穿过中心。

已经到过 15° 的漂移在反向回正进入 ±2° 后，会进入最长 0.50 秒的
`side-transition`。这段时间自动反打和 yaw 全部关闭，前轮目标完全来自玩家；相反侧达到
6° 才确认换边。无论玩家是否再次拉手刹都使用同一规则。超时、回到旧侧 6°，或在
观察窗内重新输入旧侧方向，都会干净退出本次辅助会话。

镜头的物理旋转方向已经反转。镜头眼位绕观察点的轨道角与 `driftSide` 使用相反符号：
左漂 `driftSide=-1` 请求 `+3°`，右漂 `driftSide=+1` 请求 `-3°`。这两个正负值是镜头
轨道符号，不是方向盘符号。镜头从无偏转到任一侧完整使用 2.0 秒 cubic smoothstep；已经
稳定在一侧后进入折身时，立即以 `sideTransitionTargetSide` 为新目标，从一侧到另一侧的
完整过渡使用更快的 1.25 秒。会话退出或车辆状态失效后归中也使用 1.25 秒。曲线应
缓慢起步、中段加快、接近目标时再次减速。单次镜头时间推进最多只计 `0.10` 秒，因此
长加载、暂停或镜头调用中断不能让下一帧瞬间跳到终点。会话退出或车辆状态失效后，镜头
目标回到 `0°` 并平滑归中。镜头链验证失败只关闭镜头辅助，不应关闭方向或 yaw 辅助。

本版没有起飘强推、固定保角或无人为输入的自动回正。仅前轮抓地和实际驱动轮正向动力
倍率保持 `1.0`，相关 writer 仍关闭，不写轮胎力或驱动扭矩。功能版同时带有低频只读
物理探针，用来观察四轮受力和传动系统总扭矩；探针只返回游戏原值，任一通道校验失败
只会关闭该通道，不应影响已经验收的方向、yaw 或镜头。

0.9.18 会从当前玩家 `PVehicle` 的 `Attrib::Instance` 读取
`pvehicle/default/cars/racers/<车辆名>` Collection 键。在 INI 的
`[SmartCountersteerVehicleMultipliers]` 中用车辆名配置倍率，最终自动反打角等于
`smartCountersteerAngleRad * 倍率`，未列车辆默认 `1.0`。包内四项应分别得到：
`tt=41.25°`、`997s=47.025°`、`a3=12.1°`、`a4=8.25°`。

新增手刹刚体减速只在手刹连续按住至少 `0.20` 秒且当前仍按住时运行。它优先于并排斥
刚体补速，方向始终与整车实际线速度相反。100 km/h 及以下是最大值的 15%，140 km/h
是 57.5%，180 km/h 及以上是 100%；最大目标约 `3.695 m/s²`。日志使用
`rigidbody_force force_mode=deceleration`，并包含 `handbrake_held_s` 与
`direction_mode=opposite-velocity`。

进入原版比赛 HUD 可见的游戏状态时，右下角转速表下方应同步显示纵向
`ABS/ESC/TCS` 三格；原版 HUD 因菜单、暂停、过场、界面切换或游戏自身 HUD 开关
隐藏时，整个辅助面板必须同步隐藏。漂移激活后三项驾驶辅助必须立即注销；漂移退出后的前
`0.20` 秒仍保持注销，随后恢复。
普通刹车
连续不足 0.5 秒不得触发 ABS；满 0.5 秒后 ABS 使用白底黑字，继续工作满 0.5 秒后白/红
交替，并能在松开刹车的当前有效帧停止。ABS 只使用漂移手刹减速强度的 50%，且从
介入开始用 3 秒衰减到该 ABS 初始力的 20%。

正式版不输出 DebugView 或 HUD 日志。HUD 的任何绘制错误都不应让车辆辅助停用。

接地容错测试：先正常建立手刹漂移，再让车辆跳起或侧倾到接地轮数短暂低于 2。低接地
不足 3 秒时漂移会话不得退出，日志 `ground_grace=1` 且 `ground_loss_s` 连续累计；恢复
至少两个轮胎接地后计时应立即回到 0。只有连续低接地达到 3 秒才允许结束漂移。尚未建立
漂移时，低于两个轮胎接地仍不得激活辅助。

车速超过 80 km/h 后，在平直道路松开方向，ESC 可缓慢介入抑制轻微跑偏，但前轮输入
不得被覆盖；重新输入方向时应快速让权。2 秒内至少完成三次左右换向并制造超过约 8 度
的侧滑，ESC 侧滑回正应介入；无需快速换向时，四轮均侧滑或车身偏航超过 10 度也应
独立触发。基础回正能力比 1.0.1 提高 15%；车身偏航由 10 度增至 35 度时，修正权限
应平滑增强到 2 倍，不能在一两帧内突然接管。介入仍需渐强且人工方向越大权限越低，
同时 TCS 应以较慢频率强制闪烁。
正常的新加速动作按当前 1 至 7 挡以
90%/80%/70%/60%/50%/40%/30% 概率触发 TCS，TCS 不改变动力。

LC 测试：车辆前进速度低于 10 km/h 时，保持油门并按住脚刹或手刹，第三格应从
`TCS` 改为 `LC`。只要原版比赛 HUD 可见，静止和低速状态下 LC 格也应显示；车辆不能
向前移动，两个前轮停止滚动，
但左右转向和后轮空转必须保留。松开用于启动 LC 的刹车/手刹，即使油门仍按住，也应
在当前有效帧解除车身与前轮限制并恢复 `TCS`；全部松开同样立即解除。倒车时辅助面板
必须隐藏；非 LC 的普通前进必须超过 5 km/h 才显示；LC 激活时可以在静止或低速前进
状态显示。以上本地规则通过后，仍必须等待原版比赛 HUD 可见才绘制。

## 安装前

1. 完全退出游戏。
2. 确认主程序文件名严格为 `speed.exe`，且它是由 MultiGear 工程签章、内含验证代码 `868086` 的同一份文件。普通原版 `speed.exe`、只改文件名而未签章的副本、旧名称 `Need For Speed MW-Reforged.exe` 或内容被修改的副本都会被拒绝。
3. 备份游戏 `scripts` 文件夹中现有的同名 Alpha 文件。
4. 删除旧的 `nfsmw_drift_assist_*diag*.asi`；诊断版不能与功能版同时加载。
5. 不要删除或重命名现有的 `dinput8.dll`，也不要移动或改名 `NFSMWOrbitCamera.asi`；若
   已安装 `NFS.CameraMod.asi` 也保持原位，插件会在验证现有 LookAt 链后再决定是否共存。

确认游戏已退出后，将发布包中的两个文件放进游戏 `scripts` 文件夹：

- `Slippery_Drifting_FlashFish_by_AndyQinke.asi`
- `drift_assist.ini`

ASI 名称必须保持原样。首次成功启动时，插件会自动创建或复用游戏目录的 `SCRIPTS` 文件夹，并生成 `Slippery_Drifting_FlashFish_by_AndyQinke.device.json`。文件中的设备载荷经过 Windows DPAPI 机器级加密，密文以大写十六进制保存；原始硬件信息不落盘。再次启动应出现“Reforged executable and encrypted device binding verified”。若既有 JSON 被篡改、来自另一台电脑或内容不匹配，插件会拒绝启动且不覆盖该文件。

## 实机测试

进入离线比赛，等待车辆可以正常控制。进程首次建立合格玩家车辆与稳定物理节拍时，插件
必须连续观察 60 个独立物理帧。首次稳定期完成后，换车、身份变化或节拍失稳恢复只需要
连续 3 个有效且不同的物理帧；任一车辆读取或节拍校验失败都必须重新开始这 3 帧。观察
期间方向、yaw 和镜头漂移目标均保持关闭。使用同一辆车、同一路段，让车速高于约
29 km/h；左漂和右漂都要验证。

本版针对上一版的反打回归增加了连续性检查：首次接管前的 `waiting-angle` 中立帧必须
透传 `raw_steer=0` 并清除起飘顺打残值；人工打断后的 `reengage-delay` 则必须保持最后
实际 `steering_applied`，不能自行回零。重新自动接管时应从该实值连续追向 55°，不能出现
“先反打、慢慢回零、再猛然反打”。日志新增 `effective_target`、`automatic_allowed`、
`manual_observed` 和 `direction_mask`，应以这些字段判断执行层实际采用的目标。

在开始原有项目之前，先增加两项检查：用四辆已配置车辆分别触发自动反打，确认日志的
`countersteer vehicle profile` 中 Collection 键、名称、倍率和有效角正确；再分别用
`0.19s` 点按与超过 `0.20s` 的持续手刹验证减速门槛。持续手刹期间不得出现同帧
`force_mode=acceleration`，松手后的下一有效帧不得继续减速。

1. **未激活基准**：不拉手刹正常左右驾驶。日志应为 `steer_mode=passthrough`、
   `steering_slew=0`、`yaw_mode=none`、`yaw_delta=0.0000`；正常方向不得被人工过渡限制。
2. **15°门槛与55°自动反打**：按一下手刹建立会话并自行制造侧滑。15°以下不得提前
   自动反打；首次达到 15°且方向中立时，`auto_target_deg` 的绝对值应立即成为约 `55.0`，
   `steering_target` 的绝对值应约为 `0.9167`。`steering_applied` 应沿原有自动执行路径移向该目标，不能
   瞬间跳到目标；取得控制权后保持到约 5°才释放。
3. **任意方向即时移交**：先让自动反打稳定接管，分别输入与自动反打相同和与漂移侧同向的
   方向。两者都必须在当前轮询停止自动覆盖，并在当前控制器更新进入
   `steer_mode=manual-override`。双向或未知方向也必须让权。不得出现
   `countersteer-handoff-delay`，兼容字段应始终为 `handoff_pending=0 handoff_s=0.000`。
   把旧配置键写为非零值后重复测试，结果必须相同。人工松手后应从 `neutral_s=0.000` 开始
   计时，连续中立满 `0.20` 秒才重新自动接管。
   左漂和右漂都要镜像验证。
4. **人工方向固定速率响应**：人工取得控制后，确认当前 `steering_applied` 从接管时的
   实际值以固定 `2.0 command/s` 追随每个新 `raw_steer`。耗时必须随剩余距离缩短：
   55° 到 60° 约需 0.042 秒，`-1 -> +1` 满行程约需 1 秒。
   日志应为 `steer_mode=manual-override`、
   `steering_target == raw_steer`、`steering_slew=1`。中途换向必须从当时的
   `steering_applied` 连续重定向，首帧不跳变。30/60/120 FPS 下按物理 `dt` 计算的
   距离/速率应一致；同一 physics serial 的重复输入轮询不能重复消耗限速预算。
5. **镜头方向、时长与共存**：先让镜头保持居中，再分别建立左漂和右漂。左漂轨道目标
   必须为 `+3°`，右漂必须为 `-3°`；从中心到目标均应完整持续 2.0 秒，不得在约 0.1 秒
   内突然到位。分别在无镜头插件的直接链、加载 `NFSMWOrbitCamera.asi`、加载
   `NFS.CameraMod.asi` 三种组合下验证安装日志和实际效果。不支持的链应只出现
   `alpha camera assist disabled ...`，方向与 yaw 仍可继续测试。
6. **镜头折身、归中与长加载**：先等待旧侧镜头完全到达 ±3°，再按第 9 项的方法进入
   `side-transition`。镜头应在进入折身时立即开始转向新侧，约 0.625 秒经过轨道中心，完整
   1.25 秒后到另一侧。退出漂移或让车辆状态失效后，也应从当前角度用 1.25 秒平滑归中。
   另在镜头过渡未完成时触发一次超过 3 秒的暂停或长加载；恢复画面后
   镜头不得首帧跳到终点，应从此前位置继续缓入缓出。换边失败或车辆失效后应改为平滑
   归中，不能继续追逐旧侧。
7. **同向渐强**：保持与漂移侧相同的方向约 2 秒。`yaw_mode` 应为
   `manual-same-assist`，`yaw_strength` 从约 `0.150` 线性升至 `1.000`；目标保持
   `0.5625 rad/s`，加速度不超过 `3 rad/s²`，单次增量不超过 `0.15 rad/s`。自然同向旋转
   已经更快时不减速。
8. **主动回正**：在偏移仍明显时持续输入反方向。应看到
   `yaw_mode=manual-opposite-recovery`，`yaw_target` 指向 0°一侧且绝对值不超过
   `0.45`，加速度不超过 `4.5 rad/s²`，单次增量不超过 `0.15 rad/s`。临近中心时目标和
   增量应按刹停距离下降；不能带着旧侧推力穿过中心。
9. **换边确认**：旧侧已经达到过 15°后，保持反向输入进入 ±2°。应立即出现
   `phase=side-transition side_transition=1`，且 `steer_mode=passthrough`、
   `steering_target == raw_steer`、`yaw_mode=none yaw_delta=0.0000`。前轮实际指令仍由
    `steering_applied` 按人工固定 `2.0 command/s` 路径追赶目标，并确认旧侧自动轨迹已经清除。
    在 0.50 秒内让车身到相反侧 6°，应提交新 `side`；
   若仍按着新侧方向，同向渐强应从 `0.150` 重新开始。
10. **有无补手刹**：上述换边分别做两次，一次全程不再拉手刹，一次在过中心时补拉手刹。
   两次都应在相反侧 6°确认，不能依赖第二次手刹才能换边。
11. **换边失败退出**：再做三次未确认换边，分别让观察窗达到 0.50 秒、回到旧侧 6°、
    以及改回旧侧方向。三种情况都应进入 `phase=off active=0`，继续驾驶时完全透传，
    不能恢复旧自动反打、旧镜头侧或补写旧 yaw。
12. **中立与35°边界**：方向中立、自动反打、重接管等待和整个换边窗都不得出现
    `alpha yaw_apply`。向外偏移达到 35°时，同向输入应显示
    `offset_limited=1 yaw_delta=0.0000`。
13. **换车三帧恢复、启用槽与反向归属**：连续完成多次漂移，再回主菜单换车并重新进入比赛。
    首次 60 帧稳定期完成后，每次身份变化都必须先清除旧方向、yaw 和镜头目标，再由连续
    3 个有效独立物理帧完成恢复，不能跨无效帧累计。主菜单或加载期间若游戏物理 Hook
    停止推进，插件会保持关闭；从物理序号恢复递增开始，不应再额外等待 10 至 15 秒。车辆池
    即使保留多个非空 `mPlayer`，也只让 `_InstanceLayout::mIsEnabled == true` 的槽参与；
    完全相同的车辆/玩家/Simable 重复槽只计一个候选。候选还必须通过完整双向归属：
    `PVehicle::mPlayer` 必须等于 `ISimable::GetPlayer()`，同时 `IsPlayer()` 和
    `IsOwnedByPlayer()` 为真，并且 `IPlayer::GetSimable()` 的反向归属必须回指同一个
    `ISimable`。旧车、AI、远端或池复用对象不得成为当前可写车辆。若失败，请保留包含
    `stage`、`detail`、`slot`（仅具体槽读取失败时）、`populated`、`enabled`、`disabled`、`non_null_players`、
    `qualified_players`、`duplicate_qualified`、`proven_retired`、`scan_errors`、
    `candidate=(PVehicle,Player,Simable)` 和 `alternate=(PVehicle,Player,Simable)` 的车辆读取日志。
14. **只读轮胎与驱动形式采样**：分别使用一辆前驱、一辆后驱、一辆四驱和一辆高重心
    轿车。每辆车依次完成低速直线给油、松油滑行、制动、一次左漂和一次右漂，并记录车型
    与测试顺序。应先看到 `handling probe available`，随后轮胎通道应为 `armed`；扭矩通道
    先显示 `pending`，只有当前玩家 transmission 的只读虚表 slot 8 再次验证成功后才显示
    `armed`。每组汇总应包含同一 `vehicle_generation` 下的四条 `handling_probe wheel=` 和
    一条 `handling_probe torque`。`wheel=0/1` 必须标为 `axle=front`，`wheel=2/3` 必须标为
    `axle=rear`，有效样本应为 `value_ok=1`。换车后必须出现新的代次，旧轮或旧 transmission
    指针不得继续出现在新代次中。本轮不要修改两个倍率；高重心车也只采基线，不做抓地增强。
    若启动时通道被关闭，保留新增的 `stage=...`、预期地址和运行时字节诊断；在出现
    `handling_probe vehicle_generation` 前，普通遥测里的固定 `front_grip=1.000`、
    `rear_drive=1.000` 不能当作轮胎或动力指针证据。

出现镜头方向仍相反、镜头提前到位或长加载后跳变、自动反打仍只有 45°、
`steering_applied` 瞬移、人工方向未按 `2.0 command/s` 连续到位或永远无法到达满舵、任一方向输入后
自动反打仍覆盖前轮、换车在 3 个连续有效帧后仍不恢复、旧车辆通过
反向归属、回正推力穿过中心、换边窗内仍写 yaw、控制异常或游戏闪退时，立即退出游戏并
移除 Alpha 文件。

## 回传内容

请回传一次完整左漂、一次完整右漂、反打与顺打各一次即时移交、一次松手后 0.20 秒重接管、一次
20° 到 37.5°、一次 +37.5° 到 -37.5°、一次成功和一次失败的换边，以及一次换车恢复日志。
镜头还需提供中心到侧、稳定侧到另一侧和长加载恢复
三段视频或连续截图，并标注开始、中点和结束时间。物理研究请另外标注前驱、后驱、四驱
和高重心车的车型与测试顺序，并保留各车从 `vehicle_generation` 开始到最后一组
`handling_probe torque` 的完整日志。

普通遥测行保留：

- `offset_deg`、`raw_steer`、`side`；
- `steer_mode`、`neutral_s`、`handoff_pending`、`handoff_s`、`auto_target_deg`；
- `steering_target`、`steering_applied`、`steering_slew`；
- `effective_target`、`automatic_allowed`、`manual_observed`、`direction_mask`；
- `yaw_mode`、`relative_yaw_rate`、`yaw_target`、`yaw_delta`、`yaw_strength`；
- `side_transition`、`transition_target`、`transition_s`、`offset_limited`。

其中 `steering_target` 是控制器输出的逻辑目标，`effective_target` 是执行层在人工输入、
迟到输入和自动许可仲裁后的实际目标，`steering_applied` 是实际写给游戏的结果，
`automatic_allowed=1` 才表示本帧确实允许自动反打覆盖。`steering_slew=1` 表示漂移会话内的
前轮执行路径正在生效。任一方向人工输入出现后，
自动目标必须立即停止覆盖，人工路径的 `steering_target` 应等于 `raw_steer`；旧移交字段始终
保持为零。有效人工方向的 `steering_applied` 应从当前值按固定 `2.0 command/s` 追随；
`waiting-angle` 中立帧透传原始值，`reengage-delay` 中立帧保持最后实际值。自动反打
同样使用 0.9.5 的 `2.0 command/s` 路径。

同时保留初始/恢复稳定期完成行、镜头安装行和所有镜头关闭行。成功安装格式为：

```text
alpha camera assist installed look_at=... caller=... mode=direct|orbit-compatible look_at_chain=direct|camera-mod angle=3.0deg direction=inverted entry=2.0s-smoothstep return_switch=1.25s-smoothstep
```

还要保留所有 `alpha yaw_apply` 行：

```text
alpha yaw_apply mode=... source=... sink=... age_ms=... offset_deg=... relative_rate=... target=... delta=... yaw_before=... yaw_after=... identity_ok=1 setter_ok=1
```

重点确认 `source + 1 == sink`、`identity_ok=1`、`setter_ok=1`。换边窗、中立和自动反打
期间不应出现该行。若日志提示人工 yaw 通道被永久关闭，智能反打仍可继续工作，但要停止
评估车身响应并回传关闭原因前后的完整日志。

若只出现 `alpha camera assist disabled ...` 或 `alpha camera assist disarmed ...`，方向和 yaw
辅助仍应照常工作；继续完成车辆控制测试，并把镜头一项单独标为失败。

当前抓地/动力研究仍是只读。启动及采样日志的关键格式为：

```text
handling probe channel=tire-force armed target=... read_only=1
handling probe channel=drive-torque pending: profile entry validated; waiting for player transmission vtable slot 8
handling probe channel=drive-torque armed target=... read_only=1
handling_probe vehicle_generation=... player=... pvehicle=... rigid_body=... holder=... inner=... suspension=... transmission=... wheels=... ownership_ok=1
handling_probe wheel=0 axle=front object=... calls=... drift_calls=... invalid=... D4=... D8_before=... D8_after=... DC_before=... DC_after=... traction110=... return=... args=(...) value_ok=1
handling_probe torque object=... calls=... drift_calls=... sign_pos_zero_neg=... drift_pos_zero_neg=... invalid=... field34=... return=... value_ok=1
```

静态资料已经确认索引 `0/1` 属于前轴、`2/3` 属于后轴；本次日志用于验证玩家对象归属、
`D8/DC` 字段语义、调用时序，以及正向总扭矩是否仍由原生传动系统正确分配到前驱、后驱
或四驱车的实际驱动轮。若某个探针通道显示 `disabled`，仍可继续验证操控与镜头，但应回传
关闭原因；不要同时加载旧诊断版，也不要修改前轮侧向力。本版轮胎侧向力和驱动扭矩两个 writer 均保持关闭；刚体补速另按发布构建边界执行。

主包刚体补速通道的名义满幅目标加速度为 `5.25 m/s²`（相对 5.0 m/s² 基线提高 5%）；在此前 `0.80` 发布倍率上再相对回调 10%，发布输出统一乘以
`0.612`，所以低速实际满幅目标为 `3.213 m/s²`。加速方向在反打时仅取实际前轮反打角的 `12.5%`（即旧 half-angle 中间夹角偏转的 25%；前轮正打、顺打或零度时严格沿车头），并按整车线速度模长在 70/125/150/170 km/h 阈值间线性衰减。旧 0.9.11 日志中的 47 次 `write_ok=1`
只说明旧式调用和即时读取没有报错，不能证明请求的净速度增量被保留。0.9.15 运行日志必须
同时记录 `speed_mps`、`steering_snapshot_valid`、`applied_steering`、`drift_side`、
`direction_mode`、`accel_direction`、`direction_heading_dot`、`before_velocity`、
`after_velocity`、`actual_delta`、`actual_delta_mag`、`projected_delta` 和 `orthogonal_residual`；
只有最终收窄后的反打方向投影及总增量与请求 `delta_v` 匹配，
且正交残差在容差内时才允许 `write_ok=1`。无变化、增量不足/过强或方向污染均视为失败，
并永久关闭本次进程的刚体 writer。
