# kmod

DS5Dummy 对上游 `hid-playstation` 驱动的最小补丁，供 `main` 上的
`.github/workflows/kmod.yml` 为每个 SteamOS 内核编一份 `<release>.ko`：

| 路径 | 内容 |
|---|---|
| `ds5dummy.patch` | 补丁本体：蓝牙 DualSense / Edge 改报 `1998:1221` / `1998:1103` 并停在 hidraw（不建输入设备）、`ps_remove` 容忍没有 `ps_device`、驱动名加 `-ds5dummy` 后缀；其余机型与所有 USB 设备保持上游行为 |
| `Makefile` | kbuild 外部模块定义，产物 `hid_playstation_ds5dummy.ko` |
| `README.md` | 本说明；不参与构建，也不参与重建判据 |

构建时 `scripts/kernel_source.py` 按头文件包版本换算出上游稳定标签
（`7.2.7.valve2-2` → `v7.2.7`，`6.5.0.valve23-1` → `v6.5`），从 linux-stable 的
GitHub 镜像取回该版本的 `drivers/hid/hid-playstation.c` 与 `hid-ids.h`，再用
`patch -p1 --fuzz=0` 打上 `ds5dummy.patch`，在检出目录的 `kmod/` 里就地编译。补丁
只压在与版本无关的五处（模块参数紧跟 `#include "hid-ids.h"`、`ps_probe` 的开头与
`hid_hw_start` 之前、`ps_remove` 开头、驱动名、`MODULE_DESCRIPTION`），5.13 到 7.x
共用同一份；上游一旦改动这些位置，构建直接失败，而不是带着偏移硬贴出一个可疑的
`.ko`。

发布侧每轮对 `ds5dummy.patch` 与 `Makefile` 取内容指纹，与 `seen.json` 里记下的
不同就整批重建已发布内核；这两个文件不变时，main 上的其它提交不会引起重建。

本目录与 `pkg/hid-playstation-ds5dummy/` 没有同步关系：DKMS 包继续携带一份完整的
`hid-playstation.c`（用户机器上没有可打补丁的内核源码），这里只带补丁。
