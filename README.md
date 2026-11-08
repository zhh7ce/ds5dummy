# DS5Dummy

把**真实的蓝牙 DualSense**变成 Linux 主机看到的**虚拟 USB 手柄**——每只手柄一台，
插上（开机）就自动出现，断开就自动消失。

程序为发现的每只手柄在系统里造一台虚拟 USB 设备（设备身份就是 `Sony Interactive
Entertainment DualSense Wireless Controller`，`054c:0ce6`），同时把它经蓝牙送来的
按键、摇杆、陀螺仪实时送进对应的虚拟设备；主机侧的游戏以为插了一只 USB 手柄。反方向
也通：游戏发给手柄的震动、指示灯指令会经蓝牙回到真手柄。

除了 HID，还能把主机侧的**游戏音频 + 触觉反馈**下传给手柄（耳机口出声、喇叭与马达
振动），并把**手柄麦克风**接成主机的一个虚拟录音设备。音频这一步是可关的、失败也不
影响按键桥接。

典型用途：只想认 USB 手柄的游戏、Wine/Proton、或者需要手柄以 USB 身份出现的场景。

> 内部结构、模块边界、线程模型、构建矩阵在 [AGENTS.md](AGENTS.md)。

---

## 环境要求

| 项目 | 要求 |
|---|---|
| 内核 | 需要 `dummy_hcd`（`CONFIG_USB_DUMMY_HCD=m`）、`libcomposite`、`usb_f_fs`、`configfs` |
| 发行版 | 打包脚本按 Arch Linux x86_64 写；其它发行版需自己准备等价的内核模块与 systemd 单元 |
| 蓝牙 | BlueZ 已运行，DualSense 已**配对、信任、绑定**，并且当前处于连接状态 |
| 音频 | PipeWire（不需要额外配置；不用手柄出声可以关掉） |
| 权限 | 启动需要 root：虚拟 USB 设备的创建是特权操作 |
| 图形登录 | 必须在某个用户**图形登录之后**运行：降权身份来自 systemd-logind 枚举到的活跃图形会话；未登录时服务会不断重试 |

---

## 安装

两条路：用仓库里现成的 PKGBUILD 打包装，或者直接 cmake 构建。蓝牙侧有两种后端，
**先选一个**。

### 1. 打包安装（推荐）

| 包 | 蓝牙后端 | 说明 |
|---|---|---|
| `pkg/ds5dummy_hidraw` | hidraw | 默认后端，借内核的 HID 驱动收发；**要额外装一个内核驱动补丁**（见下） |
| `pkg/ds5dummy_l2cap` | 原始 L2CAP | 程序自己在用户态走蓝牙 HID 协议，报告的收发不经过内核 HID 驱动；构建时需要 bluez 的开发文件 |

```bash
cd pkg/ds5dummy_hidraw
makepkg -d                                  # PKGBUILD 里给的命令：-d 跳过依赖检查
sudo pacman -U ds5dummy_hidraw-*.pkg.tar.zst
```

**hidraw 后端还需要装内核驱动补丁**，否则内核自带的 `hid-playstation` 会抢走这只
蓝牙手柄，程序看不到它：

```bash
cd hid-playstation-ds5dummy
makepkg -d
sudo pacman -U hid-playstation-ds5dummy-dkms-*.pkg.tar.zst
```

三个 PKGBUILD 都不手写版本号：`pkgver()` 里用 `git describe` 按当前 checkout 生成
`<tag>.r<距 tag 的提交数>.g<短哈希>`（没有 tag 时 tag 段为 `0`、提交数取总数）。因此每次
提交后重新 `makepkg` 都会得到更大的版本号，直接 `pacman -U` 就是升级。注意 makepkg 会把
算出的版本写回 PKGBUILD 并把 `pkgrel` 重置为 1，这是 `pkgver()` 本身的行为；想回到占位值
用 `git checkout -- <PKGBUILD>`。

装完包，系统里就有了 `/usr/bin/ds5dummyd`、systemd 服务、内核模块配置与系统配置文件
`/etc/ds5dummy/config.yaml`（root 属主）；用户配置示例装在
`/usr/share/doc/ds5dummy/user-config.example.yaml`。重启或执行
`sudo systemctl restart systemd-modules-load bluetooth` 让模块配置生效。

模块配置里 `dummy_hcd` 的 `num` 默认是 3，即同时最多桥接 3 只手柄。要接更多就改
`/etc/modprobe.d/ds5dummy.conf` 里的 `num=`，然后停服务、重载模块再启动
（`sudo systemctl stop ds5dummy`、`sudo modprobe -r dummy_hcd && sudo modprobe dummy_hcd`），
或者直接重启。`num` 是模块加载参数，运行中改不了。

### 2. 源码构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBT_BACKEND=HIDRAW
cmake --build build
sudo ./scripts/setup.sh            # 加载内核模块、确保 configfs 可用
```

然后按下一节写好用户配置，再 `sudo ./build/ds5dummyd`。

随包的 `idle` 插件不在主程序里：构建产物是 `build/idle.so`，要放到
`/usr/lib/ds5dummy/plugins/` 或登录用户的 `~/.config/ds5dummy/plugins/` 才会被装载
（见配置一节的“插件”）。

把 `-DBT_BACKEND=HIDRAW` 换成 `-DBT_BACKEND=L2CAP` 就构建另一个后端；两者不能同时装。

---

## 配置

设置分两份 YAML：root 写的系统配置 `/etc/ds5dummy/config.yaml`，和登录用户自己家目录
里的 `~/.config/ds5dummy/config.yaml`。程序没有用户级命令行开关，所有生效值都来自
这两份配置；改完配置要 `sudo systemctl restart ds5dummy`（配置只在启动时读一次）。

### 系统配置（`/etc/ds5dummy/config.yaml`，root 属主）

```yaml
identity:
  mode: auto            # auto 只服务活跃图形会话里的唯一用户
  # users:              # 仅 mode: list 时必填；gid 省略时等于 uid
  #   - uid: 1000
  #     gid: 1000

ffs_path: /dev/ffs-ds5
discovery_interval_s: 3
log_level: WARNING
```

| 键 | 默认 | 说明 |
|---|---|---|
| `identity.mode` | `auto` | `auto` 要求恰好一个活跃图形会话；`list` 只服务 `users` 名单内的用户 |
| `identity.users` | 空 | 仅 `mode: list` 可写；每项是裸 uid（如 `[1000, 1001]`）或 `uid`/`gid` 映射，`uid` 必填、`gid` 省略时等于 `uid`；两者都必须大于 0，`gid` 必须存在于组库 |
| `ffs_path` | `/dev/ffs-ds5` | FunctionFS 挂载点**前缀**：每只手柄挂到 `<ffs_path>.<12 位 MAC 十六进制>`，本身不被挂载；启动时还会过一遍路径白名单 |
| `discovery_interval_s` | `3` | Manager 扫描 `/sys/class/hidraw/` 找新手柄的间隔（秒，最小 1）。它同时是回收与重试已退出 worker 的间隔，调大就把手柄插入到开始服务、以及 worker 死后重新拉起的等待时间一起拉长 |
| `log_level` | `WARNING` | 系统日志级别；级别名（`OFF`/`FATAL`/`ERROR`/`WARNING`/`INFO`/`DEBUG`/`VERBOSE`/`TRACE`）或整数 |

文件缺失时使用内置默认值；未知键、类型不符或跨字段冲突（如 `mode: list` 却没写
`users`）都会报错并让服务以非零码退出。

### 用户配置（`~/.config/ds5dummy/config.yaml`，登录用户属主）

这份文件是可选的：没有它，Manager 用内置默认值启动（`enabled: true`、`bt_addr: auto`，
其余全默认），和 `/usr/share/doc/ds5dummy/user-config.example.yaml` 里没注释掉的取值
一致。默认自动桥接系统里发现到的每一只 DualSense，所以想改参数时才需要复制一份，
用 `global:` 和 `controllers.<MAC>:` 两层覆盖：

```yaml
enabled: true
bt_addr: auto          # auto | list | "AA:BB:CC:DD:EE:FF"

# global:              # 对所有手柄生效；整段注释掉 = 全部用内置默认
#   bt_wait_s: 60
#   audio: true
#   haptics_gain: 1.0
#   speaker_gain: 2
#   usb_report_interval_ms: 1   # 0 = real-time：只在蓝牙新样本到达时发一份
#   usb_model: auto             # auto | dualsense | edge

# plugins:            # 只放插件自己的键；装不装载由插件目录决定，不在这里写路径
#   idle:
#     off_after_minutes: 30   # 多久没有按键就让手柄自己关机；0 = 从不

# controllers:         # 按 MAC 覆盖单只手柄；键必须是合法 BD_ADDR
#   "AA:BB:CC:DD:EE:FF":
#     audio: false
```

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | `true` | `false` = 该用户不启用本服务 |
| `bt_addr` | `auto` | `auto` = 发现到的每一只手柄都桥接；`list` = 只桥接 `controllers` 里列出的地址；写一个完整地址 = 只桥接那一只 |
| `global` | 无 | 对所有手柄生效的一组覆盖值（见下表） |
| `controllers` | 无 | 以 MAC 为键的覆盖子树；`bt_addr: list` 时它同时是白名单，至少要有一项 |
| `log_level` | 继承系统值 | 覆盖系统级别；写法同上。顶层键，不参与分层 |
| `plugins` | 无 | 给插件的那一节，形状是 `插件名 → 键 → 取值`；写在这里等于对所有手柄的默认值，`global:` 与 `controllers.<MAC>:` 下面也可以写同名的子树，逐层只覆盖写了的那个键。这一节不决定装载哪些插件，只决定装上的插件拿到什么取值；程序本身不认识键名，取值原样交给插件 |

下面六个键既可以写在 `global:` 下，也可以写在 `controllers.<MAC>:` 下。生效顺序是
内置默认 ← `global` ← `controllers.<MAC>`，每层只覆盖写出来的键：

| 键 | 默认 | 说明 |
|---|---|---|
| `bt_wait_s` | `60` | 等待手柄连上蓝牙的最长时间；`0` 表示每轮只试一次，失败由 Manager 退避后再起一轮 |
| `audio` | `true` | `false` = 不建音频通道，只桥接按键 |
| `haptics_gain` | `1.0` | 触觉振幅倍率，`0.0`..`2.0`；`0` 静音，`1.0` 原样，`2.0` 两倍。只影响震动，不碰声音 |
| `speaker_gain` | `2` | 手柄侧扬声器前置增益，`0`..`7`。加大是让**手柄自己**放大，编码与音量数据不变 |
| `usb_report_interval_ms` | `1` | 虚拟手柄两份输入报告之间的间隔：`1`/`2`/`4` ms（1000/500/250 Hz）由程序按这个间隔自己排发送时刻，两次蓝牙样本之间会把持有的那一份原样重发；`0` = real-time，只在收到新的蓝牙样本时发一份。这是 USB 侧行为；手柄经蓝牙上报的节奏由固件固定，不受此影响 |
| `usb_model` | `auto` | 虚拟 USB 设备呈现的型号：`auto` 跟随真手柄的 HID 身份（Edge 呈现为 Edge，其余呈现为普通 DualSense）；`dualsense` / `edge` 强制按该型号呈现。型号同时决定 USB PID、产品串与 report descriptor，三者始终一起换。单 MAC 模式下手柄启动时不在线、发现不到型号时，用它固定呈现最省事 |

插件由 worker 自己装载，配置里不写路径：它扫描两个目录里的每一个 `.so`——随包安装的
`/usr/lib/ds5dummy/plugins` 和用户自己的 `~/.config/ds5dummy/plugins`。一个文件要同时
满足两条才算本程序的插件：`.ds5_plugin` 标记节里是字符串 `DS5 Plugin`（装载前只读 ELF
节表，不会先运行它），并且导出 `ds5_plugin_desc`、报告的 ABI 版本号与宿主一致（当前
为 1）。别的 `.so` 一律
跳过并在 worker 日志里说一句；打不开或初始化失败也只让那一个插件缺席，站点照常工作。
两个目录里有同名插件时用户目录那份生效，装载顺序按插件名排序。

插件自己的键写在 `plugins.<插件名>:` 下，程序只负责把那一块原样送到插件手里；
写错的取值只会让那一个插件回到它自己的默认值，不会让整个配置失效。目前的插件：

| 插件 | 键 | 默认 | 说明 |
|---|---|---|---|
| `idle` | `off_after_minutes` | `30` | 连续多少分钟没有任何按键（摇杆离开中心带、扳机、十字键、任一按键都算）之后，请手柄自己关机；`0` = 从不自动关机，范围 `0`..`60`。手柄关机后蓝牙断开，下次按下 PS 键重新连上时会被当作新的一站拉起。触摸板接触与手柄被挪动不算操作 |

`idle` 与第三方插件没有区别，都是独立 `.so`——随包那份装在
`/usr/lib/ds5dummy/plugins/idle.so`，源码在仓库根的 `plugin/idle/`，只 include
`src/plugin/plugin_api.h` 与 `plugin_util.h`、不链接宿主任何符号，所以能脱离本项目编译：

```bash
c++ -std=c++20 -fPIC -shared -I src plugin/idle/idle.cpp -o idle.so
```

自己写的插件放进 `~/.config/ds5dummy/plugins/` 就会被下一次启动装载。

插件是进程内的任意代码：它能改交给它的输入状态字节，也能自己起线程。标记与 ABI 版本号
只证明这个文件是按这份接口编出来的，**不是**签名；程序没有"只加载可信插件"的机制——
文件属主、权限、签名都没查，所以插件目录里只放你自己编译或自己审阅过的 `.so`。

Manager 每 `discovery_interval_s` 秒（默认 3 秒）扫描一次 `/sys/class/hidraw/`：只认蓝牙总线
（`HID_ID` 的 bus 为 `0005`）且 VID/PID 属于受支持组合的设备——真机 `054c:0ce6`
（DualSense）或 `054c:0df2`（DualSense Edge），以及 hidraw 后端的 DKMS 驱动合成的
`1998:1221` / `1998:1103`；MAC 取自 `HID_UNIQ`。发现当前没在服务的新地址，就为它建一站
独立的虚拟 USB 并起一个 worker；手柄断开由 worker 自己的链路看门狗发现，Manager 拆掉
那一站，等它重新出现再建。因此多手柄不需要先查地址，也不需要重启服务。要写
`controllers` 覆盖时需要地址：`bluetoothctl devices` 能列出每只手柄的 MAC。

文件里 `enabled: false` 表示该用户不启用本服务：程序以 0 退出，服务停在 inactive，
systemd 不会重启它。**文件不存在不是"不启用"**：Manager 按内置默认值照常启动，横幅把
那行配置路径标成 `(absent, built-in defaults in use)`。文件存在时，未知键、非法
`bt_addr`、不是 MAC 的 `controllers` 键、规范化后重复的条目都让服务以非零码退出；
`bt_addr: list` 没有 `controllers` 项，或 `bt_addr` 写死某只却给另一只配了覆盖，同样
按错误处理。

### 身份解析

Manager 以 root 启动后枚举**当前活跃的图形会话**（X11/Wayland、`class=user`、
`sd_session_is_active`），据此决定 worker 降权到哪个用户：

- `identity.mode: auto`：候选必须恰好一个。没有活跃图形会话 → 非零退出，systemd 每 2 秒
  重拉，直到用户登录；同时有两个及以上活跃图形会话也无法替用户决定 → 同样非零退出。
- `identity.mode: list`：只服务 `users` 名单内的用户。没有活跃图形会话 → 非零退出重试；
  有会话但名单内一个都不匹配 → 正常退出（退出码 0），服务停在 inactive；名单内有两个
  及以上用户同时有活跃图形会话 → 非零退出（同样无法替用户决定）。
- uid 或 gid 为 0 一律拒绝；名单里的 `gid` 必须存在于组库。配置的 gid 与 passwd 主组
  不一致只打 WARNING，不阻止服务。

### 安全约束

- 系统配置只从 `/etc` 读，用户配置只从目标用户的 `~/.config` 读。系统配置拒绝符号链接
  （检查文件本身；`/etc` 由 root 管理，不逐级加固），用户配置从家目录起逐级检查，
  任何一级是符号链接都拒绝。两份都拒绝超过 64 KiB 的文件。
- 用户配置的属主必须是该用户本人或 root，且不能被组或其他用户写，否则拒绝启动。
- 用户配置只接受上表列出的键；`ffs_path`、`uid`、`gid` 之类 root 级键写在
  里面会被当成未知键报错。

手柄必须先处于配对状态：

```bash
bluetoothctl
# scan on  →  按手柄 PS 键配对
# trust AA:BB:CC:DD:EE:FF
# pair AA:BB:CC:DD:EE:FF
```

---

## 运行

### systemd（常驻）

```bash
sudo systemctl enable --now ds5dummy
journalctl -u ds5dummy -f
```

启动横幅与后续日志都在 `journalctl -u ds5dummy` 里：横幅会打印目标用户（uid/gid）、
两份配置的路径、桥接哪几只手柄和最终生效的日志级别。程序内部就会自己重试（建虚拟设备
失败、没等到手柄都会退避后重来一轮），不需要人为干预；整个服务异常退出时 systemd 2 秒后
再拉起。正常退出（配置里 `enabled: false`、名单外用户）不会被重启，服务停在
inactive。停止用 `sudo systemctl stop ds5dummy`。

休眠/唤醒不需要手动处理：包里的 `systemd-sleep` 钩子会在挂起前停掉正在运行的服务、
恢复后再拉起来（原来的虚拟 USB 与蓝牙链路都过不了挂起周期）。只有在挂起前处于运行
状态的服务才会被重新拉起。

### 手动

```bash
sudo ./build/ds5dummyd
```

不需要用户级参数：身份从当前活跃图形会话推导，手柄地址等生效值来自用户配置，
命令行上也没有可覆盖它们的开关。
看到 `Press Ctrl+C to exit.` 后，**按一下手柄的 PS 键**让它连上蓝牙：Manager 按系统配置
里的间隔（默认 3 秒）扫描一次系统，发现的每只新手柄都会自动多出一台 USB 手柄；程序本身
不会自己退出。

> `--help` 只打印程序名下的用法与两份配置的路径。`--worker` 是 Manager 交给降权
> worker 的内部入口，配置随一条被交接的 fd 一起传下，不要手填。

---

## 游戏音频与震动

程序会在 PipeWire 里创建一个播放设备，名字类似
`DualSense Wireless Controller (PS5) 0c275682705e`（后半段是这只手柄的 MAC，在
`pavucontrol` 或 Steam 的输出设备列表里能看到）。**把这个设备的音量保持非静音**，
把游戏或系统的输出指到它，手柄的耳机口就会出声，触觉信息一并送给手柄。

不需要手柄出声就在用户配置里写 `audio: false`（这个播放设备也不会出现）。音频起不来
时程序只打一条警告，按键桥接照常工作。

### 麦克风

同一个设备在 PipeWire 里还会有一个录音设备，名字类似
`DualSense Wireless Controller (PS5) Microphone 0c275682705e`。把它（或系统默认
输入）选为麦克风，程序就会让手柄打开麦克风；录音停止时自动关掉。`audio: false` 时
这个录音设备也不会出现。

游戏侧的震动效果由游戏自己的手柄震动设置决定，和真 USB 手柄一样。

---

## 停止与清理

- systemd：`sudo systemctl stop ds5dummy`
- 手动运行：`Ctrl+C`
- 想彻底清掉残留的虚拟设备与挂载：

```bash
sudo ./scripts/teardown.sh
```

---

## 常见问题

**`The Manager must run as root`**
程序创建虚拟 USB 设备需要特权：用 `sudo` 手动启动，或者交给 systemd 单元（它本来就以
root 跑 Manager）。

**`no active graphical session` / `multiple active graphical sessions`**
降权身份来自活跃图形会话。未登录（或只有 TTY 会话）时服务每 2 秒以非零码退出并被重拉，
登录后会自动起来；`identity.mode: auto` 下同时有两个活跃图形会话时程序无法替用户决定，
改用 `identity.mode: list` 把要服务的用户写进 `users`。

**服务停在 inactive，日志说 `disabled the service` 或
`no active graphical session belongs to an allowed user`**
配置里写了 `enabled: false`，或该用户不在 `identity.mode: list` 的名单里。这是正常退出，
systemd 不会重启；改掉那一行（并让用户登录）后 `sudo systemctl restart ds5dummy`。
家目录没有配置文件不属于这一类：那种情况服务按内置默认值照常启动，日志里是
`No config for uid ...; using built-in defaults`。

**服务每 2 秒重启一次，日志里是配置解析或读取失败的报错**
配置没通过解析（未知键、类型不符、`bt_addr` 不是 `auto`/`list`/合法地址、
`controllers` 的键不是 MAC 或重复、`uid`/`gid` 为 0、配置的 `gid` 不在组库、
文件不可读等）。报错文本带出错键名或路径，改好后 `sudo systemctl restart ds5dummy`。

**`No dummy_udc.N in /sys/class/udc — load it first`**
内核模块没加载或 UDC 数量不够：`sudo modprobe dummy_hcd is_high_speed=0
num=<要同时桥接的手柄数>`，或跑 `sudo ./scripts/setup.sh`。仍然失败说明内核没编
`CONFIG_USB_DUMMY_HCD`。

**等满 `bt_wait_s`（默认 60 秒）还是连不上手柄（`No L2CAP channel to ... within 60 s`）**
按出现频率排：
- 手柄正被别的连接占着（尤其是 LE 连接）：`bluetoothctl disconnect <MAC>` 后重连；
- 内核 HID 驱动已经绑走了这只手柄：hidraw 后端要装本项目那个驱动补丁包，或临时
  `sudo ./scripts/setup.sh` 让它解绑；
- 链路密钥没了（换过系统、清过蓝牙数据）：重新 `bluetoothctl` 配对（`agent on`）。

**日志里反复出现 `no Bluetooth DualSense found ... is it paired and connected?`**
程序在蓝牙里找不到这只手柄。确认它已开机、已配对、已信任，并按一下 PS 键连接。
可以把用户配置里的 `bt_wait_s` 调大（例如 `120`）多等一会儿。

**手柄出声没有 / 日志提示 `pipewire-0 not writable`**
worker 以活跃图形会话那个用户的身份运行，`XDG_RUNTIME_DIR` 由它自己找回
`/run/user/<uid>`。出现这条提示说明该用户的运行时目录里没有可写的 `pipewire-0`：确认
这个用户的桌面会话里 PipeWire 正在运行。身份由会话自动决定，不需要再配 uid/gid。

**运行中途手柄断了**
拔蓝牙、手柄没电、或被别的设备抢走都会断链。Manager 会拆掉那一只对应的虚拟设备、等 2 秒
再建一轮并重新等它，其他手柄不受影响，所以只要按一下 PS 键就能恢复；不用重启服务。

**虚拟设备留下了残骸**
`sudo ./scripts/teardown.sh` 会把所有 `ds5dummy-*` 设备逐个解绑、卸掉各自的 FunctionFS
挂载，再清掉内核模块。

---

## 许可与致谢

DS5Dummy 以 **GPL-3.0-or-later** 发布，完整许可文本见 [LICENSE](LICENSE)。

本项目的 bridge 是 [DS5Dongle](https://github.com/awalol/DS5Dongle) 的移植；
DS5Dongle 以 MIT 许可发布（Copyright (c) 2026 awalol），其版权与许可声明见
[NOTICE](NOTICE)。

并同样感谢 [SAxense](https://github.com/egormanga/SAxense) 项目对高级振动的研究。

### 内核模块（`hid-playstation-ds5dummy/`）

这个目录是 Linux 内核 `drivers/hid/hid-playstation.c` 的修改副本，并附带同一内核的
`drivers/hid/hid-ids.h`；改动是让**蓝牙** DualSense / DualSense Edge 留在 hidraw-only
模式，并把合成身份改成 `1998:1221`（DualSense）与 `1998:1103`（DualSense Edge）。
DualShock 4 与所有 USB 连接的设备不走这条分支，仍由原驱动路径处理。它是**独立作品**，
不属于上面 GPL-3.0-or-later 的范围，按 **GPL-2.0-or-later** 发布：

- 版权：`hid-playstation.c` 为 Copyright (c) 2020-2022 Sony Interactive
  Entertainment；`hid-ids.h` 的版权归文件内逐条列出的作者所有。
- 两个文件头都是 `SPDX-License-Identifier: GPL-2.0-or-later`，模块声明
  `MODULE_LICENSE("GPL")`。
- 由它打出的 `hid-playstation-ds5dummy-dkms` 包同样声明 `GPL-2.0-or-later`。

它只与内核链接，与主项目的用户态程序没有代码往来，两者在仓库里只是聚合分发。内核整体
按 GPL-2.0-only 发布，所以这个模块保持 GPL-2.0 兼容（`-or-later` 允许按 v2 使用），
不随主项目升到 v3。

感谢 Qoder CN 在这段时间的免费试用，本项目使用 Qwen3.8-Flash 和 DeepSeek-Flash 完成。
