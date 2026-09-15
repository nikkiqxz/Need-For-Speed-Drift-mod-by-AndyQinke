# 前轮侧向抓地与驱动轮动力研究记录

本文记录 0.9.15-alpha 阶段已经核实的物理路径、已集成的只读探针，以及仍待实机证明的边界。当前功能宿主不会写入轮胎力或驱动扭矩；发布配置中的 `frontGripMultiplierDuringDrift` 与兼容旧名称 `rearDriveMultiplierDuringDrift` 必须保持 `1.0`。

## ZMenu 能确认到什么

[ZMenu Most Wanted 官方页面](https://zolika.dev/mods/nfsmwmenu)明确列出 `Drift handling`，所以该功能确实存在于 ZMenu，而不是游戏原生模式。页面只提供成品下载，没有公开源码链接。

[Zolika1351 的 GitHub 公开仓库页](https://github.com/Zolika1351?tab=repositories)当前没有 NFSMW ZMenu。对公开仓库、Release、Tag、Branch、Fork、Commit 和 Issue 的检索也没有找到可核验的 ZMenuMW 源码 URL 或提交；官方页面只链接 MEGA 成品包。官方变更记录只能确认 `Attrib Editor` 首见于 `22.03.10.1`、`Drift Handling toggle` 首见于 `22.04.03.1`，这些是发布版本号，不是源码 Tag。因此不能把网上转述或同名仓库当成实现证据。本地 `nfsmw-2005-re` 资料同样明确指出 MW 2005 原版没有官方 Drift Mode。

### 官方成品的静态分析

官方发布包中的 `ZMenuMW.asi` 已在不加载、不执行的前提下完成静态分析。样本是 32 位 COFF i386 PE，大小 `3,970,560` 字节，链接时间戳为 `2024-01-03 10:07:32 UTC`，SHA-256 为 `5449CD69C90A4AD9E5278DB5AE33A57E82E2043A45C7F0C577ED07597723D180`。

配置项和运行时开关的交叉引用已经确认：

- `DriftHandling` 字符串位于 `0x101C6944`，`0x100D5083` 将其读入 byte `0x1142A53D`；实际漂移逻辑在 `0x100AA942` 检查该 byte。
- `DriftHandlingSlippery` 字符串位于 `0x101C6954`，`0x100D509D` 将其读入 byte `0x103A9C01`；实际 slippery 分支在 `0x100AA9D8` 检查该 byte。
- 两个读取点都调用 `0x10032A90`，后者通过 IAT 调用 `GetPrivateProfileStringA`。这是普通 INI 读取，不是 VLT 字段访问。

`0x100AA942` 之后的实现从玩家车辆对象链取得 `RigidBody::Volatile*`，直接改写运行时刚体状态。结合 NFSPluginSDK 的结构布局，可以确认：

- `[volatile+0x20..0x28]` 是线速度，后续分支会按车体朝向和玩家输入直接调整该向量；
- `[volatile+0x30..0x38]` 是角速度，漂移转向和折身逻辑会直接调整该向量；
- `0x100AAA20` 写 `[volatile+0x2C]` 的质量值：普通模式写 `2000.0f`，slippery 模式写 `500.0f`；slippery 分支还会在 `0x100AA9F5..0x100AAA09` 调整线速度的一个分量；
- 关闭分支 `0x100AAC90..0x100AACF0` 将质量固定写回 `2000.0f`，并未保存、恢复每辆车原本的质量。

漂移分支附近没有调用 `Attrib::Collection::GetData`（`0x00454190`）、`Attrib::StringToKey`（`0x00454640`）或 ZMenu 自己的属性写入路径，也没有引用 `tires`、`STEERING`、未知抓地双 float、`TORQUE`、`GetDriveTorque()` 或单轮轮胎计算入口。因此可靠结论是：ZMenu 的 `Drift Handling` 是一套刚体速度、角速度和质量补丁，不是用 VLT 修改轮胎抓地或发动机/传动动力。

### 独立的 Attrib/VLT 编辑器

ZMenu 确实还包含与漂移模式相互独立的运行时属性编辑能力。二进制中可核验到 `Attrib Editor`、`Attribute by Name`、`Attribute by Hash`、`CollectionName`、`engine`、`TORQUE`、`FLYWHEEL_MASS`、`chassis`、`brakes`、`nos` 和 `TORQUE_BOOST` 等字符串；`0x10035350` 一带调用游戏 Attrib API，`TORQUE` 在 `0x10035AB9` 等位置作为数组字段处理并可逐元素写入。专用车辆调校菜单至少明确支持 `engine/TORQUE[0..7]` 和 `nos/TORQUE_BOOST`，通用编辑器则提供按名称或 hash 定位字段的机制。

这证明“在游戏运行期间编辑当前车辆的 VLT 字段和值”在技术上可行，也可借鉴其字段定位和数组元素编辑思路。但当前证据不能证明 ZMenu 的漂移模式通过 `tires`、`transmission` 或 `TORQUE` 工作；已识别的专用调校字段里也没有前轮抓地字段。后续研究必须把“VLT 编辑器提供的工具机制”和“漂移模式实际修改的物理量”严格分开。

### 公开 Git 中的相近实现

[berkayylmao/NFS-Chat-Chaos-Mod](https://github.com/berkayylmao/NFS-Chat-Chaos-Mod) 是第三方独立项目，其说明还把 ZMenuMW 列为可能不兼容项，因此不是 ZMenu 源码或 Fork。该项目 commit `2059ff1b71144328f50cbff18e05f50a063770af` 的 [`IcyRoads.hpp`](https://github.com/berkayylmao/NFS-Chat-Chaos-Mod/blob/2059ff1b71144328f50cbff18e05f50a063770af/src/Extensions/Game/MW05/Effects/IcyRoads.hpp#L29-L45) 会遍历 `SimSurface` 属性，暂时写入 `DRIVE_GRIP=0.35`、`LATERAL_GRIP=0.15`、`ROLLING_RESISTANCE=0.1`，停用时恢复备份。

这份源码进一步证明运行时属性写入与条件恢复可行，但它改变的是共享路面 Collection，会影响经过该路面的所有车辆和全部车轮；它既不是“只增强玩家前轮”，也不能证明 ZMenu 漂移模式的实现。`DRIVE_GRIP` 与 `LATERAL_GRIP` 属于 `simsurface` 布局，不能擅自等同于下文 `tires` RefSpec 中 hash `0xF177BE1B` 返回的前/后轴双 float。

### 非 ZMenu 的本地对照实现

研究时另有一个本地 `DriftMode.asi` 样本，SHA-256 为 `37F21CBB91A6EBECFA5BD93B9CAD51E03FD562C5DAA9DE3A1F2E48191CEAFF1A`。它是一个带 COFF/DWARF 调试符号的独立 32 位 MinGW 插件，编译单元名为 `DriftMode.cpp`；没有证据表明它属于 ZMenu，以下内容只能作为同类实现的对照，不能当作 ZMenu 结论。

该插件保留的符号、字符串和反汇编共同证明：

- 配置默认值为 `GasThreshold=0.15`、`HandbrakeThreshold=0.30`、`SlipEnterDeg=12`、`SlipExitDeg=4`、`MinDriftSpeed=15`、`DriftFrontGrip=0.66`、`DriftRearGrip=0.66`、`DriftSteer=1.30`、`NormalFrontGrip=1.0`、`NormalRearGrip=1.0`、`NormalSteer=1.0`、`HandbrakeRearGrip=0.70`、`SmoothRate=6.0`。
- 它从本地玩家 `PVehicle` 出发，经 `PVehicle+0xBC` 和游戏函数 `0x00454190` 取得 `tires` RefSpec（hash `0xBD38D1CA`），再查询 hash `0xF177BE1B` 得到两个相邻的 `float`；目前尚未破解这个 hash 的正式属性名。
- 它保存两个原始值，并平滑写成“第一个原值乘当前前轮倍率”和“第二个原值乘当前后轮倍率”；手刹时还会继续降低后轮倍率。它另行查询 `STEERING`（hash `0xFEF5CC35`）并平滑写入转向倍率。
- 玩家车辆变化时，它会重新捕获原始值，说明这种持久修改必须处理换车生命周期和原值恢复。
- 没有发现它修改传动对象、驱动扭矩 getter 或逐轮动力分配，因此它不能为“只增强实际驱动轮动力”提供直接入口证据。

这个样本最有价值的结论是：MW 的车辆 `tires` 属性中确有一对可在运行时访问的前/后轴抓地参数。它的 `0.66/0.70` 是为了主动降低抓地形成漂移，与本插件“只小幅增强前轮路线保持”的目标相反，不能直接照搬；当前仍应优先验证下面的逐轮 `D8` 路径，避免长期改写共享车辆属性。

## 安全的每车动态 VLT 覆盖方案

ZMenu 的独立 Attrib Editor 和本地对照样本共同证明 VLT 路线有研究价值，但它会修改可能被多个车辆实例共享的 Collection，生命周期风险高于逐帧、逐轮力缩放。若后续加入实验性 VLT writer，必须默认关闭，并按以下事务式模型实现：

1. 仅在唯一玩家车辆已经稳定绑定后解析该车的 `tires`、`engine` 或 `transmission` RefSpec。绑定键至少包含 `vehicleGeneration + PVehicle + Collection* + fieldHash + elementIndex`；不得沿用上一辆车的字段地址、元素数或原值。
2. 首次启用前保存 Collection 的 class/key/name、字段类型、元素数、字段地址、原始值和当前值。字段必须同时通过类型、边界和白名单校验；未知 hash 在正式命名和语义验证前只能只读。
3. 不得直接修改父级或共享 Collection，除非能证明目标 Collection 只属于当前车辆实例。若只能取得共享 VLT 记录，应停用该 writer，不能让 AI、对手车辆或同车型实例一起受影响。
4. 漂移进入时才写入目标值；漂移退出、回正、重生、退赛、返回菜单、玩家对象失效或检测到换车时，先尝试恢复旧车，再清除旧绑定。新车稳定后重新解析、重新捕获原值，不能等待固定秒数后盲目复用缓存。
5. 恢复前必须再次证明字段指针仍属于原 Collection、Collection 身份未变，并且当前值仍等于本插件最后一次写入值。任一条件不成立就放弃写回，避免覆盖游戏或其他插件在此期间作出的修改。
6. `TORQUE[0..7]` 如果最终用于实验，只能整体保存和整体恢复，并确认数组含义及引擎曲线插值方式。更优先的动力方案仍是临时缩放正向 `GetDriveTorque()` 返回值，让原生传动和差速器决定前驱、后驱或四驱分配。
7. 任一解析、身份、所有权或恢复校验失败时，只关闭 VLT 通道并输出原因；不能影响已经验收的自动反打、人工旋转辅助、转向响应和镜头功能。

下一轮只读日志应至少记录 `vehicle_generation`、`pvehicle`、`vehicle_key`、Collection 的 class/key/name、字段 name/hash/type/count/index、`field_ptr`、`original/current/applied`、共享引用或来源 vault 信息、`bind_reason`、`restore_reason`、`identity_ok` 和 `ownership_ok`。换车事件要同时输出旧、新车辆及 Collection 三元组；另外记录 `TORQUE[0..7]`、候选抓地双 float、当前 `GetDriveTorque()` 返回值和四轮 `DC`，用于判断 VLT 动力变化是否仍由游戏按原生驱动形式分配。

## 已验证目标

以下地址只适用于当前受支持的 `speed.exe`：

- SHA-256：`36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9`
- 单轮轮胎计算：`VA 0x0069DA90 / RVA 0x0029DA90`
- 单轮函数唯一调用点：`VA 0x006AA8F8`
- 四轮外层计算：`VA 0x006AA020`
- `ITransmission::GetDriveTorque()`：`VA 0x006A0580 / RVA 0x002A0580`

任何地址或短字节序列都不能替代完整 PE 指纹、调用关系、虚表槽和对象归属验证。

## 前轮侧向抓地

`0x0069DA90` 是 `float __thiscall` 单轮计算函数，接收五个浮点参数并以 `ret 14h` 返回。它最终返回 `wheel+0xD8`；调用者随后独立读取 `wheel+0xDC`，并把两者沿轮胎的两个正交局部方向组合为世界空间力。

当前反汇编证据支持以下字段语义：

- `wheel+0xD8`：横向轮胎力。它是单轮函数的返回值，也是未来前轮路线保持的候选缩放点。
- `wheel+0xDC`：纵向轮胎力或驱动力分量。`0x0069DF07` 附近使用轮角速度 `wheel+0x108`、轮半径 `wheel+0xC4` 和轮纵向速度计算滑移率，再把结果累加到该字段。

因此未来 writer 的边界应是：只缩放当前玩家车辆两个前轮在当前物理帧产生的横向力 `D8`。不得修改 `D8` 以外的纵向力 `DC`、法向载荷、整车抓地参数或共享 `SimSurface` 路面表。这样不会直接给后轮增加侧向抓地，也不会污染 AI、其他车辆或下一场比赛。

只增强前轴仍可能提高高重心车辆的侧翻力矩，所以不能把“仅前轮”视为绝对防侧翻。第一轮可写实验建议从 `1.05` 到 `1.10` 开始，硬上限暂定 `1.15`，并保持默认关闭；高重心轿车必须列入单独验收项。

## 实际驱动轮动力

SDK 已确认 `ITransmission::GetDriveTorque()` 是传动对象虚表 slot 8；当前实现 `0x006A0580` 仅执行 `fld [ecx+34h]; ret`。`PVehicle+0xFC` 指向 transmission 对象；总驱动扭矩生成路径还会在 `0x006B0044` 写入相关对象字段 `+0x80`。

插件不应自行判断车辆是前驱、后驱还是四驱，也不应在某个车轴上凭空增加逐轮扭矩。更符合游戏原生物理的方案是：仅在当前玩家车辆的有效漂移会话内，放大传动系统返回的正向总驱动扭矩，再让游戏原生传动和差速器继续分配：

- 前驱车仍只把动力分给前轮；
- 后驱车仍只把动力分给后轮；
- 四驱车仍按原生规则把动力分给四轮。

返回值为零或负数时必须原样返回，不能增强倒车、发动机制动或其他反向扭矩。正式 writer 前还必须用实机日志证明这个 getter 的采样阶段确实位于原生逐轮分配之前。

配置键 `rearDriveMultiplierDuringDrift` 暂时只作为旧接口名保留；其未来语义是“正向总驱动扭矩倍率”，不是“后轮倍率”。公开配置和日志需要清楚标注这一点，避免前驱与四驱测试者误解。

## 原版 NOS 证据与末级前向力备选

用户提供的实机行为描述是“原版氮气会沿车头方向给刚体一个向前的力”。当前静态分析已经确认原版 NOS 的接口、状态和消耗路径，但尚未证明它最终通过哪一种物理调用产生加速；因此下列“沿车头给整车质心施力”只能作为独立的末级备选，不能标成原版 NOS 的复刻实现。

### 已验证

- 当前受支持的 `speed.exe` 为 PE32、image base `0x00400000`，SHA-256 仍是 `36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9`。
- `0x006B3B30` 的 EngineRacer 构造路径在 `this+0x54` 写入 IEngine 虚表 `0x008ABF88`。结合 NFSPluginSDK 的 `IEngine.h`，可精确映射 `+0x1C GetNOSCapacity`、`+0x20 IsNOSEngaged`、`+0x24 GetNOSFlowRate`、`+0x28 GetNOSBoost`、`+0x2C HasNOS`、`+0x30 ChargeNOS`。
- `GetNOSCapacity = 0x006A03F0`，读取 EngineRacer `+0xA4`；`GetNOSBoost = 0x006A0400`，读取 `+0xA8`。`IsNOSEngaged = 0x006A0410`、`HasNOS = 0x006A0430`、`GetNOSFlowRate = 0x006A0460`、`ChargeNOS = 0x006A0470` 也与同一虚表顺序吻合。
- `0x00692930` 是当前完整映像中唯一读取 `Tweak_InfiniteNOS @ 0x00937804` 的代码路径。它先通过 IEngine 的 `HasNOS` 槽检查能力，再结合输入和车辆状态更新 EngineRacer `+0xF8`、`+0x100` 等 NOS 状态；关闭无限 NOS 时才执行正常消耗。
- NFSPluginSDK 的 `IRigidBody` 明确提供 `GetForwardVector`、两个 `ResolveForce` 重载、`Accelerate` 和 `ConvertLocalToWorld`；`ICollisionBody` 提供 `IsInGroundContact`。当前插件也已经验证当前玩家 `PVehicle+0x78` 的刚体对象、`PVehicle+0x11C` 的速度和 `PVehicle+0x130` 的接地/着地轮信息，并对玩家唯一性、刚体虚表和换车代次做检查。
- 当前目标的 `IRigidBody` 虚表为 `VA 0x008AC880`。虚表 slot 35 精确指向 `ResolveForce(Vector3) @ 0x006976C0`：实现不做坐标变换，直接把传入向量逐分量累加到 `RigidBody::Volatile+0x50/+0x54/+0x58` 的 force 累加器，因此入参必须与该世界空间累加器使用相同坐标系。slot 39 精确指向 `Accelerate(Vector3,float) @ 0x00699E10`：当 `Volatile+0x5D` 的模式字节为零时，它直接执行 `linearVelocity += distribution * amount`，写入 `Volatile+0x20/+0x24/+0x28`。所以 `amount` 是本次调用的速度增量系数，不是力，也不是由函数内部乘过 `deltaTime` 的加速度。

### 尚未验证

- 尚未把 `GetNOSBoost` 的 x87 返回值从某个确定的 IEngine 虚调用一路追踪到发动机扭矩、传动扭矩、`IRigidBody::ResolveForce` 或 `IRigidBody::Accelerate`。对 `0x00690000..0x006BFFFF` 内 18 个 `call [reg+28h]` 逐一检查后，没有一个能静态证明接收对象使用 `0x008ABF88`；多数可由参数或返回类型直接排除，余下的浮点无参候选也来自尚未证明属于 IEngine 的对象字段。全程序中大量类都在虚表 `+0x28` 使用各自的方法，不能仅凭槽位相同把候选命名为 NOS。
- 尚未确认原版 NOS 是调用上述任一刚体接口、修改发动机/传动扭矩，还是两者组合；也未通过实机验证在 `ActiveComponents` 调用前后哪个时点注入不会被同帧原生物理覆盖或重复积分。
- 因而不能用“原版氮气就是直接前向施力”作为该通道等同于原版 NOS 的依据。当前主包启用的
  刚体补速通道仍按严格验证的末级方案运行，不宣称是 NOS 复刻。

### Fail-closed 末级方案

在逐轮 `D8` 前轴横向力和 `GetDriveTorque()` 正向总扭矩两条原生路径尚未可靠落地的
情况下，0.9.15 已启用一个与已经验收的转向、yaw 和镜头控制隔离的
`forward-body-acceleration` 末级通道，并满足以下边界：

1. 只在唯一玩家、稳定车辆代次、稳定物理线程、有效 gameplay tick 和有效漂移会话同时成立时运行。菜单、加载、换车、重生、退赛、玩家身份变化或能力链失效时，当帧输出必须为零并清空爬升状态。
2. 必须有正油门、有限且合理的 `deltaTime`、至少一轮着地，并且 `dot(linearVelocity, normalizedForward) > 0`；倒车、空中、近乎静止或无法取得可靠接地状态时不施力。
3. 每个物理 tick 重新通过已验证虚函数取得并归一化车头前向向量，并用本帧实际前轮转角计算最终方向：前轮正打、顺打或零度沿车头，反打时只取实际前轮反打角的 `12.5%`（旧 half-angle 中间夹角偏转的 25%）。不硬读或缓存上一辆车的姿态矩阵。任一分量非有限、长度过小、刚体虚表不匹配或身份复核失败时立即归零。
4. 第一选择可通过 slot 35 的 `ResolveForce(normalizedForward * forceMagnitude)` 交给游戏原生积分器；若该路径的调用时序无法稳定验证，末级选择才是 slot 39 的 `Accelerate(normalizedForward, deltaV)`。后者会直接增加世界线速度，调用方必须显式计算 `deltaV = targetAcceleration * clampedDt * ramp`，绝不能把牛顿力值传给 `amount`。两个入口都必须先逐帧复核虚表槽、函数地址和刚体身份；不得直接写累计 force 或 linearVelocity 字段。
5. `ramp` 必须按 `deltaTime` 独立爬升和回落，退出速度快于进入速度；整车线速度模长接近上限时做连续软衰减，并设置绝对速度、目标加速度、单 tick `deltaV` 和累计纵向增速四重硬上限。异常质量、异常帧时长或 `Volatile+0x5D != 0` 时直接拒绝。
6. 0.9.15 发布构建的刚体 writer 名义满幅目标为 `5.25 m/s²`（相对已验证的 5.0 m/s² 基线提高 5%）；
在此前 `0.80` 发布倍率上先相对回调 10%，再额外相对降低 15%，发布输出统一乘以 `0.612`，低速实际满幅目标为
`3.213 m/s²`。调用前后读取线速度并计算 `actual_delta`；只有其最终收窄后的反打方向投影和总量与请求的 `deltaV` 匹配、正交残差
近零时才算成功。无变化、增量不足/过强或方向污染会永久关闭本次进程的 writer。通用源码构建
仍可将两个刚体开关保持关闭。旧 0.9.11 的 47 次 `write_ok=1` 只证明旧式调用后读取未异常，
不能证明净速度增量保留。
名义满幅目标还按整车线速度模长衰减：`<=70 km/h` 为 100%，`125 km/h` 为 50%，
`150 km/h` 为 15%，`>=170 km/h` 为 0%；70-125、125-150、150-170 km/h 区间线性插值，
再统一乘以 0.612 的发布强度系数（实际相对名义目标为 61.2%、30.6%、9.18%、0%）。
7. 此方案对整车质心施力，无法保留前驱、后驱、四驱的驱动轮分配，也绕过差速器、轮胎滑移、扭矩转向和轮胎纵向抓地限制。它只能提供可控的纵向补速，不能满足“给实际驱动轮加动力”的完整需求，所以优先级永久低于 `GetDriveTorque()` 路线。

## 0.9.15 只读探针

0.9.15 功能桥已经集成默认开启、完全不改值的低开销探针，不再发布一个与功能版互斥的诊断 ASI：

1. Hook `0x0069DA90`，原函数恰好调用一次且返回值原样返回。采集五个参数、调用前后 `D4/D8/DC/0x110`、返回值、轮对象指针、轮索引和物理序号。
2. `0x006A0580` 的四字节入口只先进入待确认状态；首次从当前玩家 `PVehicle+0xFC` 取得 transmission，并确认其只读虚表 slot 8 精确指回该入口后，才安装 Hook。Hook 只采集 `transmission+0x34` 与总驱动扭矩，原样返回结果。
3. `ActiveComponentsDetour` 在原物理调用前发布当前玩家身份、车辆代次、四轮指针、传动对象和本帧采样窗口，原调用返回后立即关闭窗口并低频输出聚合日志。
4. 高频 Detour 内不得加锁、直接写日志、动态分配、调用 `VirtualQuery` 或写入游戏状态；只允许固定指针比较和原子快照。
5. 只采样当前玩家车辆、当前已验证物理线程、有效车辆代次和有效物理帧。身份或生命周期任何一项不确定时跳过该帧。
6. 任一探针安装或校验失败时只关闭该通道，不影响已经验收的自动反打、人工 yaw、转向响应和镜头功能。不完整或重复的四轮指针表会整路拒绝；换车时玩家、刚体 holder/inner、悬挂、传动或任一轮对象变化都会推进车辆代次。

轮胎入口当前可使用以下较长签名，并继续结合目标文件指纹和唯一调用点验证：

```text
83 EC 24 56 8B F1 8B 86 0C 01 00 00 8B 8E 04 01 00 00 8B 49 08 D9 44 81 08
```

扭矩 getter 只有 `D9 41 34 C3` 四字节，不能单独扫描这个模式。必须在完整 PE 指纹匹配后，从已验证的 transmission 只读虚表确认 slot 8 精确指向 `0x006A0580`。

## 0.9.4 实机日志结论

2026-09-04 回传的 0.9.4 日志没有采到任何轮胎或传动对象指针。启动时已明确记录 `tire-force` 在 profile 签名或唯一调用契约处失败，`drive-torque` 在入口签名处失败；因此后续遥测中的 `front_grip=1.000 rear_drive=1.000` 只表示 writer 保持禁用，不是探针读到的物理值。

同一受支持的磁盘 `speed.exe` 中，轮胎入口、唯一 `E8 93 31 FF FF` 调用点和扭矩 getter 的 `D9 41 34 C3` 都与 profile 匹配。这只能证明目标文件正确，不能证明运行时入口仍保持原始字节；本日志中在本插件初始化前还有其他 Hook 的失败行，但现有证据不足以归责给某个具体插件。

0.9.15 因此不放宽 Hook 校验，而是将失败拆分为文本段、入口地址、入口唯一性、调用点地址、调用点唯一性、`E8 rel32` 解码和目标匹配等阶段。每条失败行都会输出 `stage` 和 `reason`，并视阶段附上 `expected_rva`、`expected_address`、`expected_aob`、`matches`、`expected_found`、`first_match`、`call_site`、`expected_target`、`decoded_target` 之一，以及有界的入口或调用点 `actual_bytes`。下一份实机日志才能区分是入口被其他 Hook 改写、调用点被改写，还是运行时映像与磁盘文件不一致。

## 轮序与实机矩阵

SDK 枚举声明顺序为 `FL, FR, RL, RR`，即 `0, 1, 2, 3`；当前适配器资料却记录目标二进制顺序为 `FL, FR, RR, RL`，并使用 `{0, 1, 3, 2}` 转换。两份资料只能共同证明 `0/1` 是前轴、`2/3` 是后轴，尚不能仅凭静态资料确认后轮左右顺序。

正式写入前至少需要分别用一辆前驱、后驱、四驱和一辆高重心车辆完成以下验证：

1. 用 `GetWheelLocalPos` 横向坐标确认左右轮序，用前轮转向响应交叉确认 `0/1`。
2. 对比各驱动形式的总扭矩与逐轮纵向力变化，证明原生动力分配未被插件重排。
3. 对比正常转向、漂移、单轮离地和碰撞时的 `D8/DC`，确认横纵字段、符号和调用时序。
4. 换车、重生、退赛和返回主菜单后确认探针立即切换代次，不采集或写回旧车辆对象。
5. 高重心车辆逐级测试小倍率前轮侧向抓地，记录侧倾与离地情况；出现明显抬轮或侧翻趋势即停止上调。

## 启用 writer 的门槛

只有上述日志证明轮序、`D8/DC` 语义、getter 时序、原生驱动形式分配和换车生命周期全部成立，才可以加入默认关闭的小倍率实验 writer。writer 还必须具备每帧对象复核、倍率硬限、只处理正向扭矩、身份失效即跳过，以及不保留跨帧全局修改的恢复边界。

在这些证据齐全前，0.9.15-alpha 只宣称刚体补速通道已按上述边界生效；不宣称前轮抓地
或驱动轮动力倍率已经生效。轮胎侧向力和驱动扭矩仍是只读探针，writer 保持关闭。
