# AGENTS.md — DS5Dummy 程序说明

面向要改这个仓库代码的人。内容全部从当前代码、构建脚本与打包文件推出，不含历史叙述。
面向终端用户的安装与运行说明在 [README.md](README.md)。

---

## 1. 系统在做什么

每只**蓝牙** DualSense 各配一台**USB** DualSense（configfs + FunctionFS +
`dummy_hcd`，一只 worker 进程一站），把两边的 HID 报告互相转发。手柄从系统里出现
就自动建站，下面是一条链路的形状：

```
真手柄 ⇄ 蓝牙(HIDP / hidraw) ⇄ [bt] ⇄ [bridge] ⇄ [usb] ⇄ FunctionFS 端点 ⇄ 主机 HID 栈 ⇄ 游戏
                                     ↑ ↓
                    [audio] 上行：0x31 麦克风帧 → Opus 解码 → PipeWire Source
                    [audio] 下行：PipeWire sink → Opus/触觉 → 0x36 报告 → 手柄
```

主机看到的设备身份是 `054c:0ce6` "Sony Interactive Entertainment / DualSense
Wireless Controller"（`src/core/device/identity.h`，编译期常量，不是配置项）；
序列号不在那一处——它是每站一个、由手柄 MAC 派生的值（见 §7）。
麦克风开关走 HID 通道（0x32 状态报告），不走音频通道：主机打开虚拟麦克风开始
录音时程序才让手柄开麦。

---

## 2. 仓库布局

| 路径 | 内容 |
|---|---|
| `src/main.cpp` | 唯一入口：解析参数、装信号 handler、按 `--worker` 分派两个角色 |
| `src/core/` | 协议与设备静态模型：报告 ID/线尺寸、输入/输出状态编解码、CRC32、USB 描述符、设备身份 |
| `src/daemon/` | 进程形态：`config`（YAML 解析 + 加固读取）、`discovery`（hidraw sysfs → 手柄 MAC）、`identity`（sd-login 会话 → worker 身份）、`options`（Manager→worker 的内部交接参数编解码）、`shutdown`（信号）、`manager`（root 多槽监督循环）、`worker`（数据面宿主）、`handoff`（fd 交接）、`privdrop`（降权执行） |
| `src/usb/` | `gadget_setup`（configfs 装配 + mount）、`ffs_device`（端点 fd 机制）、`usb`（门面 + 线程起停）、`usb_plane`（共享运行状态）、`usb_inout`（IN/OUT 两条循环）、`usb_ep0`（ep0 循环 + Ep0Responder 实现） |
| `src/bt/` | `bt`（门面）、`bt_link`（生命周期/缓存策略/线程起停）、`bt_plane`（共享运行状态）、`bt_rx`/`bt_tx`（两条传输线程）、`bt_detail`（后端接缝）、`bt_l2cap`/`bt_hidraw`（二选一的后端 TU） |
| `src/bridge/` | `bridge`（转发策略）、`state_report`（63 字节输出状态影子）、`fake_mac`（0x09 合成）、`usb_report`/`audio_report`/`mic_report`（三种报告的字节布局） |
| `src/audio/` | 双向音频链：`audio`（门面）、`audio_identity`（sink/source 共用的节点名与 `device.*` 硬件身份）、`audio_sink`（PipeWire 4 通道 sink + 断线重连）、`audio_encoder`（SRC + Opus + 编码线程）、`audio_decoder`（Opus 解码线程）、`audio_source`（单声道虚拟麦克风 Source） |
| `tests/` | googletest 单测（`ctest`，无 root、无硬件） |
| `tools/` | 诊断程序：`spike_l2cap`、`spike_rumble`、`mini_gadget`、`audio_dump`(+`dump_sink.*`)、几个 `spike_*` 特权/交接探针 |
| `pkg/ds5dummy_hidraw`、`pkg/ds5dummy_l2cap` | 两个互斥的 Arch 包：systemd 单元、`/etc/ds5dummy/config.yaml`、`/usr/share/doc/ds5dummy/user-config.example.yaml`、modprobe/modules-load 配置、install 钩子 |
| `hid-playstation-ds5dummy/` | 内核 `hid-playstation` 的 DKMS 补丁（hidraw 后端用），合成 VID/PID `1998:1221` |
| `scripts/` | `setup.sh`/`teardown.sh`（整站准备/清理；teardown 扫 `ds5dummy*` 整族并卸载每个 `ffs-ds5*` 挂载）、`setup-dummy-hcd.sh`/`teardown-dummy-hcd.sh`（只喂 `tools/mini_gadget` 的最小版） |
| `third_party/` | loguru（vendored，不改注释不格式化） |

---

## 3. 进程模型

一个二进制，两个角色，靠 `--worker` 区分。

### root Manager（默认角色）

`src/daemon/manager.cpp`。每只手柄一站（`Slot` = gadget 坐标 + worker pid），每站都
只做五件特权事：建 configfs gadget、mount FunctionFS、往 ep0 写描述符、bind UDC、
bind 后从 UDC 反查主机侧 USB 接口目录 —— 全在 `usb::Provision()` 里，失败自带回滚。

启动前置校验（`RunManager`）按固定顺序：`geteuid()==0` → 读系统配置
（`kSystemConfigPath`；缺失走内置默认，解析失败返回 1）→ 枚举活跃图形会话并选身份 →
查家目录、读用户配置（文件缺失等于"用内置默认继续启动"：`LoadUserConfig` 在
`kAbsent` 时不动出参，默认构造的 `UserConfig` 就是模板的取值 —— `enabled: true`、
`bt_addr: auto`、每只手柄的设置全默认，横幅把那行标成
`(absent, built-in defaults in use)`）→
应用生效日志级别 → `sys.ffs_path` 过 `usb::IsSafeGadgetPath`
白名单 → `usb::HasDummyUdc()` 在 `/sys/class/udc` 下能看到 `dummy_udc.N`。退出码只有
0 和 1：1 = 条件暂不成立（未登录、候选不唯一、配置错误、路径/UDC 不可用），交给
systemd 每 2 秒重拉；0 = 当前用户不启用本服务（用户配置里 `enabled: false`、不在
`identity.mode: list` 名单内），服务停在 inactive 不重启，或监督循环收到退出信号后
正常收摊（`ManagerSupervisor::Run()` 恒返回 0）。这些判断只在进入监督循环前
做一次，循环里只按发现结果起站、重试与回收 worker——登录状态变化要等下一次重启。

监督循环 `ManagerSupervisor::Run()` 以系统配置的 `discovery_interval_s`（秒，缺省
3）为节拍，每轮先补建站（`discovery` 扫描 `/sys/class/hidraw`，认出蓝牙总线上的
`054c:0ce6` 与合成的 `1998:1221`，MAC 取自 `HID_UNIQ`），再 `waitpid(-1, WNOHANG)`
回收退出的 worker。节拍同时决定两件事的最坏延迟：新手柄被认出来、以及 worker 死后
被重新拉起（后者还要再加一段退避）。
每站的生命周期：provision → fork+execv worker → 回收 → `usb::Teardown(挂载点,
实例名)` → 该 MAC 退避 `kRestartBackoffMs = 2000` ms → 下一轮重新建站。站与站互不
共享状态，一个 worker 反复崩溃不影响其他手柄，也不会让 Manager 退出。

三种 `bt_addr` 的起站依据：

- `auto`/`list`：**只按发现结果起站**，`list` 再把 `controllers` 的键集当白名单。
  发现只决定"何时起 worker"：节点消失不拆正在跑的 worker（L2CAP 后端拿到 PSM 后
  节点本来就会消失），断链由 worker 自己的看门狗发现并以退出上报。发现不到的
  MAC 不会重试——手柄不在系统里就没有要服务的东西。
- 单个 MAC：不扫描 sysfs，保留"站先起来、worker 用 `bt_wait_s` 等手柄连上"的语义。

Ctrl-C 时 Manager 给每个 worker 各发一次 SIGTERM（只发一次，避免打断它自己的 Stop
流程），然后逐站回收并拆除，直到 `slots_` 清空。

### fd 交接（不是 socket）

fork 之后子进程继承三个端点 fd，`src/daemon/handoff.cpp` 把它们挪到**固定的
3/4/5**（`kEp0Target`/`kEpInTarget`/`kEpOutTarget`；从 3 起是因为 stdio 占 0/1/2），
`close_range` 清掉其余继承 fd，然后按 `privdrop` 切身份，最后 `execv` 自身。
配置经 `BuildWorkerArgv` 重建 argv 下传，与 `internal::ParseArgv` 互为逆运算，所以
worker 解析到的就是 Manager 下发的生效配置（用户级字段已由 YAML 覆盖过）。

fork 之后、execv 之前的路径**禁止内存分配与 loguru 调用**（父进程已有 loguru 日志线程
跑过 malloc，锁状态不可信）。因此 `handoff.h`/`privdrop.h` 每个判断都有
`const char**` 的无分配重载。这条路径上的失败用 `_exit(kExecExitBase + stage)`
上报，`kExecExitBase = 120`，`ExecStage` = `kBadPlan(1)`/`kMove(2)`/`kCloseRange(3)`/
`kDropPrivs(4)`/`kExec(5)`；`ReportChildExit` 靠区间把它和 worker 正常退出码
（0/1/2）区分开。

### 降权 worker

`src/daemon/privdrop.cpp`：目标身份由 `identity` 模块从活跃图形会话选出（`auto` 用
passwd 主组，`list` 用配置条目里的 gid（省略时等于 uid）），`DropTo` 只负责执行，没有第二套身份来源
——既没有 `--uid/--gid`，也不读 `SUDO_UID/SUDO_GID`。切换顺序 `setgroups(0)` → `setgid`
→ `setuid`，不可调换（先清补充组，否则 wheel/input 组被原样带过去；丢了 root 就改不了
组），最后用 `setuid(0)` 反向验证：必须失败。

worker（`src/daemon/worker.cpp`）认领 fd（`usb::Adopt`）、建蓝牙链路、起全部数据面
线程、跑 100 ms 主循环直到退出。它不读配置文件：`bt_addr`、`bt_wait_s`、`no_audio`、
`haptics_gain`、`speaker_gain`、`verbosity` 由 Manager 从两份配置读出后写进交接 argv，
`host_sysfs_path`（`--sysfs-path`）由 Manager 建站时从 sysfs 解析出本站的 USB 接口目录后
写入，worker 只转交给音频的 sink 与 source——它已降权，也不认识 configfs 坐标，自己查不到；`usb_report_rate` 不进 argv，
它只影响 Manager 建站时写进描述符的 `bInterval`。它连 PipeWire 用的是桌面用户的身份，所以需要
`XDG_RUNTIME_DIR`；`RestoreXdgRuntimeDir()` 在环境变量丢了的时候按当前 uid（或
`SUDO_UID`）回落到 `/run/user/<uid>`。

`main.cpp` 刻意不让 `loguru::init` 扫 argv：级别由 `options` 自己解析
（`ParseVerbosity`），因为 loguru 的名字表不认 `DEBUG`，遇到不认识的值是
`CHECK_F` 直接 abort。`stdout` 设成行缓冲，否则重定向到管道时 gadget/bridge/audio
的里程碑交错会丢。

---

## 4. 模块边界规则

- 单一 include 搜索路径 `src/`（`DS5DUMMY_MODULE_INCLUDE_DIRS`）。跨模块 include
  必须写成 `<dir>/<dir>.h` 门面形式，内部头也带目录前缀 —— 从 include 就能看出
  它是否越过了门面边界。
- 每个模块的门面是 namespace 自由函数，**进程级单实例**（一个 worker 服务一只手柄）。
  多手柄靠一个 Manager 起多个 worker 进程（每站一份 bt/usb/audio 单实例）。
- 不该外泄的实现细节进 `<mod>::detail`：例如 `bt::detail` 放后端契约（§A 7 个原语）、
  feature 缓存（§B）、跨后端共享字节（§C）；`usb::detail` 放 configfs 机制层
  （`gadget_setup.h`）与端点 fd 类（`FfsDevice`）。业务模块只走门面，唯一的例外是
  `tools/mini_gadget`（诊断程序，故意绕开门面直接驱动这一层）。
- 数据面线程共享的进程内状态收 `<mod>::plane`（`bt::plane`、`usb::plane`），只在
  模块自己的 TU 里可见；它不进 detail——detail 有 mini_gadget 那个诊断工具例外，
  plane 没有。
- 依赖方向：`core` 不依赖任何人；`bt`/`usb`/`audio` 只依赖 `core`；`bridge` 是唯一
  认识"两个方向"的模块；`daemon` 是装配者。

---

## 5. 线程清单

worker 进程里本仓库起的 11 条线程，加上 libpipewire 自己为两条 RT 流各起的
data-loop 线程一共 13 条（loguru 的日志线程不计）：

| 线程 | 归属 | 干什么 | 阻塞性约束 |
|---|---|---|---|
| 主线程 | `daemon/worker` | 100 ms 一轮：睡、收音频失效通知，诊断构建下每 5 s 打一行窗口统计；建链阶段在 `OpenBtWithWait()` 里 500 ms 重试 | 唯一调用 `bt::Open/Close/StartThreads/StopThreads` 的线程 |
| bt 读线程 | `bt/bt_rx.cpp` | poll 链路 fd + `stop_fd`，100 ms tick；把裸输入报告交给 `on_input` | 静默 1 s 打 WARNING，5 s 判死；回调里不许阻塞 |
| bt 写线程 | `bt/bt_tx.cpp` | 队列空时 poll `wake_fd`/`stop_fd` 睡等；有活时 poll `stop_fd` + 链路 POLLOUT（无超时），排空 ≤8 份的发送队列 | 手上一份没发出去就原地重试同一份字节，不退回队列 |
| ep0 线程 | `src/usb/usb_ep0.cpp` | 阻塞读 ep0 setup 包，标准请求自己答，class 请求交给 `on_class_request` | `GET_REPORT(FEATURE)` 缓存未命中时会在**这条线程上**同步等手柄回复（L2CAP 预算 2 s） |
| IN 线程 | `src/usb/usb_inout.cpp` | 覆盖式槽位有最新输入报告就写 ep_in | 靠 SIGUSR1 打断阻塞 |
| OUT 线程 | 同上 | 阻塞读 ep_out，交给 `on_output_report` | 同上 |
| 编码线程 | `audio/audio_encoder.cpp` | 攒够一段就 SRC + Opus 编码，经 sink 直接调进 `bridge::OnAudioOutput` | — |
| 解码线程 | `audio/audio_decoder.cpp` | 从有界包队列取一帧，`opus_decode_float` 出 480 帧 PCM，交给 `source::PushSamples` | bt 读线程经 `bridge::OnBtInput` 只入队，不在读线程上解码 |
| sink 主循环线程 | `audio/audio_sink.cpp` | 驱动播放 sink 的 PipeWire 事件与断线重建 | — |
| source 主循环线程 | `audio/audio_source.cpp` | 驱动虚拟麦克风的 PipeWire 事件；`STREAMING ↔ PAUSED` 翻成 `bridge::OnMicOpen` | 回调里只做一次链路入队，不许阻塞 |
| 恢复 monitor 线程 | `audio/audio_sink.cpp` | 等断线 → 指数退避重建 pipeline | `on_lost` 在这条线程上；**不得在里面调 `audio::Stop()`**（Stop 要 join 的就是它） |
| PipeWire RT data-loop 线程 | libpipewire | 执行两个 `process` 回调：sink 侧 → `encoder::ProcessFrame`，source 侧 → 从 PCM 环填缓冲 | 不许日志、不许分配 |

Manager 进程只有主线程（+loguru）。

锁的分布：`bt::tx::queue_mutex`（发送队列）、`usb::plane` 的 `input_mutex`+`input_cv`
（输入槽）、`bridge/state_report.cpp` 的 `state_mutex`、`audio_encoder` 的
`input_mutex`+`queue_cv`、`audio_decoder` 的 `mutex`+`queue_cv`、`audio_sink` 与
`audio_source` 各自的 mutex+cv。`bt::plane` 一个锁都没有：运行标志与计数是 atomic，
两个 eventfd 的建立/关闭都落在没有线程在跑的时刻（`Start()` 早于起线程、`Finish()`
晚于 join）。feature 缓存同样无锁，靠的是"只在传输线程之前/之后动"的线程相位约定；
`bridge` 的 `mic_open` 是 atomic（PipeWire 主循环线程写，bt 读线程与音频编码线程读）。

---

## 6. bt —— 蓝牙传输层

门面（`src/bt/bt.h`）对外只有：`Open`、`Close`、`GetFeature`、`StartThreads`、
`StopThreads`、`PostOutput`、`Handlers`，加三个队列尺寸常量；`Stats`/`StatsWindow`
只在 `DS5DUMMY_DEBUG` 构建里存在。
输出报告只有一条路：`PostOutput` 入队，写线程排空。

**IoResult 契约**（后端接缝的返回类型，声明在 `bt_detail.h`，门面不暴露）：
`kOk`/`kAgain`（同一份字节原样重试安全）/`kTruncated`（仅读侧，整份丢）/`kClosed`
（链路不可用，只能重建）/`kFatal`（fd 没开、缓冲区非法、后端没归类过的 errno）。
调用方只对 `kAgain` 重试。

**边界**：HIDP framing 字节（`0xA1`/`0xA2`）不出 bt 模块。门面之上看到的第一个字节
永远是 report id。L2CAP 后端在 `Read()/Write()` 内部剥补，hidraw 后端由内核代劳。

- `bt_link.cpp`：生命周期（`Open` 先 `Close` 再清缓存再批量预取 feature）、缓存策略、
  两条线程的起停顺序。
- `bt_plane.cpp`：两个 eventfd（`stop`、`wake`）+ 运行标志 + 读侧两个计数（仅
  `DS5DUMMY_DEBUG` 构建）+ 回调表，全部收在一个进程级 `Plane()` static 里；诊断构建的
  `StatsWindow()` 汇总也在这里（读侧取自 plane，写侧取自 `tx::TakeWindow()`）。链路 fd
  不在这里缓存，两条线程每轮经 `detail::Fd()` 现取。
- `bt_rx.cpp`：`kRxTickMs = 100`、`kMaxInputBody = 128`（比最长的 0x31 报告 78 B 大一截）、
  静默 1 s/5 s 两档。
- `bt_tx.cpp`：有界队列 `kQueueMax = 8`，满了**丢最新**（背压终点）；入队点在锁内完成
  三件事——给 `body[1]` 高 4 位盖 seq（4 位回卷）、按尾部 4 字节写输出 CRC、踢 `wake_fd`。
  写线程对一份报告只等 `stop_fd` 与链路 POLLOUT，没有超时分支：等不到可写就原地重试
  同一份字节。
- 唤醒报告不经过队列也不经过门面：L2CAP 后端在 `detail::Open()` 末尾（切完 report
  protocol 之后）直接发一份 `kWakeReport`——142 字节的 `constexpr` 数组，含 CRC 整份冻结。
  它把 seq 钉死为 1（`0x10` 的高 4 位），与队列路径从 0 起计的编号不连续。裸 L2CAP 上
  没有别的 HID 主机跟手柄说话，不推这一份它不会开始上报；hidraw 后端没有这段代码。

两个后端在链接期二选一（`-DBT_BACKEND=`），只实现 `bt_detail.h` §A 的 7 个原语：

| | L2CAP（`bt_l2cap.cpp`） | HIDRAW（`bt_hidraw.cpp`） |
|---|---|---|
| 链路 | 裸 `AF_BLUETOOTH` L2CAP，自己走 HIDP 组帧：让 BlueZ 拉起 ACL → 释放 HID profile → 抢 PSM 0x11（控制）/0x13（中断）→ 切 report protocol | 内核 hidp/hidraw 建好的 session，打开 `/sys/class/hidraw` 节点 |
| 开链唤醒 | 末尾补发 142 字节 0x32（冻结数组，`WaitWritable` 5×10 ms 重试） | 无（内核驱动建节点时已初始化过手柄） |
| feature 取值 | 控制通道 `GET_REPORT`，整段预算 2 s | `HIDIOCGFEATURE` ioctl |
| 手柄识别 | MAC（BD_ADDR） | VID/PID 匹配真 `054C:0CE6` 或 DKMS 合成的 `1998:1221`，MAC 可选 |
| MTU/尺寸 | `kWantMtu = 672`、音频帧 `kHidpAudioFrameBytes = 399` | 由内核决定 |
| 额外依赖 | `pkg_check_modules(BLUEZ)`、`gdbus`（`--timeout 5`）调用 | 无 |

---

## 7. usb —— 虚拟 USB 半边

`src/usb/usb.h` 的两半边按"知道什么"划分：root 半边（`ResolveGadgetId`/`Provision`/
`Teardown`/`HasDummyUdc`/`IsSafeGadgetPath`）与 worker 半边（`Adopt`/`Close`/
`StartThreads`/`StopThreads`/数据面原语）。一个 gadget 由 `(挂载点, 实例名)` 这对坐标
标识，Manager 可以同时持有 N 对；`Provision` 的产出是 `usb::Station`（三个端点 fd +
本站在主机侧的 sysfs 接口目录）。root 半边的下面是一层纯机制：`usb::detail` 的
`GadgetSetup`/`GadgetBind`/`GadgetTeardown`/`FindHostDevicePath`（configfs 建到 bind，
再加 bind 后的 sysfs 反查，后者内部是 `FindHostDeviceDir` + `FindHostInterfaceDir` 两步）
与 `FfsDevice`
（端点 fd 的开/认领/读写）。机制层没有"记住上一次调用"的文件级状态，坐标每次由调用方
显式传参；`Provision` 把三个端点 fd 的所有权交给调用方（Manager 在 fork 之后关掉自己
那份副本），只有 worker 的 `Adopt` 会把设备记进 `usb::plane`。

数据面内部按 bt 的样式切成四块：`usb_plane`（共享状态与三个循环入口，收在
`usb::plane`）、`usb_inout`（IN/OUT 两条循环）、`usb_ep0`（ep0 事件循环 +
`Ep0Responder` 的方法实现）、`usb.cpp`（只剩门面与起停装配）；模块之外只经
`usb.h` 的 `Handlers` 回调（含 `Ep0Responder`）与这四个 TU 交互。

数据面必须是三条阻塞线程而不是 poll 循环：**FunctionFS 端点文件没有 `.poll` 实现**，
`poll()` 永远报 ready，真正阻塞发生在 `ffs_epfile_io()`/`ffs_ep0_read()` 内核调用里。
`StopThreads()` 靠 `StartThreads()` 装的 SIGUSR1 空处理函数打断这三个阻塞点。

OUT 读缓冲按端点的 `wMaxPacketSize`（`core::device::kUsbEpMaxPacketSize`，64）开，
不按 0x02 报告的 48 字节开：`dummy_hcd` 在短包上做溢出判定
（`scan_urb`：`to_host == false && host_len > dev_len` → 请求状态 `-EOVERFLOW`），
所以主机写一个 **49..63 字节**的短包（比报告长、比 maxpacket 短）时，48 字节缓冲
表现为"端点读坏掉"而不是"包太长"，OUT 线程退出并带走整个 worker。正好 64 字节
不会踩中——那不是短包，会被拆成 48 + 16 两份请求。`descriptors.h` 里两条
`static_assert` 把这个常量钉在描述符的 wMaxPacketSize 字节上。

输入侧是**覆盖式槽位**（`PostInputReport`）：只保留最新一份，发布快于消费就覆盖而非
排队，所以主机读到的永远是"当前状态"而不是历史序列。槽位内容不在 usb 里播种（它不认识
报告格式），第一份由 `bridge::Init()` 发布，`usb::StartThreads()` 不会冲掉它。

转发策略（class 请求怎么答、OUT 报告送去哪）全部由调用方以 `Handlers` 回调注入：
class 请求的应答经回调参数里的 `Ep0Responder` 完成（f_fs 的应答怪癖——无数据 OUT
请求的零长 read、STALL 的 read/write 方向语义——收在它的四个方法里），
**usb 层不认识 bt，也不认识报告内容**。

实例名按 MAC 派生：`usb::ResolveGadgetId(mac, ffs_base)` 给出
`("ds5dummy-<12 位小写 MAC 十六进制>", "<ffs_base>.<同样 12 位>", "<同样 12 位>")`，
第三个字符串就是本站的 USB 序列号（写进 configfs `strings/0x409/serialnumber`），
实例名同时是 gadget 目录名、FunctionFS 的命名空间和 mount 的 source。序列号必须逐站
唯一：主机把 iSerialNumber 当设备实例标识，两站共用一个值会被看成同一台设备反复插拔，
所以它不和 VID/PID 那样冻在 `identity.h` 里，而是与实例名同源派生。用整段 MAC 而不是
末几位是刻意的：实例名撞车会让第二站的 `GadgetSetup` 把第一站的活标签当成残留拆掉。手工诊断
路径（`tools/mini_gadget`）仍用固定的 `usb::detail::kGadgetInstance = "ds5dummy"` 与
`/dev/ffs-ds5`。`gadget_setup.cpp` 会自己 mount configfs（并把只读挂载 remount 成 rw）
和每站的 FunctionFS，`scripts/setup.sh` 只准备内核模块与 configfs，不再挂载 FunctionFS。
`GadgetBind` 在 `/sys/class/udc` 里挑一个没被任何 configfs gadget 占用的 `dummy_udc.N`，
所以 `num_dummy` 要 ≥ 同时服务的手柄数。进入 `mkdir`/`mount`/`symlink` 的路径字符串
必须过 `IsSafeGadgetPath`（只放行字母/数字/`.`/`_`/`/`/`-`，并显式拒绝含 `..` 的串），
`gadget_setup.cpp` 里还会再挡一遍。

bind 之后 `Provision` 会带预算地（2 s，50 ms 步长）等主机把设备枚举出来，再从
configfs 的 `UDC` 属性反查本站：`/sys/class/udc/dummy_udc.N` canonical 之后上溯两级
是 UDC **自己的**平台设备（`/sys/devices/platform/dummy_udc.N`），root hub 不在它下面，
而在**同索引的兄弟目录** `/sys/devices/platform/dummy_hcd.N` 里——`dummy_hcd_init` 用同一
个索引各注册一个平台设备，所以配对就是把名字里的 `udc` 换成 `hcd`。那个目录下的 `usbM`
（root hub）里 VID/PID 相符的那个 child 即本站**设备**目录，再往下降一级取它的接口目录
（sysfs 命名 `<设备名>:<配置号>.<接口号>`，本站形如 `9-1:1.0`；多个接口取字典序最小，只为
结果跨重启稳定，父设备是同一个）。
**交出的是接口目录而不是设备目录**：读这个键的一方（Wine 的 `get_container_id`）沿**父链**
找最近的 `usb`/`usb_device` 祖先，再取那个祖先的 `PRODUCT`/`BUSNUM`/`DEVNUM` 算容器标识；
从设备目录自己出发会跳过它、命中 root hub（身份是内核 HCD 的 `1d6b:...`），算出来的容器就
不属于本站。真声卡那一级是 `…/1-1/1-1:1.3/sound/card1`——我们背后没有 ALSA 卡，接口目录
就是这棵树上父链正确、且与真机最同形的东西。路径**去掉 `/sys` 前缀**（形如
`/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`，与 PipeWire 真设备上 `device.sysfs.path`
的口径一致）存进 `Station`，经 `--sysfs-path` 交给 worker 写进两条流的节点属性。这一步是
可选信息：等不到只留空并记 WARNING，站点照常可用。
换算只认 `dummy_udc` 前缀（真实控制器的名字换不出配对目录，返回空而不是猜）。

描述符块 `core::device::kFfsFsDescriptors` 是冻结的，唯一在装配期被改写的字节是两个
中断端点的 `bInterval`：`Provision()` 收到 Manager 按 `usb_report_rate` 算出的
毫秒数（1000/500/250 Hz → 1/2/4），`FfsDevice::WriteFfsDescriptors()` 拷一份再按
`kFfsEpInIntervalOffset`/`kFfsEpOutIntervalOffset` 覆写。它只限制**主机取报告的
上限**；蓝牙侧的上报节奏由手柄固件固定，USB 侧调多快都不会让手柄多报。

---

## 8. bridge —— 转发策略

`src/bridge/bridge.h`：**本模块没有线程、没有队列**。每个入口都在调用方的
线程上把报告组好、交出去就结束（交给 `bt::PostOutput`、`usb::PostInputReport` 或
`MicSink`）。

- `Init(bt_addr, speaker_pre_gain)` 必须是本模块第一个调用，且早于两个 `StartThreads()`；
  它记下 MAC、按固件初值备好输出状态（`byte[4]/byte[5]` 的耳机/扬声器音量**必须非零**，
  否则控制器会把每一份 0x36 静音），把用户配的扬声器前置增益写进 `byte[37]` 低 3 位，
  并发布会话里第一份空闲 USB 输入报告。`UpdateOutputState` 有意不合并音频控制类字段，
  所以这份增益不会被主机报告冲掉。
- `OnBtInput`：`[0]=0x31 [1]=flags [2..64]=63 字节状态体` → 重排成 `[0x01][状态体]`
  → `usb::PostInputReport`。`len<2`、id 不是 `0x31`、`len<65` 时丢弃（`len<65` 记
  WARNING）。flags bit1 时这份 0x31 装的是麦克风帧：`[3..73]` 的 71 字节 Opus 包体交给
  `MicSink`，不发布 USB 输入报告；开关没开、没装 sink 或 `len<74` 时丢弃，这些只写
  `DS5DUMMY_DLOG`（100 Hz，WARNING 会刷屏）。
- `SetMicSink` 装填上行 Opus 包体的去处（生产实现 `audio::OnMicPacket`），必须在
  `bt::StartThreads()` 之前调用；空 sink 表示不接收上行帧。
- `OnMicOpen`：主机开始/停止录音时把 142 字节的 0x32 状态报告（`mic_report.cpp`，
  模板里只有 `[4]` 的 bit0 翻转）交给 `bt::PostOutput`。`mic_open` 是 atomic、同值
  去重；队列满时按"丢最新"丢掉这份状态报告并记 ERROR。
- `OnUsbOutput`：`len ≥ 48` 时取 47 字节 payload 更新 63 字节 `SetStateData`，再把同样的
  47 字节搬进 78 字节的 0x31 报告体（`body[0]=0x31`、`body[2]=0x10` 置 rumble 使能）
  交给 `bt::PostOutput`。id 不是 `0x02` 或 `len` 为 0 静默丢弃且不记日志（端点上的常态噪声）。
- `OnAudioOutput`：64 B 触觉 + 200 B 喇叭拼成 398 B 的 0x36，`byte[10]` 段计数器**只在入队
  成功时**递增（队列满丢包不留空洞）。`byte[4]` 的 bit0 是 `mic_open` 的镜像
  （0xFF/0xFE）。它是唯一碰段计数器的线程，计数器因此不需要原子性。
- `OnClassRequest` 覆盖 GET/SET_REPORT、GET/SET_IDLE、GET/SET_PROTOCOL 六个类请求，
  其余 STALL。

---

## 9. audio —— 双向音频链

`src/audio/audio.h` 只搬**数据**：下行每段 `kHapticsSize=64` 字节触觉 +
`kSpeakerSize=200` 字节 Opus 喇叭，交给 `Start()` 传入的 sink（实际是
`bridge::OnAudioOutput`）；上行每帧 71 字节 Opus 包由 `OnMicPacket()` 推进来，解成
480 帧 PCM 交给虚拟麦克风。本模块不认识报告格式。

- PipeWire sink 是 4 通道 f32 @48 kHz 的 `Audio/Sink`：ch0/1 喇叭、ch2/3 触觉。节点名与
  硬件身份两样东西都由 `audio_identity.{h,cpp}` 一次生成，sink 与 source 共用（同一只手柄
  在主机上就是一台 USB 声卡，两条流报出两个身份、或者名字不同形，会让按硬件查端点的
  消费者只认其中一条）：
  - **节点名**（`MakeSinkNodeName` / `MakeSourceNodeName`）不是标签而是判断量：GE-Proton 的
    winepulse 认"这只手柄的喇叭 sink"靠的是名字里同时出现
    `alsa_output.usb-Sony_Interactive_Entertainment_`、`Wireless_Controller`、`Speaker__sink`
    三段 `strstr`。形状照真手柄有线模式下 UCM 切出的名字，中间占位的那段是本站的
    12 位 MAC 十六进制（真机那个位置是 USB function 号 `-00`），两站并发靠它不撞名。
    这条判据只在游戏进程带 `PROTON_DUALSENSE_SPLIT_AUDIO=1` 时才走全套 split 路径，那是
    启动器侧的环境变量，本程序不设置它。
    昵称/描述仍用 `USB054c:0ce6.<mac 12 位>`（`MakeDeviceId`），让两只 dongle 在
    `wpctl`/`pavucontrol` 里可读地可分辨。
  - **`device.*` 键**（`ApplyHardwareIdentity`）：`device.bus=usb`、`device.vendor.id=0x054c`、
    `device.product.id=0x0ce6`、`device.product.name`（数值取自 `core/device/identity.h`，与
    gadget 描述符同源）、`device.class=sound`、`device.form-factor=controller`、
    `device.icon-name`（sink `audio-card-analog`，source `audio-input-microphone`）——后四项
    是同一只控制器有线模式下 ALSA 节点的实测字面量。真实声卡的这些键挂在 `Audio/Device`
    上由 pulse 层合并，我们的流节点没有 Device，所以直接挂节点上——pulse 层对节点字典
    全量透传。缺了 bus/VID/PID 这组，Wine 的 winepulse 只能落到 `ROOT\MEDIA` 匿名回落，
    `is_dualsense_audio_device()` 那条判据（bus 精确 strcmp "usb"，VID/PID 按 16 进制读回）
    就不成立。
  - `Station::host_sysfs_path` 非空时写成 `device.sysfs.path`（Manager 建站时解析、经 worker
    argv 传下来，见 §7），并从同一段路径里的 `dummy_hcd.<N>` 推出 `alsa.card` 与
    `api.alsa.card`（两个键同值，读方是"或"）：那个序号是本站虚拟主控制器的编号，与真
    ALSA 卡索引在"同一主机内唯一、按站稳定"这个含义上同构，GE-Proton 用它给同型号两只
    手柄定 Canonical 身份。解析不出序号就不写，不交出一个猜来的 0。
    `device.sysfs.path` 是**承重的**，不是装饰：pipewire-pulse 会把它改名成裸 `sysfs.path`
    透出给 PulseAudio 客户端（真声卡的 sink 上用 `pactl list sinks` 就能读到那一条），
    winepulse 的 `get_container_id()` 正是读这个键，再沿父链找最近的 `usb_device` 祖先，
    取它的 `PRODUCT`/`BUSNUM`/`DEVNUM` 拼出 USB 容器 GUID——那个 GUID 是 MMDevice 给端点
    注册容器身份用的，游戏按它把音频端点和 HID 端点认成同一台手柄。所以它必须指向
    **接口目录**（`…/9-1/9-1:1.0`）：指向设备目录 `…/9-1` 时父链跳过本站、命中 root hub，
    算出的容器是内核 HCD 的。
  两条流都显式写 `node.pause-on-idle=true`：真 ALSA 硬件节点上那个 `false` 是给设备保持
  唤醒的，我们背后没有设备要唤醒，反而要靠空闲挂起让编码环在没有流连着时停下来（否则
  就是往蓝牙上持续灌静音的 0x36）。刻意不搬的是 `device.api=alsa` 与其余 `alsa.*`
  （背后没有 ALSA 设备，抄卡名/PCM 名等于撒谎，而 `device.api=alsa` 会被 WirePlumber 的
  ALSA 规则匹配到）、`device.profile.*`（真机字面量没有实测来源），以及 `alsa.device`/
  `api.alsa.path` 一类指向真实 ALSA 对象的键。
  **不填 `priority.session`**：这个 sink 刻意不参与默认设备竞选，要它当输出得由用户显式
  `wpctl set-default` 或改路由。
- 编码几何（`audio_encoder.h`/`audio_encoder.cpp`）：输入一段 `512` 帧 4 通道交织 float，
  拆成喇叭（ch0/1）与触觉（ch2/3）两路立体声（`kEncodedChannels = 2`，两路各持一个独立的
  libsamplerate 状态，内部状态不可共用）；喇叭路重采样 512→480 帧后交 Opus
  （`kOpusFrameSamples = 480`，48 kHz 下恰 10 ms 一包），触觉路重采样 512→32 帧到
  `kHapticsSampleRate = 3000`，再乘 `haptics_gain` 量化成 int8（`Start()` 传入，0 = 静音、
  1 = 原样、2 = 两倍；乘在量化前，喇叭侧不受影响）。
- 交付节奏约 93.75 次/秒（512 输入帧 @48 kHz ≈ 10.7 ms 一段）。`OutputSink` 是**直接调用**，
  不是投递到别的线程：`bridge::OnAudioOutput` 的取快照、拼 398 B、进 `bt::PostOutput` 全部
  发生在 PipeWire 侧驱动的这条链上，代价是 sink 实现必须非阻塞、不自起线程、不长时间持锁。
  背压点只有一个，就是 bt 的有界队列。
- 断线重建：指数退避 500 ms × 2^attempt，上限 5 s，连续 5 次失败经 `on_lost` 通知调用方；
  worker 收到后 `audio::Stop()`，再由自己每 30 s（`kAudioRetrySeconds`）重新 `audio::Start()`
  一次直到成功或这一轮退出——期间 HID 一直在线，音频缺席不会拖 worker 重启。启动时音频
  就没起来也走同一条重试路径。重连成功会 `ResetInputBuffer()` 丢掉
  跨会话残留样本。
- 上行（`audio_source` / `audio_decoder`）：`Start()` 给了 `MicOpenHandler` 才建。Source 是
  单声道 f32 @48 kHz 的 `Audio/Source`（`PW_DIRECTION_OUTPUT`），节点名与 `device.*` 那
  一组硬件身份都出自 `audio::identity`、与同名 sink 同一族（名字换 `alsa_input...` 前缀与
  `HiFi__Mic__source` 末段，图标换成麦克风），`source::Start` 因此和 `sink::Start` 收同一对
  `station_id`/`host_sysfs_path`；
  `STREAMING ↔ PAUSED` 经 `on_mic_open(true/false)` 变成手柄端的开关（这个迁移要靠空闲
  挂起，所以 source 上也写 `node.pause-on-idle=true`）；解码器是单声道
  Opus，71 字节包解出 480 帧（10 ms），经 `source::PushSamples` 写进 2048 帧
  （≈43 ms）的 PCM 环，RT 回调欠载补静音。包队列深度 8（≈80 ms），满了丢最旧。
- `OnMicPacket` 由 `bridge` 在 bt 读线程上调用：只做一次 71 字节拷贝入队。Source 不重连，
  解码不重采样——时钟漂移由 PCM 环的"满覆盖、空补静音"兜底。
- `pw_init/pw_deinit` 是进程级的，只由 `audio.cpp` 成对调用：sink 与 source 共享同一套
  PipeWire 全局状态，子模块各自 init/deinit 会在拆一条流时把另一条拆死。

---

## 10. 线格式常量（`src/core/protocol/report.h`）

| 名称 | 值 | 含义 |
|---|---|---|
| `kUsbInputReportId` / `kUsbOutputReportId` | `0x01` / `0x02` | USB 侧 report id |
| `kBtInputReportId` = `kBtOutputReportId` | `0x31` | BT 常规报告，收发同值，方向由链路区分 |
| `kBtAudioReportId` / `kBtWakeReportId` | `0x36` / `0x32` | 音频复合报告 / 唤醒报告 |
| `kUsbInputStateSize` / `kUsbInputReportSize` | 63 / 64 | 状态体 / EP1 IN 总长 |
| `kUsbOutputPayloadSize` / `kUsbOutputReportSize` | 47 / 48 | 主机实际发的 payload / EP2 OUT 总长 |
| `kSetStateShadowSize` | 63 | 47 有效 + 16 主机从不发送的 reserved padding，0x36 需要整 63 |
| `kBtReportSize` / `kBtAudioReportSize` / `kBtWakeReportSize` | 78 / 398 / 142 | BT 三种报告体，均自带尾部 4 B CRC（USB 侧没有 CRC） |
| `kFeatureReportIds` | `{0x09, 0x20, 0x22, 0x05}` | 建链后预取清单，顺序即预取顺序；只有 0x09（反序 BD_ADDR）和 0x20（出厂日期）的字节会被解释 |
| `kHidpDataIn/Out`、`kHidpGetReport/SetReport/FeatureReply` | `0xA1/0xA2/0x43/0x53/0xA3` | 协议镜像值；实际使用方在 bt 后端自持同名常量 |

0x32 唤醒报告的布局：`[0]=0x32 [1]=0x10 [2]=0x90 [3]=0x3f [4..66]=63B SetStateData
[67..137]=0 [138..141]=输出报告 CRC`。

0x32 还承载麦克风开关，同一 id、不同子报告：`[0]=0x32 [1]=seq<<4 [2]=0x91
[3]=0x01 [4]=0b11（开）/0b10（关）[5..137]=0 [138..141]=CRC`。模板整份冻结在
`bridge/mic_report.cpp`，运行时只翻 `[4]` 的 bit0。

上行麦克风帧在 0x31 里的布局：`[0]=0x31 [1] bit1=1 [2]=跳过 [3..73]=71B Opus`；
不带麦克风负载的 0x31 才是 `[2..64]=63B 状态体`。帧尺寸常量在
`src/core/protocol/audio_format.h`：`kMicOpusSize=71`、`kMicChannels=1`、
`kMicSampleRate=48000`、`kMicFrameSamples=480`（10 ms 一包）。

---

## 11. 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBT_BACKEND=HIDRAW \
      -DDS5DUMMY_BUILD_TESTS=ON -DDS5DUMMY_BUILD_TOOLS=ON
cmake --build build
ctest --test-dir build
```

| 变量 | 默认 | 说明 |
|---|---|---|
| `BT_BACKEND` | `HIDRAW` | `HIDRAW` 或 `L2CAP`，链接期二选一；L2CAP 额外 `pkg_check_modules(BLUEZ REQUIRED bluez)` |
| `DS5DUMMY_STRICT` | `ON` | 追加 `-Wpedantic -Wsign-conversion -Wold-style-cast` |
| `DS5DUMMY_DEBUG` | Debug 为 `ON` | 编译诊断代码：计数器、写耗时、5 s 统计窗口；关闭时相关符号不存在 |
| `DS5DUMMY_DEBUG_LOG` | Debug 为 `ON` | 编译 `DS5DUMMY_DLOG(...)` 调试日志语句；关闭时参数不被求值。生产日志走始终编译的 `DS5DUMMY_LOG` |
| `DS5DUMMY_BUILD_TOOLS` | `OFF` | `tools/` 下的诊断程序 |
| `DS5DUMMY_BUILD_TESTS` | `OFF` | googletest + CTest |

C++20，CMake ≥ 3.16，`CMAKE_EXPORT_COMPILE_COMMANDS=ON`。必备依赖：
`libpipewire-0.3`、`libspa-0.2`、`opus`、`samplerate`、`yaml-cpp`
（`pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)`）、`libsystemd`
（`pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)`，identity 用它
枚举 logind 会话）、Boost 头文件（只用 `circular_buffer`）、`fmt`、pthreads。**缺库直接
configure 失败是设计选择**——静默退化成 HID-only 会让除了在用户配置里写
`audio: false` 的人之外都不知道音频没起来。

两个刻意的"不加"：
- 不加 `-Wconversion`：拼一个字节就得写 `static_cast`，而 `static_cast` 会把常量溢出的
  `-Woverflow` 一起压掉，噪声盖住真问题。只保留 signedness 一类。
- 不在 CMake 里定义 `_FORTIFY_SOURCE`：档位是发行版工具链的策略（Arch 的 makepkg 已经在
  `CXXFLAGS` 里给 `-Wp,-D_FORTIFY_SOURCE=3`），后定义的赢，重复定义等于降级。

加固编译开关：`-Wall -Wextra -Wformat=2 -Wformat-truncation -Wvla
-Wimplicit-fallthrough -Wshadow -fstack-protector-strong`，链接
`-Wl,-z,relro,-z,now`。

`docs/` 下有可选的 `doxygen` 目标（`cmake --build build --target doxygen`，输出
`docs/doxygen/html`，已 gitignore）；Doxygen 缺失时目标不注册。仓库有 `.clang-format`，
但**它不是门禁**：HEAD 上未改动的文件本身就和格式化输出不一致。

---

## 12. 测试

`tests/CMakeLists.txt` 把被测源文件直接编进一个 `ds5dummy_tests` 二进制，
`gtest_discover_tests` 让 ctest 看到逐个用例（当前 225 个，两个后端各跑一遍）。
**硬性约束：全部测试必须能在无 root、无手柄的环境下跑** —— 需要特权或硬件的验证是人工
清单，不进 CI/单测。因此 bt 的测试只覆盖门面到后端的纯转发契约、`tx` 的入队/seq/CRC/
背压、feature 缓存策略，硬件往返一律不测；`daemon/discovery` 与 `usb` 的测试自己搭临时
sysfs 树（假 `uevent` 文件、假 `dummy_udc.N/udc/dummy_udc.N` 与同索引兄弟目录
`dummy_hcd.N/usbM/<M>-1`，即真机上的那对兄弟形状；设备目录下面的接口目录
`<M>-1:1.0` 也照样摆出来），不碰真 `/sys`。

`tools/audio_dump` 是离线回听路径：audio 下行链 + dump sink，不链 usb/bt/bridge，手柄连不
连都能跑，用于单独验证编码半段。

---

## 13. 打包与内核侧

两个包都装 `/usr/bin/ds5dummyd`、`/usr/lib/ds5dummy/teardown.sh`、
`/usr/lib/systemd/system/ds5dummy.service`、`/usr/lib/systemd/system-sleep/ds5dummy`
（休眠钩子）、`/etc/ds5dummy/config.yaml`（进 `backup=`）、
`/usr/share/doc/ds5dummy/user-config.example.yaml`、`/etc/modprobe.d/ds5dummy.conf`、
`/etc/modules-load.d/ds5dummy.conf`，都 `provides=('ds5dummy')`、
`replaces=('ds5dummy')` 且互相 `conflicts`，`pkgrel=2`，`depends` 含 `yaml-cpp` 与
`systemd-libs`，构建参数固定在
`-DCMAKE_BUILD_TYPE=Release -DBT_BACKEND=<对应后端> -DDS5DUMMY_STRICT=ON
-DDS5DUMMY_DEBUG=OFF -DDS5DUMMY_DEBUG_LOG=OFF -DCMAKE_INSTALL_PREFIX=/usr`。

systemd 单元以 root 跑（Manager 必须 root），`ExecStart=/usr/bin/ds5dummyd` 不带任何
参数与环境变量：降权目标由 Manager 从活跃图形会话推导，用户级生效值来自两份 YAML。
`Restart=on-failure`、`RestartSec=2`，stdout 与 stderr 都进 journal——启动横幅（目标
用户、两份配置路径、发现轮询间隔、桥接模式、每层手柄设置、最终日志级别）只出现在那里。

休眠不走单元指令：`pkg/ds5dummy.sleep-hook` 装到 `systemd-sleep` 目录。挂起前若单元
active 就先在 `/run/ds5dummy/was-active` 留标记再 `systemctl stop`（Manager 拆 gadget、
worker 退出）；恢复后见标记就 `systemctl start`。留在 `/run` 的标记在重启后自然消失，
所以睡眠中崩溃不会让下次开机凭空拉起服务。绑定中的 FunctionFS gadget 和蓝牙链路都
过不了挂起周期，停掉重建比原地恢复可靠。

模块参数对两个后端都一样是 `options dummy_hcd is_high_speed=0 num_dummy=1`（虚拟 UDC 只
跑 full-speed，因为本项目提供的是 FS 描述符；`num_dummy` 是同时能服务的手柄数上限，
多手柄要调大）。差别在 HID 侧：
- `pkg/ds5dummy_hidraw` 的 modules-load 里装的是 `hid_playstation_ds5dummy`（DKMS 补丁，
  把真蓝牙手柄留在 hidraw-only 并合成 `1998:1221`），`hidp` **必须**可用、不能 blacklist；
- `pkg/ds5dummy_l2cap` 装的是 in-tree `hid_playstation`，让 BlueZ 先建 HID session 再交接
  PSM。

---

## 14. 代码规范

- 注释是**文档级**的：Doxygen 块（`/** @brief ... @param ... @return ... @note @see @warning`）
  写在声明/定义之上，简体中文；调用方契约放头文件声明，实现细节/回退顺序/本地约束放定义处，
  同一段文字不在两处重复。平凡的私有/文件内 helper 也要一行职责注释。
- 注释必须与代码同步：改了行为就改注释；与行为矛盾的老注释算 bug。注释里不编造出处
  （性能数字、驱动行为、阈值来源），经验值就写"经验值"。
- 代码与注释里不留任何过程痕迹（"本次修改"、"根据需求"、阶段名、review 记录），不留
  `@author`/`@date`/手维护版本号。
- 日志统一走 `src/core/debug.h` 的两个宏，业务代码不直接调用 loguru 的 `LOG_F`：
  `DS5DUMMY_LOG(verbosity, ...)` 编进所有构建（运行期继续按 `-v` 过滤），
  `DS5DUMMY_DLOG(verbosity, ...)` 只在 `DS5DUMMY_DEBUG_LOG` 打开时存在；`LOG_F` 只出现在
  `debug.h` 的映射定义里。用这两个宏的 TU 仍需自己 include `loguru.hpp`。
- 提交信息用 conventional 前缀 + 模块名，例如
  `refactor(bt): split the transport into a POLLIN read thread and a POLLOUT write thread`。
- 第三方文件（`third_party/loguru.*`）不改。

---

## 15. 当前代码里已知的薄弱点

改到相关位置时值得先看：

1. `src/daemon/worker.cpp` 的失败提示建议 `echo blacklist hidp`，但 hidraw 后端**依赖** hidp；
   这条文案对默认后端是错的。
2. 读侧 5 s 静默判死对两个后端都生效，未按真实手柄空闲时的上报节奏验证过。
3. ep0 线程的 `GET_REPORT(FEATURE)` 在缓存未命中时会在该线程上同步等最长 2 s，期间整条
   控制面停住；缓存的"只在传输线程之外访问"这条无锁前提没有代码强制。
4. `bt_l2cap.cpp` 的 `frame[672]` 与 `kWantMtu` 是同一数值的两处写法。
5. `src/daemon/worker.cpp` 的 5 秒统计行里，USB tx 一栏打印的是累计值、括号里却是窗口
   速率（`usb_tx` 对 `usb_tx_delta`），与同一行 BT rx 的"窗口值 + 窗口速率"口径不一致，
   读日志时容易把累计值当成 5 秒计数。
6. 手柄实机上的 0x36 播放会在一到十几秒内断链（读侧报链路已死），编码半段单独验证是好的；
   断链根因尚未定位。
7. `identity.mode: list` 只认 active 会话：名单内用户的图形会话不活跃（切走 VT）而另一个
   名单外用户 active 时，`SelectWorkerIdentity` 会因为没有匹配的候选返回 `kDisabled`，
   服务停在 inactive。这是"只认 active 会话"的设计结果，不是名单写错。
8. `audio_source` 不重连：只有 Source 这条流坏掉而下行 sink 仍健康时，上行麦克风会一直
   静默——worker 的 30 s 重试要等 `on_lost` 把整条音频链拆掉才触发。PipeWire 整体重启会
   连 sink 一起打掉，那种情况能在重试里恢复。
9. 麦克风开关报告与音频共用 bt 的 8 槽发送队列，队列满时按"丢最新"丢掉；手柄状态要等
   下一次开/关录音才纠正。这条只记 ERROR，没有重发。
10. 上行链路没有实机验证过：开关包与 71 字节帧长照上游实现，但"主机打开录音 → 手柄
    开始上行"的端到端时序尚未在手柄上跑过。
11. 多手柄路径只有单测级证据：`daemon/discovery` 的解析有测试，但"发现 → 多站并发 →
    各自重试 → 整族拆除"没有在多只手柄（或多适配器）上跑过；`num_dummy` 不够时看到的是
    单站 Provision 失败加 2 s 退避，而不是启动期报错。
12. 发现依据是 hidraw 节点。L2CAP 后端拿到 PSM 后内核 HIDP 会话不复存在，节点随之消失；
    此时 worker 若死掉，Manager 已从 `slots_` 里删掉该 MAC，要等 BlueZ 重新建出节点才会
    再服务它。hidraw 后端没有这一步。这条只按代码路径推断，未在实机上验证。
