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

主机看到的设备身份随手柄型号变：普通 DualSense 是 `054c:0ce6` "Sony Interactive
Entertainment / DualSense Wireless Controller"，DualSense Edge 是 `054c:0df2`
"DualSense Edge Wireless Controller"，HID report descriptor 也跟着换（337 / 453
字节）。型号默认在发现阶段按蓝牙 HID 的 VID/PID 判定，用户配置里的
`usb_model: auto|dualsense|edge` 可以逐手柄覆盖它；两个型号的档案冻在
`src/core/device/`（`DeviceProfile`）；序列号不在那里——它是每站一个、
由手柄 MAC 派生的值（见 §7）。
麦克风开关走 HID 通道（0x32 状态报告），不走音频通道：主机打开虚拟麦克风开始
录音时程序才让手柄开麦。
转发之外的可选行为（现在有两个：多久没人碰手柄就让它自己关机、改写主机看到的按键
形态）由 `src/plugin/` 的
进程内插件承担，宿主和插件之间只隔 `plugin_api.h` 那份 C 边界（见 §10）。

---

## 2. 仓库布局

| 路径 | 内容 |
|---|---|
| `src/main.cpp` | 唯一入口：解析 argv（只剩 `--worker`/`--help`）、worker 侧读并解码配置快照、装信号 handler、分派两个角色 |
| `src/core/` | 协议与设备静态模型：报告 ID/线尺寸、输入/输出状态编解码、CRC32、USB 描述符、设备身份 |
| `src/daemon/` | 进程形态：`config`（YAML 解析 + 加固读取）、`discovery`（hidraw sysfs → 手柄 MAC）、`identity`（sd-login 会话 → worker 身份）、`options`（CLI 解析 + worker 配置快照的编解码）、`shutdown`（信号）、`manager`（root 多槽监督循环）、`worker`（数据面宿主）、`handoff`（fd 交接）、`privdrop`（降权执行） |
| `src/usb/` | `gadget_setup`（configfs 装配 + mount）、`ffs_device`（端点 fd 机制）、`usb`（门面 + 线程起停）、`usb_plane`（共享运行状态）、`usb_inout`（IN/OUT 两条循环）、`usb_ep0`（ep0 循环 + Ep0Responder 实现） |
| `src/bt/` | `bt`（门面）、`bt_link`（生命周期/缓存策略/线程起停）、`bt_plane`（共享运行状态）、`bt_rx`/`bt_tx`（两条传输线程）、`bt_detail`（后端接缝）、`bt_l2cap`/`bt_hidraw`（二选一的后端 TU） |
| `src/bridge/` | `bridge`（转发策略）、`state_report`（63 字节输出状态影子）、`fake_mac`（0x09 合成）、`usb_report`/`audio_report`/`mic_report`/`power_report`（四种报告的字节布局） |
| `src/audio/` | 双向音频链：`audio`（门面）、`audio_identity`（sink/source 共用的节点名与 `device.*` 硬件身份）、`audio_port`（sink/source 共用的建流骨架与断线退避重建）、`audio_sink`（PipeWire 4 通道 sink）、`audio_encoder`（SRC + Opus + 编码线程）、`audio_decoder`（Opus 解码线程）、`audio_source`（单声道虚拟麦克风 Source） |
| `src/plugin/` | 进程内插件的宿主侧：`plugin_api`（插件作者 include 的那份 C ABI 契约，含标记节的宏）、`plugin_util`（插件作者的可选小工具，纯头文件）、`plugin`（门面：实例表 + 输入钩子表 + 目录扫描 + `OnTick` 分派）、`plugin_loader`（目录列举、ELF 标记检查、`dlopen` 机制） |
| `plugin/` | 随包插件源码，与宿主分开编译：`idle/` 只 include `src/plugin/` 的两份插件作者头文件，不链接宿主任何符号，`c++ -std=c++20 -fPIC -shared -I src plugin/idle/idle.cpp` 就能单独编出 `idle.so`；`buttons/` 同一条路（六个 TU 编成一份 `buttons.so`，它自己的程序侧说明在 `plugin/buttons/AGENTS.md`）；两个目录里还各有一份供配置页面使用的 `<插件名>.panel.js` |
| `tests/` | googletest 单测（`ctest`，无 root、无硬件） |
| `tools/` | 诊断程序：`spike_l2cap`、`spike_rumble`、`mini_gadget`、`audio_dump`(+`dump_sink.*`)、几个 `spike_*` 特权/交接探针；`buttons_check` 是 buttons 配置的只读校验器（共用 `buttons_config`，不装载 `.so`），`buttons_web.py` + `buttons_web.html` 是本地配置页面（Python 回环服务、固定地址 `127.0.0.1:8777`、无令牌；壳画服务状态、配置层选择与只读的当前状态，并取回每层的插件节推给面板——面板不读文件、不发请求；插件界面由插件目录里的 `<插件名>.panel.js` 提供，没有面板的插件退回通用键值编辑器；判定仍调 `buttons_check`），`panels/core.js` 是随工具的 core 面板；`win_audio_probe.cpp` 是唯一的 Windows 侧探针（mingw 交叉编译，在 Wine/Proton 前缀里读 MMDevice 与 setupapi，不参与 Linux 构建） |
| `pkg/ds5dummy_hidraw`、`pkg/ds5dummy_l2cap` | 两个互斥的 Arch 包：systemd 单元、`/etc/ds5dummy/config.yaml`、`/usr/share/doc/ds5dummy/user-config.example.yaml`、modprobe/modules-load 配置、install 钩子 |
| `hid-playstation-ds5dummy/` | 内核 `hid-playstation` 的 DKMS 补丁（hidraw 后端用），只接管蓝牙 DualSense/Edge，合成 VID/PID（`1998:1221`/`1998:1103`）；DualShock 4 与所有 USB 设备走原驱动路径 |
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
`054c:0ce6`/`054c:0df2` 与合成的 `1998:1221`/`1998:1103`，MAC 取自 `HID_UNIQ`，
VID/PID 同时定出型号——Edge 与普通 DualSense 的虚拟呈现由此分岔，配置里的
`usb_model` 不是 `auto` 时按配置覆盖它），再 `waitpid(-1, WNOHANG)`
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

fork 之后子进程继承四条通道，`src/daemon/handoff.cpp` 把它们挪到**固定的
3/4/5/6**（`kEp0Target`/`kEpInTarget`/`kEpOutTarget`/`kConfigFdTarget`；从 3 起是因为
stdio 占 0/1/2）：三个端点 fd 加一条配置快照 fd。源 fd 若正好占着别的通道的目标号，
先 `F_DUPFD` 逃到 7 以上，再逐条 `dup2`；随后 `close_range` 清掉 6 之上的其余继承
fd，清掉四个目标上的 `FD_CLOEXEC`（它们必须活过 execv），然后按 `privdrop` 切身份，
最后 `execv` 自身，argv 只剩 `{self_path, "--worker"}`。

配置不再走 argv。Manager 把生效配置（用户级字段已由 YAML 覆盖过）编成一份**变长
快照**——`Options::Encode`，定长标量头 + 长度前缀的字符串与
`插件名.键路径 → 值` 条目，条目的 map 顺序让同一份配置每次编码都逐字节相同——写进
`memfd_create` 出的无名字文件（`MFD_ALLOW_SEALING`），加上
`F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_SEAL` 后交接。worker 的新镜像在
`loguru::init` 之前用 `handoff::ReadConfigFd`（`fstat` 只收非空普通文件、1 MiB 上限，
再 `pread` 读满）取回字节，交 `Options::Decode` 做**纯结构**校验
（magic、`total_bytes` 必须等于实长、字符串不得越过剩余字节、不许有尾随字节），
然后把解出的字段写进 `Options` 并关掉 fd 6。取值不在这里校验：配置层在更早的一步
已经收口。解码失败在 loguru 起来之前打到 stderr 并退出 2。

fork 之后、execv 之前的路径**禁止内存分配与 loguru 调用**（父进程已有 loguru 日志线程
跑过 malloc，锁状态不可信）。因此 `handoff.h`/`privdrop.h` 每个判断都有
`const char**` 的无分配重载；快照的编码、memfd 读写与解码都不在这段路径上，用的是
分配版。这条路径上的失败用 `_exit(kExecExitBase + stage)` 上报，`kExecExitBase = 120`，
`ExecStage` = `kBadPlan(1)`/`kMove(2)`/`kCloseRange(3)`/`kDropPrivs(4)`/`kExec(5)`；
`ReportChildExit` 靠区间把它和 worker 正常退出码（0/1/2）区分开。

### 降权 worker

`src/daemon/privdrop.cpp`：目标身份由 `identity` 模块从活跃图形会话选出（`auto` 用
passwd 主组，`list` 用配置条目里的 gid（省略时等于 uid）），`DropTo` 只负责执行，没有第二套身份来源
——既没有 `--uid/--gid`，也不读 `SUDO_UID/SUDO_GID`。切换顺序 `setgroups(0)` → `setgid`
→ `setuid`，不可调换（先清补充组，否则 wheel/input 组被原样带过去；丢了 root 就改不了
组），最后用 `setuid(0)` 反向验证：必须失败。

worker（`src/daemon/worker.cpp`）认领 fd（`usb::Adopt`）、建蓝牙链路、起全部数据面
线程、跑 100 ms 主循环直到退出。它不读配置文件：每站的数据面设置就是配置层的
`ControllerConfig`（`bt_addr`、`bt_wait_s`、`audio`、`haptics_gain`、`speaker_gain`、
`usb_report_interval_ms`、`usb_model`、`host_sysfs_path` 与插件袋），Manager 从两份配置
与发现结果叠加、把 `usb_model` 的 auto 定成具体型号后整份交给它——`Options::controller`
与它是同一个类型，交接前一次赋值，不逐字段改名或取反。`bt_addr` 与 `host_sysfs_path`
是配置层不写的两个运行期字段：前者由 Manager 按发现结果（或根级 `bt_addr`）定成本站
MAC，后者在建站时从 sysfs 解析出本站的 USB 接口目录写入；worker 只把后者转交给
音频的 sink 与 source——它已降权，也不认识 configfs 坐标，自己查不到；
`usb_report_interval_ms` 也在快照里，因为 IN 线程的发送节拍要靠它在 worker 侧自己排，
描述符里那个 `bInterval` 不构成节奏。它连 PipeWire 用的是桌面用户的身份，所以需要
`XDG_RUNTIME_DIR`；`RestoreXdgRuntimeDir()` 在环境变量丢了的时候按当前 uid（或
`SUDO_UID`）回落到 `/run/user/<uid>`。插件配置里的 `~/` 在 worker 侧展开，所以
`RestoreHome()` 在同一步把 `HOME` 改成当前 uid 的 passwd 家目录——Manager 以
root 起，继承下来的 `HOME` 是 root 的。

插件袋也走同一份快照：每个 `插件名.点分键路径` 一条长度前缀的条目，由 Manager 从
`global.plugins` 与 `controllers.<MAC>.plugins` 合并后的那一份压出来，daemon 只搬字符串、
不认识任何键名（见 §10）。worker 在连上手柄
之后、`bt::StartThreads()` 之前 `plugin::Start()`，把交出的输入钩子逐个
`bridge::AddStateHook()`；在两条传输线程都 join 之后先 `bridge::ClearStateHooks()` 再
`plugin::Stop()`——钩子还在别的线程上跑的时候不能释放实例。

`main.cpp` 刻意不让 `loguru::init` 扫 argv：日志级别的唯一来源是配置（由配置层解析成
数值、随快照下来），而 loguru 的名字表不认 `DEBUG`，遇到不认识的值是
`CHECK_F` 直接 abort。`stdout` 设成行缓冲，否则重定向到管道时 gadget/bridge/audio
的里程碑交错会丢。

---

## 4. 模块边界规则

- 单一 include 搜索路径 `src/`（`DS5DUMMY_MODULE_INCLUDE_DIRS`）。跨模块 include
  必须写成 `<dir>/<dir>.h` 门面形式，内部头也带目录前缀 —— 从 include 就能看出
  它是否越过了门面边界。
- 每个模块的门面是 namespace 自由函数，**进程级单实例**（一个 worker 服务一只手柄）。
  多手柄靠一个 Manager 起多个 worker 进程（每站一份 bt/usb/audio 单实例）。
- 不该外泄的实现细节进 `<mod>::detail`：例如 `bt::detail` 放后端契约（§A 8 个原语）、
  feature 缓存（§B）、跨后端共享字节（§C）；`usb::detail` 放 configfs 机制层
  （`gadget_setup.h`）与端点 fd 类（`FfsDevice`）。业务模块只走门面，唯一的例外是
  `tools/mini_gadget`（诊断程序，故意绕开门面直接驱动这一层）。
- 数据面线程共享的进程内状态收 `<mod>::plane`（`bt::plane`、`usb::plane`），只在
  模块自己的 TU 里可见；它不进 detail——detail 有 mini_gadget 那个诊断工具例外，
  plane 没有。
- 依赖方向：`core` 不依赖任何人；`bt`/`usb`/`audio`/`plugin` 只依赖 `core`；`bridge` 是唯一
  认识"两个方向"的模块；`daemon` 是装配者。

---

## 5. 线程清单

worker 进程里本仓库起的 12 条线程，加上 libpipewire 自己为两条 RT 流各起的
data-loop 线程一共 14 条（loguru 的日志线程不计）。插件不额外占这一页：它的钩子
都在下面已有的线程上跑（随包的 `idle` 不自起线程），但一个 `.so` 插件可以在 `init`
里起自己的线程——那种线程由插件自己在 `stop` 里 join，宿主看不见也不管：

| 线程 | 归属 | 干什么 | 阻塞性约束 |
|---|---|---|---|
| 主线程 | `daemon/worker` | 100 ms 一轮：睡、收音频失效通知、把 `plugin::OnTick()` 递给每个插件，诊断构建下每 5 s 打一行窗口统计；建链阶段在 `OpenBtWithWait()` 里 500 ms 重试 | 唯一调用 `bt::Open/Close/StartThreads/StopThreads` 的线程 |
| bt 读线程 | `bt/bt_rx.cpp` | poll 链路 fd + `stop_fd`，100 ms tick；把裸输入报告交给 `on_input` | 静默 1 s 打 WARNING，5 s 判死；回调里不许阻塞 |
| bt 写线程 | `bt/bt_tx.cpp` | 队列空时 poll `wake_fd`/`stop_fd` 睡等；有活时 poll `stop_fd` + 链路 POLLOUT（无超时），排空 ≤8 份的发送队列 | 手上一份没发出去就原地重试同一份字节，不退回队列 |
| ep0 线程 | `src/usb/usb_ep0.cpp` | 阻塞读 ep0 setup 包，标准请求自己答，class 请求交给 `on_class_request` | `GET_REPORT(FEATURE)` 缓存未命中时会在**这条线程上**同步等手柄回复（L2CAP 预算 2 s） |
| IN 线程 | `src/usb/usb_inout.cpp` | 持有最新一份输入报告：重发档链路 up 后按 `usb_report_interval_ms` 自排墙上时钟，到点就把这份重复写进 ep_in（主机不照 bInterval 轮询，§7）；real-time 档每份新发布只写一次 | 靠 SIGUSR1 打断阻塞 |
| OUT 线程 | 同上 | 阻塞读 ep_out，交给 `on_output_report` | 同上 |
| 编码线程 | `audio/audio_encoder.cpp` | 攒够一段就 SRC + Opus 编码，经 sink 直接调进 `bridge::OnAudioOutput` | — |
| 解码线程 | `audio/audio_decoder.cpp` | 从有界包队列取一帧，`opus_decode_float` 出 480 帧 PCM，交给 `source::PushSamples` | bt 读线程经 `bridge::OnBtInput` 只入队，不在读线程上解码 |
| sink 主循环线程 | `audio/audio_port.cpp` | 驱动播放 sink 的 PipeWire 事件（回调在 `audio_sink.cpp`） | — |
| source 主循环线程 | `audio/audio_port.cpp` | 驱动虚拟麦克风的 PipeWire 事件；`STREAMING ↔ PAUSED` 翻成 `bridge::OnMicOpen`（回调在 `audio_source.cpp`） | 回调里只做一次链路入队，不许阻塞 |
| sink 恢复 monitor 线程 | `audio/audio_port.cpp` | 等 sink 断线 → 指数退避重建 pipeline | `on_lost` 在这条线程上；**不得在里面调 `audio::Stop()`**（Stop 要 join 的就是它） |
| source 恢复 monitor 线程 | `audio/audio_port.cpp` | 等 source 断线 → 指数退避重建 pipeline；重建前清空 PCM 环 | 同上的 `on_lost` 约束 |
| PipeWire RT data-loop 线程 | libpipewire | 执行两个 `process` 回调：sink 侧 → `encoder::ProcessFrame`，source 侧 → 从 PCM 环填缓冲 | 不许日志、不许分配 |

Manager 进程只有主线程（+loguru）。

锁的分布：`bt::tx::queue_mutex`（发送队列）、`usb::plane` 的 `input_mutex`+`input_cv`
（持有的输入报告与链路标志）、`bridge/state_report.cpp` 的 `state_mutex`、
`audio_encoder` 的 `input_mutex`+`queue_cv`、`audio_decoder` 的 `mutex`+`queue_cv`、`audio::port` 每条流
一套 `Monitor` mutex+cv（sink 与 source 各一份，护断线位与闸门）、`audio_source` 的
`ring_mutex`。`Pipeline::running` 是 atomic，状态回调、`PushSamples` 与 `SetOpen` 在锁外读它。
`bt::plane` 一个锁都没有：运行标志与计数是 atomic，
两个 eventfd 的建立/关闭都落在没有线程在跑的时刻（`Start()` 早于起线程、`Finish()`
晚于 join）。feature 缓存同样无锁，靠的是"只在传输线程之前/之后动"的线程相位约定；
`bridge` 的 `mic_open` 是 atomic（PipeWire 主循环线程写，bt 读线程与音频编码线程读）。

---

## 6. bt —— 蓝牙传输层

门面（`src/bt/bt.h`）对外只有：`Open`、`Close`、`GetFeature`、`SetFeature`、
`StartThreads`、`StopThreads`、`PostOutput`、`Handlers`，加三个队列尺寸常量；
`Stats`/`StatsWindow` 只在 `DS5DUMMY_DEBUG` 构建里存在。
输出报告只有一条路：`PostOutput` 入队，写线程排空。`SetFeature` 是唯一的例外：它
不进队列、直写控制通道、不等应答，与 ep0 线程上的 `GetFeature` 由模块内一把锁串行化。

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

两个后端在链接期二选一（`-DBT_BACKEND=`），只实现 `bt_detail.h` §A 的 8 个原语：

| | L2CAP（`bt_l2cap.cpp`） | HIDRAW（`bt_hidraw.cpp`） |
|---|---|---|
| 链路 | 裸 `AF_BLUETOOTH` L2CAP，自己走 HIDP 组帧：让 BlueZ 拉起 ACL → 释放 HID profile → 抢 PSM 0x11（控制）/0x13（中断）→ 切 report protocol | 内核 hidp/hidraw 建好的 session，打开 `/sys/class/hidraw` 节点 |
| 开链唤醒 | 末尾补发 142 字节 0x32（冻结数组，`WaitWritable` 5×10 ms 重试） | 无（内核驱动建节点时已初始化过手柄） |
| feature 取值 | 控制通道 `GET_REPORT`，整段预算 2 s | `HIDIOCGFEATURE` ioctl |
| feature 写值 | 控制通道 `SET_REPORT`（`[0x53]` 事务头 + 完整报告），不等应答 | `HIDIOCSFEATURE` ioctl（整份报告含 report id 一起交给内核） |
| 手柄识别 | MAC（BD_ADDR） | VID/PID 匹配真 `054C:0CE6`/`054C:0DF2` 或 DKMS 合成的 `1998:1221`/`1998:1103`，MAC 可选 |
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

输入侧是**单份覆盖 + 两种发送节奏**（`PostInputReport`）：usb 只持有一份最新输入
报告，手柄每来一份状态就覆盖一次，不排队、不补发。`usb_report_interval_ms` 为
1/2/4 时 IN 线程在链路 up 后按这个毫秒数把这份状态写进 ep_in，两次蓝牙样本之间
主机收到的是逐字节相同的重复（seq 与 `sensor_timestamp` 都不变）。**节奏由
IN 线程按墙上时钟自己排**（`wait_until` 到下一格发送时刻）：dummy_hcd 不兑现中断
端点的 `urb->interval`——它的 `scan_urb` 对 `PIPE_INTERRUPT` 就写着 "this almost
certainly polls too fast"——主机取完一份立刻重新排队请求，阻塞写几乎不等待，靠它
定节奏会把一份状态以几千帧每秒灌出去（实测 7288 帧/s 对 102 个蓝牙样本/s）。
**回报率完全由这一档决定**：真手柄的 USB 回报率由自己的固件定，主机只能接受；虚拟
节点的回报率是我们写进 ep_in 的频次，所以主机侧看到的一切传感器行为都挂在这一个数
值上。踩错的后果不是"日志难看"，而是消费者直接被灌坏——实测几千帧每秒时游戏卡死/
崩溃，表现出的症状是陀螺仪读数异常。落后超过一格时以现在重排，不连发补欠下的帧；
发布不提前叫醒重发档。为 0（real-time）时只在 `PublishInput` 之后写一份，bInterval
仍取 1 ms，新样本到达的延迟最小。两档的主机滞后上限都是一个蓝牙上报周期，而不是
一串历史帧。持有的状态不在 usb 里播种（它不认识报告格式），第一份由
`bridge::Init()` 发布；`usb::StartThreads()` 不动它，ep0 的 `GET_REPORT(INPUT)`
与 IN 线程读同一份。

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
所以 `num` 要 ≥ 同时服务的手柄数。进入 `mkdir`/`mount`/`symlink` 的路径字符串
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
的口径一致）存进 `Station`，随配置快照交给 worker 写进两条流的节点属性。这一步是
可选信息：等不到只留空并记 WARNING，站点照常可用。
换算只认 `dummy_udc` 前缀（真实控制器的名字换不出配对目录，返回空而不是猜）。

描述符块 `core::device::kFfsFsDescriptors` 是冻结的，装配期被改写的字节有两处：两个
中断端点的 `bInterval`，和 HID class descriptor 的 `wDescriptorLength`（按本站的
`DeviceProfile` 取 337 或 453）。`Provision()` 收到的间隔就是
`usb_report_interval_ms` 这个数本身
（full-speed 下 bInterval 的单位恰是 1 ms，不做任何换算；real-time 档取 1），
`FfsDevice::WriteFfsDescriptors()` 拷一份再按
`kFfsEpInIntervalOffset`/`kFfsEpOutIntervalOffset` 覆写，同时按
`kFfsWDescLenOffset` 把长度两字节改成档案值；worker 的 ep0 应答
`GET_DESCRIPTOR(HID)` 时对那份 9 字节副本做同样的覆写。`bInterval` 是**给主机看的
声明**：
dummy_hcd 不按它轮询（见上面那段），所以它既不限制也不产生实际节拍——真实节奏
在 worker 的 IN 线程里。蓝牙侧的上报节奏由手柄固件固定，USB 侧调多快都不会让
手柄多报。

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
- `PowerOff()`：把整包冻结的 48 字节 0x08 feature 报告（`power_report.h`，
  payload[0]=0x02 关，尾部 CRC 是字节的一部分）经 `bt::SetFeature` 送上控制通道——
  不进发送队列、不等应答，因此它是 bridge 里唯一同步写链路的入口；调用线程会被这一次
  写占用，所以它不能在 bt/usb 的传输线程上调用。本提交只交付这个入口，还没有调用它的
  策略（什么时候该让手柄关机）。
- `SetMicSink` 装填上行 Opus 包体的去处（生产实现 `audio::OnMicPacket`），必须在
  `bt::StartThreads()` 之前调用；空 sink 表示不接收上行帧。
- `OnMicOpen`：主机开始/停止录音时把 142 字节的 0x32 状态报告（`mic_report.cpp`，
  模板里只有 `[4]` 的 bit0 翻转）交给 `bt::PostOutput`。`mic_open` 是 atomic、同值
  去重；队列满时按"丢最新"丢掉这份状态报告并记 ERROR。
- `OnUsbOutput`：`len ≥ 48` 时取 47 字节 payload 更新 63 字节 `SetStateData`，再把同样的
  47 字节搬进 78 字节的 0x31 报告体（`body[0]=0x31`、`body[2]=0x10` 置 rumble 使能）
  交给 `bt::PostOutput`。id 不是 `0x02` 或 `len` 为 0 静默丢弃且不记日志（端点上的常态噪声）。
- `OnBtInput`：`[0]` 之后的 63 字节状态体在摆成 USB 输入报告之后、发布之前按注册顺序过
  一遍输入钩子（`StateHook`）——插件改的就是主机将要读到的那一段，转发路径不因此多一次
  解码/编码。
- `AddStateHook` / `ClearStateHooks`：输入钩子表，全是 `void (*)(uint8_t*)` 裸函数指针。
  worker 在 `bt::StartThreads()` 之前把 `plugin::Start()` 交出的区间逐个注册进来，在两条
  传输线程 join 之后清空；bridge 因此不认识 plugin，也不解释钩子做了什么。
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
    昵称/描述以 `DualSense Wireless Controller` 开头、以本站的 12 位 MAC 十六进制结尾，
    让两只 dongle 在 `wpctl`/`pavucontrol` 里可读地可分辨。描述整串必须留在 62 字节以内
    （主机侧 Wine 的 `MAX_DEVICE_NAME_LEN`，超了它就丢掉描述改用 `device.product.name`），
    sink 与 source 各有一条 `static_assert` 把这条外部约束钉在编译期。
  - **`device.*` 键**（`ApplyHardwareIdentity`）：`device.bus=usb`、`device.vendor.id=0x054c`、
    `device.product.id=0x0ce6`、`device.product.name`（数值取自 `core/device/identity.h`，与
    gadget 描述符同源）、`device.class=sound`、`device.form-factor=controller`、
    `device.icon-name`（sink `audio-card-analog`，source `audio-input-microphone`）——后四项
    是同一只控制器有线模式下 ALSA 节点的实测字面量。真实声卡的这些键挂在 `Audio/Device`
    上由 pulse 层合并，我们的流节点没有 Device，所以直接挂节点上——pulse 层对节点字典
    全量透传。缺了 bus/VID/PID 这组，Wine 的 winepulse 只能落到 `ROOT\MEDIA` 匿名回落，
    `is_dualsense_audio_device()` 那条判据（bus 精确 strcmp "usb"，VID/PID 按 16 进制读回）
    就不成立。
  - `Station::host_sysfs_path` 非空时写成 `device.sysfs.path`（Manager 建站时解析、随配置
    快照传下来，见 §7），并从同一段路径里的 `dummy_hcd.<N>` 推出 `alsa.card` 与
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
- 断线重建：两条流共用 `audio::port` 的恢复状态机——建流（main loop → context → core →
  stream）、断线判定与拆建顺序只有一处实现，`audio::Start` 把同一份 `on_lost` 交给两边。
  退避为指数 500 ms × 2^attempt、上限 5 s，一条流连续 5 次重建失败就经 `on_lost` 通知
  调用方；worker 收到后 `audio::Stop()`，再由自己每 30 s（`kAudioRetrySeconds`）重新
  `audio::Start()` 一次直到成功或这一轮退出——期间 HID 一直在线，音频缺席不会拖 worker
  重启。启动时音频就没起来也走同一条重试路径。重建前 sink 调
  `encoder::ResetInputBuffer()`、source 清空自己的 PCM 环：跨会话残留样本不会进新流。
- 上行（`audio_source` / `audio_decoder`）：`Start()` 给了 `MicOpenHandler` 才建。Source 是
  单声道 f32 @48 kHz 的 `Audio/Source`（`PW_DIRECTION_OUTPUT`），节点名与 `device.*` 那
  一组硬件身份都出自 `audio::identity`、与同名 sink 同一族（名字换 `alsa_input...` 前缀与
  `HiFi__Mic__source` 末段，图标换成麦克风），`source::Start` 因此和 `sink::Start` 收同一对
  `station_id`/`host_sysfs_path`；
  `STREAMING ↔ PAUSED` 经 `on_mic_open(true/false)` 变成手柄端的开关（这个迁移要靠空闲
  挂起，所以 source 上也写 `node.pause-on-idle=true`）；解码器是单声道
  Opus，71 字节包解出 480 帧（10 ms），经 `source::PushSamples` 写进 2048 帧
  （≈43 ms）的 PCM 环，RT 回调欠载补静音。包队列深度 8（≈80 ms），满了丢最旧。
- `OnMicPacket` 由 `bridge` 在 bt 读线程上调用：只做一次 71 字节拷贝入队。Source 与 sink
  走同一套 `audio::port` 恢复策略（断线自愈，彻底失败经同一个 `on_lost` 交回 worker），
  解码不重采样——时钟漂移由 PCM 环的"满覆盖、空补静音"兜底。
- `pw_init/pw_deinit` 是进程级的，只由 `audio.cpp` 成对调用：sink 与 source 共享同一套
  PipeWire 全局状态，子模块各自 init/deinit 会在拆一条流时把另一条拆死。

---

## 10. plugin —— 进程内插件

插件是这个仓库里唯一"内容由别人写"的一层。插件是**进程级单例**：没有类、没有 self、
没有宿主替它保存的实例状态——自己的状态放在自己的静态存储里。宿主侧四个文件加上
仓库根的两份随包插件，按角色分：

| 文件 | 角色 |
|---|---|
| `plugin_api.h` | C ABI 边界：`ds5PluginDesc`（版本号、名字、版本 + `init`/`stop`/`on_tick`/`on_state_in` 四个槽）与 `ds5Host`（`abi_version` + `log` + `power_off`）。插件作者只 include 这一份，两侧之间不跨 C++ 类型 |
| `plugin_util.h` | 插件作者的可选工具，纯头文件 inline、不进 ABI：`FindSetting`（在 `ds5Setting` 数组里按键取值）、`ParseInt`（严格非负十进制 + 上下界）、`NowMs`（`steady_clock` 毫秒）。随包与第三方插件共用 |
| `plugin_loader.{h,cpp}` | 纯机制：`ListPlugins` 列目录里的 `.so`、`HasMarker` 只读 ELF 节表验标记、`dlopen(RTLD_NOW\|RTLD_LOCAL)` + `dlsym(kPluginEntryName)`、`ds5Setting`（键、值）数组的拼装。不持状态 |
| `plugin.cpp` | 门面：实例表、输入钩子表、目录扫描顺序、同名去重、`OnTick` 分派 |
| `plugin/idle/`（仓库根） | 随包插件：单独编译成 `idle.so`，静止一段时间后请手柄自己关机；与第三方插件走同一条目录扫描路径 |
| `plugin/buttons/`（仓库根） | 随包插件：六个 TU 编成一份 `buttons.so`，改写主机看到的按键形态（映射、扳机行程、轴互换/反向/居中、连点、组合键、宏，以及触摸板的半区点击与划动）。它用 `on_state_in` **改的就是交出去的那 63 字节**，用 `on_tick` 执行关机动作；程序侧说明在 `plugin/buttons/AGENTS.md`，用法在 `plugin/buttons/README.md` |

**配置页面的面板是插件的旁挂文件**：`<插件名>.panel.js` 与 `<插件名>.so` 同目录、同名，
`tools/buttons_web.py` 启动时按插件清单把它们发给页面；识别口径与宿主 `ListPlugins`
相同（只看普通 `.so`、用户目录覆盖系统目录），没有面板的插件退回通用键值编辑器。面板
不参与装载、不链接宿主，`.so` 不在时面板也不会出现；它只是一段静态 JS，服务端只发登记
过的那几个文件。随包两份插件各带一个：`buttons` 的按键映射、`idle` 的自动关机。

**页面壳是唯一的读文件者，面板只收一份推来的状态**：顶部第二栏的下拉框选 `global` 或
某只 `controllers.<MAC>`；选中后壳按这一层把 `api/state` 与每个插件的
`api/plugin?name=…&layer=…` 都取回来，装进 `DS5.sections`，再以 `ctx` 逐个交给面板的
`refresh`——插件面板拿到的 `ctx` 只有两样：这一层的插件节（含服务端下发的词表
`catalog`）与一个 `validate(items)` 回调；推过去的那一节已经去掉 `layer`/`layers`/
`path`，面板拿不到层标识。它不认 `global`/`controllers.<MAC>`、不读
配置文件路径、不发任何请求：草稿里的插件名与层由壳在收草稿时补进
`{kind:'plugin'}`，节内单个标量则以 `{kind:'section_set'|'section_remove'}` 交出、
路径前缀由壳按 `section.path` 拼上。core 面板拿到的 `ctx` 是这一层的
`base`/`layer`/`entry`（手柄层里留空的项由壳算好 `inherited` 一起给出），同样不读文件。
面板只交回修改项，写盘与哈希复查全在壳的 `saveAll()` 里。只读的「当前状态」（虚拟
UDC、插件目录、系统里的手柄、系统配置 YAML 与重启提示）画在页签之上的壳里，服务状态
与文件路径留在设备条，都不随页签与层变化，因此不属于任何面板。

层清单由服务端给出：文件里已有的手柄 + 已连接但还没写进文件的手柄；「添加手柄」还可以
手写任意 MAC，页面临时记在 `extraLayers` 里，保存时先为它写一条 `controllers.<MAC>:`。
用户配置文件不存在也一样打开页面：`read_document()` 读成空串、`state.exists` 为 false，
页面照常列出 `global` 与已连接的手柄，首次保存时由服务端建出父目录（`0o700`）与文件
（`0o600`）。页面固定在 `http://127.0.0.1:8777/`（`--port` 可换），不发放令牌；请求仍
要过 `Host`/`Origin` 检查与整份文档的 `buttons_check` 判定。

**三关识别**：文件里有 `.ds5_plugin` 标记节、内容逐字节是字符串 `"DS5 Plugin"`
（`kPluginMarkerMagic`，含结尾 NUL；装载前只读 ELF 节表，不执行任何代码）→ 导出名
`ds5_plugin_desc`（dlsym）→ `abi_version == kPluginAbiVersion`（当前为 1）。标记是给
"目录里混进了别的 `.so`"用的，不是信任
边界：它能被伪造。没有 struct size、没有布局指纹：函数签名不匹配
是**静默**的（ELF `.dynsym` 不带参数类型），所以任何签名/布局改动都必须改版本号并从新
导出名开始；旧 `.so` 的版本号对不上，装载点当场拒掉。

**装载由目录决定，不由配置决定**：`plugin::Directories()` 给出系统目录
`/usr/lib/ds5dummy/plugins` 与 `$HOME/.config/ds5dummy/plugins`（`HOME` 缺失时只有前者），
`Start()` 按这个顺序扫描、按文件名排序，每个 `.so` 依次过三关。同名插件以后出现的目录
为准（用户覆盖系统），同一目录内的同名只留第一个并记 WARNING。最终按插件名排序装载，
所以顺序只取决于目录内容。配置里没有 `path` 这样的保留键——
`global.plugins.插件名.…` 与 `controllers.<MAC>.plugins.插件名.…` 只决定这个插件
拿到的取值，不决定它存不存在。

**能力靠空槽表达**：`on_tick`/`on_state_in` 为 `nullptr` 就是"这个插件不做这件事"，宿主
跳过空槽；`init`/`stop` 必填，`init` 返回 0 表示"这一轮不生效"（配置关掉了它），宿主记
INFO 而不是 ERROR。配置是 `plugin::Settings`（`map<string,string>`，键为
`插件名.点分键路径`，值是标量文本，按扁平键有序），daemon 只校验形状并逐层合并，取值合不合法
由插件自己判；没有保留键。YAML 里插件名之下是**任意层**的嵌套映射，读取时由
`FlattenPluginSettings` 摊平成点分路径（`buttons.remap.dpad.up`）：每一段名字都要过
`plugin::IsToken`，所以"写成嵌套"与"写成一个带点的键"不是同一种拼法——后者被拒；
段数上限是 `daemon::kMaxPluginDepth`（8，含叶子那一段），收的是递归别名而不是形状。
拍平后没有"切片"对象：某个插件的配置就是
键带 `插件名.` 前缀的那些条目，转成 `init` 要的 `ds5Setting`（键、值）数组时按前缀长度整段剥掉
插件名、其余原样交给插件（层数由插件自己解释），遍历顺序就是扁平键序，不需要再排序。默认的插件配置是空的：随包 `idle.so` 与 `buttons.so` 就在系统插件目录里，默认安装
下照常装载；配置里不写它的任何键，它拿到的数组只有结尾的 `{nullptr, nullptr}`，全部走
自己的默认值。反过来，配置里写了某个插件的键、目录里却没有同名 `.so` 时，那些键只是
没人读，不影响别的插件。

**输入钩子反向派发**：`plugin` 不认识 `bridge`，`bridge` 也不认识 `plugin`。`Start()`
返回本轮装进来的 `on_state_in` 函数指针区间，worker 在 `bt::StartThreads()` 之前把它们
逐个 `bridge::AddStateHook()`；bridge 只持有一张 `void (*)(uint8_t*)` 表，在发布 USB
输入报告之前按注册顺序调过去。拆机时 worker 在两条传输线程 join 之后先
`bridge::ClearStateHooks()`，再 `plugin::Stop()`。

**线程相位**（两张表无锁靠的就是它，与 `bridge::SetMicSink` 同一条约定）：
`plugin::Start()` 早于 `bt::StartThreads()`，`plugin::Stop()` 晚于两条传输线程 join；
运行期 `on_state_in` 在 bt 读线程（~100–120 Hz）、`on_tick` 在 worker 主循环（10 Hz）。
`ds5Host::power_off` 只允许从 `on_tick` 调用：它同步碰控制通道，搬到传输线程上就是把锁
搬过去。`plugin::Start()` 自己不再收上一批，除首次外调用方必须先 `Stop()`。

宿主能力表由 worker 填好（两个 C 壳子落在 `DS5DUMMY_LOG` 与 `bridge::PowerOff()`），
`plugin::Start()` 把整表拷一份递给每个 `init`——插件跨不过去的是符号，不是拷贝。

随包的 `idle` 插件是这份契约的第一个用户：`off_after_minutes`（缺省 30，`0` = 不自动
关机，范围 0..60）。它自己取 `std::chrono::steady_clock`（`on_tick` 没有时间参数，精度
因此不取决于主循环节拍）：`on_state_in` 只在真的动了手柄时记下"上次操作"的时刻，
`on_tick` 比较 `now - last`，到期时先推起点再调 `host->power_off()`（即
`bridge::PowerOff()`，0x08 关机 feature 报告），失败也只等下一个完整周期。静止判据是
四个摇杆在 `[120, 140]`、两个扳机为 0、十字键 hat null、任何按键都没按；触摸板接触与
六轴不参与判定。`steady_clock` 不含挂起时间，且休眠钩子会停掉服务，所以挂起不会被算成
静置。

随包的 `buttons` 是这份契约的第二个用户，也是第一个改那 63 字节的：配置是 `buttons` 之下
**三层**（`buttons` → 功能组 → 源或编号），靠上面那条任意层摊平送到插件手里，它拿到的键形如
`remap.circle`、`axis.swap_lstick_rstick`、`shortcut.1`。六个 TU 一份 `.so`，按职责分：布局
（`ids`）、规则形状（`rules`）、语法（`settings`）、跨帧判定（`sense`）、单帧执行（`pipeline`）、
装配与线程相位（`buttons.cpp`）；判据读的是**手柄这一帧报进钩子的那份状态体**，插件不拿自己的
改写结果喂自己。`on_state_in` 每帧一次解码、一趟写回，`on_tick` 只把识别出的关机动作搬给
`host->power_off()`（同一个原因：那条调用同步碰控制通道，最长 2 s，不能落在读线程上）。
因为装载顺序按插件名字典序，`buttons` 排在 `idle` 之前，**`idle` 读到的是 `buttons` 改写之后的
字节**：按住一个被映射抹掉的键，`idle` 会认为静置。单缓冲契约的这条后果是已知的，写在
`plugin/buttons/AGENTS.md`。

`plugin` 只依赖 `core` 的常量；两份随包 `.so` 的源码在仓库根的 `plugin/idle/` 与
`plugin/buttons/`，都只
include `plugin_api.h` / `plugin_util.h`，不链接宿主任何符号（`core` 的字节尺寸由
`plugin_api.h` 里的 `kPluginStateSize` 提供）——所以它可以脱离本项目编译；`bridge` 不依赖 `plugin`；`worker`
是装配者：它注册钩子、调 `OnTick`、在拆机时清钩子。没有双向 include。

---

## 11. 线格式常量（`src/core/protocol/report.h`）

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
| `kFeatureReportIdBluetooth` | `0x08` | 方向相反的 feature：由本项目主动 SET，让手柄自己关机，不在预取清单里 |
| `kHidpDataIn/Out`、`kHidpGetReport/SetReport/FeatureReply` | `0xA1/0xA2/0x43/0x53/0xA3` | 协议镜像值；`0x53` 由 `bt_l2cap.cpp` 的 `SetFeature` 使用，其余的实际使用方在 bt 后端自持同名常量 |

0x08 关机报告的布局：`[0]=0x08 [1]=0x02（关）[2..43]=0 [44..47]=feature CRC32 LE`。
48 字节 = 描述符里 Report ID 8 的 47 字节 Report Count 加上 report id；CRC 覆盖前 44
字节（起点是 report id），因为整包没有逐次变化的字段，它冻在
`bridge/power_report.h` 里，运行期不重算。

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

## 12. 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBT_BACKEND=HIDRAW \
      -DDS5DUMMY_BUILD_TESTS=ON -DDS5DUMMY_BUILD_TOOLS=ON
cmake --build build
ctest --test-dir build
```

发布前跑 `./scripts/verify.sh`：双后端 `-Werror` 构建 + ctest，再用 HIDRAW 跑一遍
ASan+UBSan；构建树在 `build/verify-{hidraw,l2cap,asan}`，无需 root、手柄或 PipeWire 设备。

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
枚举 logind 会话）、Boost 头文件（只用 `circular_buffer`）、`fmt`、pthreads。装载 `.so`
插件用的 `dlopen`/`dlsym` 来自 `${CMAKE_DL_LIBS}`（glibc 上是 libdl，现代版本已并进 libc，
不算新依赖）。**缺库直接
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

## 13. 测试

`tests/CMakeLists.txt` 把被测源文件直接编进一个 `ds5dummy_tests` 二进制，
`gtest_discover_tests` 让 ctest 看到逐个用例（当前 461 个，两个后端各跑一遍）。
**硬性约束：全部测试必须能在无 root、无手柄的环境下跑** —— 需要特权或硬件的验证是人工
清单，不进 CI/单测。因此 bt 的测试只覆盖门面到后端的纯转发契约、`tx` 的入队/seq/CRC/
背压、feature 缓存策略，硬件往返一律不测；`daemon/discovery` 与 `usb` 的测试自己搭临时
sysfs 树（假 `uevent` 文件、假 `dummy_udc.N/udc/dummy_udc.N` 与同索引兄弟目录
`dummy_hcd.N/usbM/<M>-1`，即真机上的那对兄弟形状；设备目录下面的接口目录
`<M>-1:1.0` 也照样摆出来），不碰真 `/sys`。

交接侧同样自给自足：`tests/options_test.cpp` 让配置快照编码后再解码，逐字段对账，
并证明坏 magic、截断、头里的总长与实际不符、字符串越界、尾随字节都被拒；
`tests/handoff_test.cpp` 用自己建的 memfd 与 pipe 证明快照读写往返、seal 之后写不进
（`EPERM`）、非普通文件被拒，以及四条通道的挪位计划（共用号、占 stdio 都被拒）。

插件装载链的最后一环由 `tests/fixtures/plugin_probe.cpp` 补上：CMake 把它单独编成
MODULE（`ds5dummy_plugin_probe`，不链接测试二进制），测试把它拷进 `mkdtemp` 出来的
临时插件目录，再让 `plugin::Start()` 扫那个目录——走的是与真实安装完全相同的
`ListPlugins`/`HasMarker`/`dlopen` 路径，`dlsym` 找的也是真符号表。同一份源码再编两份
变体：`..._bad_abi`（版本号改成宿主不认的值）与 `..._no_marker`（不落标记节），分别
证明装载点拒绝版本不符、以及不 dlopen 没有标记的 `.so`。两份随包 `.so` 也以同样方式
编给测试用（`DS5DUMMY_TEST_IDLE_SO`、`DS5DUMMY_TEST_BUTTONS_SO`），证明它们的标记、导出名
与描述符能被真实装载，也证明用户目录同名覆盖、`off_after_minutes: 0` 的退出，以及
`buttons` 那几条规则一路走到宿主最终发出去的那些字节（含"动作落在心跳而不是钩子"）。
它们同样不需要 root 或手柄，也不读真实系统/用户插件目录。
`tests/bridge_hook_test.cpp` 覆盖反向派发的 bridge 一侧：注册的钩子改写的是 usb 最终
持有的那份报告、按注册顺序跑、清空后不再被调，麦克风帧不经过钩子。

`buttons` 插件的四个模块层 TU（`ids`/`settings`/`sense`/`pipeline`）像 `idle.cpp` 一样直接编进
测试二进制，按模块各一份测试；装配层 `buttons.cpp` 不在其中，它只经上面那条真实 `.so` 路径被
测：
`tests/buttons_ids_test.cpp`（token ↔ 位、hat 无损往返、`Invert` 的不动点，**与
`core::protocol::DecodeInputState` 逐字段相等**——布局在插件里是第二处写法，这条断言
防的是"按 ○ 出 ✕"这种静默错误；再加两个触摸点打包字的对账，以及半区归属锁存、划动的
最小位移与 16:9 折算）、`tests/buttons_settings_test.cpp`（五组语法的正反例，
每条反例只让那一条缺席并留下一行 WARNING；触摸板那族在源/目标/触发项三种角色上的取舍）、
`tests/buttons_sense_test.cpp`（时序状态机，时钟逐帧注入；含一次划动一轮只出一个）、
`tests/buttons_pipeline_test.cpp`（映射与轴的表算法，逐位断言；含"绑左半点击抹掉 `pad`
位而不碰右半那一次"）。判定
读的是帧快照而不是改写结果，所以这些测试喂的就是手柄那一帧报进钩子的字节，不需要任何
只在测试构建里存在的拨表入口；触摸板那族喂的是测试自己摆的那两个 32 位打包字，同样
不需要硬件。

配置工具那一层的单测在 `tools/test_buttons_web.py`（缺 PyYAML 时 CTest 不注册这一项）：
按行取/写/删/建四条路径与"原文相同则整份文件逐字节不变"（后者是承重断言）、模型解析与
序列化往返、面板脚本（`plugin/buttons/buttons.panel.js`）里的词表与
`plugin/buttons/ids.cpp` 的 token 表逐项对账、基础配置的标量改写/删键级联/
`controllers.<MAC>` 新建、`discover_panels` 的目录口径、层清单（已连接但未配置的手柄、
手写 MAC 的 `plugin_state`、保存时补条目），以及回环服务的固定地址路由、Host/Origin
检查、409 陈旧哈希与"保存后只有这一节变化"。保存路径要 `buttons_check` 做
权威判定，用例自己建临时目录，不需要 root、手柄或真实的用户配置目录。

`tools/audio_dump` 是离线回听路径：audio 下行链 + dump sink，不链 usb/bt/bridge，手柄连不
连都能跑，用于单独验证编码半段。

---

## 14. 打包与内核侧

两个包都装 `/usr/bin/ds5dummyd`、`/usr/lib/ds5dummy/plugins/idle.so`、
`/usr/lib/ds5dummy/plugins/buttons.so`、`/usr/lib/ds5dummy/plugins/idle.panel.js`、
`/usr/lib/ds5dummy/plugins/buttons.panel.js`（配置页面的面板，与 `.so` 同目录；
缺了它页面退回通用键值编辑器，不影响装载）、
`/usr/bin/ds5dummy-config`（配置页面入口，把参数交给
`/usr/lib/ds5dummy/buttons_web.py`）、`/usr/lib/ds5dummy/buttons_web.html`、
`/usr/lib/ds5dummy/panels/core.js`（页面壳与 core 面板）、`/usr/bin/buttons_check`
（页面保存前的权威判定，也是可单独运行的只读校验器）、
`/usr/share/doc/ds5dummy/buttons.md`
（`buttons` 插件自己的用法文档，装自 `plugin/buttons/README.md`）、
`/usr/lib/ds5dummy/teardown.sh`、
`/usr/lib/systemd/system/ds5dummy.service`、`/usr/lib/systemd/system-sleep/ds5dummy`
（休眠钩子）、`/etc/ds5dummy/config.yaml`（进 `backup=`）、
`/usr/share/doc/ds5dummy/user-config.example.yaml`、`/etc/modprobe.d/ds5dummy.conf`、
`/etc/modules-load.d/ds5dummy.conf`，都 `provides=('ds5dummy')`、
`replaces=('ds5dummy')` 且互相 `conflicts`（`pkgrel` 随内容递增；makepkg 的 `pkgver()`
回写会把它重置为 1），`depends` 含 `yaml-cpp`、`systemd-libs`、`python` 与
`python-yaml`（配置页面），构建参数固定在
`-DCMAKE_BUILD_TYPE=Release -DBT_BACKEND=<对应后端> -DDS5DUMMY_STRICT=ON
-DDS5DUMMY_DEBUG=OFF -DDS5DUMMY_DEBUG_LOG=OFF -DDS5DUMMY_BUILD_TOOLS=ON
-DCMAKE_INSTALL_PREFIX=/usr`：打开 tools 只为编出 `buttons_check`，其余诊断程序
只留在构建树里、不安装。

版本号不手写：三个 PKGBUILD 都用 `pkgver()`，在其中用 `git -C "$_srcdir" describe
--tags --long` 现查，生成 `<tag>.r<距 tag 的提交数>.g<短哈希>`（无 tag 时 tag 段为 `0`、
提交数取 `rev-list --count HEAD`）。makepkg 会把结果写回 PKGBUILD 并把 `pkgrel` 重置
为 1，所以文件里留 `pkgver=0` 占位；重新 `git checkout -- <PKGBUILD>` 即可恢复。
生成值不带 `.dirty`：`git describe --dirty` 会把 makepkg 自己的写回当成改动，下一次
构建版本号就会永远多出 `.dirty`。
DKMS 包的 `package()` 用这个版本号覆写 `dkms.conf` 的 `PACKAGE_VERSION`，
安装钩子从 pacman 传入的 `$1`（新版本）与 `$2`（被替换的旧版本）取版本，不硬编码。

systemd 单元以 root 跑（Manager 必须 root），`ExecStart=/usr/bin/ds5dummyd` 不带任何
参数与环境变量：降权目标由 Manager 从活跃图形会话推导，用户级生效值来自两份 YAML。
`Restart=on-failure`、`RestartSec=2`，stdout 与 stderr 都进 journal——启动横幅（目标
用户、两份配置路径、发现轮询间隔、桥接模式、每层手柄设置、最终日志级别）只出现在那里。

休眠不走单元指令：`pkg/ds5dummy.sleep-hook` 装到 `systemd-sleep` 目录。挂起前若单元
active 就先在 `/run/ds5dummy/was-active` 留标记再 `systemctl stop`（Manager 拆 gadget、
worker 退出）；恢复后见标记就 `systemctl start`。留在 `/run` 的标记在重启后自然消失，
所以睡眠中崩溃不会让下次开机凭空拉起服务。绑定中的 FunctionFS gadget 和蓝牙链路都
过不了挂起周期，停掉重建比原地恢复可靠。

模块参数对两个后端都一样是 `options dummy_hcd is_high_speed=0 num=3`（虚拟 UDC 只
跑 full-speed，因为本项目提供的是 FS 描述符；`num` 是同时能服务的手柄数上限，缺省
3，要接更多手柄就改这一行，它是加载参数、改完要停服务后重载 `dummy_hcd`）。差别在
HID 侧：
- `pkg/ds5dummy_hidraw` 的 modules-load 里装的是 `hid_playstation_ds5dummy`（DKMS 补丁，
  把真蓝牙 DualSense/Edge 留在 hidraw-only 并合成 `1998:1221`/`1998:1103`，其余机型与 USB 设备不受影响），`hidp` **必须**可用、不能 blacklist；
- `pkg/ds5dummy_l2cap` 装的是 in-tree `hid_playstation`，让 BlueZ 先建 HID session 再交接
  PSM。

---

## 15. 代码规范

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

## 16. 当前代码里已知的薄弱点

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
6. 0x36 播放的实机表现随主机不同：台式机上正常；笔记本上音质明显变差，链路不断。
   另有"一到十几秒断链"（读侧报链路已死）的观测，怀疑与蓝牙射频/信号有关，未做对照
   实验定位；编码半段单独验证是好的，不能排除编码侧。
7. `identity.mode: list` 只认 active 会话：名单内用户的图形会话不活跃（切走 VT）而另一个
   名单外用户 active 时，`SelectWorkerIdentity` 会因为没有匹配的候选返回 `kDisabled`，
   服务停在 inactive。这是"只认 active 会话"的设计结果，不是名单写错。
8. 麦克风开关报告与音频共用 bt 的 8 槽发送队列，队列满时按"丢最新"丢掉；手柄状态要等
   下一次开/关录音才纠正。这条只记 ERROR，没有重发。
9. 上行链路没有实机验证过：开关包与 71 字节帧长照上游实现，但"主机打开录音 → 手柄
   开始上行"的端到端时序尚未在手柄上跑过；source 的断线自愈也只验证到编译与状态机
   层面，没有在 PipeWire 重启下实跑过。
10. 多手柄路径只有单测级证据：`daemon/discovery` 的解析有测试，但"发现 → 多站并发 →
    各自重试 → 整族拆除"没有在多只手柄（或多适配器）上跑过；`num` 不够时看到的是
    单站 Provision 失败加 2 s 退避，而不是启动期报错。
11. 发现依据是 hidraw 节点。L2CAP 后端拿到 PSM 后内核 HIDP 会话不复存在，节点随之消失；
    此时 worker 若死掉，Manager 已从 `slots_` 里删掉该 MAC，要等 BlueZ 重新建出节点才会
    再服务它。hidraw 后端没有这一步。这条只按代码路径推断，未在实机上验证。
12. 重发档的发送时刻表只在 IN 线程这一侧排，`usb::StartThreads()` 收下这个间隔之后
    不再有第二次校准：主机侧的枚举速率、`dummy_hcd` 的排队请求都跟它无关，所以
    这一档报出的帧数是"我们发的"，不是"主机取的"。真实 USB DualSense 有线实测
    ~253 帧/s，与这里任何一档都不严格对应；要对齐真机取
    `usb_report_interval_ms: 4`，而"到点时端点正忙导致某一格没发出去"的情形没有
    实测过。
13. 0x08 关机报告与 `bt::SetFeature` 只有单测级证据：字节布局（48 字节、payload[0]=0x02、
    尾部 feature CRC `0x23A2EFE0`）照协议资料编排，两个后端各写了一遍发送路径，但
    "手柄收到后自己断电、链路随之断开、Manager 按既有路径拆站"这一段没有实机跑过。
    hidraw 后端把整份报告交给内核 `HIDIOCSFEATURE`，手柄是否接受还取决于内核 HID
    驱动对 feature 报告的实现。
14. 插件的可信加载没有实现：装载点只验 `.ds5_plugin` 标记、导出名与 `abi_version`——
    标记是公开的常量、可被任何 `.so` 伪造，所以它只挡"顺手放进来的无关 `.so`"，不是
    信任边界。属主、权限、签名都没查，也不做任何沙箱化；而装载是扫目录自动进行的，
    意味着 `~/.config/ds5dummy/plugins/` 里任何带标记的 `.so`（谁写的都算）都会进
    worker 进程（它有降权后的全部权限，还能起线程）。查完标记到 `dlopen` 之间不重查
    文件（TOCTOU）也没做。这是刻意推迟的一块，不是遗漏。
15. idle 插件只有单测级证据：静止判据、到期时刻、失败后重等一个周期都在
    `tests/plugin_idle_test.cpp` 里用 `detail::PretendIdleFor()`（只在定义
    `DS5DUMMY_TESTING` 的测试构建里存在）把"上次操作"拨到过去
    跑过，但"30 分钟没碰手柄 → 手柄真的断电、蓝牙随之断开"这一段依赖上面第 13 条，
    同样没实机跑过。计时走的是插件自己的 `std::chrono::steady_clock`（Linux 上不含
    挂起时间），所以挂起期间不计入静置时长——目前的 systemd-sleep 钩子在挂起前就停了
    服务，这条不构成问题，但钩子一旦去掉，"整晚没碰手柄"就不会触发自动关机。
16. buttons 插件只有单测级证据，而它是**第一个改写那 63 字节的**用户。`[55..62]` 是手柄
    自己算的 AES-CMAC，密钥在手柄里、我们算不出，改写 `[0..5]`/`[7..9]` 之后那份报告的
    签名与内容不再匹配。本项目从不校验它，但"主机与游戏也不校验它"目前**没有证据**——
    既有链路里输入状态一直原样转发。插件只写它生产的字节（`[6]`、`[9]` bit3、`[10..62]`
    其余一律不写，`[32..39]` 那两个触摸点字只读）只是把风险面收窄，不构成豁免；这条要
    实机确认。
17. buttons 的规则之间**没有仲裁**：每条各自判定、各自松手闸门。于是 `r3` 与
    `r3+dpad_up` 并存时短的那条先命中、长的跟着也命中；同一个键上 `~` 与 `*2` 各绑一条会
    出两次动作。需求里的原 K-06（遮蔽检出）被移出本期，症状可见但没被判据化。
18. buttons 有两处需求写了、语法没做：摇杆方向的触发阈值与迟滞不能逐条覆盖（缺省 48/10
    冻在 `ids.h`）、宏脉冲时长恒为 80 ms。两条的后果都是"用户写了也不生效"，走的是同一行
    忽略日志。触摸板那族（I-03/I-04）已经做了，但判据全是 `ids.h` 里的全局经验值（半区那条
    8% 的带、划动的最小位移 12% 与 16:9 折算），**没在实机上确认过手感**；六轴手势（I-05）
    仍然不做。
