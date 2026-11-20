#!/usr/bin/env python3
"""状态机与面板快照（§4.3、§4.9）：把解析、接管、守护串成一条可取消的流程。

所有会改动系统的动作都在这里排序，并且只经由两条消息改动面板可见的状态：
`_set()` 写快照字段，`_spawn_flow()` 串行化流程。状态只分八种（见 `STATE_*`），
每种都对应一句面向用户的话；原因码（`reason`）决定面板怎么解释失败（§5）。

**取消**用协作式检查点而不是 `Task.cancel()`：流程体在相邻步骤之间过
`_checkpoint()`，用户关开关时 `set_enabled(false)` 置位一个事件并**等流程自己
回滚完**再返回。这样 "关开关" 的语义是"回到停用状态"而不是"中断在某一步"，
也不会出现回滚只做了一半的事件循环竞态（E-09）。

**两把并发闸**：改动系统的流程（启用、停用、重启、内核变化后的重来）通过
`_spawn_flow()` 排队，同一时刻只有一个在跑；健康检查的节拍也走同一扇门，所以
"发现程序崩了要重来" 不会和用户手动操作叠在一起。
"""

from __future__ import annotations

import asyncio
import contextlib
import os
from typing import Awaitable, Callable, Iterable

from . import resolve as resolve_module
from . import settings as settings_module
from . import supervise
from . import takeover
from .layout import Layout
from .log import EventLog
from .system import System

STATE_DISABLED = "disabled"
STATE_RESOLVING = "resolving"
STATE_UNAVAILABLE = "unavailable"
STATE_WAITING = "waiting"
STATE_TAKING_OVER = "taking_over"
STATE_RUNNING = "running"
STATE_STOPPING = "stopping"
STATE_FAILED = "failed"

# 启动窗口（E-05）：程序在 3 s 内退出就按退出码分流，活过这 3 s 才算站住。
START_WINDOW_S = 3.0
WINDOW_POLL_S = 0.1
# 退出码 1 是"运行条件暂不具备"，按固定间隔重试且不计失败次数（L-05）。
NOT_READY_RETRY_S = 10.0
# 面板「最近一行」取日志尾部这么几行里的最后一条非空行。
LOG_LAST_LINES = 5
LOG_TAIL_LINES = 200

# 程序自己的退出码语义（AGENTS.md §3）：0 = 用户配置里关掉了服务；1 = 条件不成立；
# 2 = 交接快照解码失败；120+stage = fork/exec 前的失败。后两类都是"起不来"，
# 不该按崩溃退避重试（E-05）。
EXIT_DISABLED_BY_CONFIG = 0
EXIT_NOT_READY = 1
EXIT_BAD_SNAPSHOT = 2
EXEC_FAILURE_BASE = 120

# 原因码 → 面板文案。`{release}`/`{unit}` 在快照里填；细节另放 `detail`，
# 由面板作为第二行显示（G-10：版本号一律是完整 release）。
REASON_TEXT = {
    resolve_module.REASON_NO_ARTIFACT: (
        "当前内核 {release} 还没有对应的驱动模块，等发布后点「重新检查」"
    ),
    resolve_module.REASON_NETWORK: "连不上 GitHub，取不到驱动模块；检查网络后重试",
    resolve_module.REASON_BAD_ARTIFACT: "下载到的模块与当前内核不匹配，已丢弃；点「重新下载」",
    "cache": "模块缓存目录写不进去（磁盘满或权限问题）",
    "not_ready": (
        "运行环境还没就绪（程序退出码 1），每 10 秒重试；最近一次原因见下方日志"
    ),
    "system_service": "系统里 {unit} 正在运行，先停掉它",
    "dkms_package": "已安装内核驱动包（DKMS），两者不能同时接管；先卸载该包",
    "udc_busy": "虚拟控制器还在被其他程序占用，停掉它再启用",
    "dummy_hcd": "无法按设置加载 dummy_hcd",
    "in_tree": "无法让 in-tree hid-playstation 让位（解绑/黑名单失败）",
    "insmod": "加载驱动模块失败",
    "unload": "卸载驱动模块失败",
    "daemon_start": "转发程序无法启动，不自动重试",
    "daemon_exit": "转发程序启动后立即退出",
    "daemon_crash": "转发程序连续崩溃，已停止重试",
    "daemon_stuck": "转发程序没有退出，停用中止；开关保持开启",
    "rollback": "回滚失败，系统可能停在半接管状态；详见日志",
}


class Controller:
    """插件后端的状态机；一个后端进程一个实例。"""

    def __init__(
        self,
        system: System,
        layout: Layout,
        *,
        sleep: Callable[[float], Awaitable[None]] | None = None,
        version: str = "",
        log: EventLog | None = None,
    ) -> None:
        """构造状态机；不碰系统，`start()` 才开始。

        Args:
            system: 执行接缝。
            layout: 目录布局。
            sleep: 异步睡眠函数；测试传入立即返回的替身即可让整条流程瞬时跑完。
            version: 随插件发布的转发程序版本（U-08），原样显示。
            log: 事件日志；缺省自建一个（不外发）。
        """
        self._system = system
        self._layout = layout
        self._sleep = sleep or asyncio.sleep
        self._log = log or EventLog()
        self._version = version

        self._settings = settings_module.Settings()
        self._state = STATE_DISABLED
        self._reason = ""
        self._detail = ""
        self._stage = ""
        self._progress = -1
        self._module: dict[str, object] = {}
        self._restarts = 0
        # WAITING 的起点；只在首次进入等待时记下，重试沿用，面板因此显示累计等待。
        # None（而不是 0.0）才是"没在等"的哨兵：虚拟时钟与真实单调时钟都可能从 0 起。
        self._wait_started: float | None = None
        self._retry_kind: str | None = None
        self._retry_at = 0.0

        self._daemon = supervise.Daemon(
            system,
            self._sleep,
            layout,
            self._log,
            self._system.uname_release,
        )
        self._backoff = supervise.Backoff()
        self._abort = asyncio.Event()
        self._flow: asyncio.Task | None = None
        # 改动系统的流程互斥：健康检查的自动重试与用户的手动操作不会同时动内核。
        self._flow_lock = asyncio.Lock()
        self._supervisor: asyncio.Task | None = None
        self._closing = False

    # ------------------------------------------------------------------ 生命周期

    async def start(self) -> None:
        """插件加载时调用：读设置、必要时自动启用、启动健康检查（L-01）。

        自动启用可能包含一次下载与一次接管。Decky 把 `_main()` 当任务排进事件循环、
        不等它结束，所以这里 `await` 第一次启用不会挡住面板的 RPC；等它结束是为了让
        `start()` 返回时状态已经定下来（测试与 `shutdown()` 都按这个语义写）。
        """
        self._settings, warnings = settings_module.load(
            self._system, self._layout.settings_file
        )
        for warning in warnings:
            self._log.warn(warning)
        self._daemon.adopt()
        if self._settings.enabled:
            self._log.info(f"设置里开关是开的，自动启用（内核 {self._system.uname_release()}）")
            await self._spawn_flow(self._enable())
        else:
            self._set_state(STATE_DISABLED, "", "", "")
        self._supervisor = asyncio.ensure_future(self._supervise())

    async def shutdown(self) -> None:
        """插件卸载/重载时调用：停掉健康检查、取消在跑的流程、停用系统侧（L-06）。

        任何一步失败都只记日志：调用方（Decky）不再给我们第二次机会，抛异常只会让
        卸载半途而废。
        """
        self._closing = True
        if self._supervisor is not None:
            self._supervisor.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._supervisor
            self._supervisor = None
        await self._join_flow()
        if self._settings.enabled or self._module:
            with contextlib.suppress(Exception):
                await self._disable()

    # ------------------------------------------------------------------ 对外方法

    async def set_enabled(self, enabled: bool) -> dict[str, object]:
        """主开关（U-01、E-09）。

        Args:
            enabled: 目标状态。

        Returns:
            最终快照；停用失败时开关字段仍为开（D-07）。
        """
        self._settings.enabled = enabled
        self._save_settings()
        # 先让在跑的流程在下一个检查点退出并回滚，再排新的流程。
        self._abort.set()
        await self._join_flow()
        if enabled and self._state == STATE_RUNNING:
            # E-07：已在运行中再次拨到开只做健康检查，不重启程序、不重载模块。
            await self._health_tick()
            return self.snapshot()
        if enabled:
            # 用户亲手重开开关：给崩溃退避一个干净起点（自动重试不经过这里）。
            self._backoff.reset()
            await self._spawn_flow(self._enable())
        else:
            await self._spawn_flow(self._disable())
            if self._state != STATE_DISABLED:
                # D-07：停用没完成，开关保持开，用户再拨一次就能重试。
                self._settings.enabled = True
                self._save_settings()
        return self.snapshot()

    async def recheck(self) -> dict[str, object]:
        """重新检查（U-03）：强制走一遍远端查询。

        Returns:
            最终快照。
        """
        self._abort.set()
        await self._join_flow()
        if self._state == STATE_RUNNING:
            # 运行中不离开运行状态：重新检查只刷新模块文件与健康状态。
            await self._health_tick()
            return self.snapshot()
        if self._settings.enabled:
            await self._spawn_flow(self._enable(force_remote=True))
        else:
            await self._spawn_flow(self._resolve_only(force_remote=True))
        return self.snapshot()

    async def redownload(self) -> dict[str, object]:
        """重新下载当前内核的模块（U-09、M-09）。

        Returns:
            最终快照。运行中调用会被拒绝，`detail` 里说明先停用。
        """
        if self._state == STATE_RUNNING or self._busy():
            await self._set_hint("需要先停用才能重新下载当前内核的模块")
            return self.snapshot()
        self._abort.set()
        await self._join_flow()
        release = self._system.uname_release()
        resolve_module.clean_cache(self._system, self._layout.cache_root, "")
        self._log.info(f"已清空缓存，重新下载 {release}")
        if self._settings.enabled:
            await self._spawn_flow(self._enable(force_remote=True))
        else:
            await self._spawn_flow(self._resolve_only(force_remote=True))
        return self.snapshot()

    async def restart_daemon(self) -> dict[str, object]:
        """只重启转发程序，不碰内核模块（U-05）。

        Returns:
            最终快照；不在运行中时被拒绝。
        """
        if self._busy():
            await self._set_hint("有操作正在进行，等它结束再重启程序")
            return self.snapshot()
        if self._state != STATE_RUNNING:
            await self._set_hint("程序没在运行，无需重启")
            return self.snapshot()
        self._abort.set()
        await self._join_flow()
        await self._spawn_flow(self._restart_daemon())
        return self.snapshot()

    async def clean_cache(self) -> dict[str, object]:
        """删掉当前内核以外的缓存条目（U-06、M-07）。

        Returns:
            最终快照。
        """
        release = self._system.uname_release()
        try:
            removed = resolve_module.clean_cache(
                self._system, self._layout.cache_root, release
            )
        except OSError as error:
            self._log.error(f"清理缓存失败：{error}")
        else:
            self._log.info(
                f"清理缓存：删掉 {', '.join(removed)}" if removed else "清理缓存：没有别的条目"
            )
        return self.snapshot()

    async def get_state(self) -> dict[str, object]:
        """面板轮询用的只读快照（U-01）。"""
        return self.snapshot()

    async def get_logs(self) -> dict[str, object]:
        """日志页（U-07）：程序日志尾部 + 本插件的事件日志。"""
        return {
            "daemon": self._read_daemon_tail(LOG_TAIL_LINES),
            "events": self._log.tail(),
        }

    # ------------------------------------------------------------------ 快照

    def snapshot(self) -> dict[str, object]:
        """拼一份面板要的全部字段。

        Returns:
            可 JSON 序列化的字典。模块归属每次都现读 `/proc/modules`——外部 `rmmod`
            或手动加载 in-tree 之后面板不必等下一次健康检查才改正（L-08）。
        """
        release = self._system.uname_release()
        modules = supervise.loaded_modules(self._system)
        if takeover.CUSTOM_MODULE in modules:
            owner = "custom"
        elif takeover.IN_TREE_MODULE in modules:
            owner = "in_tree"
        else:
            owner = "none"

        status = self._daemon.status()
        now = self._system.now()
        return {
            "state": self._state,
            "reason": self._reason,
            "reason_text": self._reason_text(release),
            "detail": self._detail,
            "stage": self._stage,
            "progress": self._progress,
            "busy": self._busy(),
            "enabled": self._settings.enabled,
            "uname": release,
            "udc_num": self._settings.udc_num,
            "version": self._version,
            "module": {
                "owner": owner,
                "source": self._module.get("source", ""),
                "release": self._module.get("release", ""),
                "path": self._module.get("path", ""),
                "mtime": self._module.get("mtime", 0.0),
            },
            "daemon": {
                "pid": self._daemon.pid if status.running else None,
                "running": status.running,
                "exit_code": status.exit_code,
                "restarts": self._restarts,
                "version": self._version,
            },
            "waited_s": (
                max(0.0, now - self._wait_started)
                if self._state == STATE_WAITING and self._wait_started is not None
                else 0.0
            ),
            "retry_in_s": (
                max(0.0, self._retry_at - now) if self._retry_kind is not None else 0.0
            ),
            "log_last_line": self._last_daemon_line(),
        }

    # ------------------------------------------------------------------ 流程

    async def _enable(self, *, force_remote: bool = False) -> None:
        """启用流程：取模块 → 准备模块 → 让位 in-tree → 加载模块 → 启动程序。

        Args:
            force_remote: 跳过缓存直查远端（重新检查/重新下载用）。
        """
        # 从 WAITING 重试时保留等待起点（面板显示累计等待），其它入口视为一次新的启用。
        if self._state != STATE_WAITING:
            self._clear_retry()
        release = self._system.uname_release()
        self._set_state(STATE_RESOLVING, "", "", "取模块")
        try:
            resolved = await self._resolve(release, force_remote=force_remote)
        except resolve_module.Aborted:
            raise
        except resolve_module.Unavailable as error:
            self._set_state(STATE_UNAVAILABLE, error.reason, error.detail, "")
            return
        except OSError as error:
            self._set_state(STATE_FAILED, "cache", str(error), "")
            return

        self._module = {
            "path": resolved.path,
            "source": resolved.source,
            "release": release,
            "mtime": self._safe_mtime(resolved.path),
        }
        self._log.info(f"模块就绪（{resolved.source}）：{resolved.path}")

        self._set_state(STATE_TAKING_OVER, "", "", "准备模块")
        await self._checkpoint()
        agent = self._make_takeover()
        try:
            await agent.run(resolved.path)
        except takeover.TakeoverFailed as error:
            self._fail(error)
            return

        self._set_state(STATE_TAKING_OVER, "", "", "启动程序")
        try:
            await self._checkpoint()
            started = self._start_daemon()
        except resolve_module.Aborted:
            await self._abort_enable(agent)
            raise
        except OSError as error:
            rollback_errors = await agent.rollback()
            self._fail(takeover.TakeoverFailed("daemon_start", str(error)), rollback_errors)
            return
        if started is None:
            # 认领到已在运行的进程：不用再观察窗口，它早就站住了。
            self._mark_running()
            return

        try:
            alive, code = await self._watch_window()
        except resolve_module.Aborted:
            await self._abort_enable(agent)
            raise
        if alive:
            self._mark_running()
            return
        error_rollback = await agent.rollback()
        self._classify_exit(code, in_window=True, rollback_errors=error_rollback)

    async def _abort_enable(self, agent: takeover.Takeover) -> None:
        """用户中途关开关：停程序、再按反向顺序撤销内核侧改动（E-09、E-06）。

        只在启用的中止路径上调用。这里的失败只记日志：原始原因（用户取消）比回滚
        自身的错误重要，再抛一个异常只会把它盖掉。

        Args:
            agent: 正在执行的接管执行器，回滚栈在它里面。
        """
        self._log.info("启用被取消，回滚到启用前状态")
        try:
            await self._daemon.stop()
        except supervise.StopFailed as error:
            self._log.error(f"回滚时停转发程序失败：{error}")
        self._daemon.forget()
        for error in await agent.rollback():
            self._log.error(f"回滚失败：{error}")

    async def _disable(self, *, restore_in_tree: bool = True) -> None:
        """停用流程（D-01～D-05）。

        Args:
            restore_in_tree: False 用于内核变化后的重来——马上要换一套模块，不必先
                把 in-tree 驱动请回来再赶走。
        """
        self._clear_retry()
        self._set_state(STATE_STOPPING, "", "", "停止程序")
        agent = self._make_takeover()
        try:
            await agent.stop(restore_in_tree=restore_in_tree)
        except takeover.TakeoverFailed as error:
            self._fail(error)
            return
        self._daemon.forget()
        self._module = {}
        self._restarts = 0
        self._backoff.reset()
        self._set_state(STATE_DISABLED, "", "", "")

    async def _resolve_only(self, *, force_remote: bool) -> None:
        """开关关着时的解析：只更新"能不能用"，不动系统。"""
        release = self._system.uname_release()
        self._set_state(STATE_RESOLVING, "", "", "取模块")
        try:
            resolved = await self._resolve(release, force_remote=force_remote)
        except resolve_module.Aborted:
            raise
        except resolve_module.Unavailable as error:
            self._set_state(STATE_UNAVAILABLE, error.reason, error.detail, "")
            return
        except OSError as error:
            self._set_state(STATE_FAILED, "cache", str(error), "")
            return
        self._module = {
            "path": resolved.path,
            "source": resolved.source,
            "release": release,
            "mtime": self._safe_mtime(resolved.path),
        }
        self._set_state(STATE_DISABLED, "", "", "")

    async def _restart_daemon(self) -> None:
        """只重启程序（U-05）：模块保持加载，站重建后新程序重新接管。"""
        self._set_state(STATE_STOPPING, "", "", "停止程序")
        try:
            await self._daemon.stop()
        except supervise.StopFailed as error:
            self._fail(takeover.TakeoverFailed("daemon_stuck", str(error)))
            return
        self._set_state(STATE_TAKING_OVER, "", "", "启动程序")
        try:
            started = self._start_daemon()
        except OSError as error:
            self._fail(takeover.TakeoverFailed("daemon_start", str(error)))
            return
        if started is None:
            self._mark_running()
            return
        alive, code = await self._watch_window()
        if alive:
            self._mark_running()
            return
        self._classify_exit(code, in_window=True, rollback_errors=[])

    # ------------------------------------------------------------------ 健康检查

    async def _supervise(self) -> None:
        """健康检查循环（§4.8）：每 5 s 一次，直到 `shutdown()`。"""
        while not self._closing:
            await self._sleep(supervise.INTERVAL_S)
            if self._closing:
                return
            try:
                await self._health_tick()
            except asyncio.CancelledError:
                raise
            except Exception as error:  # noqa: BLE001 - 循环不能因一次异常退出
                self._log.error(f"健康检查异常：{error!r}")

    async def _health_tick(self) -> None:
        """一次健康检查：先看内核是否换了，再看程序死活与外部改动（L-08）。"""
        if self._closing:
            return
        release = self._system.uname_release()
        recorded = self._module.get("release")
        if recorded and release != recorded:
            self._log.warn(f"内核从 {recorded} 变成 {release}，重新解析模块（L-03）")
            await self._join_flow()
            await self._spawn_flow(self._kernel_changed())
            return

        if (
            self._state == STATE_WAITING
            and self._settings.enabled
            and self._system.now() >= self._retry_at
        ):
            self._log.info("等待到期，重试启用")
            await self._join_flow()
            await self._spawn_flow(self._enable())
            return

        if self._state == STATE_FAILED and self._retry_kind == "crash":
            if not self._settings.enabled:
                return
            if self._system.now() >= self._retry_at:
                self._log.info("崩溃退避到期，重试启用")
                await self._join_flow()
                await self._spawn_flow(self._enable())
            return

        if self._state != STATE_RUNNING:
            return

        status = self._daemon.status()
        if not status.running:
            self._log.warn(f"转发程序退出了（退出码 {status.exit_code}）")
            await self._join_flow()
            await self._spawn_flow(self._on_daemon_exit(status.exit_code))
            return
        await self._repair()

    async def _kernel_changed(self) -> None:
        """内核换了：停用（不恢复 in-tree），再按新内核重来（L-03）。"""
        await self._disable(restore_in_tree=False)
        if self._state == STATE_FAILED:
            return  # 停用就没成功，不能在新内核上继续堆操作
        if self._settings.enabled:
            await self._enable()

    async def _on_daemon_exit(self, code: int | None) -> None:
        """运行中程序退出后的处置（L-05）。

        Args:
            code: 观测到的退出码；认领的进程读不到码时为 None。
        """
        if code == EXIT_NOT_READY:
            self._enter_waiting()
            return
        if code == EXIT_DISABLED_BY_CONFIG:
            # 运行中按用户配置退出（用户把 config.yaml 改成了 enabled: false）：
            # 这不是崩溃，转回停用并把系统还原，和启动窗口内的同码分流一致。
            self._log.info("程序按用户配置退出，转回未启用")
            self._settings.enabled = False
            self._save_settings()
            await self._disable()
            if self._state == STATE_DISABLED:
                self._set_state(
                    STATE_DISABLED,
                    "",
                    "程序按用户配置退出了服务（enabled: false），已转回关闭",
                    "",
                )
            return
        self._schedule_crash(code, "daemon_exit")

    async def _repair(self) -> None:
        """纠正外部改动：模块被卸、黑名单被删、dummy_hcd 被卸（L-08、E-07）。"""
        loaded = supervise.loaded_modules(self._system)
        if takeover.CUSTOM_MODULE not in loaded:
            self._log.warn("定制模块不在内核里了，重新加载")
            module_path = str(self._module.get("path", ""))
            if not module_path:
                self._set_state(
                    STATE_FAILED, "insmod", "模块被外部卸载，且没有缓存的模块路径", ""
                )
                return
            agent = self._make_takeover()
            try:
                await agent.run(module_path)
            except takeover.TakeoverFailed as error:
                self._fail(error)
            return
        if not self._system.exists(takeover.BLACKLIST_PATH):
            self._log.warn("黑名单被删了，重新写一份")
            try:
                self._system.mkdirs("/etc/modprobe.d")
                self._system.write(takeover.BLACKLIST_PATH, takeover.BLACKLIST_TEXT)
            except OSError as error:
                self._set_state(STATE_FAILED, "in_tree", f"重写黑名单失败：{error}", "")
            return
        if not self._system.exists(takeover.DUMMY_HCD_DIR):
            self._log.warn("dummy_hcd 不在了，按设置重新加载")
            try:
                self._make_takeover().load_dummy_hcd()
            except takeover.TakeoverFailed as error:
                self._fail(error)

    # ------------------------------------------------------------------ 窗口与分流

    async def _watch_window(self) -> tuple[bool, int | None]:
        """观察启动窗口（E-05）。

        Returns:
            `(是否站住, 退出码)`。窗口内退出时第一项为 False；退出码可能为 None
            （认领的进程不是本后端的子进程时读不到）。
        """
        deadline = self._system.now() + START_WINDOW_S
        while self._system.now() < deadline:
            await self._checkpoint()
            status = self._daemon.status()
            if not status.running:
                return False, status.exit_code
            await self._sleep(WINDOW_POLL_S)
        status = self._daemon.status()
        return status.running, status.exit_code

    def _classify_exit(
        self, code: int | None, *, in_window: bool, rollback_errors: list[str]
    ) -> None:
        """按退出码分流（E-05、L-05）。

        Args:
            code: 退出码。
            in_window: 是否发生在启动窗口内（决定 `daemon_start` 与 `daemon_exit`
                的区分）。
            rollback_errors: 回滚时自身出错的步骤；非空则整体改报 `rollback`。
        """
        if code == EXIT_DISABLED_BY_CONFIG:
            self._settings.enabled = False
            self._save_settings()
            self._clear_retry()
            self._set_state(
                STATE_DISABLED,
                "",
                "程序判定当前用户不需要接管（用户配置里 enabled: false），已转回关闭",
                "",
            )
            return
        if code == EXIT_NOT_READY:
            self._enter_waiting()
            return
        if code is not None and (code == EXIT_BAD_SNAPSHOT or code >= EXEC_FAILURE_BASE):
            detail = f"退出码 {code}；{self._log_tail(3)}"
            self._fail(
                takeover.TakeoverFailed("daemon_start", detail), rollback_errors
            )
            return
        self._schedule_crash(code, "daemon_start" if in_window else "daemon_exit", rollback_errors)

    def _schedule_crash(
        self, code: int | None, reason: str, rollback_errors: list[str] | None = None
    ) -> None:
        """记一次崩溃并排下一次重试（§4.8）。

        Args:
            code: 退出码，展示用。
            reason: `daemon_exit`（运行中退出）或 `daemon_start`（窗口内退出）。
            rollback_errors: 回滚失败项；非空时改报 `rollback`。
        """
        delay = self._backoff.record()
        self._restarts += 1
        detail = f"退出码 {code}；{self._last_daemon_line()}"
        if delay is None:
            self._fail(
                takeover.TakeoverFailed(
                    "daemon_crash", f"连续崩溃 {self._backoff.failures} 次，停止重试；{detail}"
                ),
                rollback_errors or [],
            )
            return
        self._fail(takeover.TakeoverFailed(reason, detail), rollback_errors or [])
        if self._reason != "rollback":
            self._retry_kind = "crash"
            self._retry_at = self._system.now() + delay
            self._log.info(f"{delay:g}s 后重试启用（第 {self._backoff.failures} 次崩溃）")

    # ------------------------------------------------------------------ 内部工具

    async def _resolve(self, release: str, *, force_remote: bool) -> resolve_module.Resolved:
        """在线程里跑解析（阻塞的下载不该占住事件循环）。

        Args:
            release: 目标内核 release。
            force_remote: 跳过缓存。

        Returns:
            解析结果。

        Raises:
            resolve_module.Unavailable: 取不到。
            resolve_module.Aborted: 用户关了开关。
            OSError: 缓存目录写不进。
        """
        self._progress = -1

        def cancel() -> None:
            if self._abort.is_set():
                raise resolve_module.Aborted()

        def progress(done: int, total: int) -> None:
            # 在下载线程里写：int 赋值在 GIL 下是原子的，面板最多读到一个稍旧的百分比。
            self._progress = int(done * 100 / total) if total else -1

        return await asyncio.to_thread(
            resolve_module.resolve,
            self._system,
            self._layout.cache_root,
            release,
            force_remote=force_remote,
            cancel=cancel,
            progress=progress,
        )

    def _start_daemon(self) -> int | None:
        """启动程序；已有同名进程在跑就认领它而不是再起一个。

        Returns:
            新进程 pid；认领到已有进程时为 None。

        Raises:
            OSError: 二进制不存在、执行位补不上或日志写不了。
        """
        if self._daemon.pid is None:
            self._daemon.adopt()
        if self._daemon.status().running:
            self._log.info("转发程序已在运行，接管现有进程")
            return None
        return self._daemon.start()

    def _make_takeover(self) -> takeover.Takeover:
        """按当前设置构造一次接管执行器。"""
        return takeover.Takeover(
            self._system,
            self._daemon,
            self._sleep,
            self._log,
            self._settings.udc_num,
        )

    async def _spawn_flow(self, flow: Awaitable[None]) -> None:
        """排一个改动系统的流程并等它结束（同一时刻只有一个）。

        互斥靠 `_flow_lock`：健康检查的自动重试与用户的 `set_enabled` 可能同时想排
        流程，锁把"轮到谁"排成队而不是两套动作叠在内核上。`_abort.clear()` 必须在
        拿到锁之后：前面那次 `set_enabled` 刚置位的中止位不能带进新流程。

        Args:
            flow: 流程协程（`_enable`/`_disable` 等）。
        """
        async with self._flow_lock:
            self._abort.clear()
            task = asyncio.ensure_future(flow)
            self._flow = task
            try:
                await task
            except resolve_module.Aborted:
                self._log.info("流程被用户取消，已回滚")
            finally:
                if self._flow is task:
                    self._flow = None

    async def _join_flow(self) -> None:
        """等在跑的流程结束（含它的回滚）。"""
        task = self._flow
        if task is None or task.done():
            return
        with contextlib.suppress(resolve_module.Aborted, asyncio.CancelledError):
            await task

    async def _checkpoint(self) -> None:
        """取消检查点：用户关掉开关就在这里退出（E-09）。

        Raises:
            resolve_module.Aborted: 已请求取消。
        """
        if self._abort.is_set():
            raise resolve_module.Aborted()
        await self._sleep(0)

    def _fail(
        self, error: takeover.TakeoverFailed, rollback_errors: list[str] | None = None
    ) -> None:
        """把失败写成快照；回滚不干净时改报 `rollback`（§4.6）。

        Args:
            error: 失败点。
            rollback_errors: 额外的回滚失败项；与 `error.rollback_errors` 合并。
        """
        self._wait_started = None
        errors = list(error.rollback_errors)
        if rollback_errors:
            errors.extend(rollback_errors)
        if errors:
            self._set_state(
                STATE_FAILED,
                "rollback",
                f"{error.detail}；回滚失败：{'；'.join(errors)}",
                "",
            )
            return
        self._set_state(STATE_FAILED, error.reason, error.detail, "")

    def _enter_waiting(self) -> None:
        """进入 WAITING（E-05）：首次进入记下起点，重试沿用，面板显示累计等待。

        等待的原因固定是程序退出码 1（运行条件暂不具备），`detail` 取程序日志最后
        一行——环境不就绪的真实原因（没有图形会话、UDC 不可用）都在那里。
        """
        now = self._system.now()
        if self._wait_started is None:
            self._wait_started = now
        self._set_state(STATE_WAITING, "not_ready", self._last_daemon_line(), "")
        self._retry_kind = "not_ready"
        self._retry_at = now + NOT_READY_RETRY_S

    def _set_state(self, state: str, reason: str, detail: str, stage: str) -> None:
        """写快照字段并记一行迁移日志。

        Args:
            state: 八种状态之一。
            reason: 原因码；空串表示没有异常。
            detail: 面向用户的补充；空串表示无须补充。
            stage: 阶段名（U-04），只在进行中显示。
        """
        previous = self._state
        self._state = state
        self._reason = reason
        self._detail = detail
        self._stage = stage
        if stage:
            self._progress = -1
        if previous != state or reason:
            self._log.info(f"状态 {previous} → {state}" + (f"（{reason}）" if reason else ""))

    def _mark_running(self) -> None:
        """进入 RUNNING 并清掉重试计数。"""
        self._clear_retry()
        self._backoff.reset()
        self._set_state(STATE_RUNNING, "", "", "")

    def _clear_retry(self) -> None:
        """清掉"到点重试"的排期与等待计时（进入稳定状态或用户手动操作时）。"""
        self._retry_kind = None
        self._retry_at = 0.0
        self._wait_started = None

    def _busy(self) -> bool:
        """是否有流程在跑；面板据此禁用按钮（U-04、U-09）。"""
        return self._flow is not None and not self._flow.done()

    async def _set_hint(self, hint: str) -> None:
        """拒绝某操作时把原因写进 `detail`，不改变状态。"""
        self._detail = hint
        self._log.warn(hint)
        await self._sleep(0)

    def _save_settings(self) -> None:
        """写回设置；失败只记日志（开关本身仍按内存值继续本次操作）。"""
        try:
            settings_module.save(
                self._system, self._layout.settings_file, self._settings
            )
        except OSError as error:
            self._log.error(f"保存设置失败（重启后开关会回到旧值）：{error}")

    def _reason_text(self, release: str) -> str:
        """原因码对应的中文句子（§5）。"""
        template = REASON_TEXT.get(self._reason)
        if template is None:
            return ""
        return template.format(release=release, unit=takeover.SYSTEM_UNIT)

    def _read_daemon_tail(self, lines: int) -> list[str]:
        """读程序日志的尾部若干行；读不到返回空列表。"""
        try:
            text = self._system.read(self._layout.daemon_log)
        except OSError:
            return []
        return text.splitlines()[-lines:]

    def _log_tail(self, lines: int) -> str:
        """把程序日志最后几行压成一行，供快照的 `detail` 使用。"""
        tail: Iterable[str] = self._read_daemon_tail(lines)
        return " / ".join(line for line in tail if line.strip())

    def _last_daemon_line(self) -> str:
        """程序日志里最后一条非空行（E-05 的"最近一次原因"）。"""
        for line in reversed(self._read_daemon_tail(LOG_LAST_LINES)):
            if line.strip():
                return line.strip()
        return ""

    def _safe_mtime(self, path: str) -> float:
        """取修改时间；取不到返回 0（面板显示为空）。"""
        try:
            return self._system.stat_mtime(path)
        except OSError:
            return 0.0
