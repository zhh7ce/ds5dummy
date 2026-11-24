#!/usr/bin/env python3
"""Decky 插件后端入口：把 loader 的 RPC 方法逐项接到 `ds5dummy.Controller`。

这里只做三件事：按 Decky 注入的环境变量搭布局、把事件日志接到 `decky.logger`、
把每个可调用方法转给状态机。所有会改动系统的逻辑都在 `ds5dummy/` 里，本文件不含
任何内核或进程操作——这样那些逻辑可以在没有 Decky 的机器上单测（A-03）。

Decky 的调用约定：loader 在专属事件循环里实例化 `Plugin`，把 `_main()` 作为任务排进
这个循环，之后每个 `async` 方法都在同一循环里调用（`_unload`/`_uninstall` 由 loader
await）。`_main()` 本身没有人 await，所以这里不靠它"跑完"保证任何事：
`self._controller` 在第一次 `await` 之前就装好，面板的 RPC 从头到尾都拿得到它。
状态机内部用 `ensure_future`/`Lock`，依赖的正是这个"同一循环"前提。
"""

from __future__ import annotations

import asyncio
import os
import shutil
import sys

import decky

from ds5dummy import Controller, Layout, from_environment
from ds5dummy.log import EventLog, LEVEL_ERROR, LEVEL_INFO, LEVEL_WARN
from ds5dummy.system import RealSystem


def _log_sink(level: str, message: str) -> None:
    """把一条事件日志转发到 Decky 的日志（G-09：动作进插件日志）。

    Args:
        level: `info`/`warn`/`error` 之一，来自 `EventLog`。
        message: 正文；Decky 的 logger 自己加时间戳与来源前缀。
    """
    if level == LEVEL_ERROR:
        decky.logger.error(message)
    elif level == LEVEL_WARN:
        decky.logger.warning(message)
    else:
        decky.logger.info(message)


def _read_version(layout: Layout) -> str:
    """读随插件发布的 `VERSION`（U-08）；缺失时返回占位串，不让面板显示为空。

    Args:
        layout: 插件布局。

    Returns:
        版本号文本，与 `plugin.json` 同源（打包时写入）。
    """
    try:
        with open(layout.version_file, encoding="utf-8") as handle:
            return handle.read().strip() or "未知版本"
    except OSError:
        return "未知版本"


class Plugin:
    """Decky 的插件类；loader 会实例化它并按方法名派发 RPC。"""

    def __init__(self) -> None:
        """只搭骨架，不碰系统；真正的动作都在 `_main()` 之后。"""
        self._layout = from_environment()
        self._system = RealSystem()
        self._log = EventLog(_log_sink)
        self._controller: Controller | None = None

    async def _main(self) -> None:
        """插件加载：建状态机，按持久化的开关执行一次启用流程（L-01）。

        loader 把本方法当任务跑、不 await 它，所以第一次启用里的下载与接管不会挡住
        面板的 RPC——`self._controller` 在 `await` 之前已经就位。等它结束只影响
        "`start()` 返回时状态已定"这一条：测试与 `_unload` 的 `shutdown()` 都按这个
        语义写，面板则靠轮询看过程。
        """
        self._controller = Controller(
            self._system,
            self._layout,
            version=_read_version(self._layout),
            log=self._log,
        )
        await self._controller.start()
        decky.logger.info(f"DS5Dummy 插件后端就绪（随包程序版本 {_read_version(self._layout)}）")

    async def _unload(self) -> None:
        """插件卸载/重载：停用系统侧，但保留用户的开关设置（L-06、L-02）。

        `shutdown()` 里失败只记日志——Decky 不会因为卸载失败再给我们第二次机会，
        抛异常只会让卸载半途而废、留下黑名单或模块。
        """
        controller = self._controller
        self._controller = None
        if controller is not None:
            await controller.shutdown()

    async def _uninstall(self) -> None:
        """卸载插件：再执行一次幂等停用，然后删掉设置目录（C-06）。

        模块缓存与日志在数据目录里，Decky 删除插件数据时一并清理，本方法不碰它们。
        """
        controller = self._controller
        self._controller = None
        if controller is not None:
            await controller.shutdown()
        shutil.rmtree(self._layout.settings_dir, ignore_errors=True)

    async def get_state(self) -> dict[str, object]:
        """面板轮询（U-01）：当前状态、模块与程序的只读快照。"""
        return await self._require().get_state()

    async def set_enabled(self, enabled: bool) -> dict[str, object]:
        """主开关（U-01、E-09）。"""
        return await self._require().set_enabled(bool(enabled))

    async def recheck(self) -> dict[str, object]:
        """重新检查（U-03）：强制走一遍远端查询。"""
        return await self._require().recheck()

    async def redownload(self) -> dict[str, object]:
        """重新下载当前内核模块（U-09、M-09）。"""
        return await self._require().redownload()

    async def restart_daemon(self) -> dict[str, object]:
        """只重启转发程序（U-05）。"""
        return await self._require().restart_daemon()

    async def clean_cache(self) -> dict[str, object]:
        """清理当前内核以外的模块缓存（U-06）。"""
        return await self._require().clean_cache()

    async def get_logs(self) -> dict[str, object]:
        """日志页（U-07）：程序日志尾部与插件事件日志。"""
        return await self._require().get_logs()

    async def open_config(self) -> dict[str, object]:
        """起随包的配置页面并返回地址；重复调用时端口被占的那一个会自己退出。

        页面是 Python 脚本 + 独立校验器，不编进转发程序。参数显式指向同一插件
        目录里的页面、校验器与插件目录：它们随 ZIP 解包到 bin/ 与 lib/，而页面
        自己的默认值只认 /usr 布局。服务端由 `spawn` 脱离插件进程，日志落在数据
        目录，不污染状态机那一份 daemon.log。
        """
        root = self._layout.plugin_dir
        page = os.path.join(root, "lib", "buttons_web.py")
        if not os.path.isfile(page):
            return {"ok": False, "error": "配置页面未随插件安装"}

        argv = [
            sys.executable,
            page,
            "--check",
            os.path.join(root, "bin", "buttons_check"),
            "--plugins-dir",
            os.path.join(root, "plugins"),
        ]
        try:
            self._system.spawn(
                argv, root, os.path.join(self._layout.logs_dir, "config-web.log")
            )
        except OSError as error:
            return {"ok": False, "error": str(error)}
        return {"ok": True, "url": "http://127.0.0.1:8777/"}

    def _require(self) -> Controller:
        """取状态机；`_main` 还没跑过时是编程错误，直接抛给 Decky 的 RPC 层。"""
        if self._controller is None:
            raise RuntimeError("插件后端尚未初始化")
        return self._controller
