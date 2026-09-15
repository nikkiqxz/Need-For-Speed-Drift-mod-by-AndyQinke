# NFSMW MultiGear v1.0.0 技术记录

## 目标版本

当前目标是由确认过的 v1.3 English/Collectors C5C5 映像生成的
`Need For Speed MW-Reforged.exe` 或 `speed.exe`：

```text
文件大小  6,135,872
MD5       FCBBAC633822546B4B5D86DB5F2F03DE
SHA-256   188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A
```

文件尾追加 64 字节 `NFSMWRF1` footer，内含版本、长度、验证代码 `868086`、原始
大小/PE checksum、flags 和原始映像规范 SHA-256。校验时使用同一个拒绝写共享的
文件句柄完成实际大小、footer、完整 SHA-256 和原始映像摘要检查；之后再验证加载
映像的 PE 字段与全部 Hook 代码签名。

上述验证全部通过后才计算设备 ID。输入固定为 SMBIOS UUID、MachineGuid 和系统卷
序列，绑定 payload 先用 `CryptProtectData` 的 `CRYPTPROTECT_LOCAL_MACHINE` 范围加密，
再逐字节编码为大写十六进制，放入 schema 2 的 JSON 外壳：

```json
{
  "schema": 2,
  "encoding": "DPAPI-LOCAL-MACHINE-HEX",
  "ciphertext_hex": "..."
}
```

启动时严格检查外壳、十六进制字符和长度，随后用同一 DPAPI 熵解密，并以常量时间
比较解密后的规范 payload。密文每次创建都可能不同，因此不能比较重新生成的密文
字节；必须先解密再验证。文件以同目录临时文件、刷新落盘、无覆盖改名的方式发布。
已有 v0.8.0 schema 1 明文文件只有在与当前设备的完整规范 payload 严格匹配后才会
原子迁移到 schema 2；不匹配时拒绝且不覆盖。因为不存在外部签发方，删除 JSON 后
仍可重新首次绑定；此机制不能作为防复制授权。

已确认的 `GEAR_RATIO`、`GEAR_EFFICIENCY` 都是布局内 9 槽数组：倒挡、空挡和
1-7 挡。raw index 9 是第 8 挡。第 8 挡的两个 float 位于布局 `+0x2C` 和
`+0x6C`，下一个已知字段从 `+0x70` 开始，所以第 9 挡不能继续写入原布局。

## Sidecar 设计

配置命中且数组布局仍为原生 `capacity/count=9` 时，插件执行以下步骤：

1. 将第 8 挡齿比写入 raw index 9 的验证空槽；其效率只写第 7 挡的基准效率，
   不把第 8 挡动力倍率发布到共享数组。
2. 将第 8-12 挡齿比、动力倍率和未缩放的基准效率分别保存到目标 transmission
   的 sidecar；静态 getter 同样只返回基准效率。
3. `Private::Count` 只对精确目标虚拟返回 10-14；原始 header 永远保持 9/9。
4. 固定 10 槽的原生消费者保持看到 10，避免其继续索引到布局外；sidecar-aware
   的 getter/ratio helper/换挡桥才读取 10-13 的插件数据。

`raw 10-13` 的 `ShiftGear` 采用 sidecar-only 直写（`+0x88`/`+0x84`/`+0x198`），
不调用原生高挡 `ShiftGear`；未配置目标仍透传。

第 9-12 挡的 sidecar 索引为 raw 10-13，分别对应配置中的第 2-5 组额外挡位值。
缺失任何必需齿比或倍率无效时，目标不会发布为 enabled。

游戏会扫描完整效率表并据此生成跨挡共享派生数据，所以任何永久写入 raw 9 或
sidecar 静态视图的倍率都会污染第 1-7 挡。当前版本使用双重门禁：调用必须来自
已验证的实时动力调用点，并且“查询 raw 挡位”等于“当前 raw 挡位”且位于 raw
9-13，才返回 `基准效率 * 对应倍率`。主传动、controller、torque 的允许返回地址
分别是 `0x006B0038`、`0x006A38F6`、`0x006A152D`，当前挡偏移分别是 `+0x84`、
`+0x64`、`+0x68`。通用 layout efficiency getter、未知调用点、低挡、非当前挡及
当前挡读取失败都只返回基准效率。

`TorqueEfficiencyRatioDetour` 以原生 raw 9 的中性结果为代理，再按
`基准齿比 / 目标齿比` 和 `基准效率 / 目标实时效率` 换算。`0x006A1480` 的返回值
与 `ratio * efficiency` 成反比，因此这里必须使用倒数方向。这样倍率不会重复应用，
倍率为 `1` 时保持旧行为，ratio-only helper 也只处理齿比。

## 已覆盖的入口

正式版对 C5C5 地址做签名校验后才安装钩子，覆盖：

- `Private::Count`、`Private::GetElement`、`ShiftGear`
- ratio/efficiency/effective-ratio getter 和 ratio pair
- transmission update、additional shift decision
- 主传动对象（`+0xA8`）的 top-gear/set-gear
- 备用传动对象（`+0xA4`，`0x006A32E0/0x006B0290`）的 top-gear/set-gear
- 已定位的 layout/controller getter 和 torque helper

未配置目标全部调用原函数。所有 sidecar 读写都经过可读/可写检查；异常或布局
不匹配时返回安全失败值，不把高挡索引交给固定原生数组。

## HUD 兼容

原版比赛 HUD 和直线加速 HUD 的单字符格式化调用分别位于 `0x0057A7CE`、
`0x0057D22D`。完整指令序列匹配后，插件仅重定向这两个 CALL；raw 10-13 以
`%d` 显示为 9-12，其余挡位继续使用原字符映射。

当前 CustomHud 兼容层只接受 v1.8.2 精确二进制（size `330752`，MD5
`2F5C7AB528C6E465879BEA71A0E75160`，PE timestamp `0x6323255F`）。它只重定向
gear 前景绘制 CALL（RVA `0x116CB`），复用 `HUD_Digit::DrawDigit`（RVA
`0xFD50`）绘制两个 glyph，并在调用后恢复 `PositionX`。raw 11-13 分别显示
10-12；N、R、速度数字和其他 HUD 元素不经过该 CALL。文件指纹、PE 资料或代码
签名任一不符都会安全跳过，不影响核心变速箱 hooks。若模块尚未加载，启动线程会
在 10 秒窗口内重试。

## 自动挡边界

`0x006A0D90`、`0x006A1740`、`0x006A3180` 内部仍有对对象固定数组的直接读：

```text
object + 0x9C + gear*4
object + 0x78 + current*4
object + 0xA4 + (current-1)*4
```

这些表不是简单的齿比数组，不能把第 9-12 挡映射到 raw 9 后假装等价。当前实现
因此让自动高挡决策和更新 fail-closed；这保护了稳定性，但不代表自动换挡已完成。
下一阶段需要为这些控制器重实现 sidecar-aware 阈值和状态更新，再单独做自动模式
实车验收。

## 验证状态

测试套件覆盖配置解析、动力倍率数学、静态效率中性、当前挡匹配、低挡隔离、
高挡/低挡往返无残留、C5C5 地址签名和 HUD 映射。用户日志已证明 raw 9-13 的
手动换挡路径、逐级降挡和倍率隔离已经完成实车确认。启动闸门测试另覆盖 footer
篡改、完整 EXE 篡改、固定设备 ID、首次/二次绑定和绑定不匹配不覆盖。
