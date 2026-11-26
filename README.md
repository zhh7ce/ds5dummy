# DS5Dummy

把**蓝牙 DualSense**变成 Linux 主机看到的**虚拟 USB 手柄**——每只手柄一台，
插上（开机）就自动出现，断开就自动消失。

程序为发现的每只手柄在系统里造一台虚拟 USB 设备（设备身份就是 `Sony Interactive
Entertainment DualSense Wireless Controller`，`054c:0ce6`），同时把它经蓝牙送来的
按键、摇杆、陀螺仪实时送进对应的虚拟设备；主机侧的游戏以为插了一只 USB 手柄。反方向
也通：游戏发给手柄的震动、指示灯指令会经蓝牙回到真手柄。

除了 HID，还能把主机侧的**游戏音频 + 触觉反馈**下传给手柄（耳机口出声、喇叭与马达
振动），并把**手柄麦克风**接成主机的一个虚拟录音设备。音频这一步是可关的、失败也不
影响按键桥接。

典型用途：只想认 USB 手柄的游戏、Wine/Proton、或者需要手柄以 USB 身份出现的场景。

> 环境要求、内部结构、模块边界、构建矩阵与打包细节在 [AGENTS.md](AGENTS.md)。

---

## 安装

所有安装方式的前提相同：内核带 `dummy_hcd`（`CONFIG_USB_DUMMY_HCD=m`）、`libcomposite`、
`usb_f_fs` 与 configfs；BlueZ 里手柄已**配对、信任**；程序以 root 运行，并且要在某个用户
**图形登录之后**启动（降权身份来自活跃图形会话）。完整清单与模块参数见
[AGENTS.md 的「环境要求与安装」](AGENTS.md#18-环境要求与安装用户侧步骤)。

按你的系统选一条：

| 你的系统 | 怎么装 |
|---|---|
| Arch Linux（含 Arch 系衍生版） | release 上的预编译包 `ds5dummy-bin`，或用仓库里的 PKGBUILD 自己编 |
| SteamOS（原生 SteamOS / Steam Deck） | Decky Loader 插件 |
| 其他 Linux | 从源码构建 |

### Arch Linux（含衍生版）

**预编译包**：release 上挂着打包时编好的 `ds5dummy-bin.pkg.tar.zst`，装不了工具链的机器
也能直接装：

```bash
sudo pacman -U ds5dummy-bin.pkg.tar.zst
```

**自己编**：

```bash
cd pkg/ds5dummy-hidraw
makepkg -d
sudo pacman -U ds5dummy-hidraw-*.pkg.tar.zst
```

**两种装法都必须再装内核驱动补丁**，否则内核自带的 `hid-playstation` 会接管这只蓝牙
手柄，主机上除了虚拟 USB 手柄还会多出一只真手柄：

```bash
cd pkg/hid-playstation-ds5dummy
makepkg -d
sudo pacman -U hid-playstation-ds5dummy-dkms-*.pkg.tar.zst
```

这个 DKMS 包不写内核头文件依赖，装之前请自行装好当前内核对应的头文件（Arch 上是
`linux-headers` 或 `linux-zen-headers`）。

**配置页面**：主程序包已自带。运行 `ds5dummy-config` 后在浏览器打开
`http://127.0.0.1:8777/`。

### SteamOS

原生 SteamOS 是不可变系统，系统更新会把 `/usr` 整份替换掉，装在系统目录里的东西留不住，
所以走 [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) 插件这条路：装
Decky，从本项目 release 下载 `ds5dummy-decky.zip`，Decky → 设置 → 开发者 → 从 ZIP 安装
插件，再在 Decky 的插件列表里打开 **DS5Dummy**。插件自带转发程序，内核模块按当前系统内核
自动取回并缓存；系统更新换内核后同样不需要手动处理。面板说明、缓存位置与卸载见
[decky/README.md](decky/README.md)。配置页面在面板的「配置页面」里用开关启停：打开后
面板会显示一个局域网地址，在同一局域网的电脑浏览器里打开它就能改按键配置（页面没有
口令，用完记得关掉）；也可以用插件树里的 `bin/ds5dummy-config` 手工起。

### 其他 Linux

从源码构建：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo ./scripts/setup.sh            # 加载内核模块、确保 configfs 可用
```

然后 `sudo ./build/ds5dummyd`，或者自己写一份等价的 systemd 单元与内核模块配置。这里同样
要装内核驱动补丁，否则内核自带的 `hid-playstation` 会接管蓝牙手柄：`setup.sh` 对它的
解绑只是临时的，重启或手柄重连后仍会重新接管；`pkg/hid-playstation-ds5dummy/` 里的驱动
源码可以交给 dkms 在其它发行版上编译。随包的两个插件不在主程序里：构建产物
`build/idle.so` 与 `build/buttons.so` 要放到 `/usr/lib/ds5dummy/plugins/` 或登录用户的
`~/.config/ds5dummy/plugins/` 才会被装载。

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

源码构建后可以直接跑：

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

## 许可与致谢

DS5Dummy 以 **GPL-3.0-or-later** 发布，完整许可文本见 [LICENSE](LICENSE)。

本项目的 bridge 与手柄侧的 HID report descriptor、报告字节布局移植自
[DS5Dongle](https://github.com/awalol/DS5Dongle)；DS5Dongle 以 MIT 许可发布
（Copyright (c) 2026 awalol），其版权与许可声明见 [NOTICE](NOTICE)。

日志底层是随仓库分发的 [loguru](https://github.com/emilk/loguru)
（Copyright (c) Emil Ernerfeldt，public domain），源码在 `third_party/`。

按键映射配置页面里的 DualSense 线稿取自
[dualsense-tester](https://github.com/daidr/dualsense-tester)；它以 MIT 许可发布
（Copyright (c) 2023 Xuezhou Dai），版权与许可声明同样见 [NOTICE](NOTICE)。
页面的标签页、分节卡片与控件语言沿用
[DS5Dongle-Studio](https://github.com/artzox/DS5Dongle-Studio) 的配置门户
（MIT，Copyright (c) 2026 awalol 与 artzox）。

并同样感谢 [SAxense](https://github.com/egormanga/SAxense) 项目对高级振动的研究。

运行期与构建期依赖（动态链接或头文件使用，仓库里不含它们的源码）：PipeWire / SPA
（MIT）、BlueZ（LGPL-2.1-or-later）、Opus（BSD-3-Clause）、libsamplerate
（BSD-2-Clause）、yaml-cpp（MIT）、libsystemd（LGPL-2.1-or-later）、Boost
（BSL-1.0）、fmt（MIT）；测试另外用 googletest（BSD-3-Clause），配置页面用
PyYAML（MIT）。

### 内核模块（`pkg/hid-playstation-ds5dummy/`）

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
