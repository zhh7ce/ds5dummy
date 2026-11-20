#!/usr/bin/env python3
"""插件自己的事件日志：G-09 要求每一步外部动作都留痕，U-07 要求日志可查看。

日志有两个去处：Decky 的进程日志（`decky.logger`，装到后端时由 `main.py` 接上），
以及内存里的环形缓冲。环形缓冲不是重复劳动——面板的「查看日志」要的是**本插件**
做过什么，而 decky-loader 的日志里混着所有插件；`get_logs()` 返回缓冲内容，程序
自己的 stdout/stderr 另从 `daemon.log` 读尾部。

本模块刻意不 import decky：状态机与测试都不该依赖 loader 的注入。
"""

from __future__ import annotations

import time
from collections import deque
from typing import Callable, Iterable

CAPACITY = 200

# 一行日志里允许出现的最小结构：时间 + 级别 + 文本。级别只用三个词，面板按它过滤。
LEVEL_INFO = "info"
LEVEL_WARN = "warn"
LEVEL_ERROR = "error"


class EventLog:
    """带容量上限的事件日志，可选转发给外部 sink（Decky 的 `logger`）。

    Attributes:
        lines: 最近 `capacity` 行，旧的先出。元素形如 `"09:12:31 warn 文本"`。
    """

    def __init__(
        self,
        sink: Callable[[str, str], None] | None = None,
        capacity: int = CAPACITY,
    ) -> None:
        """构造日志。

        Args:
            sink: 每次写入时同步调用的回调，签名为 `(level, message)`；通常接到
                `decky.logger` 上。为 None 时只留在内存里。
            capacity: 环形缓冲的行数上限。
        """
        self._lines: deque[str] = deque(maxlen=capacity)
        self._sink = sink

    def info(self, message: str) -> None:
        """记录一条普通进度（状态迁移、解析结果）。"""
        self.write(LEVEL_INFO, message)

    def warn(self, message: str) -> None:
        """记录一条可继续的异常（缓存条目坏掉、配置取值越界）。"""
        self.write(LEVEL_WARN, message)

    def error(self, message: str) -> None:
        """记录一条失败（接管某步失败、程序异常退出）。"""
        self.write(LEVEL_ERROR, message)

    def write(self, level: str, message: str) -> None:
        """记一行；同时写环形缓冲与 sink。

        Args:
            level: `info`/`warn`/`error` 三选一，原样落进行首。
            message: 正文，允许中文；不做换行，多行文本会被压成多行记录。
        """
        for line in str(message).splitlines() or [""]:
            self._lines.append(f"{time.strftime('%H:%M:%S')} {level} {line}")
        if self._sink is not None:
            self._sink(level, str(message))

    def tail(self, count: int = CAPACITY) -> list[str]:
        """取最近的若干行，旧的在前。

        Args:
            count: 最多返回多少行；超过容量时返回全部。

        Returns:
            新列表，调用方可以随意修改。
        """
        lines: Iterable[str] = self._lines
        if count < len(self._lines):
            lines = list(self._lines)[-count:]
        return list(lines)
