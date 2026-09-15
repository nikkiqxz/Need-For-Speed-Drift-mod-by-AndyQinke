# Slippery Drifting FlashFish by AndyQinke

《Need for Speed: Most Wanted (2005)》漂移与驾驶辅助 ASI 插件。

当前正式版：**v1.3.8**

## 下载与安装

完整安装包：

- [`release/Slippery_Drifting_FlashFish_by_AndyQinke_v1.3.8_RELEASE.zip`](release/Slippery_Drifting_FlashFish_by_AndyQinke_v1.3.8_RELEASE.zip)

也可以直接使用解压后的文件：

- [`release/v1.3.8/Slippery_Drifting_FlashFish_by_AndyQinke.asi`](release/v1.3.8/Slippery_Drifting_FlashFish_by_AndyQinke.asi)
- [`release/v1.3.8/drift_assist.ini`](release/v1.3.8/drift_assist.ini)

将上面两个文件复制到游戏目录的 `SCRIPTS` 文件夹，覆盖旧文件后完全重启游戏。

> ASI 文件名必须保持为 `Slippery_Drifting_FlashFish_by_AndyQinke.asi`，改名后插件不会加载。

## 兼容性

正式包针对 2026 年 9 月 9 日签章并命名为 `speed.exe` 的版本：

- PE32 / x86
- 文件大小：6,135,872 字节
- SHA-256：`188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A`
- 包含 `NFSMWRF1` 文件尾和验证代码 `868086`

其他原版、扩容版、未签章或被再次修改的 `speed.exe` 会被安全门禁拒绝。

## v1.3.8 内容

- 智能自动反打、手动转向接管、漂移旋转辅助与平滑漂移镜头
- 漂移刚体加速、手刹刚体减速和漂移动力倍率
- ABS、ESC、TCS、LC 驾驶辅助及局内 HUD
- 每车独立自动反打倍率，最多 512 条配置
- 后轮转向：按真实车辆数据库名称匹配，支持每车低速反向角和高速同向角
- 后轮转向速度随车速平滑变化，且不再在 200 km/h 关闭
- 正式版关闭运行、HUD 和后轮转向诊断日志

详细变更及参数说明见：

- [`release/v1.3.8/README.zh-CN.txt`](release/v1.3.8/README.zh-CN.txt)
- [`source/nfsmw_drift_assist/README.zh-CN.md`](source/nfsmw_drift_assist/README.zh-CN.md)

## 源码

当前完整源码位于 [`source/nfsmw_drift_assist`](source/nfsmw_drift_assist)，包含：

- 漂移控制核心与 INI 解析器
- ASI 宿主、启动验证、驾驶辅助 HUD、后轮转向和刚体力实现
- 12 项可独立运行的核心测试、1 项可选的签章版 `speed.exe` 集成验证及研究记录
- 构建、测试、诊断与发布说明

核心测试：

```powershell
cmake -S source/nfsmw_drift_assist -B build-current -DCMAKE_BUILD_TYPE=Release
cmake --build build-current --config Release
ctest --test-dir build-current -C Release --output-on-failure
```

不提供游戏主程序时，核心测试结果为 12/12；配置受支持的签章版 `speed.exe` 后，发布验证为 13/13。

构建正式 32 位 ASI 前，请完整阅读 [`source/nfsmw_drift_assist/BUILD_ASI.zh-CN.md`](source/nfsmw_drift_assist/BUILD_ASI.zh-CN.md)。正式构建需要匹配的 x86 工具链、`nfsmw-2005-sdk` 和受支持的签章版 `speed.exe`。

## 发布校验

- ASI SHA-256：`8A057114CE144E4EA65412D9253480843E6EDBE5F8B3D2FD52282C0C45C100A7`
- INI SHA-256：`DFF0B570753CCAF2FA8CE3C5FF8312964E6D61DE12B4E2010826073147B77734`
- ZIP SHA-256：`165D8B906BC77AFFF35FD4D26A466D1CBC1F42AFA8069826A7C064CBE6400B60`

完整构建清单见 [`release/v1.3.8/BUILD_MANIFEST.txt`](release/v1.3.8/BUILD_MANIFEST.txt)。

仓库根目录原有的 `src/`、`build/`、`CMakeLists.txt` 和旧 INI 为该分支早期版本历史内容；v1.3.8 的源码、构建入口和发布文件以上述 `source/` 与 `release/` 目录为准。
