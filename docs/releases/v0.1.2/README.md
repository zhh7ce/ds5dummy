# DS5Dummy v0.1.2

把真实的蓝牙 DualSense 变成 Linux 主机看到的虚拟 USB 手柄：按键、摇杆、震动、
音频与麦克风都走这条链路。

这一版新增可选的配置工具，Arch Linux 仍然走两个 pacman 包。

## Arch Linux

安装是两个包：先装内核驱动（HIDRAW 后端依赖的 `hid-playstation` 分支），再装主程序。
驱动要靠 DKMS 现场编译，先装当前内核的头文件：

```bash
sudo pacman -S --noconfirm "$(pacman -Qoq /usr/lib/modules/$(uname -r)/kernel | head -1)-headers"
```

```bash
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/driver.v1/hid-playstation-ds5dummy-dkms.pkg.tar.zst
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/v0.1.2/ds5dummy-bin.pkg.tar.zst
```

装完重启一次。安装钩子会尝试卸掉系统自带的 `hid_playstation` 并加载
`hid_playstation_ds5dummy`，但旧模块正被手柄占用时卸不掉；重启之后系统里只有我们的
模块。手柄要已配对、信任、绑定，并且在有图形会话登录之后才会建站，状态看
`systemctl status ds5dummy`。

## Steam Deck

SteamOS 的根文件系统只读，系统更新还会整份替换 `/usr` 与 `/etc`，所以 Deck 上不装
pacman 包，改用 [Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader) 插件：

1. 先装 Decky Loader。
2. 下载本页的 `ds5dummy-decky.zip`。
3. Decky → 设置 → 开发者 → 从 ZIP 安装插件，选那个文件。
4. 回到 Decky 的插件列表，打开 **DS5Dummy**。

插件自带转发程序，内核模块按当前系统内核自动取回并缓存：第一次启用要下载一份约
1 MB 的模块，之后就一直用缓存，系统更新换内核后同样不需要手动处理。手柄不必先连上，
启用后它出现时会被自动接管。

之前按 Arch 的方式在 Deck 上装过这个项目的（`ds5dummy_bin`、`ds5dummy_hidraw` 或
`ds5dummy_l2cap`，外加 `hid-playstation-ds5dummy-dkms`），先把它们停掉、卸干净再装
插件：两边会争同一批内核模块与虚拟设备。

```bash
sudo systemctl disable --now ds5dummy
sudo pacman -R ds5dummy_bin hid-playstation-ds5dummy-dkms
```

装的是源码包就把 `ds5dummy_bin` 换成 `ds5dummy_hidraw` 或 `ds5dummy_l2cap`。卸完重启
一次，再回到上面安装插件。

面板说明、缓存位置与卸载方式见插件目录里的 README。

## 配置工具（可选）

不想手写 YAML 就单独装本页的 `ds5dummy-config-bin.pkg.tar.zst`：

```bash
sudo pacman -U https://github.com/zhh7ce/ds5dummy/releases/download/v0.1.2/ds5dummy-config-bin.pkg.tar.zst
```

装好后在终端运行 `ds5dummy-config`，浏览器打开 `http://127.0.0.1:8777/` 就能在网页上
改手柄设置（按键映射、自动关机之类），保存前会先做一次校验。
