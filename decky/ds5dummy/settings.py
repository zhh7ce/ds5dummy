#!/usr/bin/env python3
"""插件设置：启用状态与虚拟 UDC 数量（C-05、L-02、E-02）。

设置只有两项，都放在 Decky 的设置目录里一个 JSON 文件：

- `enabled`：用户拨的主开关。它必须持久化——重启 Deck、注销、Decky 重载之后都要
  保持用户最后一次的选择（L-02），后端开机时的 reconcile 也只看它（L-01）。
- `udc_num`：`dummy_hcd` 的 `num` 参数，也就是同时能服务的手柄数上限，1..8，缺省 3
  （E-02）。

读取**不因坏文件失败**：设置读不出来时按缺省值继续并把原因交给调用方记日志。插件
加载失败会让整个面板消失，这比"开关回到了默认关"严重得多；用户仍能在面板上重新
拨开关。
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass

from . import system as system_module

UDC_MIN = 1
UDC_MAX = 8
UDC_DEFAULT = 3


@dataclass
class Settings:
    """持久化的两项设置。

    Attributes:
        enabled: 主开关的持久值；True 时后端开机就执行一次启用流程（L-01）。
        udc_num: `dummy_hcd num`，取值范围 `[UDC_MIN, UDC_MAX]`；越界值在读取时
            夹到边界，不静默改用缺省。
    """

    enabled: bool = False
    udc_num: int = UDC_DEFAULT

    def to_json(self) -> dict[str, object]:
        """转成写盘用的字典；键名就是文件里的键名。"""
        return {"enabled": self.enabled, "udc_num": self.udc_num}


def parse(raw: object) -> tuple[Settings, list[str]]:
    """把 JSON 解出的对象收成设置，坏字段按缺省值处理。

    Args:
        raw: `json.loads` 的结果。不是字典时整体按缺省值处理。

    Returns:
        `(settings, warnings)`。`warnings` 是给日志看的中文原因，空列表表示文件
        完全合规。
    """
    settings = Settings()
    warnings: list[str] = []
    if not isinstance(raw, dict):
        warnings.append("设置文件不是 JSON 对象，按缺省值继续")
        return settings, warnings

    enabled = raw.get("enabled")
    if isinstance(enabled, bool):
        settings.enabled = enabled
    elif enabled is not None:
        warnings.append(f"enabled 不是布尔值（{enabled!r}），按关闭处理")

    udc = raw.get("udc_num")
    if isinstance(udc, int) and not isinstance(udc, bool):
        clamped = min(max(udc, UDC_MIN), UDC_MAX)
        if clamped != udc:
            warnings.append(
                f"udc_num={udc} 超出 [{UDC_MIN}, {UDC_MAX}]，改用 {clamped}"
            )
        settings.udc_num = clamped
    elif udc is not None:
        warnings.append(f"udc_num 不是整数（{udc!r}），用缺省值 {UDC_DEFAULT}")
    return settings, warnings


def load(system: system_module.System, path: str) -> tuple[Settings, list[str]]:
    """读设置；文件缺失或解析不了都按缺省值返回。

    Args:
        system: 执行接缝。
        path: 设置文件路径。

    Returns:
        与 `parse` 相同；文件缺失时不产生 warning（首次安装的正常状态）。
    """
    try:
        text = system.read(path)
    except OSError:
        return Settings(), []
    try:
        raw = json.loads(text)
    except json.JSONDecodeError as error:
        return Settings(), [f"设置文件不是合法 JSON（{error}），按缺省值继续"]
    return parse(raw)


def save(system: system_module.System, path: str, settings: Settings) -> None:
    """原子写回设置：先写 `.tmp` 再改名，断电不会留下半份 JSON。

    Args:
        system: 执行接缝。
        path: 设置文件路径；父目录由本函数建出。
        settings: 要保存的值。

    Raises:
        OSError: 目录建不出或写不进去（磁盘满、权限）。调用方应把这个错误暴露给
            用户——开关状态没存住，重启后会回到旧值。
    """
    directory = os.path.dirname(path) or "."
    system.mkdirs(directory)
    tmp = path + ".tmp"
    system.write(tmp, json.dumps(settings.to_json(), ensure_ascii=False) + "\n")
    system.replace(tmp, path)
