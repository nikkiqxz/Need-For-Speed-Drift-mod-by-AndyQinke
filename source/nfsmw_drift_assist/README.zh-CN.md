# NFSMW 2005 漂移与车身姿态辅助核心

## 1.3.8 后轮转向速度随车速变化

后轮转向最大执行速率改为随车速线性变化：30 km/h 及以下为 20 度/秒，
30-120 km/h 之间从 20 度/秒平滑降至 10 度/秒，120 km/h 及以上保持
10 度/秒。70 km/h 的反向/同向分界、车型角度配置及无高速关闭限制保持不变。

## 1.3.7 后轮转向执行速度调整

实机日志确认多车型匹配、70 km/h 相位切换、视觉写入和物理写入均正常，且物理
写入与恢复严格成对、无恢复失败。后轮转向执行速率由 20 度/秒降低为 10 度/秒，
急转向时的响应速度放慢 50%，减少突兀的车身重量转移。正式构建关闭后轮转向、
HUD 和普通运行诊断日志，HUD 显示功能保持启用。

## 1.3.6 后轮转向速度规则调整

后轮转向在低于 70 km/h 时使用配置的第一项反向角，达到或高于 70 km/h 时使用
第二项同向角。取消原有超过 200 km/h 关闭后轮转向的逻辑，高速下继续使用同向角。

## 1.3.5 多车型后轮视觉归属修复

诊断日志确认 `REVUELTO`、`SLR`、`U9` 的真实车名和 INI 条目均已正确匹配，物理
后轮方向也已写入；不可见的原因是视觉归属把车名节点深度固定为 1。`tt` 的车名节点
恰好位于深度 1，而其他实测车型位于深度 0。现在会在当前玩家车辆属性链的任意深度
匹配真实车名键，不再写死深度，也不会把性能部件节点当成车名。后轮转向配置容量为
512 台。

## 1.3.4 改装车真实车型记录修复

`[RearWheelSteeringVehicles]` 现在从游戏当前车辆数据库读取实际选中的改装车名称。
`PVehicle::GetVehicleName()` 在这套改装车环境中仍可能返回 donor 车名 `tt`，因此不再
用于后轮转向车型选择。性能部件节点仅用于定位该车的后轮模型，也不参与车型选择。
例如车辆 `slr` 即使使用 `aventadorsvj_top` 性能节点，也只需配置 `slr -6 2`。
正式构建继续关闭 HUD 日志、后轮转向日志和普通运行日志，HUD 显示功能保持启用。

## 1.3.1 真后轮转向正式版

后轮转向不再通过车身偏航增量模拟。玩家车辆的两个后轮方向基会在原版完整四轮
求解期间按配置角度临时旋转，求解结束后立即恢复；后轮的两套模型矩阵也会应用相同
角度。物理与画面写入均要求当前玩家车辆、悬挂和属性集合链同时匹配，AI 车辆保持
原值。实机测试已确认物理钩子成功，轮胎方向写入与恢复严格成对且无恢复失败；
30 km/h 相位切换、200 km/h 上限、漂移互斥和驾驶辅助互斥均通过。正式构建关闭
普通运行日志、HUD 日志和后轮转向日志。

## 1.2.1 大型车型倍率表修复

自动反打车型倍率表容量由 128 项扩展到 512 项。此前超过 128 项时配置解析会
故障关闭并禁用整个控制器；现在完整的 130 台玩家车辆配置可以一次加载。
后轮转向实验仍保持关闭。

## 1.2.0 旧后轮转向实验状态

当前版本已按 `laferrari_top -15 15` 匹配 tt 的性能部件节点；日志中的
`rear_physics` 证明配置角度转换成了有上限的车身偏航增量，并在 180-200 km/h
逐渐衰减。`GetWheelSteer` 钩子也确实返回后轮 2/3 的目标角度。

**这还不是真正的后轮转向。** 原版 `speed.exe` 的 `GetWheelSteer` 对后轮直接
返回零；唯一已观测到的调用点 `0x006B7144` 属于车辆状态构造函数，而该函数
在 `0x006B7273` 之后只把前轮的转角写入输出。现有钩子没有改变后轮轮胎接地
受力，也没有驱动后轮模型变换，所以车身偏航可有体感，后轮外观仍不会打角。
完成此功能需要分别定位并验证玩家后轮的轮胎力方向和模型变换入口，不能把
前轮转角槽或全车旋转当作后轮转向。

## 1.1.0 可配置后轮转向

1.1.0 在不改变 1.0.7 已验收漂移、驾驶辅助和 HUD 行为的基础上，新增按车型启用的
后轮转向。在 `drift_assist.ini` 的 `[RearWheelSteeringVehicles]` 中写入
`tt -5 5`，表示 `tt` 在 30 km/h 及以下按玩家原始方向输入最多反相 5 度，超过
30 km/h 最多同相 5 度；实际角度随输入比例变化。超过 200 km/h 完全关闭。

未列出的车型不修改。后轮转向属于驾驶辅助：漂移激活时立即释放，漂移退出 0.2 秒后
随驾驶辅助恢复。运行时只覆盖当前已验证玩家车辆的后轮 2/3，前轮与其他车辆始终透传
游戏原值；换车、菜单、遥测过期或 ABI 校验失败都会关闭覆盖。

## 1.0.7 CustomHud 显示场景同步修复

1.0.7 修复驾驶辅助 HUD 在局内始终不显示的问题。原来的“原版 HUD 可见性”地址并不是
`CustomHud.asi` 实际使用的显示状态，导致 ABS/ESC/TCS/LC 面板被错误拦截。现在按
`CustomHud` 的公开源码同步 `FEngHud_DetermineHudFeatures` 生命周期、
`FEngHud_IsHudVisible` 结果和 `0x0092FD94` HUD 表状态，并在原版状态首次发布前保留
HUD 表安全回退。普通前进超过 5 km/h、倒车隐藏、LC 静止/低速放行三条独立规则全部保留。
漂移、驾驶辅助、刚体加减速和 HUD 视觉参数未改。

## 1.0.6 回归 9 月 9 日主程序

1.0.6 不改变 1.0.5 的漂移、驾驶辅助或 HUD 行为，只将启动门禁恢复到 2026 年 9 月 9 日
签章版 `speed.exe`。HUD 继续同时服从普通前进超过 5 km/h、倒车隐藏、LC 静止/低速放行三条
本地规则和原版比赛 HUD 主可见性状态。

## 1.0.5 扩容主程序兼容重构建

1.0.5 不改变 1.0.4 的漂移、驾驶辅助或 HUD 行为，只将启动门禁更新到序章购车安全的
扩容版 `speed.exe`。该扩容研究版现已暂停，不是 1.0.6 的运行目标。

## 1.0.4 原版 HUD 可见性同步

1.0.4 完整保留 1.0.2 的车辆控制、驾驶辅助逻辑、HUD 视觉和三条本地显示规则：普通
前进速度必须超过 5 km/h 才显示，倒车时隐藏，LC 激活时允许静止或低速显示。在此基础上，
每次 D3D9 绘制还会读取原版比赛 HUD 使用的主可见性状态。只有本地规则允许且原版 HUD
可见时才绘制面板；原版 HUD 因菜单、暂停、过场、界面切换或游戏自身 HUD 开关隐藏时，
面板同步隐藏。车辆遥测失效时仍保持故障关闭，不显示旧状态。

## 1.0.2 驾驶辅助细节迭代

1.0.2 完整保留 1.0.1 已验收的漂移、刚体加减速、LC 锁止和 HUD 视觉。LC 工作时
继续忽略正常前进必须超过 5 km/h 才显示 HUD 的门槛，静止和低速起步均能看到 `LC`；
倒车隐藏规则不变。TCS 的 1 至 7 挡触发概率改为
90%、80%、70%、60%、50%、40%、30%。

ESC 第二部分的基础回正能力在 1.0.1 上提高 15%。车身相对行进方向的偏航从 10 度
增大到 35 度时，修正权限使用 smoothstep 曲线从 1 倍平滑增至 2 倍，超过 35 度保持
2 倍。既有 0.9 秒渐入、0.3 秒渐出、人工转向降权、目标偏航限制和逐帧增量限制全部
保留，避免在一两帧内接管车身姿态。

## 1.0.1 驾驶辅助与 LC

1.0.1 完整保留 1.0.0 已验收的漂移控制。ESC 侧滑回正除原有快速三次换向外，
在非漂移且车速高于 80 km/h 时还会响应四轮均侧滑或车身相对行进方向超过 10 度；
最大修正能力提高约 25%，仍使用 0.9 秒渐入和人工转向降权。TCS 的 1 至 7 挡触发
概率依次改为 75%、65%、55%、45%、35%、25%、15%。

HUD 倒车时完全隐藏；正常前进必须超过 5 km/h 才显示。车速低于 10 km/h 时同时
按住油门与脚刹或手刹会进入 LC：每个物理帧移除车身正向速度并把两个前轮角速度
归零，保留后轮旋转及左右转向，第三格显示 `LC`。松开油门或松开用于启动 LC 的
制动输入会立即释放全部限制并恢复 `TCS`。为使静止起步状态可见，LC 工作时是
5 km/h HUD 门槛的唯一例外；倒车隐藏规则仍有更高优先级。

## 1.0.0 正式版

1.0.0 保持 0.9.24 的车辆控制、驾驶辅助状态与 HUD 闪烁逻辑不变。HUD 背景改为
65% 不透明度，并以 1920x1080 为基准整体向左移动 32 像素；其他分辨率按既有比例
缩放位置。正式构建关闭 DebugView 输出并停止生成 HUD 日志文件，设备绑定 JSON 不受影响。

## 0.9.24 驾驶辅助 HUD 二次视觉迭代

0.9.24 保持全部车辆控制逻辑不变。HUD 闪烁频率在 0.9.23 基础上再提升 2 倍，
面板宽度再缩小 15%，背景改为 50% 不透明度；ESC 直线稳定辅助工作时仅白底
常亮，只有频繁换向后的侧滑回正辅助工作时才进行白红闪烁。

## 0.9.23 驾驶辅助 HUD 视觉与 ABS 响应迭代

0.9.23 保持 0.9.22 已验证的 DXVK 实际函数入口 Hook 与全部漂移操控不变。ABS
普通刹车持续触发时间由 1.0 秒缩短为 0.5 秒；HUD 闪烁频率至少提升 4.5 倍，
面板宽度缩小 35%，所有面板背景改为 85% 不透明度，文字尺寸与不透明度不变。

## 0.9.22 DXVK 实际函数入口 Hook

0.9.21 实机日志证明 VTable 写入成功，但后续没有任何画帧经过被替换的槽位。0.9.22
因此不再修改设备 VTable，而是用宿主已经启用的 MinHook 直接挂接 VTable 当时指向的
DXVK `Present` 与 `EndScene` 函数入口。这也能覆盖游戏在设备初始化阶段缓存函数地址、
后续绕开 VTable 调用的情况。

专用 HUD 日志新增每条 detour 的首次进入记录，并附带当时的状态位；之后才记录首个
成功画帧。这能明确区分“Hook 未执行”“驾驶辅助尚未发布显示状态”和“D3D 绘制失败”。

## 0.9.21 DXVK HUD 最终呈现路径

0.9.21 不改变 0.9.20 的车辆行为。针对实机使用 DXVK 且同时加载多个 HUD 插件的环境，
HUD 从仅在 `EndScene` 绘制改为优先在最终 `Present` 前开启短绘制段，确保不会被后续
界面或包装器覆盖；`EndScene` 保留为安装失败时的回退。两条 VTable Hook 都会读回
验证，所有矩形和字形仍由单批顶点提交。

插件会在自身所在的 `SCRIPTS` 目录追加写入
`Slippery_Drifting_FlashFish_by_AndyQinke.hud.log`，记录安装路径、实际 viewport、首个
成功绘制帧或绘制错误。日志失败和 HUD 失败均不影响漂移及驾驶辅助。

## 0.9.20 正式 HUD 与漂移接地容错

0.9.20 保持 0.9.19 已验收的漂移、ABS、ESC、TCS、镜头和刚体功能不变。局内 HUD
改用安装后读回验证的 D3D9 `EndScene` Hook，并通过一次 `DrawPrimitiveUP` 批量绘制
外框、三格和字形；日志会分别报告 Hook 安装成功及首个 HUD 帧成功绘制。HUD 故障仍
与车辆控制完全隔离。

`minGroundedWheels=2` 的含义仍是至少两个轮胎接地。新配置
`groundContactLossGraceSeconds=3.0` 只作用于已经建立的手刹漂移：接地轮数连续低于
门槛满 3 秒才结束会话，期间接地恢复会立即把计时清零。新会话激活前仍必须满足接地
门槛；玩家车、物理对象、碰撞、低速或倒车等既有安全条件没有放宽。

## 0.9.19 驾驶辅助模式与局内 HUD

0.9.19 完整保留 0.9.18 的漂移操控、每车反打倍率、镜头、刚体补速和手刹减速。
新增驾驶辅助模式默认在非漂移状态运行；漂移会话激活时立即注销全部驾驶辅助，漂移
退出满 `0.20` 秒后才重新激活，驾驶辅助不能与漂移物理写入同帧争抢。

- `ABS`：普通刹车连续保持 `1.0` 秒后，沿车辆实际速度反方向施加漂移手刹减速的
  `50%`；随后用 3 秒线性衰减到初始 ABS 力的 `20%` 并保持。松开刹车立即停止并
  清除计时。
- `ESC` 直线部分：车速高于 `80 km/h` 且玩家没有转向时，经短暂中立确认和 1.5 秒
  渐入后，仅用很小的偏航角速度修正抑制跑偏，不覆盖前轮输入。
- `ESC` 侧滑部分：2 秒窗口内至少完成三次有效左右换向且车身相对行进方向侧滑达到
  8 度时，使用渐入和逐帧限幅的回正修正；人工方向越大，辅助权限越低。
- `TCS`：不写车辆物理。每次新加速动作按 1/2/3/4 挡分别以
  `50%/40%/20%/10%` 概率触发显示；ESC 侧滑回正期间强制显示。

HUD 位于右下角转速表下方，纵向显示 `ABS/ESC/TCS`。漂移时三格继续显示为不工作，
但驾驶辅助逻辑完全停止。未工作时为深灰底白字；开始
工作时为白底黑字，持续 0.5 秒后在白底和红底间交替，停止工作立即恢复。HUD 使用
游戏 D3D9 设备直接绘制，不依赖 D3DX 字体；HUD 失败只关闭显示，不关闭车辆控制。

## 0.9.18 按车辆反打倍率与手刹减速

0.9.18 保留 0.9.17 已验收的操控、镜头、刚体补速和设备绑定行为。新增的
`[SmartCountersteerVehicleMultipliers]` 按 `pvehicle/default/cars/racers` 下的车辆
Collection 名匹配当前玩家车，并把该值乘到 `smartCountersteerAngleRad` 基础角上。
未列出的车辆使用 `1.0`；最终角度仍受 `maximumSteerAngleRad` 限制。包内默认配置为
`tt=0.75`、`997s=0.855`、`a3=0.22`、`a4=0.15`。

手刹连续保持满 `0.20` 秒且当前仍按下时，插件对整车刚体施加与实际速度方向相反的
减速，并停止同帧刚体补速。100 km/h 及以下使用最大减速的 15%，100-180 km/h
线性增强，180 km/h 及以上为最大；最大减速为低速最大有效补速 `3.213 m/s²` 的
`1.15` 倍，即约 `3.695 m/s²`。松开手刹立即撤销减速，短于 0.20 秒的点按不触发。

插件在读取配置或安装任何 Hook 之前仍会核验 6,135,872 字节签章文件、完整
MD5/SHA-256、64 字节 `NFSMWRF1` 文件尾、其中的小端验证代码 `868086`，以及文件尾
记录的原始规范 SHA-256。普通原版 `speed.exe`、旧名称
`Need For Speed MW-Reforged.exe` 或内容被修改的副本都会停用插件。

验证通过后，插件会在游戏目录现有的 `scripts`/`SCRIPTS` 文件夹中创建 `Slippery_Drifting_FlashFish_by_AndyQinke.device.json`；文件夹不存在时会自动创建。硬件身份由 SMBIOS System UUID、Windows `MachineGuid` 和系统卷序列号共同计算，三项缺一即拒绝。原始硬件值不会写入文件或日志；确定性绑定载荷先由 Windows DPAPI 以机器范围和禁止 UI 模式加密，再以大写十六进制写入严格 JSON。后续启动会解密并核对既有绑定；格式、密文、设备或载荷不匹配时不会覆盖旧文件，只会停用插件。

发布 ASI 的文件名仍必须严格保持 `Slippery_Drifting_FlashFish_by_AndyQinke.asi`。不要给 ASI 改名，也不要把旧版和新版同时放进 `scripts`。

这是一个与游戏内存布局解耦的 C++ 控制器核心。它根据车辆速度、车身基向量、线速度、角速度、驾驶输入、轮胎滑移和接地状态计算：

- 方向修正量；
- 油门保护与恢复制动；
- 可选的车身俯仰、侧倾和偏航角速度修正；0.9.16-alpha 只启用人工方向触发的有界偏航通道；
- 手刹触发流程的方向锁存、可选车身姿态目标、智能反打，以及前轮抓地/实际驱动轮正向动力倍率。

控制器本身不注入进程、不扫描内存，也不包含任何特定版本的裸地址。要在《Need for Speed: Most Wanted》(2005) 中运行，仍需编写一个 32 位 ASI/DLL 宿主适配层。

0.9.16-alpha 的功能宿主采用 `actuation=SteeringAndAttitude`，其操控路径与 0.9.15 完全一致。插件读取车头相对实际行进方向的夹角，首次达到 15 度且玩家方向中立时立即请求固定 55 度反打；按 `maximumSteerAngleRad=60°` 换算后，归一化目标为 `0.9167`，接管后保持到约 5 度才释放。自动反打已接管时，任意方向的有效人工输入只要超过死区，就会在当前更新立即取消自动反打并把前轮交还玩家，输入方向与自动反打相同也不例外。首次接管前，`waiting-angle` 的中立原始值直接透传并清除起飘顺打残值；自动和人工前轮均使用 0.9.5 的固定 `2.0 command/s` 路径，按剩余距离到达完整目标：从 55 度继续加到 60 度约需 0.042 秒，满左到满右约需 1 秒。人工打断并松手后，必须连续中立 `0.20` 秒才允许重新接管；`reengage-delay` 期间保持最后实际已应用方向，不主动回零，随后自动从该实值连续接续。钟摆换边清除旧侧自动轨迹。同向 yaw 目标为 `0.5625 rad/s`、加速度为 `3 rad/s²`、单次增量上限为 `0.15 rad/s`，力度在连续 2 秒内从 15% 线性升到 100%；反向输入主动让车身朝 0 度回正，最高 `0.45 rad/s`、加速度 `4.5 rad/s²`、单次增量上限为 `0.15 rad/s`，并由刹停距离和 ±2 度零区限制。已建立漂移过中心时进入 0.50 秒换边窗，相反侧达到 6 度才提交新侧。镜头采用与旧版相反的偏转符号，目标为 `-driftSide * 3°`；从中间到任一侧在 2.0 秒内平滑完成，退出归中和钟摆折身换边使用更快的 1.25 秒。

仅前轮抓地、实际驱动轮正向动力和 ZMenu 的核验结果见 [HANDLING_RESEARCH.zh-CN.md](HANDLING_RESEARCH.zh-CN.md)。0.9.16-alpha 继续使用失败隔离的只读轮胎/总扭矩探针，但最新实机日志中两条通道均因 `layout-unavailable` 关闭，没有取得可靠的抓地力或动力入口，因此轮胎侧向力与驱动扭矩两个 writer 继续保持关闭；普通遥测里的 `front_grip=1.000 rear_drive=1.000` 只是关闭状态，不是物理值。主包同时集成刚体补速通道，以 `5.25 m/s²` 为名义满幅目标（相对 5.0 m/s² 基线提高 5%）；前轮正打、顺打或零度时加速方向严格沿车头，反打时仅取实际前轮反打角的 `12.5%`（旧 half-angle 中间夹角偏转的 25%）。发布输出统一乘以 `0.612`，所以低速实际满幅目标为 `3.213 m/s²`，并按整车线速度模长在 70/125/150/170 km/h 阈值间线性衰减；写入前后速度、最终加速方向投影和正交残差会严格验证实际增量，旧 0.9.11 的 47 次 `write_ok` 不能证明净速度增量被保留。

## 可行性结论

结论是可行，但应分成两个边界清楚的部分：

1. 本目录中的控制器负责可独立测试的数学、状态机、限幅和渐入渐出。
2. 游戏适配层负责确认游戏版本、取得玩家车辆、采样输入和刚体状态，并把输出写回游戏。

公开逆向项目已经给出了足够的接口证据：

- `NFSPluginSDK` 的 MW05 `PVehicle` 类型包含 `mInput`、`mRigidBody`、速度、侧滑角、本地速度、接地轮数等成员，并提供 `PVehicleEx::GetPlayerInstance()`。
- `IRigidBody` 提供位置、线速度、角速度、前/右/上方向、姿态矩阵，以及 `SetAngularVelocity`、`ResolveForce`、`ResolveTorque` 等虚函数。
- `IInput` 提供方向、油门、刹车和手刹的读取或覆盖接口。
- `Most-Wanted-Vehicles-Decomp` 已还原原版的转向限幅、轮胎侧向力、漂移状态、后轮漂移摩擦和偏航阻尼算法，证明这些信号确实存在于游戏车辆物理链中。
- `NFSMWExOpts` 展示了成熟的 ASI/DLL、绝对地址调用、代码洞和每帧热键处理方式，并明确检查 v1.3 English 可执行文件。

风险不在控制算法，而在游戏 ABI：`PVehicle`、`PhysicsObject`、`PInput`、`RBVehicle` 等是多重继承类型，不能从其他编译器的对象布局随意推导裸偏移。优先使用已验证的虚函数和 SDK 辅助函数；任何直接字段访问都必须针对目标 `speed.exe` 单独验证。

## 架构与数据流

完整控制器可采用输入、姿态和物理倍率三阶段数据流：

```text
游戏每帧输入轮询完成
        |
        v
读取玩家 intent + 上一已积分车辆状态
        |
        v
DriftAssistController::update(state)
        |
        v
按 actuation 写回 steering / gas / brake，缓存可选姿态输出
        |
        v
ActiveComponents_TickAll 完成
        |
        v
主线程物理积分器入口前再次验证刚体
        |
        v
可选：转换并写入 angularVelocityDeltaLocal
        |
        v
游戏物理模拟
```

输入轮询 Hook 负责采样方向与手刹、运行控制器并仲裁前轮命令；人工 yaw 命令只缓存到下一物理序号。`ActiveComponents_TickAll` 原函数返回后，宿主再次验证线程、玩家身份、刚体和命令新鲜度，再通过已验证的 `IRigidBody::SetAngularVelocity` 消费一次。`nfsmw_sdk_adapter_example.cpp` 仍按输入、姿态和倍率三个阶段表达通用边界；0.9.15-alpha 只启用前轮方向和人工 yaw 两条有界通道。

适配器示例要求宿主为每个物理 tick 提供非零且递增的 `FrameContext::frameToken`，并在玩家车辆、其物理子对象或玩家/AI 所有权更换时递增且始终保持单调的 `vehicleGeneration`。代次切换后允许 `frameToken` 从任意新的正数重新开始；低于当前代次的延迟帧会在对象验证前直接丢弃，不会清除当前车辆的控制状态。一个格式有效的代次/帧号一旦被输入阶段观察到，就视为已消费；即使该帧随后因版本、对象或所有权校验失败，宿主也必须使用更大的新 `frameToken` 重试，不能复用同一帧号。只有在明确的游戏会话或进程边界调用适配器公开的 `reset()`，才允许代次和帧号计数重新开始。`FrameContext::playerControlled` 是宿主提供的安全断言，不是适配器从 `IRigidBody`、`IInput`、`ISuspension` 指针推导出的结果：宿主必须先通过版本校验的 `PVehicleEx::GetPlayerInstance()`，再用 SDK accessor 或已验证的对象关系确认本帧传入的三个接口确实属于该玩家车辆，并通过 `IsPlayer()` 与 `IsOwnedByPlayer()` 检查后才可置为真；不要依赖未经验证的多重继承裸偏移来做这项判断。无法证明时保持默认的假值。`requirePlayer=true` 时，示例会拒绝假值帧，因此不会把辅助写入 AI 车辆；如果显式关闭 `requirePlayer`，则表示宿主已明确允许 AI/非玩家车辆，不能再把它当作玩家保护措施。输入阶段、姿态阶段和倍率阶段都必须传入同一组身份；身份不匹配时示例会拒绝延迟写回。适配器还会在代次、已验证对象指针或 `playerControlled` 状态变化时清除控制器锁存，不会把上一辆车的手刹会话或方向状态带到新车；跨代清理不会向新对象恢复旧输入或倍率。0.9.15-alpha 在完成宿主侧 setter、时序和身份验证后才允许 `attitudeWriteAvailable=true`，且只对当前人工方向生成 yaw 命令。

控制器假定车身局部坐标为右 `+X`、上 `+Y`、前 `+Z`。`angularVelocityDeltaLocal` 是本帧一次性叠加的局部角速度变化，单位为 rad/s；宿主必须先转到世界坐标，再叠加到当前世界角速度。不要把它当作欧拉角或直接写入渲染矩阵。

通用侧滑辅助状态包含 `Off`、`Entering`、`Holding`、`Exiting` 四阶段。专用手刹会话还包含 `SideTransition`。0.9.15-alpha 的智能反打以 `steer_mode` 表示 `passthrough`、`waiting-angle`、`auto-countersteer`、`manual-override` 和 `reengage-delay`；兼容枚举值 `countersteer-handoff-delay` 不再由控制器输出。人工 yaw 只使用 `none`、`manual-same-assist` 和 `manual-opposite-recovery`。

`HandbrakeHold` 是独立的专用状态机。0.9.15-alpha 使用 `SteeringAndAttitude`，但不运行旧通用模式的侧滑 PD、油门保护、roll/pitch 恢复、起飘强推、保角或无人为输入的自动回正；姿态通道只承载人工方向触发的有界 yaw 响应。仅前轮抓地和实际驱动轮正向动力通道保持原值。

## 手刹触发智能反打模式

将 `activation` 设为 `HandbrakeHold`（默认值）时，控制器使用下面的专用流程：

1. `handbrakeInput` 高于 `handbrakeActivationThreshold` 时建立辅助会话。0.9.15-alpha 将 `handbrakeActivationHoldSeconds` 设为 0，所以一次有效的手刹输入采样即可激活；低速、前进速度不足或接地轮数不足时仍不会建立会话。会话建立后释放手刹不会立即结束。
2. 达到门槛后模式锁存，释放手刹不会立即结束本次辅助。开启 `useVelocityRelativeDriftAngle` 后，`bodyYawOffsetRad` 取 `-sideslipAngleRad`，表示车头相对实际行进方向的夹角；插件用它判断智能反打门槛、漂移侧和 35 度 yaw 硬边界，不会把它作为固定保角目标。
3. 当 `enableSmartCountersteer=true` 且相对漂移角绝对值达到 `countersteerActivationBodyOffsetRad`（Alpha 为 15 度）时，智能反打具备接管条件。未到门槛时 `steer_mode=waiting-angle`；中立原始值直接透传，并清除起飘时残留的顺打轨迹。
4. 自动反打无论是否已接管，任意方向的有效人工输入只要超过 `directionDeadzone`，就在当前更新进入 `manual-override` 并取消自动覆盖；与自动反打方向一致的输入同样取得玩家所有权。宿主在同一物理序号后续输入轮询中发现人工输入时也会立即停止覆盖，并在下一次控制器更新正式释放自动所有权。
5. 首次达到 15 度且玩家方向当前中立时，直接进入 `steer_mode=auto-countersteer`，不先等待 0.20 秒。只有已经自动反打、随后被人工方向打断的情况下，玩家再次回到中立才从零累计 `smartCountersteerReengageDelaySeconds`（Alpha 为 0.20 秒）。等待期间使用 `steer_mode=reengage-delay` 并保持最后实际已应用方向，不主动回零；计时完成且角度仍达门槛后，自动反打从该实值连续接续。
6. 自动接管时，反打方向由相对漂移角的当前符号决定：左漂向右、右漂向左；请求角固定为 `smartCountersteerAngleRad`（Alpha 为 55 度），不会再随漂移角大小增加。按 `maximumSteerAngleRad=60°` 换算后，默认 `steering_target` 的绝对值约为 0.9167。
7. 15 度只用于取得自动反打控制权。自动反打一旦接管，即使角度短暂回落到 15 度以下也会继续保持，直到相对漂移角进入 `exitBodyOffsetRad`（Alpha 为 5 度）或出现任意超过死区的人工方向输入才释放，避免在 15 度附近反复接管/释放。人工打断并松手后，必须连续中立 0.20 秒，且计时完成时仍需重新达到 15 度，才可再次接管。尚未达到过门槛且已松开手刹时，`noDirectionTimeoutSeconds` 只负责结束空闲会话，不会回正车身。未激活、低速、倒车、接地不足、对象失效或功能关闭时使用 `passthrough`，不保留旧自动命令。

0.9.15-alpha 不包含起飘强推、15 度保角或无方向自动回正。只有当前存在人工方向时才允许 yaw：同向目标为 `0.5625 rad/s`、加速度为 `3 rad/s²`、单次增量上限为 `0.15 rad/s`，权限在连续 2 秒内由 15% 线性升至 100%；反向输入主动指向 0 度，最大 `0.45 rad/s`、加速度 `4.5 rad/s²`、单次增量上限 `0.15 rad/s`，按剩余角度和刹停距离减速，并在 ±2 度零区撤销写入。方向中立、自动反打、重接管等待和 `SideTransition` 期间保持 `yaw_mode=none`、`yaw_delta=0.0000`。

前轮执行层区分自动和人工目标，但两者都沿用 0.9.5 已验收的固定 `2.0 command/s` 速率。有效人工方向取得所有权后，当前 `steering_applied` 从当时实际值按剩余距离连续追向完整 `raw_steer`；小幅补转会更快完成，人工目标再次改变时也从当时实值无跳变地重定向。`waiting-angle` 中立帧透传原始值；人工打断后的 `reengage-delay` 中立帧保持最后实际值，随后自动反打从该值连续接续。人工输入检测、即时移交、yaw 方向判定和 0.20 秒重接管计时仍使用未限速的 `raw_steer`。

漂移期间核心仍输出两个兼容倍率：`frontGripScale` 和 `rearDriveScale`。它们采用乘法语义：`1.0` 不变，小于 `1.0` 削弱，大于 `1.0` 增强；配置清洗范围为 `0..3`，并按 `blend` 从 1.0 平滑过渡到配置值。未来抓地倍率只能应用于玩家车辆前轮本帧的侧向轮胎力，不增加法向载荷，也不修改共享路面属性。`rearDriveScale` 是沿用旧接口的名称；未来动力倍率只能放大玩家车辆的正向总驱动扭矩，再由游戏原生传动系统把动力分给实际驱动轮，前驱作用于前轮、后驱作用于后轮、四驱作用于四轮。零或负扭矩、发动机制动和倒车必须原样保留。控制器只产生倍率，不会凭空改变游戏的轮胎或发动机参数。

已建立的漂移在反向回正进入 ±2 度时打开 0.50 秒换边观察窗。期间自动反打和 yaw 均关闭，并清除旧侧自动轨迹；前轮只服从人工输入，并继续使用固定 `2.0 command/s` 速率。相反侧达到 6 度即提交新侧，无论此时手刹是否再次按下。若超时、回到旧侧 6 度或重新输入旧侧方向，则清空状态并退出会话。

0.9.15-alpha 的手刹会话使用手刹触发、相对漂移角测量、方向仲裁、人工 yaw 和显式换边状态。`enableThrottleProtection` 对应的 `throttleScale`/`brakeAdd`、旧模式的 yaw/roll/pitch 姿态控制，以及轴倍率写入都不会在本版功能宿主中触发。

## 配置

示例见 `drift_assist.example.ini`。键名与 `AssistConfig` 成员逐一对应。核心提供无外部依赖的 `ParseAssistConfigIni()` 文本解析函数；宿主负责读取文件内容、处理路径和热重载，再把解析后的结构体交给 `DriftAssistController`。解析器忽略未知键/区段，支持没有 section 的默认区段；控制器随后通过 `setConfig()` 做范围和有限值清洗。换车或重新加载配置时仍应调用 `setConfig()` 和 `reset()`。

手刹会话模式相关的键如下（C++ 字段使用弧度，INI 解析器也应明确按弧度写入）：

| 键 | 作用 |
|---|---|
| `activation=HandbrakeHold` | 选择手刹会话流程；名称为兼容保留，本版不再等待长按时长。 |
| `actuation=SteeringAndAttitude` | 0.9.15-alpha 启用智能反打与人工方向触发的有界 yaw 通道。 |
| `useVelocityRelativeDriftAngle` | 使用 `-sideslipAngleRad` 测量车头相对实际行进方向的夹角，用于智能反打门槛、漂移侧、换边和 yaw 硬边界。 |
| `handbrakeActivationHoldSeconds` | 旧配置兼容字段；0.9.15-alpha 设为 0，当前手刹会话逻辑不再用它延迟激活。 |
| `handbrakeActivationThreshold` | 手刹输入判定为按下的归一化阈值。 |
| `directionDeadzone` | 人工方向输入判定死区；任意方向输入超过死区时都在当前更新取消自动反打并把前轮交还玩家。 |
| `noDirectionTimeoutSeconds` | 尚未达到过反打门槛时的空闲会话超时；只结束会话，不会驱动车身回正。 |
| `enableSmartCountersteer` | 开启或关闭本版智能反打。 |
| `countersteerActivationBodyOffsetRad` | 允许自动反打的最小相对漂移角；Alpha 为 15 度。 |
| `exitBodyOffsetRad` | 已接管后释放自动反打的近回正角；Alpha 为 5 度，用于避免在 15 度门槛附近抖动。 |
| `smartCountersteerAngleRad` | 自动反打的固定物理角请求；Alpha 为 55 度。 |
| `smartCountersteerHandoffDelaySeconds` | 仅为旧 INI/源码兼容保留；0.9.15-alpha 接受该键但控制器强制按 0 处理，任意有效人工方向输入都会立即让权。 |
| `smartCountersteerReengageDelaySeconds` | 自动反打被人工打断后，再次允许接管前的连续中立时间；Alpha 为 0.20 秒，首次接管不等待。 |
| `manualSteeringTransitionSeconds` | 旧 INI/源码兼容键；0.9.15-alpha 接受并清洗该值，但 live 人工前轮路径不读取它，而是固定使用 0.9.5 的 `2.0 command/s` 速率。 |
| `maximumSteerAngleRad` | 把物理角换算为归一化方向命令所用的最大转向角；Alpha 为 60 度。 |
| `enableManualYawAssist` | 允许当前人工方向触发受限 yaw；中立和自动反打阶段仍不写 yaw。 |
| `manualSameDirectionTargetYawRateRadS` | 同向输入只补足到该相对横摆速度；Alpha 为 `0.5625 rad/s`。 |
| `manualSameDirectionYawAccelerationRadS2`、`manualSameDirectionMaximumYawAngularDelta` | 同向补转的加速度和单次硬限幅；Alpha 为 `3 rad/s²` 和 `0.15 rad/s`。 |
| `manualSameDirectionRampSeconds`、`manualSameDirectionInitialStrength` | 连续同向输入的力度爬升时间和初始力度；Alpha 为 2 秒、15%，松开或改变方向即重置。 |
| `manualOppositeMaximumRecoveryYawRateRadS`、`manualOppositeYawDampingAccelerationRadS2`、`manualOppositeMaximumYawAngularDelta` | 反向输入主动朝 0 度回正的最高角速度、加速度和单次硬限幅；Alpha 为 `0.45 rad/s`、`4.5 rad/s²` 和 `0.15 rad/s`，同时受刹停距离及单帧距离限制。 |
| `manualYawHardBodyOffsetRad` | 达到该相对漂移角后禁止继续向外补转；Alpha 为 35 度。 |
| `pendulumTransitionZeroBandRad`、`pendulumTransitionConfirmBodyOffsetRad`、`pendulumTransitionWindowSeconds` | 钟摆换边的中心零区、相反侧确认角和观察时间；Alpha 为 2 度、6 度和 0.50 秒。 |
| `sameDirectionFullSteerSeconds`、`countersteerTransitionSeconds`、`minimumCountersteerAngleRad`、`maximumCountersteerAngleRad` | 旧自动满舵和动态反打兼容项；0.9.15-alpha 全部设为 0。 |
| `minimumDriftBodyOffsetRad`、`bodyOffsetHold*`、`driftAngleEntry*`、`bodyRotation*`、`entryYawBoost*` | 旧起飘、保角和主动增角参数；0.9.15-alpha 全部停用并归零。 |
| `attitudeAuthority` | 旧通用姿态模式的兼容参数；0.9.15-alpha 人工 yaw 使用上方独立目标、加速度与单次限幅，不把它作为总权限。 |
| `frontGripMultiplierDuringDrift` | 漂移时仅用于前轮侧向抓地的预留倍率；0.9.15-alpha 的功能宿主尚不写入，必须保持 1.0。 |
| `rearDriveMultiplierDuringDrift` | 沿用旧名称的正向总驱动扭矩预留倍率，未来由原生传动系统分配到实际驱动轮；0.9.15-alpha 尚不写入，必须保持 1.0。 |

镜头角度不是 INI 参数。功能宿主固定使用 `-driftSide * 3°` 作为目标，偏转符号相对早期版本反向。每次目标改变时都从当前实际偏转重新开始 smoothstep：中间到最大偏转用 2.0 秒；折身从一侧到另一侧或会话退出、状态失效后归中使用更快的 1.25 秒。

角度字段使用弧度：

```text
5 deg  = 0.08726646 rad
8 deg  = 0.13962634 rad
10 deg = 0.17453293 rad
11 deg = 0.19198622 rad
12 deg = 0.20943951 rad
15 deg = 0.26179939 rad
25 deg = 0.43633231 rad
28 deg = 0.48869219 rad
30 deg = 0.52359878 rad
32 deg = 0.55850536 rad
35 deg = 0.61086524 rad
45 deg = 0.78539816 rad
55 deg = 0.95993109 rad
60 deg = 1.04719755 rad
90 deg = 1.57079633 rad
```

枚举值建议按以下字符串解析：

- `activation`: `HandbrakeHold`、`Manual`、`Automatic`、`ManualOrAutomatic`
- `actuation`: `SteeringOnly`、`SteeringAndAttitude`、`AttitudeOnly`

布尔值使用 `true` / `false`。输入、权重和控制权通常应限制在 `[0, 1]`；方向输入和方向修正通常在 `[-1, 1]`。

## 构建与测试

要构建可加载的 32 位 ASI，请先阅读 [BUILD_ASI.zh-CN.md](BUILD_ASI.zh-CN.md)。默认构建只生成核心静态库和测试程序；`NFSMW_BUILD_ASI_HOST` 生成故障关闭的 x86 宿主，`NFSMW_ENABLE_ALPHA_BRIDGE` 才选择有界功能桥。输入、车辆、协调器、phase/timing 四种只读诊断和功能桥强制互斥。任何构建和打包命令都只写指定构建/输出目录，不会自动覆盖游戏 `scripts` 目录。

控制器核心不依赖 Windows API，可以先在本机工具链上构建和测试。若包内提供 CMake 工程，可从本目录执行：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

游戏宿主必须编译为 32 位 x86 DLL/ASI，并使用与所选 SDK 兼容的调用约定。独立核心可用 MSVC、Clang 或 GCC；本目录的 `NFSPluginSDK` 类型适配示例则明确限定为 MSVC Win32。`PVehicle` 等游戏对象使用 MSVC 多重继承 ABI，不能让 `i686-w64-mingw32` 按它自己的类布局直接解释这些对象。使用 s-b SDK 的 MinGW 路线时，只使用其已验证 Hook/AOB，捕获游戏已经调整好的接口子对象指针，或编写经过实机验证的 thunk。

核心测试至少应覆盖：

- 低速、倒车、非玩家、非比赛、接地不足或无手刹输入时不介入，且不残留旧自动方向；
- `handbrakeActivationHoldSeconds=0` 时，首个有效手刹采样立即建立会话；
- 首次接管前 15 度以下使用 `waiting-angle`，中立值透传并清除起飘顺打残值；首次达到门槛且方向中立时立即进入 `auto-countersteer`；
- 自动反打一旦接管，在 14.x/15.x 度之间波动时保持锁存，进入 5 度近回正区才释放；
- 左、右漂移得到符号相反的固定 55 度反打，按 60 度标定得到绝对值约 0.9167 的归一化命令；
- 自动反打已接管时，任意方向的有效人工输入超过死区都在当前更新转为 `manual-override`；`countersteer-handoff-delay` 不再出现，`handoff_pending=0 handoff_s=0.000`；
- 人工所有权下，从当前 `steering_applied` 以固定 `2.0 command/s` 追向每个新 `raw_steer` 且不削终点；55 度继续补到 60 度约需 0.042 秒，满左到满右约需 1 秒；
- 首次自动接管没有 0.20 秒等待；只有自动反打被人工打断后才进入 `reengage-delay`；
- 重接管计时按秒而非固定帧数，在 30/60/120 FPS 下均不会提前；连续中立 0.20 秒内保持最后实际方向，满时自动从该实值连续接续；
- 重接管计时完成时角度若已跌破 15 度，应转入 `waiting-angle` 并透传中立，不能强行恢复旧自动目标；
- 方向中立、自动反打和重接管等待时，`yaw_mode=none` 且 yaw 命令为零；
- 人工同向输入只补足到 `0.5625 rad/s`，受 `3 rad/s²` 和 `0.15 rad/s` 单次上限约束，力度在连续 2 秒内由 15% 线性升至 100%，松开或换向会重置；
- 人工反向输入主动建立朝 0 度的回正速度，受 `0.45 rad/s`、`4.5 rad/s²`、`0.15 rad/s` 单次上限、刹停距离和单帧距离共同约束，且在 ±2 度内不写 yaw；
- 只有旧侧曾达到 15 度后才能进入 `SideTransition`；跨入 ±2 度后 0.50 秒内相反侧达到 6 度才确认新侧，超时、回旧侧或重新输入旧侧均干净退出；
- `SideTransition` 期间方向目标完全交给玩家并清除旧侧自动轨迹，自动反打和 yaw 均关闭；有效人工方向仍使用固定 `2.0 command/s` 速率，确认新侧后同向力度从 15% 重新爬升；
- 35 度外不继续向外补转；左右符号镜像；
- 镜头目标按 `-driftSide * 3°` 取与早期版本相反的符号；首次从中心到一侧用 2.0 秒 smoothstep，归中和折身换边用 1.25 秒；
- 首次稳定观察严格需要 60 个独立物理帧；首次完成后的车辆/身份变化或节拍恢复只需双向归属一致的 3 个连续独立物理帧。观察期间不输出方向、yaw 或镜头漂移目标；
- 仅前轮抓地和实际驱动轮正向动力倍率保持 1.0，writer 仍关闭；
- `dt` 异常、零速度、退化基向量、对象变化和 `resetState` 均安全复位。

单元测试通过只证明控制器数学行为正确，不证明任何游戏地址、虚表或字段偏移适用于本机的 `speed.exe`。

## 真实游戏集成

### 1. 固定目标版本

当前工作区测得的目标是 32 位 PE32/i386：文件大小 `6,135,808` bytes（`0x005DA000`），映像基址 `0x00400000`，入口 RVA `0x003C4040`（VA `0x007C4040`），`SizeOfImage=0x00693000`，PE 时间戳 `0x438E4C8C`，SHA-256 为 `36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9`。公开项目中常见的 `6,029,312` bytes（`0x005C0000`）属于另一份可执行文件 profile，不能拿来替代本机目标指纹。

这些 PE 字段仍不足以证明任一 Hook 地址兼容。宿主默认把必需代码锚点的 RVA 和签名留空，因此即使上述指纹匹配也会故障关闭。0.9.15-alpha 只有在输入、物理阶段、`SetAngularVelocity` 签名与虚表槽、玩家身份和车身状态均验证通过后才允许写方向或人工 yaw；前轮侧向力和正向总驱动扭矩 writer 保持关闭。

不要把这里或来源仓库中的绝对地址用于其他语言、未升级版本、破解补丁或经过重排的可执行文件。若确实需要支持多个版本，应为每个版本维护独立地址表，并用 AOB 签名和函数前导字节校验，而不是仅按模块基址猜测。

### 2. 选择加载与 Hook 层

可使用 Ultimate ASI Loader，或使用 BepInEx 6 NativeBootstrap 仅作为原生 DLL 注入器。NFSMW 是原生 Win32 游戏，没有 Unity/Mono 插件 API。

宿主可基于：

- `berkayylmao/NFSPluginSDK` 的 MW05 类型和扩展辅助函数；
- `s-b-repo/nfsmw-2005-sdk` 的 ASI 入口、MinHook、AOB 扫描和输入轮询 Hook。

0.9.15-alpha 的功能路径包含四个明确边界：

1. 在游戏输入轮询完成后读取手刹和玩家方向，结合已验证的相对漂移角调用控制器；漂移会话内任意超过死区的人工方向输入都立即取消自动反打，自动与人工目标均由固定 `2.0 command/s` 的 0.9.5 路径写回前轮。
2. 在同一车辆身份下采样车身基向量、线速度和角速度，计算车头相对实际行进方向的夹角与相对横摆速度。
3. 只把当前人工方向生成的 yaw 命令交给下一物理序号；`ActiveComponents_TickAll` 返回后重新验证并调用 setter 一次。
4. 稳定观察完成后才延迟安装镜头 Hook。调用链只接受游戏直接调用或 `NFSMWOrbitCamera.asi` 中唯一验证的调用点；既有 LookAt 入口只接受原版签名，或由 `NFS.CameraMod.asi` 所有且落在可执行段内的跳转链。任何所有者、入口、唯一性或安装校验失败都只关闭镜头辅助，不影响方向和 yaw。

宿主使用经签名和虚表槽双重验证的 `IRigidBody::SetAngularVelocity`（RVA `0x00296FF0`，虚表槽 25）。命令必须满足 `sourcePhysicsSerial + 1 == sinkPhysicsSerial`，并且只消费一次；过期、跨线程、身份变化或模式变化时丢弃。setter 调用或读回失败会永久关闭人工 yaw 通道，但智能反打继续工作。仅前轮抓地/实际驱动轮正向动力 writer 仍不调用。

### 3. 取得并验证玩家车辆

在 32 位 MSVC + 原 NFSPluginSDK 的已验证目标上，可调用 `MW05::PVehicleEx::GetPlayerInstance()`。0.9.15-alpha 功能桥会检查 `PVehicle::g_mInstances` 的全部 64 个槽位，并把中间空槽视为正常孔洞。只有实例项 `_InstanceLayout::mIsEnabled == true` 的槽才参与候选筛选；完全相同的 `(PVehicle, Player, Simable)` 重复槽按一个候选计数。每个候选的非空 `mPlayer` 对象必须先满足 SDK 的 `mDirty == false`、`mObjType != Invalid`、`mRigidBody != nullptr`，再验证双向归属：车辆成员指向的玩家必须等于 `ISimable::GetPlayer()` 返回值，而该玩家的 `GetSimable()` 又必须回指同一车辆 simable；同时还要通过 `IsPlayer()` 和 `IsOwnedByPlayer()`。只有一个不同的合格对象时才继续写入，因此换车时已禁用的旧车或重复槽不会制造长期歧义，而真正出现两个不同且合格的玩家车辆时仍会故障关闭。换车后的快速恢复还要求这组双向关系和完整身份连续 3 个独立物理帧保持一致。不要据此声称旧车辆/getter 诊断也已采用相同扫描规则。随后使用：

```cpp
auto* vehicle = MW05::PVehicleEx::GetPlayerInstance();
auto* input = vehicle ? vehicle->mInput : nullptr;
auto* rigidBody = vehicle ? vehicle->GetRigidBody() : nullptr;
```

每帧都要允许指针失效。加载、换车、过场、重置、比赛结束和返回前端时，旧对象可能被销毁。不要长期缓存未经重新验证的 `PVehicle*`、`IInput*` 或 `IRigidBody*`。

这条路径不能直接移植到 MinGW：s-b SDK 有意把 `PVehicle`、`PhysicsObject`、`PInput`、`RBVehicle` 等多重/虚继承类型保持为 opaque。无论使用哪条路径，都应检查 PE 指纹和 Hook 锚点、对象 vptr 所在只读区、将要调用的虚函数是否落在已验证的 `speed.exe` `.text` 范围；任一失败都只禁用辅助，不继续写内存。

### 4. 组装 VehicleState

建议来源如下：

- `dt`: 物理更新的真实模拟步长，不使用渲染帧时间。
- `body`: `IRigidBody::GetRightVector/GetUpVector/GetForwardVector` 或经验证的 `GetMatrix4`。
- `linearVelocityWorld`: `IRigidBody::GetLinearVelocity()`。
- `angularVelocityWorld`: 用于计算相对横摆速度；0.9.15-alpha 只在当前人工方向存在且通过全部门控时生成有界写回。
- `speedMps`: `PVehicle::GetSpeed()`；不确定单位时传 `0`，让核心由线速度求长度。
- 输入: `IInput::GetControls()`，或输入轮询后的运行时镜像。
- `groundedWheels`: 优先用 `ISuspension::GetNumWheelsOnGround()` 或逐轮 `IsWheelOnGround()`；只在布局已确认时读取 `PVehicle` 裸字段。控制器的规范槽位固定为 `FL, FR, RL, RR`。静态资料已经共同证明 `0/1` 是前轴、`2/3` 是后轴，但 SDK 的 `FL, FR, RL, RR` 与适配器示例的 `{0, 1, 3, 2}` 仍未统一后轮左右顺序。必须用只读运行时探针确认每个索引的左右位置和对象归属，不能把任一注释当成完整映射结论。
- `wheelSlip`、`wheelLoad`: 只能在轮胎/悬挂对象布局已验证后填写。核心按后轮载荷加权滑移，零载荷空转不会触发自动漂移。
- `surfaceNormal`: 0.9.15-alpha 不做车身自动回正，可令 `hasSurfaceNormal=false`；setter 与物理阶段均已验证时，人工 yaw 路径可令 `attitudeWriteAvailable=true`。
- `assistRequested`: 旧 `Manual` 模式可映射到自定义热键；`HandbrakeHold` 模式直接使用 `handbrakeInput`，且本版保持时间为 0。
- `collisionRecent`: 由最近碰撞事件或短计时器提供。
- `resetState`: 换车、传送、复位或从 gameplay 离开时置真一次。

0.9.15-alpha 不使用固定世界参考朝向。核心根据同一车辆、同一采样时刻的车身基向量和线速度计算 `-sideslipAngleRad`；适配层若只能提供上一帧状态，应在遥测中标注延迟，不能用预测值提前跨过 15 度门槛。

若读取 `PVehicle::mSlipAngle`，必须先确认其量纲。反编译研究表明原版 Chassis 的侧滑角可能以一整圈归一化值保存，而本核心统一使用由局部速度 `atan2(lateral, abs(longitudinal))` 得到的弧度；不要不经换算直接混用。

默认 `minGroundedWheels=2`，所以缺少可靠接地数据时控制器会按故障关闭处理，而不是仅凭侧滑或手刹介入。0.9.15-alpha 的方向与人工 yaw 均保留该门控；不要为了更容易触发而绕过车辆身份和接地验证。

### 5. 写回控制输出

0.9.15-alpha 的自动与人工方向分支都沿用 0.9.5 的固定 `2.0 command/s` 速率，实际耗时取决于剩余行程：

```text
if automaticOwnershipAllowed:
    steering_target  = clamp(automaticCountersteerTarget, -1, 1)
    steering_applied = advanceExistingAutomaticPath(steering_target, dt)
else if currentManualInput:
    steering_target  = rawSteering
    steering_applied = moveTowards(lastApplied,
                                   steering_target,
                                   2.0 * dt)
else if reengageDelay:
    steering_applied = lastActuallyAppliedCommand
else:
    steering_applied = rawSteering  # waiting-angle / side-transition passthrough
    clearOldTrajectory()
gas      = originalGas
brake    = originalBrake
```

`steer_mode=auto-countersteer` 时允许自动所有权。宿主必须让任意方向、绝对值超过 `directionDeadzone` 的有效人工输入立即取消自动覆盖，并在当前或下一次控制器更新切换到 `manual-override`，使 `steering_target == raw_steer`；与自动反打同向及迟到的已观测输入也不得继续被自动命令压住。每个有效人工目标都从当时的 `steering_applied` 开始，以固定 `2.0 command/s` 追向完整 `[-1, 1]` 目标；目标中途变化则从当前实值重定向。同一 physics serial 的重复轮询不得重复消耗速率预算。人工松手后必须连续中立 0.20 秒；`reengage-delay` 保持最后实际指令，计时完成后自动轨迹从该值连续接续。首次接管前的 `waiting-angle` 中立值透传并清除起飘顺打残值；`SideTransition` 清除旧侧自动轨迹。未激活的 `passthrough` 不覆盖游戏方向。保留玩家手刹输入，不要由辅助系统强制锁死手刹。

镜头使用已提交的 `output.driftSide`，只在 `output.active && driftSide==±1` 时把目标设为 `-driftSide * 3°`。该负号修正了早期版本的实机反向问题。目标每次变化都从当前角度启动独立 smoothstep，因此不会瞬切：首次从中心到最大偏转保持 2.0 秒；折身从一侧到另一侧，以及会话退出、身份失效或运行状态无效后的归中，均使用更快的 1.25 秒。

仅前轮抓地和实际驱动轮正向动力建议由适配器提供等效回调：

```text
applyFrontLateralGripScale(output.frontGripScale, frontWheelMask)
applyPositiveDriveTorqueScale(output.rearDriveScale)
```

这些回调属于未来或其他模式。0.9.15-alpha 的 `frontGripScale` 与 `rearDriveScale` 固定为 1.0，功能宿主不得调用倍率 writer。内置只读探针按玩家车辆代次稀疏采集四轮 `D4/D8/DC/0x110`、轮胎函数参数/返回值和 transmission 总扭矩；静态资料已确认前轴索引为 `0/1`，但仍须用实机日志确认对象归属、字段语义和调用时序。动力 writer 只处理玩家车辆的正向总驱动扭矩并交给原生传动系统分配，不得逐轮猜测前驱、后驱或四驱布局，也不得增强零/负扭矩、发动机制动或倒车。

0.9.15-alpha 只允许已验证的 `SetAngularVelocity` 人工 yaw 通道；不调用 `ResolveTorque`。方向中立、自动反打、重接管等待和 `SideTransition` 时不得排队命令。同向命令不把自然旋转拉慢；反向通道主动朝 0 度回正，在 ±2 度零区撤销写入；35 度外不继续向外补转。

### 6. 生命周期与故障关闭

出现以下任一条件时应跳过写回并调用 `reset()`：

- 不是 gameplay 状态；
- 玩家车辆、输入或刚体为空或内存验证失败；
- 正在加载、过场、传送、车辆销毁或换车；
- `dt` 非有限、过大或为零；
- 基向量含非有限值，或明显不是近似正交单位基；
- 可执行文件版本校验失败。

0.9.15-alpha 只保留一条待消费的人工 yaw 命令。身份、生命周期或输入快照验证失败时，宿主必须立即清除待用 yaw、自动反打命令、前轮执行状态、人工目标过渡、人工打断标志、同向力度爬升、换边观察窗、`neutral_s` 计时和镜头漂移目标；恢复后的首帧从 `passthrough` 重新开始，不能补写旧命令。进程首次建立资格需要连续 60 个独立物理帧；首次稳定期完成后，换车、身份变化或节拍失稳恢复只需启用槽、双向归属和完整身份均一致的 3 个连续独立物理帧。观察期间方向、yaw 与镜头漂移目标都故障关闭，镜头目标回到 0 度。主菜单或加载时游戏物理阶段可能停止推进，此时不会写回；物理序号恢复后才开始三帧观察。重生或车辆表重建后的玩家查找继续扫描完整 64 个槽位，先排除禁用槽、合并完全相同的身份，再以车辆到玩家、玩家回到车辆的双向关系区分当前车辆与残留旧对象。

建议提供运行时总开关和只读遥测。0.9.15-alpha 的关键行至少记录 `phase`、`raw_steer`、`steering_target`、`steering_applied`、`steering_slew`、`offset_deg`、`steer_mode`、`neutral_s`、`handoff_pending`、`handoff_s`、`auto_target_deg`、`relative_yaw_rate`、`yaw_target`、`yaw_mode`、`yaw_strength`、`side_transition`、`transition_target`、`transition_s` 和 `yaw_delta`；两个 `handoff_*` 字段只为旧日志格式兼容保留，必须恒为 `0`。车辆筛选失败行另记录 `populated`、`enabled`、`disabled`、`non_null_players`、`qualified_players`、`duplicate_qualified`、`proven_retired`、`scan_errors`、`candidate=(PVehicle,Player,Simable)` 和 `alternate=(PVehicle,Player,Simable)`；`proven_retired` 只统计经已验证的玩家反向关系明确排除的旧车槽。还应保留 initial/recovery probation 完成行、`alpha camera assist installed ...` 或 camera disabled/disarmed 行，以及 `handling probe channel=...`、探针验证失败的精确 `stage`/运行时入口字节、`handling_probe vehicle_generation=...`、四轮 `handling_probe wheel=...` 和 `handling_probe torque ...`。成功写回记录 `alpha yaw_apply mode=... source=... sink=... age_ms=... offset_deg=... relative_rate=... target=... delta=... yaw_before=... yaw_after=... identity_ok=1 setter_ok=1`。发布版默认关闭逐帧详细日志；物理探针约每 100 ms 采一个物理帧并约每秒汇总，避免高频路径直接写日志。

## 调参顺序

不要同时修改所有增益。建议按以下顺序：

1. 保持 `actuation=SteeringAndAttitude`，先确认未激活、方向中立、自动反打、重接管等待和 `SideTransition` 期间始终 `yaw_mode=none yaw_delta=0.0000`。
2. 保持兼容字段 `handbrakeActivationHoldSeconds=0`，确认按下手刹的首个有效采样即可建立会话；若误触发，再调整手刹输入阈值而不是恢复起飘延迟。
3. 调 `countersteerActivationBodyOffsetRad`，决定多大漂移角才允许自动反打；默认从 15 度开始。
4. 调 `smartCountersteerAngleRad`，决定自动反打强度；默认固定 55 度，按 `maximumSteerAngleRad=60°` 换算为约 0.9167 的归一化命令。
5. 调 `directionDeadzone` 和 `smartCountersteerReengageDelaySeconds`，确认任意方向超过死区都立即取消自动反打、自动被打断后松手连续中立满 0.20 秒才重接管；再把兼容键 `smartCountersteerHandoffDelaySeconds` 写成非零值，确认行为不变。
6. 确认每个新人工方向目标都从当前实际前轮指令按固定 `2.0 command/s` 速率到位；55 度继续补到 60 度约需 0.042 秒，满左到满右约需 1 秒。`waiting-angle` 中立值透传，`reengage-delay` 保持最后实际值，同一 physics serial 重复轮询不得重复推进。
7. 单独调同向目标、加速度、爬升时间和初始力度；`yaw_strength` 应在连续同向输入时从 0.15 平滑升至 1.0，松开或换向后重新开始。
8. 再调反向主动回正的最高速度和加速度，用 `alpha yaw_apply` 核对下一物理序号消费、刹停距离限速、±2 度零区撤销写入和 35 度外限幅。
9. 最后验证 `SideTransition` 的 2 度零区、6 度确认角和 0.50 秒窗口；仅前轮抓地和实际驱动轮正向动力倍率始终保持 `1.0`，writer 不得运行。

常见现象与调整方向：

| 现象 | 优先调整 |
|---|---|
| 按下手刹仍未建立会话 | 检查 `handbrakeActivationThreshold`、`minSpeedMps`、`minLongitudinalSpeedMps` 和 `minGroundedWheels`；保持时间应为 0 |
| 未到 15 度就自动反打 | 检查 `countersteerActivationBodyOffsetRad` 和 `offset_deg` 的单位、绝对值判断 |
| 首次达到 15 度仍等待 0.20 秒 | 首次中立接管不得走 `reengage-delay`；该延迟只用于人工打断后的重接管 |
| 任意有效方向输入后自动反打仍覆盖前轮 | 超过死区的任何方向输入都必须立即停止宿主自动覆盖，并进入 `manual-override`；迟到输入事件也必须在下一控制器更新释放所有权 |
| 日志仍出现移交等待 | `countersteer-handoff-delay` 不应再出现，兼容字段必须始终为 `handoff_pending=0 handoff_s=0.000`，非零旧 INI 值也不得改变行为 |
| 人工前轮速率异常或不同帧率结果不同 | 每次新人工目标都应从当前 `steering_applied` 按固定 `2.0 command/s` 重定向；耗时必须随剩余距离缩短，同一物理序号的重复轮询不得重复推进 |
| 松手后立刻又被接管 | 检查 `neutral_s` 是否从 0 重新累计，以及是否确实先发生过自动反打和人工打断 |
| 反打方向相反 | 核对 `offset_deg` 左负右正及游戏方向轴符号；左漂应向右反打，右漂应向左反打 |
| 反打过强或过弱 | 调整 `smartCountersteerAngleRad`，并核对 `maximumSteerAngleRad`；默认 55/60 对应约 0.9167 命令 |
| 自动接管后在 15 度附近反复切换 | 15 度是取得控制权的门槛，不是释放门槛；已接管时应持续到 5 度附近才释放。人工打断后则必须重新达到 15 度才能接管 |
| 左右输入时车身仍响应极慢 | 人工取得所有权后，前轮应按固定 `2.0 command/s` 立即开始移动，同时出现 `manual-same-assist` 或 `manual-opposite-recovery` 及对应 `alpha yaw_apply`。同向刚按下时 `yaw_strength≈0.15` 是预期渐入，连续同向 2 秒后才到 1.0 |
| 反向输入仍只减速、不主动回正 | 应出现 `manual-opposite-recovery`，且 `yaw_target` 指向 0 度一侧；若日志仍显示旧版“只阻尼”模式，说明加载的是旧 ASI |
| 车身跨过 0 度后立刻被写向另一侧 | 进入 ±2 度应先出现 `phase=side-transition`，其间必须无 yaw 写入；只有相反侧达到 6 度才确认新侧 |
| 换边后插件一直不恢复或不退出 | 核对 0.50 秒观察窗；相反侧到 6 度应重启新侧，超时、回旧侧到 6 度或转回旧侧方向应退出 |
| 中立、自动反打或换边观察时车身被插件推动 | 立即停测；这些阶段必须 `yaw_mode=none yaw_delta=0.0000` 且没有 `alpha yaw_apply` |
| 镜头不偏转，或与镜头插件同时加载后失效 | 检查 `alpha camera assist installed` 的 `mode=direct|orbit-compatible` 与 `look_at_chain=direct|camera-mod`；unsupported/disabled 只代表镜头链未通过，不应影响方向或 yaw |
| 会话退出或车辆失效后镜头不归中 | 目标应立即改为 0 度，并从当前偏转开始在 1.25 秒内平滑归中；保留失效前后和 camera disabled/disarmed 日志 |
| 多次漂移或换车后功能消失 | 功能桥应扫描全部 64 槽，只让 `enabled=1` 且双向归属一致的不同身份计入 `qualified_players`，重复身份见 `duplicate_qualified`；物理序号恢复后观察 3 个连续独立帧，若合格数长期为 0 或大于 1，保留完整启动、换车和筛选日志 |
| 抓地或动力发生变化 | 两个倍率必须为 1.0，功能宿主不得调用前轮抓地或正向总驱动扭矩 writer |

每次只改一到两个参数，并使用同一车辆、同一路段、同一速度区间重复测试。前驱、后驱、四驱和不同轴距车辆通常需要不同预设。

## 来源与证据

- [berkayylmao/NFSPluginSDK](https://github.com/berkayylmao/NFSPluginSDK)：MW05 头文件 SDK、`PVehicle`、`IRigidBody`、`IInput`、扩展辅助函数。
- [PVehicle.h](https://github.com/berkayylmao/NFSPluginSDK/blob/master/NFSPluginSDK/Game.MW05/Types/PVehicle.h)：玩家车辆组件与运行时状态。
- [IRigidBody.h](https://github.com/berkayylmao/NFSPluginSDK/blob/master/NFSPluginSDK/Game.MW05/Types/IRigidBody.h)：刚体读写、力和力矩接口。
- [IInput.h](https://github.com/berkayylmao/NFSPluginSDK/blob/master/NFSPluginSDK/Game.MW05/Types/IInput.h)：输入读取和覆盖接口。
- [Extensions.h](https://github.com/berkayylmao/NFSPluginSDK/blob/master/NFSPluginSDK/Game.MW05/Extensions.h)：玩家车辆、刚体和输入的验证与获取方式。
- [Brawltendo/Most-Wanted-Vehicles-Decomp](https://github.com/Brawltendo/Most-Wanted-Vehicles-Decomp)：MW05 车辆物理匹配反编译研究。
- [SuspensionRacer.cpp](https://github.com/Brawltendo/Most-Wanted-Vehicles-Decomp/blob/master/Speed/Indep/Src/Physics/Behaviors/SuspensionRacer.cpp)：原版漂移状态、转向、轮胎力、偏航阻尼和牵引控制。
- [Chassis.cpp](https://github.com/Brawltendo/Most-Wanted-Vehicles-Decomp/blob/master/Speed/Indep/Src/Physics/Behaviors/Chassis.cpp)：Ackermann 转向、抓地力和气动力。
- [ExOptsTeam/NFSMWExOpts](https://github.com/ExOptsTeam/NFSMWExOpts)：v1.3 English ASI、代码洞、函数地址和版本检查实例。
- [s-b-repo/nfsmw-2005-sdk](https://github.com/s-b-repo/nfsmw-2005-sdk)：32 位 ASI/DLL 入口、Hook、AOB 扫描、输入轮询和版本边界说明。

这些仓库都是逆向研究或社区 SDK，不是 EA 官方 API。类型、名称和地址可能不完整；来源之间出现差异时，以目标二进制的反汇编、运行时验证和故障关闭策略为准。

0.9.15-alpha 使用配置中的 `maximumSteerAngleRad=60°` 把固定 55 度反打换算为约 0.9167 的方向命令，但不会读写截图中的绝对地址。在当前受支持的同一份 `speed.exe` 中直接核验得：`0x008ABB7C=15.64605`（35 mph），`0x008ABB78=0.52359`（约 30 度），两者只在同一段原版漂移门槛判断中被引用；`0x008AADE8=60.0` 只参与一处角度/调校公式，可作 60 度标定线索，但不是已验证的每车转向接口。`0x008AB22C=0.85` 被多个公式复用；截图把 `frictionScale` 与 `aerodynamicScale` 同时标为 `0x00891050`，实际值为 `0.75` 且有大量跨系统引用，连界面逻辑也使用它，因此不能仅凭标签确定语义。上述地址都位于共享只读数据区，不是函数入口、setter 或每车 yaw/轮胎控制接口，绝不能全局改写。仅前轮抓地与实际驱动轮正向动力 writer 也仍未启用；发布配置必须让两个倍率保持 `1.0`。静态资料已确认 `0/1` 是前轴，但前轮侧向力路径仍需只读探针验证左右轮、对象归属、字段语义和调用时序；动力路径只允许放大正向总驱动扭矩并保留原生驱动形式。

## 安全与使用范围

本项目仅面向合法持有的游戏副本、离线单机研究和无障碍/驾驶体验实验。不要在在线、排名、竞速服务器或任何带反作弊的环境中加载；不要用于绕过完整性检查、反作弊、版权保护或平台限制。

修改实时车辆物理可能造成游戏崩溃、存档异常或不可预期行为。测试前备份存档；首次接入使用独立测试存档；版本不匹配、指针验证失败或数值非有限时必须禁用写回。构建、测试和打包不会自动覆盖游戏目录；安装必须在游戏完全退出后由测试者手动复制 Alpha ASI 与 INI。该控制器不需要也不应修改游戏存档。
