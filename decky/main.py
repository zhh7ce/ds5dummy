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
import socket

import decky

from ds5dummy import Controller, Layout, from_environment
from ds5dummy.log import EventLog, LEVEL_ERROR, LEVEL_INFO, LEVEL_WARN
from ds5dummy.system import RealSystem

# 配置页面监听的端口。写死不给用户选：页面上只有一个入口，撞端口时把错误显示在
# 面板上比让用户猜一个空闲端口更省事。
CONFIG_WEB_PORT = 8777


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


class ConfigWeb:
    """配置页面的那个本地服务：按需起停，绑 0.0.0.0 让局域网里的电脑能打开。

    页面本身是树里的 `lib/buttons_web.py`（Python 回环服务 + `buttons_check` 校验）。
    它自己不会弹浏览器——面板只给出一个地址，由用户在那台电脑上手输。服务脱离插件
    进程运行（`spawn` 自成会话），日志落在数据目录，不混进 daemon.log。

    生命周期跟着插件后端：`_unload`/`_uninstall` 会把它停掉，所以不会留下孤儿进程。
    """

    def __init__(self, layout: Layout, system: RealSystem, log: EventLog) -> None:
        """只记录依赖，不启动任何东西。

        Args:
            layout: 插件布局（树在 `layout.tree_dir`）。
            system: 执行接缝。
            log: 插件事件日志。
        """
        self._layout = layout
        self._system = system
        self._log = log
        self._pid: int | None = None
        self._error = ""

    def _running(self) -> bool:
        """服务是否还活着；进程已经退出就顺手把 pid 忘掉。"""
        if self._pid is None:
            return False
        if self._system.alive(self._pid):
            return True
        self._pid = None
        return False

    def _lan_address(self) -> str:
        """本机在局域网里的地址。

        用 UDP `connect` 让内核选出口网卡（不会真的发包）；拿不到就退回回环地址，
        面板上显示 `127.0.0.1` 至少能让用户在本机打开。
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(0.2)
            sock.connect(("192.168.1.1", 9))
            return sock.getsockname()[0]
        except OSError:
            return "127.0.0.1"
        finally:
            sock.close()

    def url(self) -> str:
        """面板上显示给用户的地址：局域网 IP + 固定端口。"""
        return f"http://{self._lan_address()}:{CONFIG_WEB_PORT}/"

    def state(self) -> dict[str, object]:
        """拼给面板的状态块。"""
        running = self._running()
        return {
            "on": running,
            "url": self.url() if running else "",
            "error": "" if running else self._error,
        }

    def _python(self) -> str | None:
        """跑页面用的解释器。

        **不能用 `sys.executable`**：Decky 后端是 PyInstaller 打包的，那里
        `sys.executable` 是 `PluginLoader` 本体。把它当 python 起一遍，等于又拉起
        一个 Decky Loader，画面整个切走——用户看到的就是"Steam 重启了"。
        系统 python 才是页面要的：SteamOS 自带 `/usr/bin/python3` 且带 PyYAML。

        Returns:
            解释器路径；找不到时 None。
        """
        for candidate in ("/usr/bin/python3", "/usr/bin/python"):
            if os.path.isfile(candidate):
                return candidate
        return shutil.which("python3")

    async def set_enabled(self, enabled: bool) -> dict[str, object]:
        """起或停服务，返回新状态（停的时候顺手清掉上次的错误）。

        Args:
            enabled: True 起、False 停。

        Returns:
            `state()` 的结果。
        """
        if not enabled:
            await self.stop()
            return self.state()
        if self._running():
            return self.state()

        root = self._layout.tree_dir
        page = os.path.join(root, "lib", "buttons_web.py")
        if not os.path.isfile(page):
            self._error = "配置页面未随插件安装"
            return self.state()

        python = self._python()
        if python is None:
            self._error = "系统里找不到 python3，无法启动配置页面"
            return self.state()

        argv = [
            python,
            page,
            "--check",
            os.path.join(root, "bin", "buttons_check"),
            "--plugins-dir",
            os.path.join(root, "plugins"),
            # 绑所有地址：用户要的就是"在局域网里别的电脑上改"。
            "--bind",
            "0.0.0.0",
            "--port",
            str(CONFIG_WEB_PORT),
        ]
        self._error = ""
        log_path = os.path.join(self._layout.logs_dir, "config-web.log")
        try:
            self._pid = self._system.spawn(argv, root, log_path)
        except OSError as error:
            self._error = f"启动失败：{error}"
            return self.state()

        # 等到端口真的能连上再说"已启动"：子进程立刻退出（端口被占）和"起错了程序"
        # 都在这里现形，面板不会显示一个假地址。最多等 2 s。
        ready = False
        for _ in range(40):
            if not self._running():
                break
            try:
                socket.create_connection(
                    ("127.0.0.1", CONFIG_WEB_PORT), timeout=0.2
                ).close()
                ready = True
                break
            except OSError:
                await asyncio.sleep(0.05)
        if ready:
            self._log.info(f"配置页面已启动：{self.url()}")
            return self.state()

        detail = ""
        try:
            lines = self._system.read(log_path).strip().splitlines()
            detail = lines[-1] if lines else ""
        except OSError:
            detail = ""
        await self.stop()
        self._error = f"配置页面没有起来（端口 {CONFIG_WEB_PORT} 被占用，或页面脚本出错）"
        if detail:
            self._error += f"：{detail}"
        return self.state()

    async def stop(self) -> None:
        """停掉服务（幂等）；停完把 pid 清空。"""
        if self._pid is None:
            return
        pid, self._pid = self._pid, None
        if self._system.kill(pid):
            self._log.info("配置页面已停止")
        # 回收：已经在退出路上的子进程几毫秒内就没了；回收不到也不影响功能。
        for _ in range(20):
            if self._system.wait(pid) is not None:
                break
            await asyncio.sleep(0.01)
        self._error = ""


class Plugin:
    """Decky 的插件类；loader 会实例化它并按方法名派发 RPC。"""

    def __init__(self) -> None:
        """只搭骨架，不碰系统；真正的动作都在 `_main()` 之后。"""
        self._layout = from_environment()
        self._system = RealSystem()
        self._log = EventLog(_log_sink)
        self._controller: Controller | None = None
        self._config = ConfigWeb(self._layout, self._system, self._log)

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
        await self._config.stop()

    async def _uninstall(self) -> None:
        """卸载插件：再执行一次幂等停用，然后删掉设置目录（C-06）。

        模块缓存与日志在数据目录里，Decky 删除插件数据时一并清理，本方法不碰它们。
        """
        controller = self._controller
        self._controller = None
        if controller is not None:
            await controller.shutdown()
        await self._config.stop()
        shutil.rmtree(self._layout.settings_dir, ignore_errors=True)

    async def get_state(self) -> dict[str, object]:
        """面板轮询（U-01）：当前状态、模块、程序与配置页面的只读快照。"""
        return self._with_config(await self._require().get_state())

    async def set_enabled(self, enabled: bool) -> dict[str, object]:
        """主开关（U-01、E-09）。"""
        return self._with_config(await self._require().set_enabled(bool(enabled)))

    async def set_config_web(self, enabled: bool) -> dict[str, object]:
        """配置页面的开关：起/停那个本地服务，绑 0.0.0.0 供局域网访问。

        返回新的快照（含 `config_web`），面板据此把滑块拨到位并显示地址。
        """
        await self._config.set_enabled(bool(enabled))
        return await self.get_state()

    async def recheck(self) -> dict[str, object]:
        """重新检查（U-03）：强制走一遍远端查询。"""
        return self._with_config(await self._require().recheck())

    async def redownload(self) -> dict[str, object]:
        """重新下载当前内核模块（U-09、M-09）。"""
        return self._with_config(await self._require().redownload())

    async def restart_daemon(self) -> dict[str, object]:
        """只重启转发程序（U-05）。"""
        return self._with_config(await self._require().restart_daemon())

    async def clean_cache(self) -> dict[str, object]:
        """清理当前内核以外的模块缓存（U-06）。"""
        return self._with_config(await self._require().clean_cache())

    async def get_logs(self) -> dict[str, object]:
        """日志页（U-07）：程序日志尾部与插件事件日志。"""
        return await self._require().get_logs()

    def _with_config(self, snapshot: dict[str, object]) -> dict[str, object]:
        """给核心状态机的快照补上配置页面那一段。

        面板拿到 RPC 返回值就整个替换进界面，所以**每个返回快照的方法都要过这一层**：
        少一个字段就是一次渲染异常，Decky 会把它显示成插件错误页。
        """
        snapshot["config_web"] = self._config.state()
        return snapshot

    def _require(self) -> Controller:
        """取状态机；`_main` 还没跑过时是编程错误，直接抛给 Decky 的 RPC 层。"""
        if self._controller is None:
            raise RuntimeError("插件后端尚未初始化")
        return self._controller
