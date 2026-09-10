# 微雪无屏语音样机：2026-09-09 固件改动与联调

## 硬件范围

用户确认：ESP32-S3-AUDIO-Board，只有板载麦克风、喇叭、RGB 和按键；没有屏幕和摄像头。
RESET 是硬件复位，电池开关是硬件电源开关，不给它们分配软件功能。
K1/K2/K3 通过 TCA9555 的 IO9/10/11 读取，BOOT 使用 GPIO0；不改变现有硬件引脚。
依据：[官方文档](https://docs.waveshare.net/ESP32-S3-AUDIO-Board/)、[1.1 原理图](https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf)。

这是开发联调版本，代码检查和构建不能代替烧录及真机验收。

## 本版交互

| 操作或状态 | 设备行为 |
| --- | --- |
| K1 / K3 短按 | 喇叭音量加 / 减 10；不改麦克风增益 |
| K2 待机长按约 2 秒 | 发送开始会议录音请求，等待服务端授权，10 秒无指令则提示失败；待机短按不开始 |
| 网页开始 | 收到新的授权配置，音频通道接受后，播报“开始会议录音”；播报播放完并留 150 ms 间隔后才采音；收到第一帧后确认开始 |
| K2 录音中短按 | 暂停 / 继续同一条录音；暂停时麦克风停止、黄色常亮，继续播报完成后恢复蓝灯采集 |
| K2 长按约 2 秒 | 结束当前录音或暂停的会话，发送尾部音频，等待服务端确认；设备停止和文本整理分别表示 |
| BOOT 长按 3 秒 | 进入重新配网；不清设备凭据和正式服务入口 |
| BOOT 短按 | 配网中重播提示和热点密码；其他状态不改变录音或触发配网 |
| 无已保存网络 | 自动进入配网，播报提示和本机独立密码 |
| 配网热点 | `XingTun-Audio-XXXX`，WPA2，独立随机 8 位数字密码；每次窗口 10 分钟，超时关闭，长按重开 |
| 手机配网 | 只填写 Wi-Fi 网络及密码；浏览器手动地址 `http://192.168.4.1`，仅在连接设备热点时使用 |
| 联网成功 | 播报“已连接网络”；连接服务成功前不能视为可开始 |
| 上电 / 重连 | 不自动恢复旧录音；服务端须先同步 idle，再下发新的开始 revision |
| 失联或上限 | 断连立即关麦；控制静默 15 秒、租约到期或客户正数时长上限到期均独立停录；管理员录音持续至结束指令或再次按键 |

热点密码只保存在 `wifi_provision/ap_password`，通过现场喇叭播报；不进入日志、HTTP、设备状态 JSON 或公共手册。
配网成功后约 2 秒关闭 AP，提醒手机切回互联网。错误密码可重试，超时不会无限重新开放热点。
当前 K3 能将音量降到 0；若听不到提示，按 K1 提高音量，再在配网模式重播密码。

灯效按用户原有习惯调整：黄色慢闪配网，黄色流动连接 / 请求中 / 收尾，黄色常亮暂停，低亮绿色常亮待机，蓝色常亮录音，红色慢闪错误 / 警报。
按官方原理图驱动 7 颗灯。官方出厂例程 `rgb_led_driver.c` 实际使用 `LED_STRIP_COLOR_COMPONENT_FMT_RGB`，本板独立模式显式采用 RGB，其他板默认 GRB；不能以该例程遗留的 GRB 注释为准。

## 音频与状态修复

- `desired_config` 在有界队列的工作任务处理，验证 revision，去重且拒绝过期指令，活动录音不能被另一次开始替换。
- 更高 revision 的 idle 在网络接收回调中提前关闭采集入口，启动流程在连接、播报和开启麦克风前复查取消状态；停止不需要等待开始提示播完才生效。
- 每条开始指令从入队到取得录音锁、开启采集、发送确认都保留来源连接代次；等待停止收尾期间发生重连，旧指令会失效，不能借用新连接的授权。
- 按键请求的 10 秒超时由独立定时器处理，网络写入或控制任务等待不会延迟失败反馈；已超时的发送结果不能恢复请求中状态。
- 每次启动不再重置同一 bootId 下的音频序号，避免后端把第二轮录音开头当成重复包。写入失败也保留已分配的序号。
- 麦克风回调只进入有界 PCM 缓冲，不执行网络发送；网络工作任务保留未确认包，按原序号有限重传。
- 正常停止等待输入线程完成已在读取的最后一帧，再提交剩余缓冲；截止包在 `end` 前有重传窗口，之后等待最终 ACK 和音频 `completed`，最多约 10 秒。
- 服务器丢帧、断电 / 断网、缺少最终确认或停止屏障超时均标记中断；音频确认不表示转写已经完成。
- 独立模式待机不再自动开启唤醒词，MCP 直接开始不能绕过控制通道授权。已有播放接口在录音或启动期间拒绝插播。
- 开始前取消已有 URL 播放，并在最多 6 秒内等待下载线程退出、解码队列与扬声器输出排空；未排空则拒绝开始。延迟执行的播放请求也会复查录音状态。
- 专用构建跳过屏幕、背光和摄像头初始化，以语音与 RGB 提示为主。
- Wi-Fi station 事件通过 16 项有界队列离开 ESP 事件线程后再通知应用，避免切换配网时注销事件与管理器锁互相等待；旧连接事件按代次丢弃，关键事件溢出显式报告中断后同步连接状态。
- 独立模式在 NVS 空间或版本异常时停止启动并保留数据，交由内部维护恢复；不再自动擦除整个分区导致设备配对身份丢失。

## 控制台必须配合的接口

按键通过以下事件申请录音，不能自行创建测试或激活音频通道。相邻 Voice Lab 服务端正按同一合同补上客户权限、配额与会话控制；部署状态及真实联调须单独确认。

```json
{
  "type": "event",
  "eventType": "recording_start_requested",
  "requestId": "<bootId>-<deviceUptimeUs>",
  "mode": "meeting_live",
  "expiresAfterMs": 10000,
  "bootId": "<bootId>"
}
```

服务端需要校验设备绑定、客户可用性和测试独占，创建与客户正确关联的测试，再经现有控制通道下发新的 `desired_config`。
请求仅用于申请，不能把设备事件直接当成客户授权。重复 requestId 不能创建两次测试；服务端须落实请求有效期及拒绝反馈，固件另有独立的本地超时。
网页开始仍使用既有 `desired_config`；连接后的首个 active 快照会得到 `idle_required_after_connect`，应先结束旧测试并同步更高 revision 的 idle。

状态增加 `captureState` / `finalizationState`，其中 `audio_confirmed` 只表示音频收尾已确认，`interrupted` 表示结果可能不完整。
控制台需要据此呈现实际状态，尤其是本地按键停止和设备超时停止；不能只根据网页按钮推断设备状态。

**2026-09-09 新增客户租约固件时尚未烧录与真机验收。** 当时旧管理指令保留 60 秒硬上限；2026-09-10 后续按用户要求改为管理员持续录音，见本文末尾。客户正数时长上限不会被 ping 或重复开始延长。
完成后端部署和 session / lease 联调后，才能提供更长会议；不能仅凭构建成功作为长会议产品发货。
客户账号隔离、设备分配、结果归属、下载、公开手册和二维码也仍由控制台交付与联调验证。

### 客户会话租约合同 v1

`hello` 顶层与 `capabilities` 均声明 `recordingControlVersion: 1`。新客户录音在 `desired_config` 中附加：

```json
{
  "recordingAuthorization": {
    "schemaVersion": "voice-lab-recording-lease-v1",
    "sessionId": "<server-session-id>",
    "requestId": "<request-id>",
    "source": "device",
    "bootId": "<boot-id>",
    "leaseDurationMs": 30000,
    "maxDurationMs": 3600000
  }
}
```

`source` 可为 `device` 或 `web`。两者均要求当前 bootId；device 来源还必须匹配尚未到期的本机 requestId。授权仍须先经过连接 idle 同步和 revision 检查。租约从收到命令时开始计时，等待音频握手或播放提示会消耗租约；队列延迟不能延长它。客户最大时长允许 60 秒至 1 小时；2026-09-10 新增管理员 `maxDurationMs: 0`，表示不设固定总时长。两类租约均允许 1 至 30 秒，服务端正常使用 30 秒。

服务端可发送 `type: recording_request_result`、`requestId`、`accepted`、`reasonCode`、`message`、可选 `sessionId`。只有匹配当前有效请求的拒绝回执会立即结束等待并播报现有“开始失败”提示；接受回执本身不能开启麦克风，仍须收到完整的授权配置。过期、重复或错配回执不会恢复请求。

正在采集时，固件每最多 10 秒发送 `type: event`、`eventType: recording_lease_renew_requested`、`sessionId`、`bootId`、整数 `renewalId`。回应为 `type: recording_lease`，并返回同样的 sessionId / bootId / renewalId 和 `leaseDurationMs`。回应须在该次申请后 10 秒内到达，且旧租约尚未到期；新截止时间以申请发送前的本机单调时刻为准，不以回应到达时刻为准。重复、错序、跨会话、无申请及过期回应均无效，不能超过本次会话最大时长。

`type: recording_stop` 携带匹配的 `sessionId` 和 `bootId` 时立即关麦，串行工作任务继续完成已有截止包和尾部确认流程。旧会话的停止不能影响新会话。所有客户采集状态均携带 sessionId 和 bootId：

| 状态 | 服务端含义 |
| --- | --- |
| `recording: true` | 音频接受、提示播放和首帧采集均完成 |
| `recording: false, captureState: stopped, captureSampleEnd: <样本截止点>, finalizationState: pending` | 麦克风已停；截止点包含尚未打包的 PCM，与音频 ACK 的 sampleEnd 共用坐标。服务端仅在有界窗口接收不超过该截止点的尾包 |
| `captureState: stop_failed, finalizationState: pending` | 输入线程停止屏障未确认，不提供可授权尾包的截止点；最终结果标记中断 |
| `finalizationState: audio_confirmed` | 本次音频截止已确认；不代表 ASR / 文本已整理完成 |
| `finalizationState: interrupted` | 音频未完整确认或租约 / 连接中断，结果可能不完整 |

断连和 15 秒服务静默的既有保护继续生效，重连不会恢复旧会话。服务端仍须自行执行租约、归属和音频闸门校验，不能依赖固件自报授权。

## 构建与验证

新增发布变体：`esp32-s3-audio-board-voice-lab-audio-only`。原屏幕板型变体保留。
本机使用已安装的 ESP-IDF 6.1；工程建议优先 6.0.2，本次未换装 SDK。

```powershell
cd C:\QIU\XingTunAI\voice-lab-adapter\xiaozhi-esp32
$env:PYTHONUTF8 = '1'
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
python scripts\build.py waveshare/esp32-s3-audio-board --name esp32-s3-audio-board-voice-lab-audio-only --language zh-CN --wake-word disabled
```

发布变体的默认服务为 `voice-lab.cloud:443` / TLS；已有 NVS 配置优先，烧录不会自动修正旧服务器设置，也不清除身份。
这里的身份保持指使用 IDF 的分段烧录命令；不要给已配对样机执行整片擦除，也不要把合并镜像从地址 0 整段覆盖，否则可能覆盖 NVS。
确认编译目标后，可使用 `idf.py -p COM30 flash monitor` 分段烧录。2026-09-09 已按用户要求烧录 COM30，记录见下。
设备仍需在内部预登记 / 配对；客户配网页不承担写入配对码的工作。内部一次性配对可用已有 USB `vl pair`，不得把短码印到盒内或写进源码。

项目自有 Wi-Fi 组件位于 `local_components/esp-wifi-connect`，通过 `main/idf_component.yml` 的 `override_path` 使用，未手工修改托管依赖目录。
录音控制 6 组测试执行真实 C++ constexpr 逻辑，包含启动时收到停止；主机测试共 73 项全部通过，无跳过。修复了原测试在 Windows 当前目录内删除临时目录的清理问题。
语音文件由本机 Microsoft Huihui Desktop 离线合成，使用 Ogg Opus / 16 kHz / mono / 20 ms；生成脚本为 `scripts/generate_voice_lab_prompts.ps1`。

2026-09-09 灯色校准后的无屏版本构建通过，应用镜像大小 `0x2538a0`，应用分区余量约 41%。以下是最近已烧录的灯色版本历史哈希；新增租约代码构建会更新本地 build 产物，但未自动烧录（尚未完成全部发货验收）：

| 产物 | SHA-256 |
| --- | --- |
| `build/xiaozhi.bin` | `a84d12df2e438112e20a377c707a30dd85fe40f941e1e2cbee9eea135e2bede7` |
| `build/merged-binary.bin` | `4a808692678d935158bcac1b3bb192ca2f18f00f040fe4e6ec770c01a963a263` |

本机日志：`../tools/waveshare-build-2026-09-09-verified.log`、`../tools/waveshare-tests-2026-09-09.log`。

还完成了原带屏 / 摄像头分支的兼容构建（同为 ESP32-S3 / Wi-Fi，`VOICE_LAB_STANDALONE_MODE=n`，关闭唤醒词），应用分区余量约 29%。此验证使用独立目录 `../tools/waveshare-display-build`，日志 `../tools/waveshare-display-build-2026-09-09.log`。当前默认 `build/` 已确认为无屏客户变体；未验证其他芯片、蜂窝网络、唤醒 / AEC 真机路径。

## 2026-09-09 COM30 烧录与启动检查

用户确认设备接在电脑并授权烧录。实机为 ESP32-S3 rev v0.2，8 MB PSRAM，MAC 尾号 `B2:AD:EC`。
首次烧录应用 SHA-256 为 `0356cb0448c9b5f81cf115198dfb7c924378394a2e642666dafbadf06f7b8fb1`；后续灯色校准版本见上表及下节。

- 烧录前读取实机分区表，与当前固件完全一致；NVS 为 `0x9000` / `0x4000`。
- 本地保留配置区恢复备份，不打印或复制凭据到文档。按照 `build/flash_args` 写入启动程序、分区表、OTA 初始数据、资源和应用，共 5 段；每段写入后的哈希校验均通过。
- 应用首次启动前，再读 NVS 并比较 SHA-256，与烧录前相同，确认烧录没有改变 Wi-Fi / 配对配置。
- 重启识别 SKU `esp32-s3-audio-board-voice-lab-audio-only`，ES8311、ES7210 和双工音频初始化完成；跳过小智云协议启动。
- 约 8.5 秒自动连回已保存 Wi-Fi，约 9.4 秒完成 Voice Lab WebSocket 握手并进入“待机，请在网页开始测试”。保留的服务端口为 1883，实测可连接，未强制覆盖成构建默认 443。
- 观察 45 秒启动日志，未见 panic、断言、反复重启或按键初始化失败。结束后已释放 COM30。
- 本次通过 USB Serial/JTAG 观察日志；发送 `vl status` 未收到命令响应，因此没有将该命令作为状态验证依据。若后续需要 USB 写入配对码，须先确认实际 stdin 串口路由。

日志位于 `../tools/waveshare-flash-2026-09-09.log` 和 `../tools/waveshare-boot-2026-09-09.log`，后者已过滤凭据相关信息。用户已确认听到联网播报，首次灯色异常在下节处理。

## 2026-09-09 灯色校准与用户确认

用户反馈联网后灯环常红，期望沿用“绿色待机、蓝色录音、红色错误 / 警报”。串口记录仍为待机，没有开始采集的记录。
[官方出厂例程](https://files.waveshare.net/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board-Demo.zip)
中 `ESP-IDF/factory_01/main/rgb_led_driver/rgb_led_driver.c:49` 实际配置为 RGB，原有公共驱动默认 GRB，导致红绿互换。

- CircularStrip 新增可选通道顺序参数，默认仍为 GRB；仅本板 Voice Lab 独立模式显式选择 RGB。
- 按用户习惯固定：绿常亮待机、蓝常亮录音、红慢闪异常；黄慢闪配网、黄流动连接 / 请求中 / 收尾。
- 每次状态切换记录 `indicator`、`recording`、`control_connected`，不记录凭据。
- 修复初次开机扫描被当成断网的问题。仅跳过尚未联网且未录音的 starting 阶段扫描事件；运行中的断网和重连故障仍保留异常处理。
- 修正版只更新 `0x20000` 应用段并通过写入哈希校验；启动前确认 NVS 与更新前相同。两个板型分支构建及 73 项主机测试通过。
- 最终启动日志只有 `connecting recording=0 control_connected=0` → `ready recording=0 control_connected=1`，约 8.3 秒进入待机，没有中间错误指示；观察 35 秒无 panic 或反复重启，COM30 已释放。
- 用户现场明确确认：“是，绿色常亮”。联网播报和待机绿灯已验收；蓝色录音、红色异常及黄色配网仍需在对应实际场景确认。

最终日志：`../tools/waveshare-build-rgb-2026-09-09.log`、`../tools/waveshare-tests-rgb-2026-09-09.log`、`../tools/waveshare-flash-rgb-final-2026-09-09.log`、`../tools/waveshare-boot-rgb-final-2026-09-09.log`。

剩余真机验收：

- 手机错误密码、正确配网、10 分钟超时、长按重开，以及换网后设备身份保持。
- 配网提示及 8 位密码可听清、重复播报、最低音量恢复和七颗灯的其他状态色序。
- 网页启动、按键请求获准 / 拒绝 / 超时、提示语不进入正式录音。
- 连续两次测试、重复开始、结束前最后一句、断网 / 服务静默停采、重启不恢复旧测试。
- 正常尾部确认与中断结果在网页分别显示，手机 / 电脑能下载正确文本。

禁止以编译成功代替这些验收，冻结发货版本前应记录镜像 SHA-256、烧录结果和设备编号。

## 2026-09-09 客户租约改动验证（未烧录）

- 新增 `voice_lab_recording_lease.h`，主机测试直接编译真实 C++ 单调时钟逻辑，覆盖请求匹配 / 到期、接收延迟、续约申请时刻、重复 / 错序 / 过期回应、会话最大时长与跨会话失效等 7 组情形。
- 主机测试共 80 项通过，无跳过；`voice_lab_client.*`、新增录音 guard / lease、语音提示、板级按键 / UI 头文件及录音 guard / lease C++ 测试通过 clang-format 检查。其他触及的 application、audio、公共 Wi-Fi 和原板级源文件仍有格式问题，未声明全部修改文件通过格式检查。
- 原已跟踪文件的 `git diff --check` 无空白错误；首次暂存 `local_components` 后，`git diff --cached --check` 检出旧工程页面 `wifi_configuration.html` 的 33 处、`wifi_configuration_done.html` 的 1 处已有行尾空白。本次保留这些已构建的上游页面内容，未声明全部暂存文件通过空白检查。
- ESP-IDF 6.1 无屏发布变体完整构建及截止点字段增量构建通过，应用 `0x254780`，分区余量 41%。独立带屏兼容目录构建也通过，应用 `0x2cb270`，分区余量 29%。
- 停止状态新增 `captureSampleEnd`，在输入屏障后锁住发送计数与 PCM 缓冲一次计算，包含尚未打包的尾部样本；最终 ACK 也须覆盖这个相同截止点。停止屏障失败时报告 `stop_failed`，不声明可授权尾包的截止点。
- 未使用 COM30，未烧录本次租约固件，未启动实际录音。实机仍是前述已验收绿色待机的版本。长会议、网页授权续期、按键拒绝提示和断线收尾仍须与新服务端一同验收。

| 本次无屏构建产物 | SHA-256 |
| --- | --- |
| `build/xiaozhi.bin` | `4d7ec3c4c536ec43fcdc433ba275f574c7db0a20c1fb06eb69bf9a315100ce32` |
| `build/merged-binary.bin` | `5c9d277ce25a908de78f5cb4f83f8a97260cc2f98813627560ceee6f05fb4a1b` |

日志：`../tools/waveshare-tests-lease-2026-09-09.log`、`../tools/waveshare-build-lease-2026-09-09.log`、`../tools/waveshare-build-lease-final-2026-09-09.log`、`../tools/waveshare-display-build-lease-2026-09-09.log`。

截止点补充后的复测日志：`../tools/waveshare-tests-lease-cutoff-2026-09-09.log`、`../tools/waveshare-build-lease-cutoff-2026-09-09.log`、`../tools/waveshare-display-build-lease-cutoff-2026-09-09.log`。80 项主机测试与两分支构建再次通过，仍未烧录。

## 2026-09-10 租约固件实机更新

用户授权后，已将上述租约应用烧录到 COM30 的 Waveshare 样机（MAC 尾号 `B2:AD:EC`）。本节取代上节“未烧录”的当前状态，保留上节作为历史构建记录。

- 固件源码提交 `97c9a8fa95777149b735efd07a6db9b6e939e852`；应用 SHA-256 为 `4d7ec3c4c536ec43fcdc433ba275f574c7db0a20c1fb06eb69bf9a315100ce32`，大小 2443136 字节。
- 仅写入 `0x20000` 应用段，写入后哈希校验通过；未擦除或写入 NVS、分区表及其他串口设备。
- 重启后自动连回已保存的 Wi-Fi 和 `wss://voice-lab.cloud:1883`，进入待机；服务器收到 `recordingControlVersion=1`。
- 启动日志 ELF SHA-256 为 `fea2bc92626c3c7ed326735203878cda9f408cebb146192df3f015e932e35c57`。新旧构建描述时间相同，不能用该时间判断是否更新成功。
- 排查开始录音红灯时，发现服务端在约 3 秒重试后强制断开，而设备启动提示与音频连接需要更长时间。配套服务端已延长为完整 12 秒确认窗口，并修正重连先同步待机及控制/音频 bootId 的格式比较。
- 本次遵照用户要求未重复本地主机测试。烧录和联网检查已完成，K2 开始/停止及实际音频仍以本次现场验收结果为准。

烧录与启动日志：`../tools/waveshare-flash-lease-2026-09-10.log`、`../tools/waveshare-boot-lease-2026-09-10.log`；开始失败诊断日志：`../tools/waveshare-start-diagnosis-2026-09-10.log`、`../tools/waveshare-start-diagnosis2-2026-09-10.log`。后续串口观察使用只读监视器，不向诊断 USB 端口写命令或触发复位。

## 2026-09-10 管理员持续录音与按键结束关联

用户要求网页或录音键启动后持续录音，直到结束指令或再次按键。本次取消不带租约的管理员指令原有 60 秒固定上限；内部总时限 `0` 表示不设固定总时长，状态字段 `legacy_recording_limit_seconds` 返回 `0`。

带管理员授权的按键会话可使用 `maxDurationMs: 0`，仍须按 1–30 秒租约持续续期。未续期、断连、控制静默 15 秒或音频无法确认都会停止；取消固定总时长不取消这些保护。服务端负责只向允许持续录音的管理员会话授予 `0`，客户正数上限继续保持，并在后续客户会话开始时重新生效。

管理员网页开始、K2 长按结束也需要准确关联实验。固件在收到开始授权时保存 `recordingId = desired_config.test.correlationId` 与 `recordingRevision = 开始指令 revision`，并在以下状态中返回同一快照：

- `recording: true`。
- `captureState: stopped / stop_failed, finalizationState: pending`。
- `finalizationState: audio_confirmed / interrupted`。

三类状态继续携带 `bootId` 和存在时的 `sessionId`。结束或重连时不得将快照替换为更新的 idle revision；服务端应严格检查连接、bootId、recordingId 和 recordingRevision，保留 pending 阶段的尾包接收，最终状态再结束对应实验。

本次新增 constexpr 测试场景覆盖：无固定总时长持续续期超过 1 小时、停止续期仍按租约到期、切回客户会话后正数上限恢复，以及非法短时长被拒绝。遵照用户要求，未运行本地主机测试。此节源码更新尚待单独构建、烧录及网页 / 按键真机验收，不能沿用上一节镜像哈希表示本次产物。

### 同一会话暂停 / 继续与正常结束

最新 K2 操作为长按约 2 秒开始 / 结束，短按暂停 / 继续；重新配网保留 BOOT 长按 3 秒。
暂停先停止输入线程、发送已采集前缀，并等待音频 ACK 覆盖截止点后报告 `captureState: paused, paused: true, recording: false, captureSampleEnd`；不发送 `end`，不关闭音频 WebSocket，不改变会话、起始 revision、样本或序列号。确认期间黄色流动，服务端确认后黄色常亮并播放 `vl_recording_pause.ogg`。
继续时先在麦克风停止状态播放 `vl_recording_resume.ogg` 并留 150 ms 间隔，再报告 `captureState: recording, paused: false, recording: true` 和原开始身份，收到对应确认后才启用采集。

每次暂停 / 继续状态携带递增正整数 `transitionId`。服务端完成暂停前缀处理或恢复后，返回 `type: recording_pause_ack`，回显 `transitionId`、`paused`、`recordingId`、`recordingRevision`、`bootId` 和存在时的 `sessionId`，并提供布尔 `success`。固件在控制接收回调中即时核验全部字段和连接代次，等待上限为 10 秒；旧确认、重复确认及跨会话确认无效。拒绝或超时明确中断并进入收尾，不能未获确认就恢复麦克风。用户正常结束打断等待时，交给结束流程处理。

暂停期间会话仍活跃，继续续期并检查控制失联。租约请求同时由独立心跳任务驱动，租约回包即时处理；等待暂停确认和播放提示不会阻塞续期处理。客户正数总时长照常计算，暂停不延长客户授予的上限。

暂停 / 继续状态不携带 finalizationState。服务端需按状态中的开始身份同步网页，保护已确认前缀，不能因跨控制 / 音频通道到达顺序不同而丢弃已采集音频。
本版本另在 hello 及 capabilities 声明 `recordingFinalizationVersion: 1`。网页结束采用两阶段：先让设备停止采集并确认尾包，收到携带原开始快照的最终状态后才结束对应实验；重发的 idle revision 不得替换该快照。

### WebSocket 正常结束后的堆损坏修复

实机已发现音频 completed 后析构与接收回调竞争，导致返回待机后重启。本次通过 `local_components/esp-ml307` 项目维护副本修复：所有 WebSocket 缓冲释放前先关闭并等待传输接收退出；TCP / TLS 接收退出用最后一次对象访问的原子标记，等待期间不占发送锁，随后等待发送完成再释放连接。来源、许可和线程约束见该目录 `UPSTREAM.md`。
