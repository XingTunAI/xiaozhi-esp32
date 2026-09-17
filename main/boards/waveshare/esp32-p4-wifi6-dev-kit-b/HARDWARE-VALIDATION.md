# P4 整板适配与验收

此表针对 Rev1.2 KIT-B，不能用于无屏 S3 录音板。硬件依据为 Voice Lab 归档原理图与官方例程。控制器初始化不等于物理外设及长期业务验收。

| 模块 | 配置与复用 | 验收边界 |
|---|---|---|
| 屏幕 | ILI9881C，I2C 0x45 控制电源/背光，DSI，横屏旋转 270° | 彩条现场确认；LVGL 上板启动通过；刷新撕裂仍待专项验证 |
| TP | GT9xx，0x5d，LVGL 统一旋转坐标 | 已识别；按钮和边缘坐标待现场点击 |
| Ethernet | IP101，PHY 地址 1，MDC31/MDIO52/RESET51，原项目 network.c 初始化基线 | 上板初始化、链路和 DHCP 已通过；120 次局域网 ping 收到117次，长稳未验收 |
| ES8311 | I2C7/8，MCLK13/BCLK12/WS10/DOUT9/DIN11，PA53 | 真机复位与驱动初始化通过；采样与可听播放待验证 |
| microSD | slot0，CLK43/CMD44/D0=39/D1=40/D2=41/D3=42，低有效电源45，LDO4 | 不格式化、不创建测试文件；需插卡后验证挂载和读写 |
| C6 Wi-Fi | slot1，CLK18/CMD19/D0=14/D1=15/D2=16/D3=17，RESET54 | 真机扫描15个AP；启动握手有主从版本不匹配警告；此前数据断流未修复 |
| USB Host | 高速控制器、CH334 Hub；H3 置 HOST、外设接 USB-A | 主机及 UAC 驱动真机安装成功；Hub实体枚举、音频外设与插拔待测 |
| UAC | espressif/usb_host_uac 1.4.0；识别输入/输出接口及格式，连接事件在独立有界队列处理 | 支持 UAC1；未接设备时不能验证流，未实现业务音源切换；不自动启动麦克风 |
| 摄像头 | MIPI CSI 接口 | 型号与实物接入待确认，尚未初始化 |

## UAC 后续接入

1. 提供声卡型号，接入后记录 VID/PID、UAC 版本、输入输出声道数、位宽及采样率。
2. 优先验证设备实际支持的 PCM 格式，再接 AudioCodec 输入/输出与既有 AudioService 重采样流程。不能假设所有设备支持 24 kHz。
3. 分别验证采集、播放、双工、拔插重连、队列上限和连续运行，再与显示和网络做并发验收。
4. 网络与媒体协议沿用 Voice Lab 的控制/媒体独立通路、序号、ACK 与尾包确认，不另写业务协议。

## 本机证据

构建、刷写、串口日志保存在 `C:/QIU/p4-screen-20260916/`。16 MB 完整备份在 `backup/pre-ui/`，包含设备数据，不提交 Git。首次倒置 UI 已验证启动；外设扩展版的实际结果须在上板后补充。

主机测试：67 项，结果 OK，2 项因主机 C++ 编译环境缺失跳过。新增模块需要目标板构建与实测，主机测试不能代替它们。

## 外设扩展版结果

- 应用 SHA-256：ad7925dc6b8da9244da2c56b43f595aa6ea472ef768c8b79f628c4c9202a5487；应用大小3830032字节。COM32应用段写入哈希通过，55秒启动观察无崩溃。
- 日志：flash-peripherals.log、boot-peripherals.log。有线IP为192.168.5.80，此地址仅表示本轮DHCP结果。
- 网口测试明确绑定电脑WLAN实际地址192.168.5.55；先前选择的以太网192.168.5.153属于断开网卡，其失败不算设备故障。10次短测全部回复；追加120次收到117次，最小1ms、最大442ms、平均48ms。电脑本身经过Wi-Fi，此结果不能单独定责PHY。
- 用户确认没有SD卡，稍后可能插卡；初始化超时与当前实物一致，不记为驱动已损坏。

## 设备设置页面

入口为首页右上角“设备设置”，分为网络配置、版本信息、存储与音频。

- 网络状态来自线程安全快照；UI不等待网络RPC。操作通过有界任务队列提交。Wi-Fi扫描最多等待30秒，连接等待25秒，成功后复用SsidManager保存配置；凭据不打印到日志。支持手动SSID及扫描选择，密码使用隐藏输入。当前配置页面向个人/开放Wi-Fi，不提供企业认证配置。
- 有线DHCP路由优先级高于Wi-Fi，未引入新的Voice Lab协议、云端自动连接或自动OTA。设置页不代表原无线稳定性故障已经修复。
- 版本页区分应用版本/构建时间/ELF标识、ESP-IDF、Hosted主机版本与从C6查询到的实际版本。查询失败或全零显示未知，不伪造版本号。
- SD卡在用户点击检测后挂载，禁止自动格式化或写测试文件；再次检测已挂载卡会只读检查一个扇区。挂载失败保留UI运行。初始“未插入”来自本轮用户确认，不是独立卡检测开关状态。
- UAC显示已打开的逻辑音频接口数量，麦克风与扬声器可能来自同一个物理声卡；数量不等于物理设备数量。没有启动音频流。

## 设置版上板记录

- 应用大小3894272字节，分区余量6%；SHA-256：7575243782c8b8b29c72a16461a1f2a6c3971a46535d3fb635ae8af4c872aee3。
- 应用段已烧入并通过哈希校验。日志：build-settings.log、flash-settings.log、boot-settings.log。
- 45秒启动观察：GT9xx、ES8311、IP101、USB Host/UAC初始化成功；有线DHCP仍为192.168.5.80；C6扫描到15个AP。内部空闲SRAM约93867字节、最低88728字节，观察窗口无崩溃或重启。
- C6的Req_GetCoprocessorFwVersion查询超时；页面显示未提供有效版本，不能认定从机固件版本就是0.0.0。本次没有刷写C6。
- wifi命名空间当前不存在，没有保存的Wi-Fi配置；扫描通过不等于SSID/密码连接、保存重启恢复或无线业务长稳已验收。设置页面与软键盘仍需现场触摸检查。
- 设置版并行对照：绑定电脑WLAN地址192.168.5.55，P4 30次收到28次（2次超时），网关30次全部回复。日志settings-ping-device.log、settings-ping-gateway.log。P4有线路径仍有待定位丢包，不能把DHCP与短测成功当作整板联网验收完成。
- 用户询问蓝屏/频繁重启后，使用watch_settings.py在COM32被动观察150秒，未主动复位。settings-passive-crash.log中uptime从32秒持续到172秒，空闲SRAM94059字节保持不变，未见ROM启动、panic、assert或回溯。未收到用户交互复现确认；此结果只覆盖该观察窗口，不能排除触屏操作触发或显示刷新故障。

## 软键盘定位修复


用户反馈点击输入框看不到键盘。核对LVGL源码，键盘构造默认LV_ALIGN_BOTTOM_MID，原lv_obj_set_pos(280,390)保留该对齐导致键盘超出屏幕。改为LV_ALIGN_TOP_LEFT (280,390)，尺寸960×252，显示时置顶；事件使用current target并在键盘尚未创建时保护空指针。密码不输出日志。

增量构建通过，应用段刷写哈希通过，SHA-256：1d30b3604621a17709c3864eb74e02e5f67000885731afa2d1f714b395d84163。日志build-keyboard-fix.log、flash-keyboard-fix.log。烧录后仅被动监听，不为检查再次主动复位。键盘显示与输入需现场确认。

## 2026-09-17 双 USB Host

用户确认 H2 有 5V，希望保留 USB-A Host 并用 H2 接 UAC。usb_host_config_t.peripheral_map 设置 BIT0 | BIT1，同时启用 HS 和 FS 控制器。IDF v6.1 的 usb_wrap_ll_phy_select(&USB_WRAP, 0) 使用软件寄存器将 FS OTG 切到 GPIO24 D- / GPIO25 D+，不烧写 eFuse；USB Serial/JTAG 随之映射到 GPIO26/27。现用 CH343 UART 烧录不受该映射影响。此配置依赖当前 IDF 的双端口支持与 P4 LL API。

Rev1.2 跳线位置以微雪 FAQ 为准：HOST 对应直连 1 号 USB-A，DEVICE 对应 CH334 的 2–4 号口。此前将 HOST 一概描述为经 Hub 的说法不准确。来源：https://docs.waveshare.net/ESP32-P4-WIFI6-DEV-KIT/FAQ/

H2 的 CC1/CC2 是固定 Rd；软件切换 PHY 不改变 Type-C 的 CC 电阻或供电电路。用户已确认 5V，仍需用实际声卡和线材验证枚举。UAC 目前只枚举/读取格式，不开启录放音流。

补充 Wi-Fi 现场验证：用户拔掉网线，屏幕 Wi-Fi IP 为 192.168.5.116；电脑 WLAN 192.168.5.55 发出 30 次 ping 全部回复，平均 45 ms（3–187 ms）。同时网关 30 次收到 29 次，平均 40 ms。只说明短时无线数据通路可用，未完成音频长稳。

双路版已于 2026-09-17 重新写入 COM32 应用分区 0x20000，3894544 字节，刷写哈希验证通过。应用 SHA-256：9827d44967858ee5e26e4e14433387bd0a00c1d9de924904f329502cb76ac6a7。启动日志确认 USB dual host: HS USB-A + FS H2 (GPIO24 D-, GPIO25 D+)，UAC 1.4.0 安装成功、初始化 ESP_OK。尚无外接 UAC 枚举证据，不表示录放音验收完成。新日志保存在 ../local-build-backups/p4-dual-usb-20260917/flash.log 与 boot.log；原 C:/QIU/p4-screen-20260916 已由用户删除，本文旧日志路径仅保留历史说明。

## UAC 2.0 插入导致循环重启（2026-09-17）

被动串口日志 bluescreen.log 多次捕获 UAC version 0x0200 not supported，紧接 Load access fault，MEPC 0x4814064e，MTVAL 0x34，随后 SW_CPU_RESET。使用当次 ELF 解码定位 usb_host_uac 1.4.0 的 uac_host_device_open:2144：fail 分支在 uac_iface 为空或已释放后访问 ringbuf。用户观察为蓝色闪一下再恢复 UI，与日志中的周期性重启一致。

修复通过板级 uac_cleanup_fix.cmake 在构建目录生成修正后的驱动翻译单元：仅在接口非空时、释放接口之前销毁 ringbuffer。不修改 managed_components；上游代码形态改变时配置阶段报错，要求重新审核补丁。双路 Host 保留，UAC 2.0 仍返回不支持；该修复不等于实现 UAC 2.0 音频流。

1.4.0 崩溃修复固件已先行烧入，带同一 UAC 2.0 设备观察 60 秒：两个接口均返回 ESP_ERR_NOT_SUPPORTED，无 panic 或重启，uptime 达 62 秒，空闲 SRAM 最后为 90171 字节。日志 boot-crash-fix.log。主机测试 67 项通过，2 项因环境跳过。

用户要求升级驱动，已将 main/idf_component.yml 中 P4 的 usb_host_uac 从 1.4.0 固定升级到官方 1.5.0。官方 1.5.0 README 仍明确仅支持 UAC 1.0；此次升级不宣称支持 UAC 2.0。新增 subframe_size 字段不影响本板零初始化后按字段访问的用法；当前尚未开启流。保留已验证的 open 失败清理修复。
来源：https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/readme
变更说明：https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/changelog

## UAC2 移植与资源验证（进行中）

目标设备由用户确认为 reSpeaker Flex XVF3800 圆形阵列，运行 USB 固件。Seeed 文档明确其 USB 为 UAC 2.0，USB 固件存在 2/6 通道版本；不能根据圆形外观断定实际 USB 通道格式。

引入 MIT 开源 Averyy/esp-uac2-host，commit a072adb6a2410f9f93474637a483724c93d5fc6f，保存于 local_components/uac2_host，来源和差异见 UPSTREAM.md。保留 UAC1 1.5.0 驱动与失败路径修复。UAC2 当前限定 full-speed（H2），尚未适配高速微帧调度；不可声称支持任意 USB-A 高速 UAC2。连接后读取格式/时钟，并进行有界 5 秒 PCM 字节统计，不存储或上传音频。

首次移植构建和烧录通过，但 USB Host 0 初始化 ESP_ERR_NO_MEM，随后 Wi-Fi SDIO RX buffer alloc failed、WifiInit 超时，未进入 UAC2 枚举。日志 boot-uac2.log；此版不能作为可用验收。板级配置正在将通用内存内部优先阈值降到 512，内部保留池增加至 128KiB，并增加 SRAM/DMA 最大连续块/PSRAM/当前任务栈水位日志，需实测确认效果。

## PSRAM 绘制缓冲实测（2026-09-17）

实际瓶颈是屏幕绘制/旋转缓冲占用内部 DMA 内存。保留 128KiB 内部预留及 512 字节阈值，并通过 MipiLcdDisplay 的可选 draw_buffers_in_psram 参数，仅在本板将 LVGL draw/rotation 缓冲分配为 DMA+SPIRAM；默认参数 false 保持其他板行为。屏幕刷新视觉效果仍需现场确认。

新版 SHA-256：7e1695e20f375f27b2231af9b9ec19bc4707957cfcc21b435f21610a510caf48，已刷写哈希验证。日志 flash-uac2-psram.log / boot-uac2-psram.log。主机测试 67 项通过（2 项环境跳过）。

| 阶段 | 内部 SRAM 空闲 | 内部 DMA 空闲 | DMA 最大连续块 | PSRAM 空闲 |
|---|---:|---:|---:|---:|
| USB 初始化前 | 190851 B | 152039 B | 139264 B | 27700752 B |
| USB/UAC1+UAC2 初始化后 | 160731 B | 121919 B | 118784 B | 27700104 B |
| Wi-Fi 获取 IP 后 | 156547 B | 117735 B | 112640 B | 27698392 B |
| uptime 47 秒 | 155011 B | 116199 B | 110592 B | 27698392 B |

初始化任务最小栈余量 2832 B（仅该任务，不能代表所有任务）。Wi-Fi 恢复到 192.168.5.116。观察期无 panic，旧版最大 DMA 连续块仅 80 B 的分配失败已解除。

资源结论边界：上述数据覆盖 LVGL、以太网驱动、Wi-Fi、双 USB Host 和两个音频类驱动初始化，不覆盖音频流并发负载。阵列发现 TX iface=1 / RX iface=2，但当前连接速度不是 Full Speed，被此移植版速度保护拒绝，5 秒采集测试尚未执行；不能据此宣称 UAC2 音频工作或 CPU/带宽足够。待用户确认最终使用 USB-A HS 还是 H2 FS，再验证流。

固件应用约 4000096 B，单个 OTA 分区 4128768 B，剩约 128672 B（126KiB，3%）；16MiB 物理 Flash 分为两个约 3.94MiB 应用区和 8MiB assets，不能把整颗 Flash 当作单应用余量。PSRAM 足够容纳当前缓冲，不代表内部 DMA、USB FIFO 或 CPU 负载已验收。实际配置 CPU 400MHz；产品截图的 360MHz 不是本次固件的配置值，芯片版本相关限制另行核对，未进行调频修改。

## UAC2 高速采集通过与 Voice Lab 资源预算

用户确认 USB-A / H2 两种接法都有可能，后续需接 Voice Lab。因此新增按设备速度和 bInterval 计算服务周期：FS 1ms，HS 125–1000us；按服务周期计算音频包长，HS feedback 按微帧单位缩放。拒绝未实现的多事务高带宽端点及更长服务周期，不把“两个接口可枚举”等同于任意格式支持。

烧入版本 SHA-256：968f3b5e12d84eb05f67ac1f7ae8d3237eec42105d7c3cf5f2cc621305107c15。日志 build-uac2-hs.log、flash-uac2-hs.log、boot-uac2-hs.log。

实机设备描述：UAC2，高速，总线地址1；播放接口1/EP01，采集接口2/EP81；双通道16bit（2字节采样槽），MPS32，bInterval3（500us），时钟源1，当前16000Hz。5秒采集收到321504字节，其中255239非零字节，read/stop均ESP_OK；约64KB/s，与16000×2×2的理论有效载荷相符。只统计字节，没有存储/上传音频。时钟控制位0x05表明采样率控制只读，现驱动先SET遇STALL再GET确认同为16000Hz后正常启动；未误认为采样率可任意调整，后续可优化只读时钟请求。

采集开始：SRAM156463 B，最小154344 B；DMA117651 B，最大连续112640 B；PSRAM27689216 B；采集任务最小栈余量4160 B。采集期间包含C6初始化和扫描，结束后Wi-Fi成功获得192.168.5.116；网络初始化后SRAM155283 B（最小150472 B），DMA116471 B（最大连续110592 B），PSRAM27697192 B。

该阶段结论：HS 格式的短时 UAC2 采集、LVGL 显示驱动和网络初始化已共存；当时尚未接入 Voice Lab。H2 FS 接线、播放/音质/热插拔/长稳/CPU 占用仍未完成验收。

## 2026-09-17 Voice Lab 联合负载补充

USB UAC2 通道 0 已接入业务采集，ES8311 保留提示音；C6 经 SDIO 升级并回读为 2.12.12。P4 500 ms 聚包与独立媒体任务固件已刷入：普通文件录音 89.16 秒完整保存，60 秒媒体重建测试分成 12/48 秒两段均完成，普通实时录音 59 秒正常结束，USB read_fail=0。测试中控制连接重建会按服务器策略结束录音，不承诺自动续录。短测未见 MAC 变化，不能据此排除所有偶发网络故障。固件哈希、correlation、文件完整性结果和剩余边界见 [WEBSOCKET-INTEGRATION.md](WEBSOCKET-INTEGRATION.md)。
