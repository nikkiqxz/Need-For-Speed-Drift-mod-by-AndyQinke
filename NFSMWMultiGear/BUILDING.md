# 构建 NFSMW MultiGear v1.0.0

v1.0.0 是面向打标 Reforged C5C5 主程序的 Win32 ASI。它为精确配置的变速箱提供
手动第 8-12 挡，并在所有 Hook 之前验证主程序签章与本机设备绑定。

## 环境

- Visual Studio 2022，安装 C++ 桌面开发和 Windows SDK
- CMake 3.16+
- Win32/x86 生成器
- 与源码相邻、固定到 commit `3b3d05b9194844883aa42d75dd7aae66838092ff`
  的 `nfsmw-2005-sdk`

目录结构：

```text
parent/
  NFSMWMultiGear/
  nfsmw-2005-sdk/
```

## 配置、构建和测试

```powershell
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 `
  -DNFSMW_SDK_DIR="../nfsmw-2005-sdk" `
  -DNFSMW_HOOKS_BACKEND=minhook -DBUILD_TESTING=ON
cmake --build build-x86 --config Release
ctest --test-dir build-x86 -C Release --output-on-failure
```

产物位于 `build-x86/Release/NFSMWMultiGear.asi`。正式版不会创建运行日志；SDK 还会生成同名 `.dll`，
但交付时只使用 `.asi`，不要把它安装到 BepInEx。

打标工具只接受经过确认的原始 C5C5 主程序，并且不允许原地修改：

```powershell
.\tools\StampReforgedExe.ps1 `
  -InputPath '.\Need For Speed MW-Reforged.original.exe' `
  -OutputPath '.\Need For Speed MW-Reforged.exe'
```

如果主机环境同时存在大小写不同的 `PATH`/`Path`，MSBuild 会报重复键错误；
请在启动 CMake/MSBuild 前清理其中一个变量后再构建。该环境问题与源码无关。

## 安装前检查

完全退出游戏，备份原主程序和 `SCRIPTS\NFSMWMultiGear.asi`。部署打标后的
Reforged 主程序（可使用 `Need For Speed MW-Reforged.exe` 或 `speed.exe`）和新 ASI，不覆盖现有 `.cfg`、`.log`，也不要
手工创建设备 JSON。插件没有热卸载流程；更换版本必须重新启动游戏。

## 验收

使用手动模式测试 raw 9、10、11、12、13（分别对应第 8、9、10、11、12 挡），
确认首次自动生成 `SCRIPTS\NFSMWMultiGear.device.json`。正式版不写入日志；如需
逐项观察启动阶段，请使用开发构建完成验收后再部署正式版。

倍率回归验收必须先用全 `1` 配置记录第 1-7 挡基线，再对第 8-12 挡逐个设置非
`1` 倍率。每个倍率只能在对应高挡为当前挡时生效；第 1-7 挡、其他非当前高挡，
以及从高挡降回低挡后的动力都应保持基准状态。每个实际进入的高挡还应出现一次
`TRACE POWER_APPLIED`；主传动路径的 `callerRva` 必须是 `0x002B0038`。

自动模式目前只保留原生第 8 挡范围；不要将自动模式结果作为 Phase 4 完成条件。
