# ESP32-P4-WIFI6-DEV-KIT-B 屏幕适配

目标为 PCB Rev1.2、P4 revision 3.1、16MB Flash / 32MB PSRAM，以及配套的 7-DSI-TOUCH-A（ILI9881C，720×1280）。此板型与一体式 P4 Touch LCD 7 不同，不可互刷。

唯一变体 `esp32-p4x-wifi6-dev-kit-b-screen` 复用小智应用和 MipiLcdDisplay，实现横屏 1280×720 原生 LVGL 柜台演示。包含金价与计价、订单预览、服务说明、确认结果四页，所有价格均为演示数据，不创建订单、不支付。ES8311 探测成功后复用小智音频驱动，缺失则回退 DummyAudioCodec 保留界面；不自动录音。GT9xx 触摸自动探测 0x5d/0x14。当前 StartNetwork 会把有线/Wi-Fi 的 IP 可用状态交给现有 Voice Lab 客户端，使用独立设备标识和 HTTPS/WSS 配对；不启动小智 OTA。USB UAC2 阵列提供采集，板载 ES8311 保留提示音播放。最新接入状态见 [WEBSOCKET-INTEGRATION.md](WEBSOCKET-INTEGRATION.md)。

显示参数与初始化表参考 `waveshare/esp32_p4_platform 2.0.1` 和 `waveshare/esp_lcd_ili9881c 2.0.0`。初始化表保留 Apache-2.0 标记；通过项目已有的乐鑫 ILI9881C 驱动发送。复位/背光使用 I2C1（SDA7/SCL8）上屏幕控制器 0x45 的 0x95/0x96 寄存器，不使用一体屏的 GPIO26/27。DSI 双 lane、1000 Mbps、80 MHz DPI、RGB565。当前继承 MipiLcdDisplay 的局部刷新、软件旋转与单帧缓冲，未开启防撕裂；需真机验证，不能直接将双缓冲开关等同于横屏刷新同步完成。

底部新增录音按钮和状态栏，使用既有服务端授权与音频收尾流程。按钮操作在独立任务执行，避免阻塞 LVGL；需服务端为设备开通自助录音，不能用管理员 mock 测试代替授权。最近录音回放尚未接入。当前板级 DSI 使用同步 CPU 拷贝，动态界面联合负载中曾出现发送积压和 USB 路径崩溃，仍需长稳验收，详细证据见 Voice Lab 根目录交接记录。

中文字体为 OFL 授权 Noto Sans SC 的 4bpp 子集，许可见 FONT-LICENSE.txt。静态布局、字库和预览由 `scripts/generate_counter_ui.py` 生成；动态录音栏由 `counter_ui.cc` 渲染，静态预览不覆盖其运行状态，也不代表实机截图。

首页右上角“设备设置”包含网络配置、版本信息、存储与音频。网络页显示实际有线/Wi-Fi状态，支持扫描及触屏输入密码，连接成功后保存；有线DHCP优先。版本页区分主机驱动与C6实际查询结果。SD卡当前未插入，以后插卡后点击检测，不自动格式化。完整驱动与验收边界见 [HARDWARE-VALIDATION.md](HARDWARE-VALIDATION.md)。

构建入口（优先 ESP-IDF 6.0.2）：

```sh
python scripts/build.py waveshare/esp32-p4-wifi6-dev-kit-b --name esp32-p4x-wifi6-dev-kit-b-screen --language zh-CN --wake-word disabled
```

## 2026-09-16 现场进度

2026-09-17 更新：C6 已通过 P4 的 SDIO OTA 免焊线升级到 ESP-Hosted 2.12.12，重启读回版本成功。操作、固件哈希和验收边界见 [C6-SDIO-UPDATE.md](C6-SDIO-UPDATE.md)。下方为历史记录，其中 `C:/QIU/p4-screen-20260916/` 目录已按用户要求删除，不应视为仍可用的备份位置。

- 新分支：`codex/p4-wifi6-dev-kit-b-screen`。
- 已通过 COM32 读取 P4 revision 3.1 / MAC `e8:f6:0a:e2:c6:a4`。
- 用户要求先刷入可用固件，因此先刷了独立官方 `13_Displaycolorbar`（已选择 7 寸屏配置），不是本目录的小智适配草稿。
- 官方彩条固件用本机 ESP-IDF v6.1 构建，刷写全部哈希校验通过。
- 启动日志确认进入 7-DSI-TOUCH-A 初始化，采集窗口内未看到彩条输出完成日志，后续 COM32 消失。用户随后明确确认“可以了有彩色条纹”：本版实体屏幕彩条显示通过，已跑通屏幕供电、背光与 DSI 显示；串口消失原因未定位。
- 现场源码、构建、备份及日志在本机 `C:/QIU/p4-screen-20260916/`。
- 两次整片备份串口中断，未形成完整备份；成功保存 `0x00000..0x60000` 区域，覆盖这次彩条刷写的全部扇区。该文件不是整片备份。后续若刷更大固件，需另行备份新增覆盖区域。

彩条点屏已由用户确认。UI 刷写前已另外完成分块读取并拼接的 16 MB 整片备份，位于 `C:/QIU/p4-screen-20260916/backup/pre-ui/`，逐块 SHA-256 见 manifest.json。备份包含设备原有数据，不进入 Git。UI、触摸和启动稳定性仍须独立验证。

## LVGL UI 真机记录

- ESP-IDF v6.1 构建通过，应用大小 3571744 字节，应用分区余量 13%。首次 UI 应用 SHA-256：9774006e410f0eb3b7c8609bb7039a4fff7cbd946be4bd01b019db09348a8423。
- 首次 UI 已写入 COM32，所有分段写入哈希校验通过。20 秒启动观察无崩溃，进入 Counter UI ready；面板 ID 98/81/5c，GT9xx ID 39/32/37，触摸注册地址 0x5d。
- ESP-Hosted 组件仍有启动钩子初始化任务；板级 StartNetwork 未发起网络连接。“离线”不表示组件代码未链接或没有初始化日志。
- 用户确认屏幕有 TP，并要求相对首版倒置。LVGL 旋转从 90° 调整为 270°；输入处理使用 LVGL 自带 lv_display_rotate_point，同步转换原始触摸坐标，避免二次旋转。
- 本机日志：flash-xiaozhi-ui.log、boot-xiaozhi-ui.log；位于 C:/QIU/p4-screen-20260916/。显示方向、点击准确度、撕裂与长稳以现场观察为准。
- 倒置版已构建并仅更新 0x20000 应用段，写入哈希通过。应用 SHA-256：32a097420cbdac3947225dafc2b829fdfe277fdeee7a674164136fc7d0a810b9。日志：flash-xiaozhi-ui-rotated.log、boot-xiaozhi-ui-rotated.log。
