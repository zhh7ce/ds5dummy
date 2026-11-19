# SteamOS 内核模块（hid-playstation 分支）

每个 SteamOS 频道一个 Release，tag 就是频道名（`jupiter-3.10`、`jupiter-staging`
等）。Release 里每个内核一份 `hid_playstation_ds5dummy.ko`：HIDRAW 后端需要的
`hid-playstation` 分支，只接管蓝牙 DualSense / Edge 并合成 `1998:1221` /
`1998:1103`，其余机型与所有 USB 设备仍走原驱动。

资产名是 `<release>.ko`，`release` 就是模块 `vermagic` 的第一段，与设备上的
`uname -r` 逐字相等。下载地址不需要索引文件：

```
https://github.com/zhh7ce/ds5dummy/releases/download/<频道>/$(uname -r).ko
```

同一个内核会出现在多个频道的仓库里，每个频道各存一份：每份产物用的是该频道自己的
工具链。有没有对应的模块由 HTTP 状态回答：404 表示这个频道的 Release 里还没有这个
内核的产物。各频道的 tag 固定不变，内核版本迭代只追加资产；补丁变更时只重建受影响的
内核并覆盖同名资产。

支持 6.0 及以上的内核。更老的内核（SteamOS 3.0/3.1 时代的 5.13）不再提供产物：它们的
头文件包带着与当前工具链不兼容的预编译 gcc 插件，编不出来。

模块由固定流程产出：补丁取自 `main` 的 `kmod/`，驱动源码取自 linux-stable
镜像上该内核基数版本的标签（用 `patch -p1 --fuzz=0` 打上补丁），工具链取自头文件包
所在的同一频道仓库（`core-*` / `extra-*`）。每份产物的来源记录在下面的列表里。
