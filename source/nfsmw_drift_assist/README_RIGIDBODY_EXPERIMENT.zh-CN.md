# 刚体加速、减速与 LC 通道说明 1.0.1

> 本文中的 `speed.exe`、0.9.15 文件名和独立 companion 仅是历史记录。当前发布包已将
> 刚体通道集成到唯一的 `Slippery_Drifting_FlashFish_by_AndyQinke.asi` 中；不要再加载
> `nfsmw_drift_assist_asi.asi` 或 `rigidbody_companion.asi`。主程序必须是经过
> `NFSMWMultiGear` 签章、文件名为 `speed.exe` 的版本，并包含验证码 `868086`。
> 普通原版 `speed.exe` 不会通过启动闸门。

把包内的 branded ASI 与 `drift_assist.ini` 放入游戏 `SCRIPTS`/`scripts` 目录。首次
验证成功时，插件会在同一目录创建加密的
`Slippery_Drifting_FlashFish_by_AndyQinke.device.json`；JSON 只保存 DPAPI 密文的大写
十六进制，不保存明文硬件标识。

0.9.16 主发布包已经集成并启用经过严格验证的刚体补速通道（
`NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=ON` 与
`NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=ON`）。它只适配当前已核验的
`speed.exe`，
建议先在离线单人和短时测试中验证。不要和其他漂移诊断 ASI 同时加载；出现速度异常、
失控、闪退或无法退出漂移时立即退出游戏并移除本插件。

0.9.19 在非漂移驾驶辅助模式中复用同一安全刚体通道实现普通刹车 ABS：刹车连续满
0.5 秒后以漂移手刹减速的 50% 起步，3 秒内线性降到该初始力的 20%。漂移状态具有绝对
优先级；驾驶 ABS 与漂移补速、漂移手刹减速不会同时提交。

1.0.1 新增 LC。低于 10 km/h 同时按住油门和脚刹或手刹时，通过已验证的
`IRigidBody::SetLinearVelocity` 每帧只移除正向速度分量，并通过 `ISuspension` 的公开
角速度接口仅把前两个车轮归零。它不写后轮、不覆盖转向；输入释放时不再提交任何 LC 写入。

## 文件

- `Slippery_Drifting_FlashFish_by_AndyQinke.asi`：方向、yaw、镜头功能以及刚体写入通道。
- `drift_assist.ini`：与本包匹配的配置。

把这两个文件复制到游戏的 `scripts` 目录。完全退出游戏后再替换文件。

## 本次发布行为

刚体通道只在漂移会话已经进入持续 `Holding`、漂移侧已提交、玩家身份和物理节拍
连续、至少两个车轮接地、油门有效且没有刹车时运行。刚拉手刹、等待反打门槛、
钟摆换边窗、回中和退出阶段都不会补速。

0.9.16 以已验证的 `5.0 m/s^2` 为基线，名义配置目标按 5% 提高到
`5.25 m/s^2`。为控制体感强度，发布 writer 在此前 `0.72` 的基础上再相对降低
15%，在所有速度段统一使用 `0.612` 的强度系数，因此低速满幅的实际目标为
`3.213 m/s^2`；保留约 1 秒渐入、约 0.25 秒
渐出和每物理帧 `0.35 m/s` 的速度增量上限。它按每帧实际前轮命令计算方向后调用已核验的
`IRigidBody::Accelerate`，不会在起飘阶段提供推力。前轮正打、顺打或回到零度时，方向
严格等于车头方向；前轮反打时，相对车头的加速偏转收窄 75%，仅保留此前中间夹角
偏转的 25%，即取实际前轮反打角的 `12.5%`，让施力方向更靠近车头。

目标加速度还会按整车线速度模长衰减（阈值按 km/h，区间内线性插值）。这样在大角度
侧滑时不会因为车头前向投影较小而错误地保持满推力；反向运动判定仍只使用车头基准轴：

| 整车线速度 | 速度衰减（相对名义目标） | 发布实际目标（相对 5.25） |
| --- | ---: | ---: |
| `<= 70 km/h` | `100%` | `61.2%` (`3.213 m/s^2`) |
| `70-125 km/h` | `100% -> 50%` | `61.2% -> 30.6%` |
| `125-150 km/h` | `50% -> 15%` | `30.6% -> 9.18%` |
| `150-170 km/h` | `15% -> 0%` | `9.18% -> 0%` |
| `>= 170 km/h` | `0%` | `0%` |

## 手刹减速

当辅助会话已激活、手刹连续按住至少 `0.20` 秒且当前仍处于按下状态时，同一
`IRigidBody::Accelerate` 通道改为沿实际线速度反方向提交减速。减速优先于补速，两者
不会在同一帧共存；松开手刹立即取消，未满 0.20 秒的点按完全不产生减速写入。

最大有效补速为 `3.213 m/s²`，所以最大减速按 `1.15x` 计算为
`3.69495 m/s²`。车速 100 km/h 及以下使用最大减速的 15%，100-180 km/h 线性增加，
180 km/h 及以上使用 100%。减速方向取实际速度反向而不是车头反向，避免大侧滑时
因为车身朝向与运动方向不一致而注入额外横向力。

旧 0.9.11 实机日志中共有 47 帧显示 `write_ok=1`，但旧式检查只证明调用返回后读取
没有报错，不能证明请求的净速度增量真正保留。本轮在此前 `0.72` 发布倍率上再相对
降低 15%，得到 `0.612`；应以新日志中的 `target_accel`/`delta_v` 和体感共同确认最终强度。
0.9.16 改用
严格的前后速度差验证；`projected_delta` 按本帧最终收窄后的反打方向核验。

日志以 `rigidbody_force execution=write force_mode=...` 开头。0.9.18 会记录 `longitudinal`（车头基准前向投影，
只用于反向保护）
和 `speed_mps`（整车线速度模长），并记录 `steering_snapshot_valid`、
`applied_steering`、`drift_side`、`direction_mode`、`accel_direction`、
`direction_heading_dot`、`before_velocity`、`after_velocity`、`actual_delta`、
`actual_delta_mag`、`projected_delta` 和 `orthogonal_residual`。只有实际总增量和最终收窄后的反打方向投影都与请求的 `delta_v` 匹配、正交残差
在容差内，并同时出现 `accepted=1 write_attempted=1 write_ok=1`，才表示该帧写入严格
验证成功。无变化、增量不足/过强或方向污染都会立即判失败并永久关闭本次进程的刚体
writer。`not-sustained-drift`、`no-throttle` 和 `runtime-not-ready` 都表示该帧没有写入。

## 重要限制

`Accelerate` 是整车刚体质心的线速度增量，绕过轮胎滑移、差速器和原生驱动轮分配。
它不能实现前轮抓地增强，也不是已经找到发动机或传动系统的真实动力倍率。当前轮胎
侧向力和驱动扭矩 writer 仍保持关闭，不能把本实验结果描述成抓地力或驱动轮动力功能。

## 当前 ABI 依据

该包只针对已核验的目标：已签章 `speed.exe` SHA-256
`188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A`。
`IRigidBody` 虚表为 `0x008AC880`，`GetForwardVector` 为 slot 13，`Accelerate` 为
slot 39（RVA `0x00299E10`）。入口字节核对为：

```text
8B 41 30 8B 00 8A 48 5D 84 C9 75 2A
```

任何 profile、虚表、对象链或入口签名不匹配都会故障关闭，不会回退到未经验证的地址。

## 构建开关说明

通用源码构建默认仍使用 `NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=OFF` 和
`NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=OFF`，以便进行只读或控制逻辑开发；本次
0.9.15 发布构建明确将两个开关设为 `ON`，并包含上文的逐帧前后速度、最终方向投影和正交
残差验证。轮胎侧向力与驱动扭矩 writer 仍保持关闭，不能把刚体补速描述成轮胎抓地或
驱动轮动力倍率。
