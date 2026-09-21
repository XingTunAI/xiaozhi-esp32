# P4 手动关闭热点后的 ASR 时长差

用户报告转写比录音少近一分钟，并确认中间手动关闭过热点。本轮只读取设备状态和服务器证据，没有启动新录音、调用新的 ASR、改变配置或部署。

目标为 P4 2026-09-21 11:08:25（北京时间）开始的 Run `e6ac18bd-8dc7-56d9-9203-f55dd304c7e1`。

| 层次 | 音频秒数 | 证据 |
| --- | ---: | --- |
| 设备采集 / 完整 TF 归档 | 199.24 | 3187840 样本，16000Hz mono PCM16 |
| 实时服务端入口 | 160.42 | 时间轴 0–150 秒、188.82–199.24 秒 |
| 实际 ASR 输入 | 135.92 | 三段分别 18.50、107.00、10.42 秒 |

实时入口缺口为 150.00–188.82 秒，共 38.82 秒，服务器队列丢帧计数为0；设备状态上报 realtimeAudioGap=true，与用户断热点操作相符。服务器没有收到这一段实时音频，不能把各 Provider 子 Session 内 missingFrames=0 解释为整段录音无缺口。

已收到的实时音频中，另有24.50秒未进入 Provider，位于 VAD 切分范围外；仅凭 VAD 标记不能保证这些片段完全没有语音。两部分差值合计63.32秒。

停止后，设备自动完成 TF 补传。服务器 11:13:27 报 complete=true、receivedBytes=6375680、localBytes=0，Run 状态 completed / offline_audio_durably_saved。

直接读取完整归档 WAV 验证：199.24秒、3187840帧、单声道、16bit、16kHz。其前150秒及最后10.42秒与实时入口对应数据 SHA256 一致；中间38.82秒存在1242240字节PCM（1079271个非零字节），不是用静音占位拼齐的文件。

结论：本次完整音频已经恢复，实时转写缺口没有自动补齐。现有策略仅在停录空闲时补传、持久化确认后删除TF分段，不自动触发补转写，详见 [空闲归档策略](2026-09-20-idle-archive-upload.md)。不将“归档 completed”当作“ASR 覆盖完整”。

本地原始诊断：`tmp/p4-asr-current-run.json`、`tmp/p4-asr-gap-evidence.json`、`tmp/p4-asr-recovery-status.json`、`tmp/p4-asr-full-archive-check.json`。服务器访问使用 deploy-private-server 技能的固定主机校验和只读管理员 API，未输出凭据。

## 开头没有文字的进一步核对

相对于本段采集起点，0–20.5秒的500ms入口记录都为 vadSpeech=false / quiet；21秒开始出现语音判定，21.5秒进入 active。第一段 Provider 输入从20.00秒开始（包含前滚），所以前20秒有录音但根本未送给ASR。第一条最终文字时间为35.852秒，20秒至首句之间是已经送入Provider但未产生更早文字的范围，不能全部归因于VAD。

开头多数整秒RMS约−42至−69dBFS，夹杂第4秒、第10秒等瞬时高能量；只有电平和判定证据，没有试听证据，不能断言是静音、噪声、低声说话或确定的VAD误判。也不能称为麦克风前20秒未启动或用中途关热点解释开头。证据保存于 `tmp/p4-asr-start-evidence.json`。


仓库归档说明：本文中的 tmp/、local-build-backups/ 和相邻 voice-lab 工程路径相对于本机 voice-lab-adapter 工作区；这些原始证据未纳入本仓。最新 USB 界面版本见 [同步与验证记录](2026-09-21-p4-usb-baseline.md)。
