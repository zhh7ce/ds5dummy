#!/usr/bin/env python3
"""程序侧守护：起停转发程序、崩溃退避、以及 `/proc/modules` 这类只看不动的探针。

与 `takeover.py` 的分界：那个模块动内核（dummy_hcd、解绑、insmod/rmmod、黑名单、
in-tree 驱动），本模块只动转发程序这个进程，并且只读地看内核模块状态。两者都被
`state.py` 按顺序调用，彼此不调用。

**pid 要落盘**（L-02）：Decky 重载插件时后端进程会被换掉，而转发程序（及其虚拟 USB
站）还在跑。新后端若不认领这个进程，第二次启动会去抢已经被占用的 UDC，失败得莫名
其妙。所以启动时把 pid 写进数据目录，下次加载先认领；认领还要核对
`/proc/<pid>/exe` 指向本插件自带的二进制——pid 会被系统复用，只看"活着"不够。
"""

from __future__ import annotations

import json
import os
import signal
from typing import Awaitable, Callable, NamedTuple

from . import system as system_module
from .layout import Layout
from .log import EventLog

PROC_MODULES = "/proc/modules"

# 健康检查的节拍（§4.8）。它同时是"外部改动被发现"的最坏延迟（L-08）。
INTERVAL_S = 5.0

# D-01：SIGTERM 之后等程序拆完站。程序自己拆一个站要停 worker、拆 gadget、卸挂载，
# 5 s 是给"有手柄正在连"的情形留的余量。
STOP_GRACE_S = 5.0
# 强杀之后再等一小段，让内核完成回收；仍不退出就是僵死（D-07）。
KILL_GRACE_S = 1.0
# 等待循环的轮询步长：既够及时，又不会把事件循环刷满。
POLL_S = 0.1

# 崩溃退避阶梯（§4.8）：崩溃第 k 次后等 DELAYS_S[k-1] 秒再拉起。
DELAYS_S = (2.0, 4.0, 8.0, 16.0, 30.0)
# 连续崩溃上限：用完这 5 档退避仍站不住，就停止重试并报 FAILED(daemon_crash)。
CRASH_LIMIT = 5


class StopFailed(Exception):
    """程序在 SIGKILL 之后仍未退出；`state.py` 把它翻成 `daemon_stuck`（D-07）。"""


class Status(NamedTuple):
    """程序此刻的生死与最近一次退出码。

    Attributes:
        running: 进程是否还在。
        exit_code: 本次观测到的退出码；仍在跑时为 None。被信号杀掉记为负的信号号
            （如 `-15`）；认领的进程不是本后端的子进程时，死后也读不到码，同样是
            None。
    """

    running: bool
    exit_code: int | None


def loaded_modules(system: system_module.System) -> set[str]:
    """读 `/proc/modules` 的模块名集合。

    Args:
        system: 执行接缝。

    Returns:
        第一列（模块名）的集合；`/proc` 读不到时返回空集——之后的判断会按"没加载"
        处理，而下一步的 `insmod` 会把真实错误暴露出来。
    """
    try:
        text = system.read(PROC_MODULES)
    except OSError:
        return set()
    return {
        line.split(" ", 1)[0]
        for line in text.splitlines()
        if line.strip()
    }


def module_loaded(system: system_module.System, name: str) -> bool:
    """单个模块是否已在内核里。

    Args:
        system: 执行接缝。
        name: 模块名（不带 `.ko`、下划线原样）。

    Returns:
        是否加载。
    """
    return name in loaded_modules(system)


class Backoff:
    """崩溃重试的计数与等待时长（§4.8）。

    语义：每一次崩溃（包括启动窗口内以非 0/1 退出）调用一次 `record()`，得到"下次拉
    起前等多少秒"；`record()` 返回 None 表示已经用完 5 档退避，不再重试。计数只在
    程序成功站住（进入 RUNNING）或用户手动操作时经 `reset()` 清零，所以"反复在几秒
    内崩溃"会一路走到上限，而不是被一次成功尝试蒙混过去。
    """

    def __init__(self) -> None:
        """从零开始计数。"""
        self.failures = 0

    def reset(self) -> None:
        """清零（程序已站住，或用户手动重启/重开开关）。"""
        self.failures = 0

    def record(self) -> float | None:
        """记一次崩溃。

        Returns:
            下次拉起前的等待秒数（2/4/8/16/30）；已经连续崩溃 `CRASH_LIMIT` 次时
            返回 None，调用方应停在 `daemon_crash`。
        """
        self.failures += 1
        if self.failures > CRASH_LIMIT:
            return None
        return DELAYS_S[min(self.failures - 1, len(DELAYS_S) - 1)]


class Daemon:
    """转发程序这个进程的起停与观察。

    Attributes:
        exit_code: 最近一次观测到的退出码；还没死过时为 None。
        pid: 当前进程 pid；没在跑时为 None。
    """

    def __init__(
        self,
        system: system_module.System,
        sleep: Callable[[float], Awaitable[None]],
        layout: Layout,
        log: EventLog,
        release: Callable[[], str],
    ) -> None:
        """构造守护对象；不启动任何东西。

        Args:
            system: 执行接缝。
            sleep: 异步睡眠函数（`asyncio.sleep` 或测试替身），签名 `(秒) -> awaitable`。
            layout: 插件布局，提供二进制、日志与 pid 文件路径。
            log: 事件日志。
            release: 返回当前内核 release 的回调，写进 pid 文件便于事后对账。
        """
        self._system = system
        self._sleep = sleep
        self._layout = layout
        self._log = log
        self._release = release
        self.pid: int | None = None
        self.exit_code: int | None = None

    def adopt(self) -> int | None:
        """认领上一次后端留下的、仍在跑的转发程序（L-02）。

        Returns:
            pid；没有 pid 文件、进程已死、或 `/proc/<pid>/exe` 不是本插件的二进制
            （pid 被复用）时返回 None，此时 pid 文件也会被删掉。
        """
        try:
            record = json.loads(self._system.read(self._layout.pid_file))
        except (OSError, json.JSONDecodeError):
            return None
        pid = record.get("pid") if isinstance(record, dict) else None
        if not isinstance(pid, int) or not self._system.alive(pid):
            self.forget()
            return None
        if not self._exe_matches(pid):
            self.forget()
            return None
        self.pid = pid
        self.exit_code = None
        self._log.info(f"认领已在运行的转发程序 pid={pid}")
        return pid

    def start(self) -> int:
        """启动转发程序并写下 pid 文件。

        Returns:
            新进程的 pid。

        Raises:
            OSError: 二进制不存在、执行位补不上，或日志写不了（由 `System` 抛出）。
        """
        # 执行位不能指望 ZIP 里带着：Decky 的安装路径（`zipfile.extractall`）不恢复
        # mode，所以每次起进程前补一次。
        self._system.make_executable(self._layout.daemon_bin)
        pid = self._system.spawn(
            [self._layout.daemon_bin], self._layout.plugin_dir, self._layout.daemon_log
        )
        self.pid = pid
        self.exit_code = None
        record = {
            "pid": pid,
            "exe": self._layout.daemon_bin,
            "release": self._release(),
            "started_at": self._system.wall(),
        }
        self._system.mkdirs(self._layout.data_dir)
        self._system.write(
            self._layout.pid_file, json.dumps(record, ensure_ascii=False) + "\n"
        )
        self._log.info(f"启动转发程序 pid={pid}")
        return pid

    def status(self) -> Status:
        """非阻塞看一次生死。

        Returns:
            当前状态；进程已死时把退出码记进 `exit_code` 并删掉 pid 文件，因此同一
            次死亡只会被报告一次退出码。
        """
        if self.pid is None:
            return Status(False, self.exit_code)
        code = self._system.wait(self.pid)
        if code is not None:
            self._note_exit(code)
            return Status(False, code)
        if not self._system.alive(self.pid):
            # 不是我们的子进程（后端重启后认领的），退码无从得知。
            self._note_exit(None)
            return Status(False, None)
        return Status(True, None)

    async def stop(self) -> None:
        """终止转发程序：SIGTERM → 等 `STOP_GRACE_S` → SIGKILL → 再等一小段。

        Raises:
            StopFailed: SIGKILL 之后仍在（D-07 的僵死情形）。
        """
        pid = self.pid if self.pid is not None else self.adopt()
        if pid is None:
            return
        code = self._system.wait(pid)
        if code is not None:
            self._note_exit(code)
            return
        if not self._system.alive(pid):
            self._note_exit(None)
            return

        self._system.kill(pid, signal.SIGTERM)
        if await self._wait_gone(pid, STOP_GRACE_S):
            return
        self._log.warn(f"pid={pid} 在 {STOP_GRACE_S:g}s 内没有退出，改发 SIGKILL")
        self._system.kill(pid, signal.SIGKILL)
        if await self._wait_gone(pid, KILL_GRACE_S):
            return
        raise StopFailed(f"pid {pid} 在 SIGKILL 之后仍未退出")

    def forget(self) -> None:
        """丢掉 pid 记录（进程已死或确认不是我们的）。不碰进程本身。"""
        self.pid = None
        self._system.remove(self._layout.pid_file)

    async def _wait_gone(self, pid: int, timeout: float) -> bool:
        """轮询等进程消失。

        Args:
            pid: 目标进程。
            timeout: 最多等多少秒。

        Returns:
            消失（或被回收）为 True。
        """
        deadline = self._system.now() + timeout
        while self._system.now() < deadline:
            code = self._system.wait(pid)
            if code is not None:
                self._note_exit(code)
                return True
            if not self._system.alive(pid):
                self._note_exit(None)
                return True
            await self._sleep(POLL_S)
        return False

    def _note_exit(self, code: int | None) -> None:
        """记下死亡：保留退出码、清 pid、删 pid 文件。"""
        if self.exit_code is None:
            self.exit_code = code
        self.pid = None
        self._system.remove(self._layout.pid_file)

    def _exe_matches(self, pid: int) -> bool:
        """`/proc/<pid>/exe` 是否指向本插件的二进制。

        Args:
            pid: 目标进程。

        Returns:
            指向本插件二进制为 True；读不到链接（权限、进程刚好退出）时退回
            "进程还活着"，不因此否定认领。
        """
        try:
            target = self._system.readlink(f"/proc/{pid}/exe")
        except OSError:
            return True
        return os.path.realpath(target) == os.path.realpath(self._layout.daemon_bin)
