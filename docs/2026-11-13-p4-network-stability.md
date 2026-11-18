# P4 四路上传自停：网络稳定性排查（2026-11-13）

设备：微雪 ESP32-P4 Wi-Fi6 Dev Kit B，`p4-terminal-e8f60ae2c6a4`（管理端名 Counter terminal E2C6A4），K01 声卡四路原始麦，固件 `p4-mm-afe-usb-k01raw-v3`（提交 `544a05b`）。
服务端：`https://voice-lab.cloud`，镜像 `xingtun/voice-lab:20261112-k01-four-mic-59e5c61`（voice-lab 分支 `deployed/20261112-live-k01-four-mic`），前面是 Caddy，库是 SQLite。

> **状态（2026-11-18 更新）：服务端已修复并上线。**
> - 修复在 voice-lab 提交 `1cd6a77`（`deployed/20261112-live-k01-four-mic` 分支；`main` 上是 `ec7f878`），11-13 晚上线。
> - 现在线上跑的镜像是 `xingtun/voice-lab:20261113-testruns-1cd6a77`。
> - 下文的排查过程和数据都基于修复前的镜像，详见“规避与修复”一节。

## 结论

**根因在服务端，不在办公室 Wi-Fi，也不在 P4/C6 链路。**

- 管理接口 `GET /api/v1/admin/test-runs?limit=N` 每次要 8.5–19 秒。
- 它在 `async def` 处理函数里同步执行 SQLite 查询，执行期间 uvicorn 的事件循环整个停住：
  - 不回 `ingress_ack`；
  - 不读 socket，设备侧 TCP 发送窗口被顶满；
  - 新的 WebSocket 升级也要排队等。
- 谁在调这个接口：
  - 管理页停在“记录与诊断”标签时，每 10 秒自动拉一次 `limit=100`；
  - 用脚本或会话盯进度时调用的 `limit=1` 也一样慢（原因见“服务端证据”第 3 条）。
- 设备侧的两道超时把每次服务端停顿放大成断线和重传风暴：
  - TLS 单次发送预算 3 秒（`TlsWritePolicy::kBudgetUs`）；
  - 媒体 ACK 进展租约 4 秒（`PumpMediaLocked`）。
- 一次风暴的过程：断开 → 重新握手（WebSocket 升级被阻塞 4–9 秒）→ 补传整个 4 秒窗口（约 512 KB 重复数据）。期间 PSRAM 缓存持续增长，满了就按设计结束录音。

**对照验证**：同一固件、同一位置、同一 Wi-Fi，录音期间不让任何客户端调 `test-runs`：

- 结果：四路连续上传零重连，缓存始终清空（数据见下表）；
- 服务端事件循环超过 500 ms 的卡顿计数在整轮里没有增加。

## 现象回顾

| 时间（北京时间） | 网络 | 结果 | 当时的 `test-runs` 调用 |
| --- | --- | --- | --- |
| 09:38–09:59 | ZX88888，工位 −47 dBm | 20 分钟零缺口，3 次重连都补传成功 | 约每 5 分钟一次 `limit=1`（09:37:55、09:38:10、09:38:54、09:44:05、09:49:27、09:54:49） |
| 15:2x–15:4x | ZX88888，风扇位 / 工位 | 20 秒、49 秒自停 | 07:2x–07:4x UTC 每 10 分钟窗口里有 5–10 次 |
| 16:22–16:28，run `f4e13d0a` | 小屈 iPhone 热点 | 采 325.5 秒，只到 205 秒 | 每次媒体重连时刻都对应一次 `limit=1`（见下） |
| 16:57–17:01，run `f7ec765d`（本次基线） | ZX88888，约 −33 dBm | 采 248.5 秒，只到 128.5 秒，缓存满自停 | 管理页停在“记录与诊断”，每 10–20 秒一次 `limit=100` |
| 17:06:39–17:27:30，run `823e983f`（本次对照） | 同上 | 20 分 50 秒全部到达，零重连 | 无 |

热点那轮（`f4e13d0a`）的对照，三列时间都是 UTC：

- 服务端接受媒体 WS 的时刻：08:24:19.1、08:24:27.8、08:24:50.7、08:25:43.1、08:25:54.1、08:26:36.8、08:27:22.9、08:28:09.6。
- Caddy 记录的慢请求 `GET /api/v1/admin/test-runs?limit=1` 的完成时刻：08:24:19、08:24:27、08:24:50、08:25:43、08:25:54、08:26:36、08:27:22、08:28:09。每次耗时 8.5–9.8 秒。

两组时刻逐个吻合：服务端从停顿里恢复的那一刻，积压的 WS 升级才被接受。“约 45 秒断一次”就是盯进度的轮询节奏。同一时段小屈的 reSpeaker（`xvf3800-e0c2508fee68`）控制通道也在反复重连，说明受影响的是整个服务端，不只这块板。

## 设备端证据（基线 run `f7ec765d-43c3-41cd-9bee-fbf8d5c79fea`，串口 COM7）

- 18 次媒体重连：11 次 `TLS send deadline expired`，7 次 `reason=ack_timeout`。
- 发送停顿是“完全不动”，不是“慢”。例：`TLS send summary: elapsed_ms=3000 bytes=64048 written=5120 ... want_retries=2998`：3 秒里 socket 一直写不进去，因为服务端不读，TCP 窗口关闭了。
- 服务端一恢复，链路立刻以约 2 倍实时速度追赶：每 0.5 秒一个 ACK，每个推进 16000 样本，约 2 Mbit/s。
- 重连握手分两段：
  - `tcp=`（DNS + TCP + 与 Caddy 的 TLS）平均 0.83 秒；
  - `handshake=`（WebSocket 升级，要等应用接受）平均 4.5 秒，最长 8.7 秒。
- 后半段出现 20 秒的固定循环：握手约 5 秒 → 追赶约 10 秒 → ACK 停止 → 4 秒后 `ack_timeout`。和服务端“每 20 秒停 9.5 秒”一一对应。
- 每次重连，服务端审计记 `audio_ingress_duplicates_suppressed` 6–8 批（150–200 帧）。也就是说，在途窗口里的数据服务端其实都收到了，只是 ACK 没能及时发出来。

## 服务端证据

1. `GET /health` 里自带事件循环延迟统计：`maxLagMs=23063`，`over500Ms=123`（容器 11-12 启动以来）。现场单独调一次 `test-runs?limit=100`：耗时 9204 ms，`over500Ms` 立刻变成 124。同一批调用里 `live-state` 101 ms、`audit?limit=60` 32 ms、`/health` 20–50 ms。
2. Caddy 访问日志（`journalctl -u caddy`）：
   - 16:39:54 起，同一台办公室 Windows 浏览器（出口 120.239.207.208）每 10–20 秒请求一次 `test-runs?limit=100`；
   - 每次耗时 8.2–19.3 秒；
   - 同一时刻排在它后面的 `live-state` 也拖到 3–9 秒。
3. 查询计划（只读连接执行 `EXPLAIN QUERY PLAN`），`database.list_test_runs`：
   - 对 `test_runs` 全表扫描后再排序（`USE TEMP B-TREE FOR ORDER BY`），所以 `limit=1` 也要对全部 345 条 run 算完子查询；
   - `transcriptCount` 子查询走 `idx_transcript_events_device(device_id)`，每条 run 都要扫该设备全部转写事件（全表 207,203 行），再逐行回表 `realtime_sessions`；
   - `voice-lab.db` 有 607 MB，容器内存上限 512 MiB，页缓存放不下，基本每次都读盘。
4. 前端 `static/app.js`：`refresh()` 只在 `view === "evidence"`（“记录与诊断”）时带上 `test-runs?limit=100`，后台刷新间隔 `BACKGROUND_REFRESH_INTERVAL_MS = 10000`。停在“实验台”标签不会触发。

## 三个待验证假设的结论

1. **`PumpMediaLocked` 的 4 秒 ACK 租约导致死循环**：部分成立，但它是放大器，不是根因。
   - 服务端一停就是 9 秒以上，4 秒租约和 3 秒 TLS 预算必然触发；
   - 重连后的 WS 升级本身又要等服务端恢复，补传的 512 KB 还会推高下一轮的积压。
   - 服务端不停顿时，这两道超时一次都没触发（见对照）。
2. **P4 经 C6 的链路吞吐或稳定性不足**：排除。
   - 同一位置、同一时段，没有服务端停顿时，四路（约 1.05 Mbit/s）持续实时上传；
   - 追赶阶段实测约 2 Mbit/s；
   - 基线里所有“发不动”的 3 秒窗口都落在服务端停顿区间内；
   - 服务端到办公室出口的 RTT（巨无霸有线 ping）约 12 ms。
3. **某个周期性任务干扰网络**：成立，但周期任务在服务端，是对 `test-runs` 的轮询（管理页每 10 秒，监控每 45 秒到 5 分钟），不在设备上。设备空闲时每分钟一次的选网扫描在录音期间不会执行（v3 设计），日志也确认录音期间没有 `Scan start`。

链路能力的直接测量（17:43，同一位置，RSSI −27～−30 dBm，固件 `p4-mm-afe-usb-k01raw-v3-netdiag`）：

- `p4 net ping 192.168.5.1 20`：20/20，RTT 1–58 ms，多数在 10 ms 以内。
- `p4 net ping voice-lab.cloud 20`：20/20，RTT 13–36 ms，中位约 17 ms。
- `p4 net tput 192.168.5.225 18555 20`（目标是巨无霸上的临时 TCP 接收端，用完已删）：
  - 20 秒平均 3567 kbit/s，每秒 2162–4427 kbit/s；
  - 全程 `nrtx=0`；
  - 在途字节始终被 5760 字节的 lwIP 发送缓冲封顶（`inflight≤5600`）。
- 结论：局域网裸 TCP 上行约为四路所需的 3.4 倍。按 5760 B / 17 ms 估算，到服务器的理论上限约 2.7 Mbit/s，也有 2.5 倍余量；但 Wi-Fi 抖到 50 ms 以上时会暂时掉到 1 Mbit/s 左右。如果以后要更大余量，可以评估把 `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` 调大。这不是本次问题的原因，未改动。

另外排除：

- 蓝牙共存：本构建没有开 `CONFIG_COUNTER_BLE_LINK`。
- 服务端 ping 超时：uvicorn 的 20 s/20 s 保活不是触发源，断开时刻和它对不上。
- 识别 Provider：对照轮里 VAD 两次触发火山识别会话（generation 58、59），事件循环没有卡顿，上传也没有受影响。

## 前后对照

| 指标 | 基线：有 `test-runs` 轮询 | 对照：无轮询 |
| --- | --- | --- |
| run | `f7ec765d-43c3-41cd-9bee-fbf8d5c79fea` | `823e983f-c1e0-4347-9387-e9f84a5704bc` |
| 固件 / 位置 / 网络 | v3 / 同一位置 / ZX88888 −31～−34 dBm | 相同 |
| 采集时长 → 服务端收到 | 248.5 s → 128.5 s | 1251 s → 1250.5 s，`finalizationState=audio_confirmed` |
| 媒体重连 | 18 次（TLS 超时 11 次，ACK 超时 7 次） | 0 次（全程只有开录时那 1 次握手） |
| 64 KB 单批发送超过 0.5 s | 绝大多数批次，常到 3 s 超时 | 60 次，最长 1.66 s，无一超时 |
| 设备积压峰值 | 涨满 120 s 缓存 | 133 帧（2.7 s），随即清空 |
| WS 升级等待 | 平均 4.5 s，最长 8.7 s | 0.04 s |
| 服务端 `over500Ms` 增量 | 每 20 秒 +1 | 0（录音期间每秒采样 `/health` 共 600 次，最慢 0.14 s） |
| 结束方式 | 缓存满自停 | 管理端“结束并保存”，`audio status: completed` |
| 服务端重复批 | 每次重连 6–8 批 | 1 批（结束时的尾包补发） |

## 规避与修复

### 服务端修复：已完成（11-13 晚上线）

voice-lab 提交 `1cd6a77`（`deployed/20261112-live-k01-four-mic` 分支；`main` 上是 `ec7f878`；测试 `tests/test_test_run_listing.py`）：

1. `list_test_runs` 处理函数改成普通 `def`，由 FastAPI 放到线程池执行，不再卡事件循环。
2. 查询改成先用 CTE `recent` 取 `LIMIT` 条 run，再计数。计数路径是 `realtime_sessions(correlation_id)` → `transcript_events(session_id)`。
3. 新增索引 `idx_test_runs_started`、`idx_realtime_sessions_correlation`。

线上实测（11-18 查，镜像 `xingtun/voice-lab:20261113-testruns-1cd6a77`，已运行 4 天）：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `test-runs` 单次耗时 | 8.5–19 s | 共 457 次：中位 0.074 s，P95 0.81 s，最长 2.25 s |
| `/health` 的 `over500Ms` | 每次调用加 1 | 本次启动以来为 0，`maxLagMs` 为 115 |

修复后还没有再跑“开着‘记录与诊断’页录 20 分钟四路”的对照，下次上板时补。

仍可留意的点：

- 同一文件里，其它在 `async def` 里直接调 `database.*` 的接口也可能卡住事件循环。录音期间看 `/health` 的 `over500Ms` 是否增加。
- voice-lab 容器内存上限是 512 MiB，库 607 MB。视情况提高上限，或归档旧的 `transcript_events`。

### 修复前的规避方法（留作参考）

只对修复前的镜像有意义：

- 录音期间不要把管理页停在“记录与诊断”标签。
- 盯进度改用 `/api/v1/admin/devices/<deviceId>/audit?limit=60` 或 `/health`，不要调 `test-runs`。

设备端加固（可选，偏离 XIAO ESP32-S3 基线，需要 Rex 评审）：

- S3 基线同样是 2 秒写超时加 4 秒进展租约，遇到 9 秒的服务端停顿也会断。小屈的 reSpeaker 控制通道当时也在反复重连。
- 如果希望设备扛住 10–20 秒的服务端停顿，可以考虑：
  - 把 TLS 发送预算从“总时长 3 秒”改成“连续无进展 N 秒”；
  - 四路时 ACK 租约放宽到 10–15 秒。
- 120 秒的 PSRAM 缓存足够覆盖这个停顿。
- 这样做的代价是真断网时发现得更慢。服务端已修复，目前没有必要改设备；以后再遇到服务端长时间停顿时再评估。

## 续接要点（给小屈）

- 当时的现象（只在修复前的镜像上能复现）：开四路录音后，在管理页切到“记录与诊断”并停留。
  - 串口会出现 `TLS send deadline expired` 和 `Media reconnect: reason=ack_timeout`；
  - `/health` 里的 `over500Ms` 每 10–20 秒加一。
- 如果四路又出现断线自停，先看 `/health` 的 `over500Ms` 有没有涨，再查 Caddy 日志里同一时刻的慢请求，最后才怀疑 Wi-Fi。
- 服务端事件循环健康度：`curl https://voice-lab.cloud/health`，看 `eventLoopLag`。
- 板级网络诊断命令（本次新增，`main/boards/waveshare/esp32-p4-wifi6-dev-kit-b/net_diag.{h,cc}`，只在这块板的固件里，不改变上传行为）：
  - `p4 net ping <ip|域名> [次数]`：设备侧 RTT，录音期间也能用；
  - `p4 net tcp`：打印一次所有活动 TCP 连接的 cwnd、在途字节、重传次数、RTO 等；
  - `p4 net watch on|off`：每秒打印一次到 443 端口连接的上述状态，录音时用来看是本地发不出去还是对端不收；
  - `p4 net tput <ipv4> <端口> <秒>`：向局域网接收端做裸 TCP 上行测速。接收端随便起一个 TCP 服务丢弃数据即可。
- 时间对齐：服务器已 NTP 同步；Rex 公司电脑 11-13 实测慢约 11.7 秒，串口日志如果用电脑时间打时间戳，和服务端日志对照前要先加上这个差值。
- 串口抓取：pyserial，115200，`dtr=False`、`rts=False`。关注 `Media reconnect`、`TLS send summary`、`WebSocket handshake done, cost= tcp= handshake=` 三类日志。
