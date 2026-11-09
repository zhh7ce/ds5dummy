# DS5Dummy v0.1.0

把真实的蓝牙 DualSense 变成 Linux 主机看到的虚拟 USB 手柄：按键、摇杆、震动、
音频与麦克风都走这条链路。

安装是两个包：先装内核驱动（HIDRAW 后端依赖的 `hid-playstation` 分支），再装主程序。
主程序装完会自动 `systemctl enable --now` 服务。

## Arch Linux

驱动要靠 DKMS 现场编译，先装当前内核的头文件：

```bash
sudo pacman -S --noconfirm "$(pacman -Qoq /usr/lib/modules/$(uname -r)/kernel | head -1)-headers"
```

```bash
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/driver.v1/hid-playstation-ds5dummy-dkms.pkg.tar.zst
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/v0.1.0/ds5dummy_bin.pkg.tar.zst
```

装完重启一次。安装钩子会尝试卸掉系统自带的 `hid_playstation` 并加载
`hid_playstation_ds5dummy`，但旧模块正被手柄占用时卸不掉；重启之后系统里只有我们的
模块。手柄要已配对、信任、绑定，并且在有图形会话登录之后才会建站，状态看
`systemctl status ds5dummy`。

## SteamOS（Steam Deck）

根文件系统只读，而且 pacman 的签名校验会挡住依赖包；先解除只读、放开签名校验，
再装当前内核的头文件：

```bash
sudo steamos-readonly disable
sudo cp /etc/pacman.conf /etc/pacman.conf.bak
sudo sed -i 's/^SigLevel.*/SigLevel = Optional TrustAll/' /etc/pacman.conf
sudo pacman -S --noconfirm "$(pacman -Qoq /usr/lib/modules/$(uname -r)/kernel | head -1)-headers"
```

再执行上面 Arch 的两条安装命令，然后恢复签名校验与只读，最后重启：

```bash
sudo cp /etc/pacman.conf.bak /etc/pacman.conf
sudo steamos-readonly enable
sudo reboot
```

`SigLevel = Optional TrustAll` 关掉的是包签名校验，只在安装这两个包期间需要，装完用
备份还原。

SteamOS 的镜像更新会整份替换 `/usr` 与 `/etc`，更新后重跑这些安装命令即可。
