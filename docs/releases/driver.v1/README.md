# DS5Dummy 内核驱动（DKMS）

HIDRAW 后端需要的 `hid-playstation` 分支：只接管蓝牙 DualSense / Edge，并合成
`1998:1221` / `1998:1103`，其余机型与所有 USB 设备仍走原驱动。

这个 release 的下载地址是固定的，应用版本迭代不会换它；驱动本身更新时会发
`driver.v2`，届时安装页换成新链接。

## Arch Linux

驱动要靠 DKMS 现场编译，先装当前内核的头文件；装完重启一次。

```bash
sudo pacman -S --noconfirm "$(pacman -Qoq /usr/lib/modules/$(uname -r)/kernel | head -1)-headers"
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/driver.v1/hid-playstation-ds5dummy-dkms.pkg.tar.zst
```

## SteamOS（Steam Deck）

先按 v0.1.0 发布页的说明解除只读、放开签名校验、装好内核头文件，再执行上面的
`pacman -U` 命令，装完重启一次：
<https://github.com/zhh7ce/ds5dummy/releases/tag/v0.1.0>

SteamOS 的镜像更新会整份替换 `/usr` 与 `/etc`，更新后重跑安装命令即可。
