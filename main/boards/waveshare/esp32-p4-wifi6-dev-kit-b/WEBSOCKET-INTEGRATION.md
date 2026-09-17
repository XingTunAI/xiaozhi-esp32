# P4 → Voice Lab WebSocket 接入

日期：2026-09-17。固件分支：`codex/p4-wifi6-dev-kit-b-screen`。

## 复用与适配

- 服务端协议以 `C:/QIU/XingTunAI/voice-lab/docs/API.md`、`docs/tech/ESP32_DEVICE_MEDIA.md` 和已有真机故障记录为准。
- 复用 `VoiceLabClient` 的控制/媒体连接、录音授权、revision 幂等、20 ms PCM、VLA2、有限 PSRAM 队列和 ACK 提交。当前每 25 帧聚合成 500 ms 包；控制消息、会话启动/结束、媒体发送分别由独立任务处理，媒体锁与控制连接锁分离。
- 4 秒未确认窗口满时暂停发送，采集进入有界的 12 秒 PSRAM 队列；队列溢出仍明确中止。媒体重建保留原 boot/sequence/sample 编号并先重发未确认包。控制重连必须服从服务端重新下发的 idle 或授权撤销，不能承诺自动续录。
- 通用 Board 接口补充网络就绪和诊断快照。普通 Wi-Fi 板保持原有 WifiManager 行为，P4 使用板级以太网/C6 状态；任一路有 IP 都表示可用，避免拔掉一路后误报整机断网。
- 输入适配器只接收已实测的 USB 16 kHz、双通道、16-bit、2-byte subslot 格式。选取 USB 通道 0 输出单声道，不混合两个 DSP 输出。其具体 DSP 路由未回读，不宣称已经完成声学调优；其他采样率/六通道版本目前不自动适配。
- USB 部分帧先补齐为 320 个单声道样本，再交给既有采集回调。每次新的外部采集会话重置 USB 缓冲，防止上一会话残留进入下一会话；断开设备时串行释放句柄。
- 开机不再运行 5 秒 UAC 测试；服务端授权开始后才由音频任务启动 USB 流。USB 播放不在本次范围，提示音继续使用 ES8311。

## 配对与分区

- 默认服务：`https://voice-lab.cloud:443`，TLS 校验保留。
- P4 使用本机 eFuse MAC 生成标识，不依赖 C6 初始化时序。独立设备标识：`p4-terminal-e8f60ae2c6a4`；服务器显示名 `Counter terminal E2C6A4`。
- 管理员生成一次性 enrollment code，经本机串口 `vl pair` 领取独立设备凭据并写入 NVS；管理员 Token 不传给设备，不写源码或日志。
- 独立分区表 `partitions.csv`：NVS/otadata 偏移不变，两个 OTA 槽均为 `0x4f0000` 字节，assets 移至 `0xa00000`、容量 6 MiB。首次迁移必须同时刷分区表、应用与资源包，仅 OTA 应用无法迁移旧布局。
- 首次迁移前已备份 `0x8000..0x20000` 元数据区域到本机 `local-build-backups/p4-dual-usb-20260917/pre-websocket-metadata.bin`。该文件包含设备数据，不进入 Git。

## 验证边界

已通过 84 项主机测试、P4 构建，并完成真机配对、WSS 握手和下述持续上传短测。首次单帧发送固件约 7 秒触发 4 秒未确认窗口上限；这是修复前的失败记录，不代表当前结果。

2026-09-17 C6 通过 SDIO OTA 升级到 2.12.12 后复测（correlation `b5742077-3068-4c79-b7d7-d6ea54bd1322`）：USB 采集 `read_fail=0`，约 7.7 秒再次触发未确认窗口上限，说明单独升级 C6 未解决问题。当时 P4 固件为 `29c84199c4fc33a0ed0b6d3fd4e32e6384d34a58049eb0745a5297f0821958ae`。

随后保持 C6、MAC、同一专用 mock Profile 不变，修改 P4 聚包、任务隔离和背压处理：

| 测试 | 结果 |
| --- | --- |
| 普通 counter_file，correlation `8f627be2-d5a2-4554-a701-18ff20b7d2cb` | 89.16 秒、2,853,120 字节、179 个 chunk，服务端 completed；段内序列/样本间隙、哈希错误均为 0；设备正常结束 |
| counter_file 中途重建媒体连接，`245c4035-d311-4b80-a263-09e7db0dccff` | 保留未确认包后重新 accepted，采集持续；服务端按连接封存为 12 秒和 48 秒两段，均 completed，共 1,920,000 字节；两段内部连续且哈希正确，总量与最终 ACK 960,000 样本一致 |
| meeting_live 中途重建控制连接，`c6cefc5c-f167-47b3-866e-9de9ffcffe19` | 服务端要求 idle，设备结束采集并收到 completed，没有重启；不属于自动续录通过 |
| 恢复原 Profile 后 meeting_live，`3079d2ad-ab54-41c6-abfb-20c4eabd49f2` | 60 秒观察窗内持续上传，实际发送 2,950 帧/59 秒；最终 ACK 到达 2,119,680（本次起点 1,175,680），收到 completed，USB read_fail=0 |

媒体重建由物理串口 `vl reconnect audio` 注入；控制重建为 `vl reconnect control`。这验证连接重建路径，不等于实际拔线、丢包、长时断网或租约会话验收。控制测试后的 SSH 管理连接曾超时，随后重新确认并关闭专用测试通道。

ARP/连通性观察中，Ethernet `192.168.5.80` 对应 `e8:f6:0a:e2:c6:a4`，C6 Wi-Fi `192.168.5.116` 对应 `b0:a6:04:98:73:70`，各 10 次 ping 成功且观察期 MAC 未变化。设备标识仍使用 P4 base MAC。未发现 MAC 冲突证据，但短测不能排除偶发冲突。当前证据支持 P4 发送/调度策略参与故障，未单独隔离聚包与任务分离各自贡献。

普通文件短测固件 SHA256：`6bcff660e3d8510de1d450c3a9fa6be348de1f142f4c0a2edf98138d822a8618`。增加物理串口重建诊断命令后，已烧录固件 SHA256：`49dc438a79557868bc65a2c7306dda032a757d791edaad05eb3cf55483a39280`。均仅刷应用，保留 NVS 和 C6 固件。日志和逐文件校验结果位于工作区外的 `local-build-backups/p4-dual-usb-20260917/`，不含管理员凭据。

已修复停止外部采集后的硬件停止请求：由音频输入任务调用 codec 停止，保持 ADC/USB 硬件所有权，且重新检查活动位以防与新会话冲突。此次停止后的观察窗口未再出现 UAC2 环形缓冲溢出。专用 mock channel 测后关闭，未修改其他设备或 Provider 配置。

后续仍需独立验收：长时上传、弱网/断网与租约行为、USB 热插拔、网口/Wi-Fi 切换、显示负载、USB 播放和 AEC。短时 mock Provider 测试只证明传输与留档，不证明语音识别质量。

## 2026-09-17 后续 Wi-Fi 现场

用户拔线后曾出现一次未捕获完整启动现场的非预期重启，原因未定；不能宣称所有断联已修复。已修复启动时已保存 Wi-Fi 连接超时后不再重试的问题，新增启动 reset_reason、断开 reason 和物理串口 `p4 status` 诊断。诊断版应用已刷入，SHA256 `13a4be86e22be65dccb706f34ef6aa55bb02b3741637a5bcfdbefa68806be7b7`；纯 Wi-Fi 实时上传最终 ACK 对应 217.8 秒，completed、read_fail=0、bootId 不变。之后仅格式化源码重建，未再次刷入。

统一交接记录位于 `C:/QIU/XingTunAI/voice-lab/docs/2026-09-17-P4-FIRMWARE-AND-CONNECTION-VALIDATION.md`，含代码位置、鉴权、C6 OTA、correlation 和待验证边界。纯 Wi-Fi 长稳及此前异常重启根因仍待验证。

## 屏幕录音按钮候选

已实现底部开始/停止、录音时长、申请/保存/失败状态；点击走既有服务端授权和音频收尾，网络操作不阻塞 LVGL。当前诊断候选 `78fcd2e4b3803d3dcff38bba9336cb6336957660d9d4b0f1a21d02910b297c4f` 使用同步 CPU 显示拷贝。相同 LVGL 事件的未授权拒绝、停止后 completed 已验证；开始授权成功和实际触摸视觉仍待验收，最近录音回放未实现。

后续测试抓到 USB 提交路径 Load access fault（另一个候选），以及当前候选 Wi-Fi reason=3、双 WebSocket 断开后恢复的现场。关闭 DMA2D/冻结 UI 均未建立充分的根因证据，不能宣称稳定性已修复。完整版本、correlation 和待确认的 owner/服务通道见 Voice Lab docs 根目录 `2026-09-17-P4-FIRMWARE-AND-CONNECTION-VALIDATION.md`。当前设备在线、未录音，上次会话标记 interrupted=true。
