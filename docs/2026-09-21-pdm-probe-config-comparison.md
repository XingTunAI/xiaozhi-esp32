# PDM 原始探针与当前固件配置对比

核对日期：2026-09-21。仅诊断；本轮没有改写或烧录设备固件。

## 证据范围

- 旧实验说明：`../voice-lab/docs/tech/ESP32-P4-WIFI6-DEV-KIT/2026-09-14-DUAL-PDM-PROBE.md`。
- 保存的原始探针源码：`../voice-lab/firmware/p4-voice-terminal/main/probe.c`；Git e2ba5b5 保存该基线。`main/CMakeLists.txt` 的 `P4_PROBE_ONLY` 分支只编译 probe.c。
- 当前实现：`xiaozhi-esp32/main/boards/waveshare/esp32-p4-wifi6-dev-kit-b/pdm_capture.h` 和 `usb_capture_codec.h`。
- 旧试验的 WAV、冻结构建目录 `voice-lab/var/p4-probe/` 在本机未找到。不能重新播放旧录音，也不能将当前仓库依赖锁文件当成旧探针的冻结构建配置。
- 官方 IDF v5.5.4 的 i2s_pdm.h、i2s_pdm.c、i2s_hal.c、P4 i2s_ll.h 与本机 C:/esp/v6.1/esp-idf 对比；下载及差异位于 `tmp/p4-idf554-compare/`。

## 已确认对比

| 项目 | 9 月 14 日 P0 探针 | 当前实现 |
| --- | --- | --- |
| 外设/连线 | I2S0 master，CLK2、DAT3，不反相 | 相同 |
| 数据格式 | 16kHz、PCM16、双声道交错，硬件 PDM→PCM | 相同；输出时取 L/R 单路或送 AFE |
| 默认时钟配置 | DSR_8S、mclk_multiple=256、bclk_div=8 | 相同；按驱动计算目标 PDM CLK 为 16000×64=1.024MHz，尚未实测波形 |
| 默认高通与硬件增益 | 高通启用，35.5Hz，amplify_num=1 | 相同 |
| 启动预热 | enable 后读取丢弃 10×320 帧，约 200ms | 没有；enable 后首批样本直接进入录音 |
| DMA 描述符 | 8×320 帧 | 12×320 帧 |
| 读取 | 20ms 双声道块，1000ms 超时 | 原始模式相同块大小，100ms 超时；AFE 按 feed chunk 读 |
| 增益 | 原始 x1 | 原始 L/R 可选 x1/x2/x4；当前测试 x4，AFE 保持 x1 |
| AFE | 不创建、不运行 | 原始模式也会创建 AFE 资源，但不 feed/fetch；AFE 模式运行 BSS |
| 录音时其他工作 | 8秒存 PSRAM，停采后才传 UART；没有网络/SD/UI/播放 | 网络、SD、屏幕同时运行；开始前有板载提示音 |
| IDF | 记录为 v5.5.4 | v6.1 |
| CPU/PSRAM | 文档记载 400MHz/200MHz | 当前 sdkconfig 同为 400MHz/200MHz |

IDF 两版 i2s_pdm.h 仅发现一处注释纠正，默认配置宏未改变。PDM RX HAL 初始化逻辑仅空白差异，已核对的 P4 PDM 降采样、高通寄存器和增益设置函数一致。驱动层存在时钟资源管理等实现差异，因此不能据此排除所有 IDF 回归，也不能声称实测时钟完全一致。

## AFE 来源必须区分

此前移植参考的是后来业务固件的 `main/audio.c`，不是 `probe.c`。业务代码会装载模型，开启 VAD/WakeNet，以内部 RAM feed buffer 持续采集；当前关闭 VAD/WakeNet，模型传 nullptr，feed buffer 使用 vector，并按录音启停。两边相同的主要配置是 MM、SR/HIGH_PERF、SE/NS 开启、AEC/AGC 关闭、优先核 1、优先级 6、MORE_PSRAM、ringbuf 50。

当前业务源码依赖锁为 esp-sr 2.5.3，当前适配固件为 2.4.7；这只能说明两个现存工程不同，不能证明 9 月 14 日 P0 探针用了某个 AFE 版本，因为 P0 根本不运行 AFE。当前实机日志显示实际链路 `[input] -> |SE(BSS)| -> [output]`。

## 与实测症状的关系

- R×4 录音 1a5eb40f-9d7e-5954-bda3-fcd01f29f884：开头 0–约90ms 强瞬态；原始输入首秒峰值 32729，数字放大后削波。旧版主动丢弃约200ms预热，当前没有，这是最直接、可复现验证的差异。
- 约12.18–12.44秒、92.00–92.33秒也有强突发噪声。它们位于原始声道，不经过 AFE；不能用缺少开机预热解释所有中途异常。
- 无 DMA 溢出或读取失败只证明软件收到数据，不证明 PDM 线上数据正确。
- 用户纠正实际测试常在10cm内且手持麦板，前面的“30cm”仅为测试提示/话术，不能作为实际距离或灵敏度标定。
- 尚未找到会在原始 PCM 路径把人声额外除小的配置。旧/新测试话术、距离、背景和是否手持不同，RMS不能直接用于证明驱动衰减。

## 建议的单变量修复顺序

1. 在开始提示与正式录音之前完成 PDM enable 和200ms有效样本预热，再建立录音样本起点；不要直接删除已经计时/上传的头200ms，避免丢话和破坏样本序号。
2. 原始 L/R 模式延迟创建 AFE，尽量减少与 P0 无关的资源占用。
3. 保持同一增益、固定麦板，验证开头瞬态消失与否；中途尖峰另行保留原始双声道证据，并对比并发外设活动。
4. 最后单独检查 AFE 模型/选路/版本差异，不把P0通过等同于BSS音质已验收。


仓库归档说明：本文中的 tmp/、local-build-backups/ 和相邻 voice-lab 工程路径相对于本机 voice-lab-adapter 工作区；这些原始证据未纳入本仓。最新 USB 界面版本见 [同步与验证记录](2026-09-21-p4-usb-baseline.md)。
