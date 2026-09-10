# Voice Lab 维护的网络组件

通过 `main/idf_component.yml` 的 `78/esp-ml307.override_path` 选择本目录；不得修改 `managed_components` 生成副本。

来源为本机已安装的 `78/esp-ml307` 3.6.6。原 manifest 指向上游提交 `9f2a278ac6bf4ca3d6bc5057e8fcf10097cc583f`，仓库为 <https://github.com/78/esp-ml307>。保留原 Apache-2.0 `LICENSE`、README、manifest、CMake 和完整 `include` / `src`：复制时共 49 个文件、221967 字节。未复制组件管理器校验输出。本机原副本已有 Voice Lab 的 TLS 1.2 / 密码套件设置等调整，本次按现有可用副本保留，没有声称与上游字节完全一致。

## 2026-09-10 生命周期修复

实机在收到 `audio completed` 与 WebSocket close 后进入 ready，随后出现堆损坏。原析构仅在 `connected_` 为 true 时停止 TCP；收到 close 帧已将其设为 false，但接收任务仍在更新 WebSocket 缓冲，缓冲可能比 TCP 成员先被释放。

- WebSocket 析构体始终先停止并销毁 TCP，再释放任何缓冲、回调、握手事件组。
- ESP TLS / TCP 用 `shutdown` 唤醒阻塞 I/O。接收函数返回后，以 release 原子标记作为最后一次对象访问；所有者观察 acquire 标记后才释放对象。移除仅用于退出通知的事件组，避免通知尚未返回时事件组已被释放。
- 所有者等待接收任务时不持发送锁，避免接收任务回复 pong 时互锁；确认接收退出后再等待正在进行的发送，最后销毁 TLS 或关闭 TCP 文件描述符。
- TLS 文件描述符只由 TLS 销毁路径关闭，避免预先 `close` 后重复关闭被重用的 fd。
- 被动 TCP 断开也必须由所有者完成 join 和 fd 释放；不能以连接状态为 false 省略资源回收。
- 从接收回调调用 Disconnect 只请求 shutdown，不等待自身。回调中不得直接析构传输所有者；应调度到外部任务。明确违反此生命周期约束或 10 秒仍无法退出时停止执行并报告，不再释放仍被使用的资源。
- 跨任务连接状态改为原子变量，处理接收任务创建失败，保留原蜂窝网络实现和平台选择。

本次未运行用户已拒绝的本地主机测试。需要经受影响固件构建和实机连续开始、暂停、恢复、停止及重连验证；不能仅凭源码修复宣称蜂窝网络或其他板型已验收。

## 2026-09-10 PCM 即时发送

仅在 `CONFIG_VOICE_LAB_STANDALONE_MODE` 启用时，为 ESP TLS 连接设置 `TCP_NODELAY`，让小块 PCM 不因 Nagle 合包等待前一次 TCP ACK。保留原 TLS 验证、发送锁和接收任务退出顺序。该选项不改变蜂窝网络或非 standalone 固件的传输行为。
