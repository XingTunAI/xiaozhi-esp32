## 2026-09-10 采集就绪握手

按键与网页启动都遵循：采集配置准备（麦克风关闭）→ 提交成功 config_ack → 音频连接携带 Voice-Lab-Capture-Ready: 1、Voice-Lab-Recording-Id、Voice-Lab-Revision、Voice-Lab-Boot-Id → 服务端确认本次配置已应用且接收队列已建立 → 回传 accepted 及 captureReadyVersion / recordingId / revision / bootId → 设备校验完全一致后播报并开启采集。超时、旧配置、旧连接或身份不一致均不采集。这里的就绪表示接收入口就绪，识别 Provider 仍由服务端 VAD 按需启动。

麦克风数据保持 16 kHz / 单声道 / PCM16，每 20 ms 一帧。完整录音保存在服务端；设备只使用有上限的短时 RAM 传输队列，不保存完整录音。

发送调度：新采集帧优先排空。应用层每轮最多重发最旧的一帧以取得累计 ACK，已有新 PCM 排队时不重放旧窗口；TCP 本身负责可靠有序传输。维持现有四秒 RAM 上限和错误显式上报，不以扩大缓存掩盖网络持续不通。

当前网络对照：Voice Lab standalone 的 Wi-Fi STA 采用 20 MHz 带宽；修改只作用于设备，不改动路由器。2026-09-10 的 40 MHz 实测出现频繁 TCP 重传与发送停顿，此配置需要重新做实机持续录音验收，不能仅据理论参数宣称修复。

BOOT 配网回退（2026-09-10）：手动进入 AP 后等待 5 分钟，再尝试 NVS 中保存的网络。没有旧网络时保留 AP；旧网络在 60 秒连接窗口内不可用时重新开放 AP。提交/验证 Wi-Fi 的 HTTP 请求与超时退出互斥，避免验证中关闭热点；验证成功后才保存 Wi-Fi，并正常退出 AP。配网不清除设备登记凭据。
