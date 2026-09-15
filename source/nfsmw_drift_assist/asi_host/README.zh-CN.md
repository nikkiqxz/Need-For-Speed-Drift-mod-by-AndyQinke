# Reforged x86 ASI 宿主说明（1.0.6）

> 当前唯一发布入口是顶层 `outputs/nfsmw_drift_assist/CMakeLists.txt` 配合
> `work/configure_build_v106_x86.cmd`。它生成并打包
> `Slippery_Drifting_FlashFish_by_AndyQinke.asi`。本目录下的旧独立脚手架只保留作
> 历史参考，不要用它生成发布文件。

发布版只接受经过 `NFSMWMultiGear` 签章的
由 MultiGear 签章的 `speed.exe`，并在首次通过完整文件/footer 验证后创建
`SCRIPTS\Slippery_Drifting_FlashFish_by_AndyQinke.device.json`。普通 `speed.exe`
即使改名也会被拒绝；ASI 文件名改成其他名称同样会被入口拒绝。

本目录提供加载入口、目标版本闸门和配置读取。默认构建不安装任何游戏 Hook，也不
写入输入、刚体、轮胎或动力总成；可显式开启只读输入、车辆、协调器或物理阶段诊断
Hook，用来确认目标版本的动作轮询 ABI、活动车辆对象、运行时字段和角速度更新阶段。
四种诊断强制互斥，均不会接入漂移控制器或修改车辆行为。

刚体前向加速在通用源码构建中默认关闭。`NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=ON`
会在 alpha bridge 中启用只读 dry-run 规划器；它只记录经过身份、物理节拍、接地、油门、
车头方向和速度上限检查后本来会提交的 `deltaV`，不会调用游戏写接口。将
`NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=ON` 后才会尝试调用已核验的
`IRigidBody::Accelerate`（slot 39，RVA `0x00299E10`）。当前发布构建以 `5.25 m/s²`
为满幅目标（在已验证的 5.0 m/s² 基线上提高 5%），并立即核对写入前后速度、前向投影、
总增量和正交残差；空操作或任一检查失败
都会永久关闭刚体 writer。该通道是整车质心的纵向补速，不是前轮/后轮/四驱的驱动轮动力分配，也不是轮胎
抓地力修改。本次 0.9.13 发布构建明确将上述两个开关设为 `ON`；其他通用构建仍可保持默认
`OFF`。

满幅目标还会按整车线速度模长衰减：`<=70 km/h` 为 100%，`125 km/h` 为 50%，
`150 km/h` 为 15%，`>=170 km/h` 为 0%；70-125、125-150、150-170 km/h 三段均为线性插值。

0.9.18 还读取玩家 `PVehicle` 内嵌 `Attrib::Instance` 的 Collection 键，以 INI 的
`[SmartCountersteerVehicleMultipliers]` 为每辆 racer 缩放 55 度基础反打目标。连续按住
手刹满 0.20 秒时，同一刚体通道优先沿实际速度反方向减速；减速从 100 km/h 的最大值
15% 线性增至 180 km/h 的 100%，最大值为最大有效补速的 1.15 倍。

0.9.19 新增独立驾驶辅助状态机：漂移时立即暂停，退出 0.20 秒后恢复。ABS 复用刚体
减速 writer；ESC 复用已验证的角速度 setter，但拥有单独的故障隔离；TCS 只发布 HUD
状态。0.9.20 的 D3D9 EndScene HUD 会验证 Hook 写入结果，用单批预变换顶点绘制三格
和固定位图字形，不依赖 D3DX；安装成功和首帧绘制都有独立日志，绘制失败不会影响漂移
或驾驶物理控制。已激活的手刹漂移还加入 3 秒接地不足容错，接地恢复立即清零计时。

0.9.21 针对 DXVK 和多 HUD 插件环境，把最终显示优先移到 `Present` 前，并保留
`EndScene` 回退。专用日志写入 ASI 同目录的
`Slippery_Drifting_FlashFish_by_AndyQinke.hud.log`。

0.9.22 根据实机日志进一步把 Hook 从设备 VTable 替换改为 DXVK 实际函数入口 detour，
以兼容游戏预先缓存函数地址的调用方式。日志会分别报告函数 Hook 和 detour 首次进入。

1.0.2 保留 LC 对 5 km/h HUD 门槛的绕过，更新七挡 TCS 概率，并让 ESC 第二部分在
既有渐入和逐帧限幅内提高 15% 基础权限，再随 10 至 35 度车身偏航平滑增至 2 倍。

1.0.4 保留驾驶辅助 HUD 自己的三条规则：普通前进速度必须超过 5 km/h、倒车隐藏、
LC 激活时允许静止或低速显示。D3D9 绘制入口同时读取原版
`CHudWidgetArray_FlipVisibilityOnGameMasterChange` 使用的
`CFEManager -> game state -> +0x34` 主开关。只有本地三规则与原版 HUD 主状态都允许时
才绘制面板；遥测可用状态仍用于防止菜单或换车期间显示缓存状态。

## 构建

在 Visual Studio 的 x86 Native Tools 命令行中，从本目录的上一级执行：

```powershell
cmake -S . -B build-asi -G 'Visual Studio 17 2022' -A Win32 `
  -DBUILD_TESTING=OFF `
  -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_SDK_SOURCE_DIR='C:/path/to/nfsmw-2005-sdk'
cmake --build build-asi --config Release --target nfsmw_drift_assist_asi
```

默认 `NFSMW_ENABLE_INPUT_DIAGNOSTIC=OFF`、`NFSMW_ENABLE_VEHICLE_DIAGNOSTIC=OFF`、
`NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=OFF` 和 `NFSMW_ENABLE_PHASE_DIAGNOSTIC=OFF`，
因此上面的构建不会安装诊断 Hook。
需要做只读输入采样时，必须在全新的构建目录显式打开它，并同时提供已经针对目标
`speed.exe` 核验过的唯一签名。当前工作区测得的输入轮询函数为 RVA
`0x002349B0`（VA `0x006349B0`），签名如下：

```powershell
cmake -S . -B build-input-diagnostic -G 'Visual Studio 17 2022' -A Win32 `
  -DBUILD_TESTING=OFF `
  -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_INPUT_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x002349B0 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=83 EC 0C 55 56 8B F1 8B 46 24 33 ED 3B C5 89 74 24 0C 0F 85" `
  -DNFSMW_SDK_SOURCE_DIR='C:/path/to/nfsmw-2005-sdk'
cmake --build build-input-diagnostic --config Release --target nfsmw_drift_assist_asi
```

诊断 Hook 只在 PE/profile 校验通过且签名在整个映像中**恰好命中一次**、并且命中
配置的 RVA 时安装。目标函数是一个无栈参数的成员函数：对象指针由 ECX 传入；宿主
使用 `void __fastcall(void* self, void* unusedEdx)` 桥接并链回原函数。原函数返回后，
诊断代码只读检查 `self`、虚表槽位 4（第 5 项）、`self+0x20` 和完整的 76 个 `float` 动作槽；
每 250 毫秒最多输出一条 `OutputDebugStringA`。目标 EXE 的静态模板名称和实机按键
校准共同确认：`r0=GAS`、`r1=BRAKE`、`r2=STEERLEFT`、
`r3=STEERRIGHT`、`r4=HANDBRAKE`、`r5=GAMEBREAKER`、`r6=NOS`、
`r7=SHIFTDOWN`、`r8=SHIFTUP`、`r9=RESET`。它不会写入动作输出缓冲区、控制器或
车辆物理。指针、数值或 ABI 校验
失败时只跳过本次采样。Hook 会先在禁用状态创建并发布原函数跳板，再排队启用；启用前
失败会移除禁用 Hook，尝试启用后的失败则保留跳板并永久透明链回，不会漏掉原输入轮询，
也不会在可能存在在途调用时释放跳板。后一种极端失败状态会让 ASI 保持驻留但不采样，
避免加载器因返回失败而卸载仍可能被调用的代码。

## 只读车辆诊断（可选）

需要检查活动车辆对象时，应使用与输入诊断**分开的全新构建目录**，打开
`NFSMW_ENABLE_VEHICLE_DIAGNOSTIC=ON`。当前 `speed.exe` 上已核验的入口是
`ActiveComponents_TickAll`，RVA `0x000BA940`（VA `0x004BA940`），唯一签名为：

```text
A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24
```

在 Developer PowerShell 中执行（先按上面的构建段落设置 `$src` 和 `$sdk`）：

```powershell
$vehicleBuild = 'C:\path\to\vehicle_diagnostic_x86'
cmake -S $src -B $vehicleBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_VEHICLE_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x000BA940 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $vehicleBuild --target nfsmw_drift_assist_asi
```

该入口的 ABI 是 `void __cdecl(float dt)`。宿主先调用原函数，再只读检查
`0x00913E74` 的活动数组和 `0x00913E7C` 的数量，最多扫描 64 项。每个条目只沿
`entry+0x08` 读取候选车身，验证主/次虚表、`body+0x98` 子物理指针及其虚表，最后
记录 `body+0x100..0x140` 的有限浮点原始值。同时，它会独立扫描 SDK 记录的
`PVehicle::g_mInstances @ 0x009352B0`（每项 8 字节，最多 64 项），只读记录
`PVehicle` 的 `mRigidBody(+0x78)`、`mPlayer(+0x84)`、`mInput(+0xE8)`、
`mSuspension(+0xF0)`、`mSpeed(+0x11C)`、`mSlipAngle(+0x12C)`、
`mWheelsOnGround(+0x130)` 和 `mLocalVel(+0x134)`。日志每 500 毫秒最多输出一批，
已知 `0x008AC06C` 标为 `player-candidate`，`0x008AC0FC` 标为 `ai-candidate`；
这些只是候选分类，不会调用任何游戏虚函数，也不会写入车辆、输入或物理字段。
所有读取都有提交页、映像范围、有限值和 MSVC SEH 检查，失败的条目只被跳过。

典型日志顺序：

```text
profile accepted
read-only vehicle diagnostic installed at 0x004BA940 (cdecl float)
vehicle diagnostic dt=... entries=... count=...
vehicle[0] kind=player-candidate entry=... body=... vtbl=... sub=... raw100..11c=[...]
vehicle[0] raw120..140=[...]
vehicle diagnostic valid=... invalid=... emitted=...
pvehicle[0] kind=player-candidate pv=... vtbl=... rb=... input=... suspension=... speed=... slip=... grounded=...
pvehicle diagnostic occupied=... valid=... invalid=... emitted=...
```

该版本只是对象布局探针，不会产生漂移效果；输入诊断和车辆诊断必须分开编译，不能
同时打开两个选项。

## 只读物理协调器诊断（可选）

若要观察主线程物理协调器，请使用第三个全新的构建目录，打开
`NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=ON`。当前目标入口为
`WorldPhysicsDispatch_MainThreadCoordinator`，RVA `0x0035AAD0`（VA `0x0075AAD0`），
唯一签名为：

```text
8B 0D C8 85 98 00 85 C9 C7 05 90 32 90 00 00 00 00 00 74 05 8B 01 FF 50 44
```

在 Developer PowerShell 中执行（先按上面的构建段落设置 `$src` 和 `$sdk`）：

```powershell
$coordinatorBuild = 'C:\path\to\coordinator_diagnostic_x86'
cmake -S $src -B $coordinatorBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x0035AAD0 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=8B 0D C8 85 98 00 85 C9 C7 05 90 32 90 00 00 00 00 00 74 05 8B 01 FF 50 44" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $coordinatorBuild --target nfsmw_drift_assist_asi
```

Hook 的 ABI 是 `void __cdecl()`，原函数始终先后完整链回。探针只读记录
`DAT_009885C8` 的全局值、该值指向的 `+0x48` 子对象、子对象运行时虚表和第 17 项
（字节偏移 `+0x44`），并从 `子对象-0x48` 读取完整对象主虚表。日志中的 `slot17`
会显示实际函数地址、RVA 及是否位于游戏映像可执行区；未知虚函数不会被调用，也不会
写入全局、虚表、对象或物理字段。典型日志：

```text
read-only coordinator diagnostic installed at 0x0075AAD0 (cdecl no-args; slot17 is observed only)
coordinator diagnostic calls=... freq=...Hz global=... value=... full=... subVtable=... primaryVtable=... slot17=... rva=... text=... valid=...
```

输入、车辆和协调器诊断使用不同锚点，必须分别构建；同一时间只将一个诊断 `.asi`
放入游戏 `scripts`。这个构建仍不会产生漂移效果。

## 只读物理阶段/时序诊断（可选）

`NFSMW_ENABLE_PHASE_DIAGNOSTIC=ON` 是一个内部包含两个 Hook、对外仍与其他诊断互斥的
只读构建。它同时验证并 Hook `ActiveComponents_TickAll @ 0x004BA940` 和
`FUN_006E7A00 @ 0x006E7A00`，在两处原函数前后记录同一玩家刚体的角速度三分量。
第二锚点必须通过以下额外 CMake 参数提供：

```text
NFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA=0x002E7A00
NFSMW_DRIFT_PHASE_WORLD_SIGNATURE=56 8B 74 24 08 56 E8 E5 33 07 00 56 E8 9F 4A 07 00 83 C4 08 56 B9 C0 66 91 00
```

玩家车要求 `mPlayer(+0x84)` 非空且唯一，`mRigidBody(+0x78)` 的虚表必须是
`0x008AC880`。实际角速度地址按 `holder=*(rb+0x30)`、`inner=*holder`、
`omega=inner+0x30` 逐级验证后读取；`rb+0x30` 本身不是 `vec3`。日志每 250 毫秒输出
一组四阶段配对，未选中的调用直接完整链回。任一签名、`.text`、布局或 Hook 安装检查
失败都会让诊断保持关闭。两个原函数跳板会在排队启用前同时发布；启用后的失败路径即使
无法完成禁用回滚，也会保留跳板并永久透明链回，不会在可能存在在途调用时释放它们。
代码不调用任何游戏虚函数，也不写任何游戏值。完整构建命令和日志字段见上一级
[BUILD_ASI.zh-CN.md](../BUILD_ASI.zh-CN.md)。成功构建会额外输出
`nfsmw_drift_assist_phase_timing_diag.asi`。

输出目录会同时出现：

- `nfsmw_drift_assist_asi.dll`：原生 DLL/BepInEx NativeBootstrap 形式；
- `nfsmw_drift_assist_asi.asi`：同一 PE 文件的 ASI Loader 形式。

SDK 入口来自 `nfsmw-2005-sdk/src/entry.c`。其 `DllMain` 延迟启动 worker，
并导出 `BepInExNativePlugin_Load`；不要再把第二份入口源加入目标。

## 目标闸门

默认 profile 对齐本工作区实测目标：PE32/i386、映像基址 `0x00400000`、
入口 RVA `0x003C4040`、`SizeOfImage=0x00693000`，以及磁盘文件大小
`0x005DA000`（6,135,808 bytes）。该文件的人工审计记录还包括 PE 时间戳
`0x438E4C8C` 和 SHA-256
`36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9`。
此外，
`NFSMW_DRIFT_REQUIRED_SIGNATURE_RVA` 与
`NFSMW_DRIFT_REQUIRED_SIGNATURE` 默认为空/零，因而插件会明确拒绝启动并且
不会写入游戏。只有在合法的目标 `speed.exe` 上测得唯一的代码锚点后，才在
CMake 配置时提供这两个值，例如：

```powershell
-DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x00123456 `
"-DNFSMW_DRIFT_REQUIRED_SIGNATURE=55 8B EC 83 EC ??"
```

若 exe 被打补丁、换语言或版本不同，应同时替换全部 profile 字段，并重新
验证锚点唯一性；不能只改文件名或映像基址。签名重复、偏移变化、PE 字段
不匹配、模块不是 `speed.exe` 时，入口返回 `NFSMW_FAIL`，不安装任何控制器。

profile 和锚点通过后，宿主会从自身 DLL/ASI 所在目录读取 `drift_assist.ini`，调用
`ParseAssistConfigIni()`；文件缺失或解析失败会把 `enabled` 置为 `false`。默认宿主和
只读诊断不会改变游戏行为；只有显式启用 `NFSMW_ENABLE_ALPHA_BRIDGE`，并同时验证输入、
物理阶段和角速度 setter 三个锚点后，才安装 0.9.13 功能桥。镜头 Hook 在稳定观察完成后
另行延迟安装；镜头链校验失败只关闭镜头辅助，不让车辆控制功能桥整体失败。

诊断日志需要使用 DebugView（Sysinternals）或调试器查看。正常的成功顺序类似：

```text
profile accepted
read-only input diagnostic installed at 0x006349B0 (ECX/fastcall bridge)
input diagnostic self=... mirror=... rows[0..9] ...
```

如果日志只出现 `read-only input diagnostic installed`，但没有后续 `input diagnostic
self=...`，说明游戏尚未调用输入轮询，或运行时对象校验没有通过；这时不要把地址
改成别的虚表项，也不要关闭校验来强行读取。进入单机比赛、等待车辆真正出现后再
按方向和手刹键采样。

若看到 `target rejected`、`signature is missing, shifted, or duplicated` 或
`MinHook installation failed`，不要继续安装其他车辆 Hook；保留日志并关闭该选项。

## 0.9.13 功能桥边界

0.9.13-alpha 已在当前受支持 profile 上完成玩家身份和生命周期验证、输入轮询后的动作
采样、车身状态采样，以及人工 yaw 的受限写回。车头相对行进方向首次达到 `15°` 且
方向中立时，智能反打固定请求 `55°`，按 `60°` 最大转向角换算为约 `0.9167` 的归一化
命令；自动反打接管后保持到约 `5°` 才释放。首次达到门槛前，`waiting-angle` 中立输入
真实透传并清除起飘顺打残值，首个自动帧从实际零点直接朝正确反打侧移动。任意方向的有效
人工输入只要超过死区，就会在当前轮询停止自动覆盖并立即交还玩家；与自动反打方向一致的
输入也不例外。每个新人工目标从当前已应用值按固定 `2.0 command/s` 连续到达完整目标，
55° 继续补到 60° 约需 0.042 秒，满左到满右约需 1 秒。人工松手后的 `0.20` 秒保持最后实际命令，不向零
移动；随后自动反打从该实值按 0.9.5 的 `2.0 command/s` 路径接续。原始人工输入仍直接用于方向判断、yaw 意图、移交和
`0.20` 秒重接管计时，不会因最终前轮命令过渡而延迟识别。

人工 yaw 只在当前存在人工方向时生成：同向输入只补足到 `0.5625 rad/s`，加速度
`3 rad/s²`、单次增量上限 `0.15 rad/s`，自然旋转已经更快时不减速，权限在连续 2 秒内
从 15% 线性升到 100%；反向输入主动朝 0 度回正，最大 `0.45 rad/s`、加速度
`4.5 rad/s²`、单次增量上限 `0.15 rad/s`，并按刹停距离、单帧距离和 ±2 度零区限速；
`35°` 外不继续向外补转。方向中立、自动反打、`0.20` 秒重接管等待和换边观察期间都
保持 `yaw_mode=none`，不排队 yaw 命令。

旧侧已经达到 15 度后，反向输入把车身带入 ±2 度中心区会进入 `SideTransition`。
接下来的 0.50 秒内旧侧自动轨迹立即清除且没有 yaw 写入；有效人工方向继续走固定 `2.0 command/s`
路径，中立输入透传。相反侧达到 6 度即确认
新侧，不要求再次拉手刹，并从 15% 重新开始同向力度爬升。观察窗超时、车身回到旧侧
6 度或玩家重新输入旧侧方向，都会清空本次会话。

进程首次建立合格车辆与稳定物理节拍时，需要从最后一次失败开始连续取得 60 个有效、
独立的物理帧。首次稳定期完成后，换车、身份变化或节拍失稳恢复只需从最后一次失败开始
连续取得 3 个有效独立物理帧；期间方向、yaw 和镜头漂移目标全部关闭。短暂暂停同样立即
撤销输出并清空控制器会话，但会保留已经验证过的稳定玩家身份，下一份有效样本会重新验证
归属并走快速恢复路径，不再重新等待完整 60 帧。

镜头目标使用修正后的反向符号 `-driftSide * 3°`。从无偏转进入一侧使用 2.0 秒
smoothstep 曲线完成；退出后归中使用更快的 1.25 秒。折身进入
`SideTransition` 后立即采用待确认的新侧作为目标，并在 1.25 秒内从当前侧平滑移动到
另一侧。镜头 Hook 只接受游戏直接调用，或 `NFSMWOrbitCamera.asi` 中经唯一返回模式验证
的调用点；既有 LookAt 入口只接受原版签名，
或由 `NFS.CameraMod.asi` 所有且落在可执行段内的跳转链。调用所有者、入口、唯一性或安装
校验失败时只关闭镜头辅助，智能反打与人工 yaw 继续工作。

功能桥查找玩家车辆时扫描 `PVehicle::g_mInstances` 的全部 64 个槽位，把中间空槽当作
正常孔洞而不是数组结尾。先读取实例项 `mIsEnabled`，禁用槽不参与候选；完全相同的
`(PVehicle, Player, Simable)` 重复槽只计一个不同身份。多个非空 `mPlayer` 不再直接导致拒绝：候选还必须满足 SDK
对象状态前提，并通过 `ISimable::GetPlayer()`、`IsPlayer()`、`IsOwnedByPlayer()`，还要求
对应 `IPlayer::GetSimable()` 反向指回当前候选 `ISimable`；只有唯一合格对象才允许写入。
真正存在两个不同且同时合格的启用对象时仍停止写入。物理阶段在主菜单/加载期间不推进时
也保持关闭；恢复推进后只需连续 3 个有效物理帧，不再因禁用旧车或重复槽额外等待。
这个修复只属于功能桥，不能据此推断旧车辆/getter 诊断也采用相同筛选规则。

宿主使用经签名与虚表槽双重验证的 `IRigidBody::SetAngularVelocity`：RVA
`0x00296FF0`、虚表槽 25，签名为：

```text
8B 41 30 8B 08 8B 54 24 04 8B 02 83 C1 30 89 01 8B 42 04 89 41 04 8B 52 08 89 51 08 C2 04 00
```

输入阶段生成的人工 yaw 命令只能由下一物理序号消费，即
`sourcePhysicsSerial + 1 == sinkPhysicsSerial`。`ActiveComponents_TickAll` 原函数返回后
才执行 setter；每条命令最多消费一次。过期、跨线程、玩家/刚体身份变化、模式变化或
资格失效时直接丢弃。setter 调用或读回失败会永久关闭人工 yaw 通道，但智能反打继续
工作。成功日志格式为：

```text
alpha yaw_apply mode=... source=... sink=... age_ms=... offset_deg=... relative_rate=... target=... delta=... yaw_before=... yaw_after=... identity_ok=1 setter_ok=1
```

镜头成功安装日志格式为：

```text
alpha camera assist installed look_at=... caller=... mode=direct|orbit-compatible look_at_chain=direct|camera-mod angle=3.0deg direction=inverted entry=2.0s-smoothstep return_switch=1.25s-smoothstep
```

若出现 `alpha camera assist disabled ...` 或 `alpha camera assist disarmed ...`，只将镜头项
判为失败；除非另有车辆控制错误，方向和 yaw 仍可继续测试。

`yaw_mode` 只使用 `none`、`manual-same-assist` 和 `manual-opposite-recovery`。本版仍不
启用起飘强推、保角、无人为输入的自动回正、前轮抓地或驱动轮动力写入。截图中的全局浮点
常量没有每车归属和调用时序保证，不能替代上述刚体 setter。`nfsmw_sdk_adapter_example.cpp`
只能作为通用接口约定参考，不能把其他 SDK 的多重继承偏移直接套进当前 MinGW 宿主或
其他 `speed.exe` 版本。

静态资料已经确认 `0/1` 是前轴、`2/3` 是后轴。0.9.13-alpha 已在功能桥内集成独立失败
隔离的只读探针：轮胎通道以唯一长签名和唯一调用点验证 `0x0069DA90`，按玩家完整对象链
稀疏采集四轮 `D4/D8/DC/0x110` 与函数参数/返回值；扭矩通道只有在当前玩家 transmission
的只读虚表 slot 8 精确指向 `0x006A0580` 后才安装，并原样返回总扭矩。任一通道失败都不
关闭方向、yaw 或镜头。后轮左右顺序、字段语义和调用时序仍需实机日志确认，在此之前不得
启用前轮侧向抓地 writer。驱动轮动力未来只放大玩家车辆的正向总驱动扭矩，再由原生传动
系统分配至前驱、后驱或四驱车的实际驱动轮；零/负扭矩、发动机制动和倒车不增强。
0.9.13-alpha 中轮胎侧向力和驱动扭矩这两个物理 writer 仍保持关闭；刚体前向补速 writer
已单独通过开关启用，并受本页前述严格验证保护。
