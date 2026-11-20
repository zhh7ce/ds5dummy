#!/usr/bin/env python3
"""Decky 插件后端的内核：布局、解析、接管、守护、状态机。

对外只用 `Controller`（`state.py`）：`main.py` 把 Decky 的 RPC 名字接到它的方法上，
测试直接构造它并注入 `FakeSystem`。其余模块都是它的零件，不承诺对外稳定。
"""

from .layout import Layout, from_environment
from .state import Controller

__all__ = ["Controller", "Layout", "from_environment"]
