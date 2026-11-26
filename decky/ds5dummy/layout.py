#!/usr/bin/env python3
"""插件目录布局：哪些东西写在插件目录，哪些在 Decky 的设置/数据目录（C-05）。

Decky 给插件三处互不重叠的位置，职责不能混：

- 插件目录（`homebrew/plugins/<插件>`）：随插件更新**整目录替换**，所以只放代码与
  随包的便携树（`bin/ds5dummy/`，里面是 `bin/`、`lib/`、`plugins/`、`etc/` 与
  `share/`），不放任何运行期状态。
- 设置目录（`homebrew/settings/<插件>`）：用户可见的开关与虚拟 UDC 数量。
- 数据目录（`homebrew/data/<插件>`）：内核模块缓存、程序日志、pid 文件。卸载插件时
  随数据目录一起被 Decky 删除，所以本模块不负责清理它们。

三个位置都由 decky-loader 以环境变量交给后端；缺变量（例如在 Decky 之外手工跑）
时按 `DECKY_HOME`（缺省 `/home/deck/homebrew`）推出来，推不出来就让调用方看见错误
而不是悄悄写进当前目录。
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PLUGIN_NAME = "ds5dummy"
DEFAULT_HOMEBREW = "/home/deck/homebrew"

# 便携树在插件目录里的固定位置：ZIP 里 `bin/ds5dummy/` 是整棵 tarball 树，
# 程序、随包库与随包插件都在里面，彼此的相对位置与 tarball 一致。
_TREE_DIR = "bin/ds5dummy"

# 树内相对路径：程序与版本号。
_DAEMON_BIN = "bin/ds5dummyd"
_VERSION_FILE = "VERSION"


@dataclass(frozen=True)
class Layout:
    """三处根目录，以及由它们派生出的全部固定路径。

    Attributes:
        plugin_dir: 插件目录（decky-loader 的 `homebrew/plugins/<插件>`）。
        settings_dir: 插件设置目录。
        data_dir: 插件数据目录；模块缓存、日志与 pid 文件都在它下面。
    """

    plugin_dir: str
    settings_dir: str
    data_dir: str

    @property
    def settings_file(self) -> str:
        """设置文件；单个 JSON，开关与 UDC 数量都在里面。"""
        return os.path.join(self.settings_dir, "settings.json")

    @property
    def cache_root(self) -> str:
        """模块缓存根：`<cache_root>/<release>/<release>.ko`（M-02）。"""
        return os.path.join(self.data_dir, "kmod")

    @property
    def tmp_dir(self) -> str:
        """下载临时目录（M-06）：与缓存同层，跨文件系统 rename 才不会退化成复制。"""
        return os.path.join(self.cache_root, ".tmp")

    @property
    def logs_dir(self) -> str:
        """程序日志目录；每次启动覆盖写 `daemon.log`。"""
        return os.path.join(self.data_dir, "logs")

    @property
    def daemon_log(self) -> str:
        """转发程序的 stdout/stderr，也就是面板「最近一行」与日志页读的那个文件。"""
        return os.path.join(self.logs_dir, "daemon.log")

    @property
    def pid_file(self) -> str:
        """转发程序 pid 的落盘位置，插件后端重启后靠它认领已在跑的进程（L-02）。"""
        return os.path.join(self.data_dir, "daemon.pid")

    @property
    def tree_dir(self) -> str:
        """随插件发布的便携树根：`<插件目录>/bin/ds5dummy`。

        这棵树与 tarball 解压后的目录逐字节相同，程序按自身位置找配置
        （`<树>/etc/config.yaml`）与随包插件（`<树>/plugins`）。
        """
        return os.path.join(self.plugin_dir, _TREE_DIR)

    @property
    def daemon_bin(self) -> str:
        """随插件发布的转发程序本体（C-02、C-04）。"""
        return os.path.join(self.tree_dir, _DAEMON_BIN)

    @property
    def version_file(self) -> str:
        """打包时写入的应用版本号，与 `package.json` 同源（U-08 显示它）。

        放在插件根上（不是树里）：它是面板的展示信息，不是树的一部分。
        """
        return os.path.join(self.plugin_dir, _VERSION_FILE)


def from_environment() -> Layout:
    """按 decky-loader 交给后端的环境变量构造布局。

    缺变量时按 `DECKY_HOME`（缺省 `/home/deck/homebrew`）补齐；插件目录还有最后
    一条退路——本文件往上两级，也就是仓库里的 `decky/`。这条路径只在 Decky 之外
    手工跑后端时生效，正常安装永远走环境变量。

    Returns:
        三处目录都已是绝对路径的布局。
    """
    plugin_dir = os.environ.get("DECKY_PLUGIN_DIR")
    settings_dir = os.environ.get("DECKY_PLUGIN_SETTINGS_DIR")
    data_dir = os.environ.get("DECKY_PLUGIN_RUNTIME_DIR")
    if plugin_dir and settings_dir and data_dir:
        return Layout(plugin_dir, settings_dir, data_dir)

    homebrew = os.environ.get("DECKY_HOME") or DEFAULT_HOMEBREW
    if not plugin_dir:
        # 没在 Decky 里跑时，插件目录就是本文件往上两级（decky/ds5dummy/layout.py）。
        plugin_dir = str(Path(__file__).resolve().parent.parent)
    settings_dir = settings_dir or os.path.join(homebrew, "settings", DEFAULT_PLUGIN_NAME)
    data_dir = data_dir or os.path.join(homebrew, "data", DEFAULT_PLUGIN_NAME)
    return Layout(plugin_dir, settings_dir, data_dir)
