# 编译 Reforged 1.0.6 与 ASI

## 当前发布路径（1.0.6）

当前可直接使用的发布产物是根目录打包脚本生成的
`Slippery_Drifting_FlashFish_by_AndyQinke.asi`，不是本页旧版本示例中的
`nfsmw_drift_assist_alpha.asi`。它必须与同包的 `drift_assist.ini` 一起放在游戏的
`SCRIPTS`/`scripts` 目录；不要把 `.dll`、旧版 `.asi` 或刚体 companion 同时放入目录。

本版只接受经过 `NFSMWMultiGear` 签章的
由 MultiGear 签章并命名为 `speed.exe` 的主程序：文件名、PE32/基址、完整文件大小与哈希、64 字节
`NFSMWRF1` footer 以及小端验证码 `868086` 必须全部匹配。单纯把普通
`speed.exe` 改名不会通过验证，也不会创建设备绑定文件或安装 Hook。

推荐在 x86 Developer PowerShell 中从工作区根目录执行：

```powershell
cmd /c work\build_and_package_v106.cmd
```

脚本会使用已配置的 Reforged 可执行文件完成 x86 Release 构建、11 项测试和发布压缩包
生成；它不会自动复制文件到游戏目录。首次通过启动验证后，插件才会在游戏目录的
`SCRIPTS` 文件夹创建 `Slippery_Drifting_FlashFish_by_AndyQinke.device.json`，其
DPAPI 密文以大写十六进制写入 JSON。设备不匹配或文件被篡改时会拒绝覆盖旧绑定。

下方保留的是早期 Alpha/诊断构建的历史说明。它们中的旧文件名、旧版本号和
`speed.exe` 示例不适用于当前 0.9.16 发布包；实测请以本节和包内
`README.zh-CN.md`、`ALPHA_TEST.zh-CN.md` 为准。

## 历史 Alpha 说明（仅供源码维护）

当前源码同时保留默认故障关闭宿主、四种只读诊断和有界功能桥。只有显式开启
`NFSMW_ENABLE_ALPHA_BRIDGE=ON`，并为受支持的 `speed.exe` 提供已经核验的输入、
物理阶段和 `IRigidBody::SetAngularVelocity` 三个签名，才会生成可实测的
`nfsmw_drift_assist_alpha.asi`。功能桥与所有诊断
选项强制互斥，不能放进同一个构建。镜头 Hook 会在稳定观察完成后延迟安装；镜头调用链
验证或安装失败只关闭镜头辅助，不会让已经通过硬门槛的方向与 yaw 功能桥整体失败。

Alpha 0.9.13 使用 `actuation=SteeringAndAttitude`。按下手刹会立即建立辅助会话，但插件
不会施加起飘强推、固定角度保持或无人为输入的自动回正。只有当前存在人工方向输入时才
允许有界 yaw：同向输入只补足到 `0.5625 rad/s`，加速度 `3 rad/s²`、单次增量上限
`0.15 rad/s`，权限在连续 2 秒内从 15% 线性升至 100%；反向输入主动朝 0 度回正，
最高 `0.45 rad/s`、加速度 `4.5 rad/s²`、单次增量上限 `0.15 rad/s`，并受刹停距离、
单帧距离和 ±2 度零区限制。相对漂移角达到 `35°` 后不再继续向外补转。方向中立、自动
反打、重接管等待和换边观察窗期间均不写 yaw。

车头相对实际行进方向首次达到 `15°` 且玩家方向中立时，插件立即请求固定 `55°`
反打，按 `60°` 最大转向角标定得到约 `0.9167` 的归一化目标。首次达到门槛前，
`waiting-angle` 的中立输入真实透传 `0` 并清除起飘顺打残值；自动反打从该实际值沿
0.9.5 的固定 `2.0 command/s` 路径直接朝正确侧接管。自动反打已接管时，任意方向的有效
人工输入超过死区都会在当前轮询停止自动覆盖，并立即取得控制目标；每个新人工目标从当前实际
命令以固定 `2.0 command/s` 按剩余距离到达完整行程，不限制最终角度；55° 继续补到 60°
约需 0.042 秒，满左到满右约需 1 秒。`manualSteeringTransitionSeconds` 仅为旧配置兼容键。
人工松手后的 `0.20` 秒 `reengage-delay` 保持最后实际命令，不向零移动；恢复时自动反打
从该命令连续接续。遥测使用 `steer_mode` 的 `passthrough`、`waiting-angle`、`auto-countersteer`、
`manual-override`、`reengage-delay` 五种有效值；兼容枚举 `countersteer-handoff-delay`
不再输出，`handoff_pending` 与 `handoff_s` 恒为零。`neutral_s` 和 `auto_target_deg` 描述
当前重接管与自动目标状态。旧侧曾达到 15 度后，反向输入过中心会进入 0.50 秒
`SideTransition`：立即清除旧侧自动轨迹且不写 yaw；有效人工方向仍走 `2.0 command/s` 路径，
中立输入透传。相反侧达到 6 度才确认新侧。具体实机步骤见
[ALPHA_TEST.zh-CN.md](ALPHA_TEST.zh-CN.md)。

CMake 构建、测试及发布打包只会写入指定的构建/输出目录，**不会自动复制或覆盖游戏
目录**。必须先完全退出游戏，再由测试者手动备份并替换 `scripts` 中的 Alpha ASI 与
INI；任何自动部署脚本都不属于本工程的构建流程。

## 给第一次操作的人

先记住一个关键区别：

- `.lib` 是给其他程序链接的代码库；
- `.dll`/`.asi` 才是能被 Loader 加载的插件；
- 当前目录既能编译控制核心，也提供可选的 x86 ASI 宿主。默认宿主只做版本闸门和
  INI 读取；未通过目标校验时不会安装任何游戏 Hook。功能 Alpha 必须额外显式选择
  `NFSMW_ENABLE_ALPHA_BRIDGE`。

你可以按下面五步完成 32 位核心测试和 Alpha ASI 构建。推荐脚本固定使用独立的
`work\core_v099_tests_x86` 和 `work\functional_alpha_v099_x86` 目录，不会复用旧版
CMake 缓存。每一步都在同一个 **Developer PowerShell for VS 2022** 窗口中执行。

### 第 1 步：打开正确的终端

在 Windows 开始菜单搜索并打开 **Developer PowerShell for VS 2022**。推荐脚本会再
调用 `vcvarsall.bat x86`，确保最终使用 32 位编译器。

输入下面两行：

```powershell
cl
cmake --version
```

只要能看到 MSVC 版本号和 `cmake version 3.x`，就可以继续。若提示“不是内部或外部命令”，说明打开的不是 VS 开发者终端，或 Visual Studio 没有安装“使用 C++ 的桌面开发”。

### 第 2 步：确认源码、SDK 和构建脚本

复制并执行：

```powershell
$taskRoot = 'C:\Users\wepie\Documents\Codex\2026-08-31\za'
$src = "$taskRoot\outputs\nfsmw_drift_assist"
$sdk = "$taskRoot\work\external\nfsmw-2005-sdk"
$script = "$taskRoot\work\configure_build_v099_x86.cmd"
Test-Path "$src\CMakeLists.txt"
Test-Path "$sdk\CMakeLists.txt"
Test-Path $script
```

三行都应显示 `True`。若有一行是 `False`，不要继续构建，先核对对应路径。

### 第 3 步：运行推荐构建脚本

执行：

```powershell
& $script
```

脚本会依次完成：配置并构建 0.9.13 功能 ASI、配置并构建 32 位控制核心、运行全部核心
测试。最后看到 `100% tests passed` 且命令返回提示符，才表示本次构建完整成功。

### 第 4 步：确认输出文件

执行：

```powershell
$coreBuild = "$taskRoot\work\core_v099_tests_x86"
$asiBuild = "$taskRoot\work\functional_alpha_v099_x86"
Get-ChildItem "$coreBuild\nfsmw_drift_assist_tests.exe", `
  "$coreBuild\nfsmw_drift_requirements_tests.exe", `
  "$coreBuild\nfsmw_drift_camera_tests.exe", `
  "$coreBuild\nfsmw_drift_steering_response_tests.exe", `
  "$asiBuild\nfsmw_drift_assist_asi.dll", `
  "$asiBuild\nfsmw_drift_assist_alpha.asi" | Select-Object Name,Length
```

预期文件在：

```text
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\core_v099_tests_x86\nfsmw_drift_assist_tests.exe
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\core_v099_tests_x86\nfsmw_drift_requirements_tests.exe
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\core_v099_tests_x86\nfsmw_drift_camera_tests.exe
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\core_v099_tests_x86\nfsmw_drift_steering_response_tests.exe
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\functional_alpha_v099_x86\nfsmw_drift_assist_asi.dll
C:\Users\wepie\Documents\Codex\2026-08-31\za\work\functional_alpha_v099_x86\nfsmw_drift_assist_alpha.asi
```

前四个文件是测试程序，不能放进游戏；最后一个 `nfsmw_drift_assist_alpha.asi` 才是
Alpha 功能插件。

运行时配置源文件是 `$src\drift_assist.alpha.ini`。CMake 不会把它自动复制到构建目录；
手动安装或整理发布目录时，应将它与 Alpha ASI 放在一起，并命名为 `drift_assist.ini`。

### 第 5 步：确认确实是 x86

仍在同一个终端执行：

```powershell
dumpbin /headers "$asiBuild\nfsmw_drift_assist_alpha.asi" | Select-String 'machine'
```

应看到 `14C machine (x86)`。如果看到 `8664 machine (x64)`，不要把产物放进游戏，先重新按 `Win32/x86` 工具链配置。

### 手动构建命令（脚本不可用时）

当前工程使用公开的 `s-b-repo/nfsmw-2005-sdk` 作为 ASI 入口和 MinHook
实现。工作区已经准备好一份 SDK，不需要再次下载。为避免旧的 CMake 缓存影响结果，
使用一个全新的构建目录，并在 **x86 Native Tools Command Prompt for VS 2022** 或已
执行过 `vcvarsall.bat x86` 的终端中逐行执行：

```powershell
$asiBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\functional_alpha_v099_x86_manual'
cmake -S $src -B $asiBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_ALPHA_BRIDGE=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x002349B0 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=83 EC 0C 55 56 8B F1 8B 46 24 33 ED 3B C5 89 74 24 0C 0F 85" `
  -DNFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA=0x000BA940 `
  "-DNFSMW_DRIFT_PHASE_WORLD_SIGNATURE=A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24" `
  -DNFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_RVA=0x00296FF0 `
  "-DNFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_SIGNATURE=8B 41 30 8B 08 8B 54 24 04 8B 02 83 C1 30 89 01 8B 42 04 89 41 04 8B 52 08 89 51 08 C2 04 00" `
  -DNFSMW_DRIFT_TIRE_FORCE_RVA=0x0029DA90 `
  "-DNFSMW_DRIFT_TIRE_FORCE_SIGNATURE=83 EC 24 56 8B F1 8B 86 0C 01 00 00 8B 8E 04 01 00 00 8B 49 08 D9 44 81 08" `
  -DNFSMW_DRIFT_TIRE_FORCE_CALL_RVA=0x002AA8F8 `
  "-DNFSMW_DRIFT_TIRE_FORCE_CALL_SIGNATURE=E8 93 31 FF FF" `
  -DNFSMW_DRIFT_DRIVE_TORQUE_RVA=0x002A0580 `
  "-DNFSMW_DRIFT_DRIVE_TORQUE_SIGNATURE=D9 41 34 C3" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
```

CMake 最后应显示 `Configuring done` 和 `Generating done`。确认配置成功后，再执行：

```powershell
cmake --build $asiBuild --target nfsmw_drift_assist_asi
Get-ChildItem "$asiBuild\nfsmw_drift_assist_asi.dll", `
  "$asiBuild\nfsmw_drift_assist_alpha.asi" | Select-Object Name,Length
```

使用 `NMake Makefiles` 时，文件直接位于 `$asiBuild` 根目录，不会再有 `Release` 子目录。
如果你的终端提示找不到 `nmake` 或 `cl`，请重新打开 **x86 Native Tools Command
Prompt for VS 2022**，或把错误截图发来，不要切换到 64 位工具链。

确认位数：

```powershell
dumpbin /headers "$asiBuild\nfsmw_drift_assist_alpha.asi" | Select-String 'machine'
```

必须看到 `14C machine (x86)`。`.asi` 是构建后由 CMake 复制的同一个 PE 文件，
不需要再次手动改名，也不要把 `.lib` 改名为 `.asi`。

上面的推荐脚本和手动命令都显式启用了 `NFSMW_ENABLE_ALPHA_BRIDGE`，并提供了当前目标
已经核验的输入、物理阶段、角速度 setter、轮胎函数和总扭矩入口签名，因此生成的是
Alpha 0.9.13 功能构建。若省略这些选项，
只会得到默认故障关闭宿主，不会安装任何 Hook，也不是可实测的漂移辅助。

目标指纹对齐 2026 年 9 月 9 日签章并命名为 `speed.exe` 的主程序：PE32/i386、映像基址 `0x00400000`、入口 RVA `0x003C4040`、`SizeOfImage=0x00693000`、文件大小 `0x005DA040`（6,135,872 字节），完整 SHA-256 为 `188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A`。文件尾必须是 64 字节 `NFSMWRF1` 结构并包含验证代码 `868086`；构建仍会检查原始主体 SHA-256、签章和全部 Hook AOB。其他扩容或未签章 `speed.exe` 不会通过门禁。

不要复用工作区旧的 `work\asi_build_probe` 目录；它保存的是另一份目标 profile。
公开项目中常见的 `0x005C0000`（6,029,312 字节）属于另一份可执行文件，不能拿来
替代当前目标 profile。不要通过删掉文件大小检查或填入猜测的签名来绕过这个闸门；
应先针对你实际持有的版本测量每个 Hook 的前导字节，并确认 AOB 在映像中唯一。

### 只读输入诊断（可选）

如果要先确认输入轮询，不要直接启用漂移控制。使用一个新的构建目录，并显式打开
`NFSMW_ENABLE_INPUT_DIAGNOSTIC`；该选项默认关闭。当前已对工作区目标 `speed.exe`
核验以下唯一锚点：RVA `0x002349B0`（VA `0x006349B0`），完整前导字节为：

```text
83 EC 0C 55 56 8B F1 8B 46 24 33 ED 3B C5 89 74 24 0C 0F 85
```

在 Developer PowerShell 中执行：

```powershell
$diagBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\input_diagnostic_x86'
cmake -S $src -B $diagBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_INPUT_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x002349B0 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=83 EC 0C 55 56 8B F1 8B 46 24 33 ED 3B C5 89 74 24 0C 0F 85" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $diagBuild --target nfsmw_drift_assist_asi
```

诊断 Hook 使用已验证的无栈成员函数 ABI：对象指针由 ECX 传入，宿主以
`void __fastcall(void* self, void* unusedEdx)` 桥接，在原轮询函数返回后只读检查
`self`、虚表槽位 4（第 5 项）、`self+0x20` 和完整 76 个 `float` 槽，最多每 250 毫秒向
`OutputDebugStringA` 输出一次动作行 `0..9` 和 `self` 地址。目标 EXE 的静态模板名称和
实机按键校准共同确认：`r0=GAS`、`r1=BRAKE`、`r2=STEERLEFT`、
`r3=STEERRIGHT`、`r4=HANDBRAKE`、`r5=GAMEBREAKER`、`r6=NOS`、
`r7=SHIFTDOWN`、`r8=SHIFTUP`、`r9=RESET`。它不写动作输出缓冲区、方向输入、
控制器或车辆物理；版本、签名、指针
或数值校验失败时只跳过采样，Hook 安装失败则故障关闭。用 DebugView 查看日志，
看到 `read-only input diagnostic installed` 后再进行按键采样。该阶段仍不会产生漂移效果。

### 只读车辆对象诊断（可选）

若要确认活动车辆数组和候选车身对象，使用另一个全新的构建目录，并打开
`NFSMW_ENABLE_VEHICLE_DIAGNOSTIC=ON`。当前目标上已核验的入口为
`ActiveComponents_TickAll`，RVA `0x000BA940`（VA `0x004BA940`），唯一签名：

```text
A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24
```

在同一个 Developer PowerShell 中执行：

```powershell
$vehicleBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\vehicle_diagnostic_x86'
cmake -S $src -B $vehicleBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_VEHICLE_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x000BA940 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $vehicleBuild --target nfsmw_drift_assist_asi
```

这是 `void __cdecl(float dt)` Hook。原函数返回后，诊断只读扫描 `0x00913E74` 活动
数组（数量 `0x00913E7C`，最多 64 项），沿 `entry+0x08` 检查候选车身主/次虚表、
`body+0x98` 子物理指针和 `body+0x100..0x140` 原始浮点字段；同时扫描
`PVehicle::g_mInstances @ 0x009352B0`（每项 8 字节，最多 64 项），记录
`mRigidBody(+0x78)`、`mPlayer(+0x84)`、`mInput(+0xE8)`、`mSuspension(+0xF0)`、
`mSpeed(+0x11C)`、`mSlipAngle(+0x12C)`、`mWheelsOnGround(+0x130)` 和
`mLocalVel(+0x134)`。已知主虚表 `0x008AC06C`/`0x008AC0FC` 分别标记为玩家/AI
候选；每 500 毫秒最多输出一批日志。所有读取均经过映像/提交页、有限值和 SEH 检查；
不调用游戏虚函数，不写车辆、输入或物理数据。输入诊断与车辆诊断必须分开构建。该
产物只用于确认对象布局，仍不会产生漂移效果。

### 只读物理协调器诊断（可选）

若要确认主线程物理协调器的运行时对象和积分器入口，使用第三个全新的构建目录，
打开 `NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=ON`。当前目标 `speed.exe` 上已核验的
入口为 `WorldPhysicsDispatch_MainThreadCoordinator`，RVA `0x0035AAD0`
（VA `0x0075AAD0`），唯一签名为：

```text
8B 0D C8 85 98 00 85 C9 C7 05 90 32 90 00 00 00 00 00 74 05 8B 01 FF 50 44
```

在同一个 Developer PowerShell 中执行：

```powershell
$coordinatorBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\coordinator_diagnostic_x86'
cmake -S $src -B $coordinatorBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=ON `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x0035AAD0 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=8B 0D C8 85 98 00 85 C9 C7 05 90 32 90 00 00 00 00 00 74 05 8B 01 FF 50 44" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $coordinatorBuild --target nfsmw_drift_assist_asi
```

该 Hook 是 `void __cdecl()` 无栈参数入口。原函数仍会被完整调用；探针只读取并记录：

- `DAT_009885C8` 的全局地址和值。此版本中该值是协调器完整对象的 `+0x48` 子对象地址；
- 子对象运行时虚表（预期 `0x008B0D78`）及其第 17 项（字节偏移 `+0x44`）实际函数地址；
- 由 `子对象 - 0x48` 得到的完整对象主虚表（预期 `0x008B0E18`）。

日志中的 `slot17` 会给出运行时目标的 RVA 和是否位于游戏映像可执行区；探针不会调用该
未知虚函数，也不会写入全局、虚表、对象或任何车辆物理值。所有读取经过映像范围、提交页
和 SEH 检查，失败时只记录并继续调用原函数。典型日志：

```text
read-only coordinator diagnostic installed at 0x0075AAD0 (cdecl no-args; slot17 is observed only)
coordinator diagnostic calls=... freq=...Hz global=... value=... full=... subVtable=... primaryVtable=... slot17=... rva=... text=... valid=...
```

该构建只用于确认积分器调用频率和对象布局，不会产生漂移效果。

### 只读物理阶段/时序诊断（可选回归工具）

该诊断用于确定玩家刚体角速度在两个已知物理阶段的哪一侧发生变化。它只 Hook
`ActiveComponents_TickAll`（RVA `0x000BA940`，VA `0x004BA940`）和
`FUN_006E7A00`（RVA `0x002E7A00`，VA `0x006E7A00`）；两者的 ABI 都是
`void __cdecl(float dt)`。已在目标 PE 完整映像中核验的两个唯一签名为：

```text
A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24
56 8B 74 24 08 56 E8 E5 33 07 00 56 E8 9F 4A 07 00 83 C4 08 56 B9 C0 66 91 00
```

在新的构建目录中执行：

```powershell
$phaseBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\phase_timing_diagnostic_x86'
cmake -S $src -B $phaseBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=OFF -DNFSMW_BUILD_ASI_HOST=ON `
  -DNFSMW_ENABLE_PHASE_DIAGNOSTIC=ON `
  '-DCMAKE_CXX_FLAGS=/WX /utf-8' `
  -DNFSMW_DRIFT_REQUIRED_SIGNATURE_RVA=0x000BA940 `
  "-DNFSMW_DRIFT_REQUIRED_SIGNATURE=A1 7C 3E 91 00 56 8B 35 74 3E 91 00 8D 0C 86 3B F1 57 8B 7C 24" `
  -DNFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA=0x002E7A00 `
  "-DNFSMW_DRIFT_PHASE_WORLD_SIGNATURE=56 8B 74 24 08 56 E8 E5 33 07 00 56 E8 9F 4A 07 00 83 C4 08 56 B9 C0 66 91 00" `
  "-DNFSMW_SDK_SOURCE_DIR=$sdk"
cmake --build $phaseBuild --target nfsmw_drift_assist_asi
```

公共版本闸门会在安装任何 Hook 前，要求两个签名都在完整映像中恰好命中一次、命中
配置的 RVA，且两个目标都位于经过 PE 节表验证的可执行 `.text`。两个 Hook 会先以
禁用状态创建并发布原函数跳板，再排队一次启用。任一检查或 MinHook 步骤失败都会让
诊断保持关闭；若批量启用已开始且禁用回滚也失败，代码会保留跳板和透明链回路径直到
进程退出，而不会释放仍可能被游戏线程使用的跳板。

探针从 `PVehicle::g_mInstances @ 0x009352B0` 中选取唯一一个 `mPlayer(+0x84)` 非空的
条目，读取 `mRigidBody(+0x78)` 并要求其虚表为 `0x008AC880`。角速度不是
`rb+0x30` 的三个浮点：必须依次读取 `holder=*(rb+0x30)`、`inner=*holder`，最终才从
`inner+0x30/+0x34/+0x38` 取得三个有限浮点。每一级都做溢出、提交页和 SEH 检查；
探针不调用 slot 10、slot 25、setter 或任何未知虚函数。

每 250 毫秒选择一个调用并输出可配对的 `active.before`、`active.after`、
`world.before`、`world.after`。日志包含 `pair`、各锚点调用计数、线程 ID、`dt`、QPC、
玩家/刚体/内部指针、诊断代次、三分量角速度、阶段内差值和有效位。未选中的调用只链回
原函数，不扫描车辆表。典型日志：

```text
read-only phase timing diagnostic installed active=004BA940 world=006E7A00 (cdecl float; two signatures and .text validated)
phase timing pair=... phase=active.before ... thread=... dt=... generation=... pv=... rb=... inner=... omega=(...) flags=0x1FF valid=1
phase timing pair=... phase=active.after  ... delta=(...) delta_valid=1
phase timing pair=... phase=world.before  ...
phase timing pair=... phase=world.after   ... delta=(...) delta_valid=1
```

输出目录另有单独命名的 `nfsmw_drift_assist_phase_timing_diag.asi`。本版没有 Hook
`0x0075AAD0`；它只能在后续作为辅助观察点，不能从本版日志推断该入口前后的时间。
输入、车辆、协调器和 phase/timing 四种诊断必须分别构建、分别测试，同一时间只能在
游戏 `scripts` 中放一种诊断 ASI。

## 详细技术说明

`.asi` 本质上是给 ASI Loader 加载的 32 位 PE DLL。当前 `CMakeLists.txt` 在
`NFSMW_BUILD_ASI_HOST=ON` 且提供 SDK 路径时，会生成控制器静态库、宿主 DLL，
并自动复制出同名 `.asi`；`nfsmw_sdk_adapter_example.cpp` 仍是接口示例，未猜测
任何车辆内存地址。若同时显式启用 `NFSMW_ENABLE_ALPHA_BRIDGE=ON`，同一个目标还会
编译经过当前 profile 验证的方向覆盖、车辆状态采样、人工 yaw 与镜头跟随功能桥，并额外在构建目录生成
`nfsmw_drift_assist_alpha.asi`。

面向正式发布或其他 `speed.exe` 版本时，仍需要逐项验证宿主层的以下责任：

1. DLL/ASI 入口和初始化线程；
2. v1.3 English 32 位可执行文件和 Hook 锚点校验；
3. 输入轮询 Hook，采集方向、油门、刹车和手刹；
4. 输入帧与玩家车辆对象的同帧、同代次关联；
5. 物理阶段调用节奏、车身状态、角速度采样和人工 yaw 的下一序号单次消费；
6. 玩家车辆、刚体、输入、悬挂对象的每帧验证；
7. 镜头 LookAt 入口、调用所有者和既有 Hook 链验证，以及失败时仅关闭镜头辅助；
8. 若日后启用，仅前轮轮胎横向力和实际驱动轮正向动力的倍率写回；动力通过正向总驱动扭矩交给原生传动系统分配。

Alpha 0.9.13 功能桥已针对当前受支持 profile 接入输入采样、玩家身份验证、物理阶段
状态采样和经过验证的 `IRigidBody::SetAngularVelocity`。智能反打只在方向中立时接管；
自动反打接管期间任意超过死区的人工方向输入都在当前轮询停止自动覆盖并即时移交，可生成渐进的同向补转、
主动反向回正或无 yaw 写入的换边观察。首次接管前的中立输入透传并清除起飘残值；每个
有效人工目标从当前实际值按固定 `2.0 command/s` 到位；重接管的 0.20 秒保持最后实际命令；自动
反打使用同一 0.9.5 固定速率路径。所有路径都不削减最终转向角。
镜头按漂移侧使用修正后的相反轨道符号，目标为 `-driftSide * 3°`；从中心到侧边使用
2.0 秒 smoothstep，归中与折身换边使用更快的 1.25 秒。宿主验证游戏直接
调用或 `NFSMWOrbitCamera.asi` 的唯一调用点，也只接受原版 LookAt 入口或由
`NFS.CameraMod.asi` 所有的已验证跳转链。镜头链不受支持时，方向和 yaw 仍继续工作。
功能桥查找玩家车辆时会扫描 `PVehicle` 全部 64 个槽位并允许中间空槽；车辆池残留的
非空 `mPlayer` 还必须满足 SDK 对象状态以及 `GetPlayer`、`IsPlayer`、
`IsOwnedByPlayer` 契约，并由 `IPlayer::GetSimable()` 反向指回该候选；只有唯一合格对象
才会被选中。本版附带独立失败隔离的低频只读物理探针，用于采集当前玩家四轮受力与
transmission 总扭矩；它原样返回游戏计算结果。最新实机日志中四轮通道均无有效样本，
transmission 入口严格签名校验失败，因此本版不写前轮抓地或实际驱动轮正向动力，
两个倍率必须保持 `1.0`，writer 保持关闭；普通遥测里的 `front_grip=1.000 rear_drive=1.000`
只是关闭状态，不是物理样本。控制核心本身
不会自行注入游戏或扫描内存；实际 Hook 全部属于显式启用且通过 profile 的宿主目标。

### 刚体加速通道（0.9.13 发布构建）

轮胎侧向力与传动总扭矩的只读探针仍可能因运行时布局或调用时序不满足而报告
`layout-unavailable`；日志中的 `front_grip=1.000`、`rear_drive=1.000` 只是 writer 未启用的
占位值。通用源码构建仍可使用 dry-run 来验证规划逻辑；本次 0.9.13 发布构建则显式开启
刚体补速 writer。通用构建若只需要 dry-run，可加入：

```text
-DNFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=ON
-DNFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=OFF
```

这会启用 dry-run 规划和低频 `rigidbody_accel` 遥测，不改动游戏。0.9.13 发布构建还需将
第二个开关设为 `ON`；它只调用当前 profile 已验证的 `IRigidBody::Accelerate`（vtable
slot 39，RVA `0x00299E10`），通过 `deltaV = targetAcceleration * dt` 给整车刚体沿车头方向补速。
当前 0.9.13 发布构建固定以 `5.25 m/s²` 为满幅目标（相对已验证的 5.0 m/s² 基线提高 5%），
并保留约 1 秒渐入。它绕过轮胎
滑移、差速器和驱动轮分配，因此不能替代“只给前轮抓地”或“按前驱/后驱/四驱分配动力”。
目标加速度按整车线速度模长衰减：`<=70 km/h` 为 100%，`125 km/h` 为 50%，
`150 km/h` 为 15%，`>=170 km/h` 为 0%；三个区间内均为线性插值。日志中的
`longitudinal` 仍保留为车头前向投影，`speed_mps` 才是衰减和安全限速使用的速度。
写入后会立即比较 `before_velocity` 与 `after_velocity`：实际总增量及车头前向投影必须与
请求 `delta_v` 匹配，正交残差必须在容差内。无变化、幅度错误或方向污染均会永久关闭该
刚体通道而不撤销已验收的转向/yaw。通用源码构建仍建议保持两个开关为 `OFF`；发布构建
已经过独立的构建期和运行期边界校验，但仍应按本页的目标版本和离线短测限制使用。

0.9.13 发布构建的两个刚体开关必须同时为 `ON`：

```text
-DNFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=ON
-DNFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=ON
```

## 推荐工具链

当前可编译的 ASI 目标使用 `s-b-repo/nfsmw-2005-sdk` 的统一入口和 MinHook，
因此最直接的路线是：

- Visual Studio 2022 的“使用 C++ 的桌面开发”；
- 32 位 x86 Native Tools/Developer PowerShell；
- CMake 3.20 或更高版本；
- `s-b-repo/nfsmw-2005-sdk` 源码目录；
- Ultimate ASI Loader；
- 经过目标版本验证的 Hook 签名；默认构建故意留空，Alpha 功能构建必须显式提供。

`nfsmw_sdk_adapter_example.cpp` 是另一条 MSVC C++ 适配器的接口参考，不参与上面
的默认构建。不要把两套 SDK 的多重继承对象或结构体偏移混在同一个宿主里。

### 核心验证（命令行写法）

在 Visual Studio Developer PowerShell 中执行：

```powershell
$src = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\outputs\nfsmw_drift_assist'
$coreBuild = 'C:\Users\wepie\Documents\Codex\2026-08-31\za\work\core_v099_tests_x86'
cmake -S $src -B $coreBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=ON -DNFSMW_BUILD_ASI_HOST=OFF
cmake --build $coreBuild
ctest --test-dir $coreBuild --output-on-failure
```

这一步的产物是 `.lib` 和测试程序，不是 ASI。它确认状态机和配置解析器在 x86 工具链下可用。

### 当前宿主的边界

`asi_host/asi_main.cpp` 已包含 SDK 入口、PE/profile 闸门和
`drift_assist.ini` 读取。它从自身 DLL/ASI 的目录寻找配置，不依赖进程当前工作
目录；文件不存在或解析失败时会把 `enabled` 置为 `false`。通过完整 profile 后，
才会调用 `ParseAssistConfigIni()` 并记录 DebugView 日志。

默认构建没有安装输入、车身姿态、前轮抓地或实际驱动轮正向动力 Hook；只读诊断选项打开时仅
安装对应观察 Hook，不会改变任何游戏值。Alpha 0.9.13 功能构建会安装输入 Hook、物理节奏
Hook 和独立验证的低频轮胎/总扭矩只读探针，处理顺序为：

```text
输入轮询完成 -> 读取玩家原始方向和手刹
同一调用周期 -> 排除禁用实例槽、合并重复身份，再从非空玩家指针中验证唯一合格玩家车辆并只读采样姿态、速度、角速度和接地状态
控制器更新 -> 任意超过死区的人工方向输入立即取得所有权；方向连续中立且符合条件时生成固定智能反打目标
前轮写入 -> waiting-angle 中立透传并清残值；自动和人工目标都沿 0.9.5 的固定 2.0 command/s 路径按剩余距离到位；reengage-delay 保持最后实值
人工同向存在 -> 按 15% 到 100% 的 2 秒渐入缓存 0.5625/3/0.15 有界补转命令
人工反向存在 -> 按刹停距离和 ±2 度零区缓存 0.45/4.5/0.15 有界主动回正命令
跨中心换边 -> 最多 0.50 秒完整透传且不缓存 yaw，相反侧 6 度才提交新侧
下一次物理 Hook 原函数返回 -> 重新验证身份和模式，经 setter 单次消费 yaw 命令
稳定状态 -> 发布 `-driftSide * 3` 度镜头目标；首次进入用 2.0 秒 smoothstep，折身换边和退出/失效归中用 1.25 秒
首次资格建立 -> 连续观察 60 个独立物理帧；首次完成后的换车、身份或节拍恢复在最后一次失败后连续观察 3 帧
换车、重置或身份变化 -> 清空会话及镜头漂移目标；禁用旧车不参与候选，重复身份只计一次，旧车还需通过玩家到 Simable 的反向归属
每 100 ms 的候选物理帧 -> 复核玩家、刚体 holder/inner、悬挂、四个互异轮对象和 transmission，随后只在该次原物理调用内开放只读窗口
物理原函数返回 -> 关闭探针窗口；约每秒汇总四轮 D4/D8/DC/0x110 与正/零/负总扭矩计数，不在高频 Detour 内写日志
```

输入与车辆样本必须来自同一稳定玩家身份，并通过调用序号、线程、指针和有限值验证。
任一检查失败时，宿主跳过方向覆盖并丢弃待用 yaw。人工 yaw 命令必须满足
`sourcePhysicsSerial + 1 == sinkPhysicsSerial`；过期、跨线程、身份或模式变化均不写入。
setter 调用或读回失败时只永久关闭人工 yaw 通道，智能反打继续工作。

镜头 Hook 在稳定观察完成后才安装。调用所有者只接受游戏本体或验证唯一返回点后的
`NFSMWOrbitCamera.asi`，LookAt 链只接受原版入口或 `NFS.CameraMod.asi` 所有的可执行跳转。
这些检查或安装任一步失败时只记录 `alpha camera assist disabled/disarmed`，不撤销车辆控制。

### 输出位数检查

```powershell
dumpbin /headers "$asiBuild\nfsmw_drift_assist_alpha.asi" | Select-String 'machine'
```

必须看到 `14C machine (x86)`，不能是 `8664 machine (x64)`。NFSMW 2005 的目标进程是 32 位，64 位 DLL 无法加载。

不建议使用单文件 `cl /LD` 命令绕过 CMake，因为 SDK 的 `entry.c`、MinHook 和
profile 头文件必须同时加入目标；请使用上方推荐脚本或“手动构建命令”。

### 安装和首次测试

1. 只读诊断阶段才将对应构建目录中的诊断 ASI 放到 `<NFSMW>\scripts\`；各种诊断与
   Alpha 功能版不要同时放入目录。
2. 将 Ultimate ASI Loader 的 `dinput8.dll` 放到 `speed.exe` 同目录前先备份已有文件。
3. 输入诊断版应在 DebugView 中看到 `read-only input diagnostic installed` 和
   `input diagnostic self=...`；车辆诊断版应看到 `read-only vehicle diagnostic installed`
   和 `vehicle diagnostic ...`。两者都只读，不会产生漂移。
4. Alpha 0.9.13 实测时先完全退出游戏，备份同名文件，再手动复制
   `nfsmw_drift_assist_alpha.asi` 与 `drift_assist.ini`。构建系统不会代替这一步，也不会
   自动写入或清理游戏目录。
5. 启动后应看到版本 `0.9.13-alpha`、包含 `60-sample initial and 3-sample recovery probation`
   的 `functional alpha installed ... yaw_setter=...`，以及 `alpha camera assist installed ...`
   日志。镜头安装行应包含 `mode=direct|orbit-compatible`、
   `look_at_chain=direct|camera-mod`、`angle=3.0deg`、`direction=inverted`、
   `entry=2.0s-smoothstep` 和 `return_switch=1.25s-smoothstep`。还应看到
   `handling probe channel=tire-force armed ... read_only=1`，以及 transmission 首次通过
   slot 8 复核后的 `handling probe channel=drive-torque armed ... read_only=1`；操作与回传要求按
   [ALPHA_TEST.zh-CN.md](ALPHA_TEST.zh-CN.md) 执行。测试结束后再手动移除或恢复备份。

只在离线单机和独立测试存档中验证，不要在联机、排名或带反作弊的环境加载。

## SDK 说明

`s-b-repo/nfsmw-2005-sdk` 自带统一 ASI/BepInEx 入口和
`nfsmw_add_plugin()` CMake 辅助函数，当前目标通过它生成 `.dll` 和 `.asi`。
它的 `PVehicle`、`PInput` 等对象是 opaque 表示，不能直接套用本目录另一条
`NFSPluginSDK` MSVC 多重继承适配器；两套 SDK 的对象布局和调用约定必须分开。

## 正式发布前的缺口

Alpha 0.9.13 是仅针对当前 profile 的离线功能测试版，不是通用成品。正式发布前仍需：

- 目标 `speed.exe` 的哈希、PE 时间戳、入口点和 Hook 签名已核对；
- 玩家车辆和四个接口对象的归属验证已实现；
- 输入覆盖、车辆采样和人工 yaw setter 路径继续积累实机回归；
- SDK 声明的 `FL, FR, RL, RR` 与适配器示例 `{0, 1, 3, 2}` 存在轮序冲突。当前内置
  只读探针必须先通过实机日志确认前轮物理索引、真实车轮和对象归属；前轮侧向力 writer 若未来启用，
  必须放在独立版本中重新确认物理阶段与调用约定；
- 驱动轮动力 writer 若未来启用，只允许放大玩家车辆的正向总驱动扭矩，由原生传动系统
  分配到实际驱动轮；零/负扭矩、发动机制动和倒车必须保持原值，0.9.13-alpha 中仍关闭；
- 左右轴符号、局部坐标到世界坐标的转换已用测试车辆验证；
- 失败时能停止写回并恢复临时覆盖，而不是继续使用旧指针。

当前支持版本以外的真实地址和时序仍必须针对使用者合法持有的游戏版本单独验证，不能
复用 Alpha 的固定 profile 或绕过故障关闭。
