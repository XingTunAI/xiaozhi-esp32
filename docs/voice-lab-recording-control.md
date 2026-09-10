## 2026-09-10 采集就绪握手

按键与网页启动都遵循：采集配置准备（麦克风关闭）→ 提交成功 config_ack → 音频连接携带 Voice-Lab-Capture-Ready: 1、Voice-Lab-Recording-Id、Voice-Lab-Revision、Voice-Lab-Boot-Id → 服务端确认本次配置已应用且接收队列已建立 → 回传 accepted 及 captureReadyVersion / recordingId / revision / bootId → 设备校验完全一致后播报并开启采集。超时、旧配置、旧连接或身份不一致均不采集。这里的就绪表示接收入口就绪，识别 Provider 仍由服务端 VAD 按需启动。

麦克风数据保持 16 kHz / 单声道 / PCM16，每 20 ms 一帧。完整录音保存在服务端；设备只使用有上限的短时 RAM 传输队列，不保存完整录音。

发送调度：新采集帧优先排空。应用层每轮最多重发最旧的一帧以取得累计 ACK，已有新 PCM 排队时不重放旧窗口；TCP 本身负责可靠有序传输。维持现有四秒 RAM 上限和错误显式上报，不以扩大缓存掩盖网络持续不通。
