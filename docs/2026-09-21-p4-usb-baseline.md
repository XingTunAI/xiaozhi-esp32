# P4 USB 固件基线与仓库同步

日期：2026-09-21。

## 当前使用方式

双麦音质未达标，按用户决定暂停支持，设备已通过 `p4 audio usb` 保存 USB 输入选择。外设页移除音源选择、PDM L/R、AFE 和增益按钮及其动作，保留状态显示、SD 检测和既有 BLE 入口。双麦源码与串口诊断命令仍保留，历史 NVS 选择未作强制迁移；其他设备若曾保存 PDM，需要另行切回 USB。

当前工作区一并保存 SD 恢复、空闲归档上传、HTTP 连接复用修正，以及可选 BLE 连接探针、设备端确认和屏幕展示代码。BLE 变体由 `config.ble.json` 选择，普通变体默认关闭。不能把屏幕展示代码或构建通过视为完整微信控制流程验收。

## 已完成验证

- 当前 P4 BLE 构建配置、ESP-IDF 6.1 完成编译链接，应用大小 `0x44a210`，现有 `0x4f0000` 分区余量约 13%。
- USB 界面版本应用 SHA256：`61F89B0698B9F5FEF41E22DF6323AD5B9B07B8BB8EAB2F8DA7BB3357F4AE7D41`。
- 仅烧录应用到 `0x20000`，esptool 写后哈希验证成功。
- 重启出现一次 USB root port reset 告警，随后 UAC2 枚举成功；输入为 16000 Hz、双声道 PCM16，选择 channel 0。
- 最终设备联网、已配对、未录音、无中断标记，下一段输入为 USB。
- 本次同步前主机测试共 88 项全部通过，无跳过。

本次 UI 清理后没有重新验收 USB 音质；双麦音质、微信手机连接、长时间稳定性和其他芯片/网络变体不在本次通过范围。停止时曾报告的重启尚无对应调用栈，不能称为已修复。

## 记录与本机证据

- [双麦质量测试记录](2026-09-21-p4-dual-mic-quality-test-record.md)
- [原始探针配置差异](2026-09-21-pdm-probe-config-comparison.md)
- [热点断开与 ASR 缺口](2026-09-21-p4-hotspot-asr-gap.md)
- 本机 adapter 工作区：`local-build-backups/p4-usb-ui-20260921/`，`tmp/p4-usb-ui-build.log`、`tmp/p4-usb-ui-flash.log`、`tmp/p4-usb-ui-boot.log`、`tmp/p4-usb-ui-final-status.log`。

原始音频、日志、设备私有配置和固件备份不纳入 Git。本次同步范围是 xiaozhi-esp32 固件仓，服务器及小程序仓独立维护。
