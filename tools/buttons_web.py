#!/usr/bin/env python3
"""DS5Dummy 用户配置的本地页面：回环 HTTP 服务 + 可拓展的面板包。

工具有两层：

- **壳**（`tools/buttons_web.html` + 本文件）：读写一份用户配置、算哈希、原子
  替换、整份校验，并把每个面板的 HTML 拼成一张页面。壳不认识任何插件。
- **面板**：一个 `*.panel.js` 声明它管哪个插件、渲染自己的 UI、把草稿交回壳。
  基础配置与当前状态是随工具走的 `core` 面板；插件面板与 `.so` 放在同一目录，
  文件名为 `<插件名>.panel.js`，服务按宿主的装载顺序（系统目录在前、用户目录
  覆盖）扫描，只有同名的 `.so` 存在时才显示。没有面板的插件落到通用键值面板。

写回是"只换这一段"：被编辑的节之外逐字节不动。插件节按面板交回的模型或扁平
键值重新生成，所以节内注释不保留；基础配置按单个键就地替换，行尾注释保留。
一份配置是否有效，最终由 `buttons_check` 调用的 daemon 解析器说了算，服务不
另写一套取值规则。

安全前提是它只监听 127.0.0.1：地址固定为 `http://127.0.0.1:8777/`，POST 校验
Host/Origin 与 Content-Type。它不是多用户服务，同一时间只允许一个保存。

用法: python3 tools/buttons_web.py [--config PATH] [--port N] [--no-open]
                                 [--check PATH] [--plugins-dir DIR]... [--verbose]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import webbrowser
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import yaml
from yaml.nodes import MappingNode, ScalarNode, SequenceNode

# ---------------------------------------------------------------------------
# 目录：与 plugin/buttons/ids.cpp 的 token 表同序、同拼写
# ---------------------------------------------------------------------------

#: 可绑定源，顺序就是 ids::Source 的枚举值。`kind` 只决定页面上的分组。
SOURCE_ROWS = [
    ("dpad_up", "十字键上", "dpad"),
    ("dpad_right", "十字键右", "dpad"),
    ("dpad_down", "十字键下", "dpad"),
    ("dpad_left", "十字键左", "dpad"),
    ("triangle", "△", "face"),
    ("circle", "○", "face"),
    ("cross", "✕", "face"),
    ("square", "□", "face"),
    ("l1", "L1", "shoulder"),
    ("r1", "R1", "shoulder"),
    ("l2", "L2（数字位）", "shoulder"),
    ("r2", "R2（数字位）", "shoulder"),
    ("create", "Create", "system"),
    ("options", "Options", "system"),
    ("l3", "L3（左摇杆按下）", "system"),
    ("r3", "R3（右摇杆按下）", "system"),
    ("home", "Home / PS", "system"),
    ("pad", "触摸板按下", "system"),
    ("mute", "Mute", "system"),
    ("lfn", "左 Fn（Edge）", "edge"),
    ("rfn", "右 Fn（Edge）", "edge"),
    ("lpaddle", "左背键（Edge）", "edge"),
    ("rpaddle", "右背键（Edge）", "edge"),
    ("lstick_up", "左摇杆上", "stick"),
    ("lstick_right", "左摇杆右", "stick"),
    ("lstick_down", "左摇杆下", "stick"),
    ("lstick_left", "左摇杆左", "stick"),
    ("rstick_up", "右摇杆上", "stick"),
    ("rstick_right", "右摇杆右", "stick"),
    ("rstick_down", "右摇杆下", "stick"),
    ("rstick_left", "右摇杆左", "stick"),
    ("touch_left", "触摸板左半触点", "touch"),
    ("touch_right", "触摸板右半触点", "touch"),
    ("touch_two", "两指触摸", "touch"),
    ("pad_left", "左半点击", "touch"),
    ("pad_right", "右半点击", "touch"),
    ("swipe_up", "上划", "swipe"),
    ("swipe_right", "右划", "swipe"),
    ("swipe_down", "下划", "swipe"),
    ("swipe_left", "左划", "swipe"),
]

#: 六根轴，顺序就是 ids::Axis 的枚举值。
AXIS_ROWS = [
    ("lstick_x", "左摇杆 X"),
    ("lstick_y", "左摇杆 Y"),
    ("rstick_x", "右摇杆 X"),
    ("rstick_y", "右摇杆 Y"),
    ("ltrigger", "左扳机行程"),
    ("rtrigger", "右扳机行程"),
]

#: 整根摇杆（`center_` 的粒度）。
STICK_ROWS = [("lstick", "左摇杆"), ("rstick", "右摇杆")]

SOURCE_TOKENS = [row[0] for row in SOURCE_ROWS]
AXIS_TOKENS = [row[0] for row in AXIS_ROWS]
STICK_TOKENS = [row[0] for row in STICK_ROWS]

#: 线上有位、能当输出的那 23 个；其余都是判出来的形态。
WIRE_TOKENS = SOURCE_TOKENS[:23]
#: 可以用 `turbo.` 连点的键：线上位里排除两个扳机与触摸板。
TURBO_TOKENS = [token for token in WIRE_TOKENS if token not in ("l2", "r2", "pad")]
#: 划动是一次性事件，不能当映射源。
REMAP_SOURCE_TOKENS = SOURCE_TOKENS[:36]

TIMINGS = [
    {"id": "chord", "label": "同时按住（无后缀）"},
    {"id": "tap", "label": "单击 ~"},
    {"id": "double", "label": "双击 *2"},
    {"id": "long", "label": "长按 >毫秒"},
]

TURBO_MIN_HZ = 1
TURBO_MAX_HZ = 60
LONG_MIN_MS = 50
LONG_MAX_MS = 10000
DEFAULT_TAP_MS = 300
DEFAULT_DOUBLE_MS = 300
MACRO_PULSE_MS = 80

GROUP_LABELS = {
    "dpad": "十字键",
    "face": "面板键",
    "shoulder": "肩键与扳机",
    "system": "系统键",
    "edge": "Edge 专属",
    "stick": "摇杆方向（判定）",
    "touch": "触摸板（判定）",
    "swipe": "划动（判定）",
}

#: 顶层功能组的固定顺序；`其他` 里的未知组按出现顺序排在后面。
GROUP_ORDER = ["remap", "turbo", "shortcut", "macro", "axis"]

#: 一段 `buttons` 子树里最多允许的行数与单值长度；页面不是通用 YAML 编辑器。
MAX_ITEMS = 2000
MAX_VALUE = 4096
MAX_CONFIG_BYTES = 1 << 20
#: 一次保存最多几项修改；基础配置一页也就几十个键。
MAX_EDITS = 256
#: 面板脚本的大小上限；它是随插件发布的一份静态资源，不是数据文件。
MAX_PANEL_BYTES = 1 << 20

TOKEN_RE = re.compile(r"[A-Za-z0-9_-]{1,32}\Z")
DECIMAL_RE = re.compile(r"[0-9]+\Z")
#: 配置里的键路径每段能有的形态：基本键与带冒号的 MAC。
KEY_RE = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")
#: `controllers` 的条目名；页面新加条目时按这个形态收口。
MAC_RE = re.compile(r"[0-9A-F]{2}(?::[0-9A-F]{2}){5}\Z")
#: 页面地址固定在这个端口：页面就是本机的一个固定入口，不是一次性的链接。
DEFAULT_PORT = 8777


def catalog() -> dict:
    """页面要的全部词表与区间；页面自己不重复一份。"""
    sources = []
    for index, (token, label, kind) in enumerate(SOURCE_ROWS):
        sources.append(
            {
                "token": token,
                "label": label,
                "kind": kind,
                "group": GROUP_LABELS[kind],
                "wire": token in WIRE_TOKENS,
                "stick": kind == "stick",
                "touch": kind == "touch",
                "swipe": kind == "swipe",
                "turbo": token in TURBO_TOKENS,
                "remap_source": token in REMAP_SOURCE_TOKENS,
            }
        )
    return {
        "sources": sources,
        "axes": [{"token": token, "label": label} for token, label in AXIS_ROWS],
        "sticks": [{"token": token, "label": label} for token, label in STICK_ROWS],
        "timings": TIMINGS,
        "ranges": {
            "turbo": [TURBO_MIN_HZ, TURBO_MAX_HZ],
            "long_ms": [LONG_MIN_MS, LONG_MAX_MS],
            "default_tap_ms": DEFAULT_TAP_MS,
            "default_double_ms": DEFAULT_DOUBLE_MS,
            "macro_pulse_ms": MACRO_PULSE_MS,
        },
        "restart": "sudo systemctl restart ds5dummy",
    }


# ---------------------------------------------------------------------------
# 按行定位一条 YAML 路径（PyYAML 只用来取行号）
# ---------------------------------------------------------------------------


class LayerError(Exception):
    """这一层定位不了，或定位到了但不可编辑；消息直接给页面。"""


@dataclass
class Analysis:
    """一条 YAML 路径在原文里的几何：已存在时是行范围，不存在时是插入点。"""

    present: bool = False
    editable: bool = True
    reason: str = ""
    key_line: int = -1
    key_indent: int = -1
    key_end: int = -1
    child_indent: int = -1
    end_line: int = -1
    has_children: bool = False
    insert_at: int = -1
    insert_indent: int = 0
    missing_keys: list[str] = field(default_factory=list)
    #: 路径末段是标量时，它在原文里的取值区间（行内、不含引号）与引号形态。
    scalar_start: int = -1
    scalar_end: int = -1
    scalar_style: str | None = None


def split_document(text: str) -> tuple[list[str], bool]:
    """按 '\n' 切行；行尾的 '\r' 原样留在行里，并记住末尾有没有换行。"""
    if text == "":
        return [], False
    trailing = text.endswith("\n")
    lines = text.split("\n")
    if trailing:
        lines.pop()
    return lines, trailing


def join_document(lines: list[str], trailing_newline: bool) -> str:
    """把行拼回整份文本。"""
    text = "\n".join(lines)
    if trailing_newline:
        text += "\n"
    return text


def indent_of(line: str) -> int:
    """行首空格数；制表符按缩进 0 算。"""
    return len(line) - len(line.lstrip(" "))


def is_blank(line: str) -> bool:
    """整行只有空格（或为空）。"""
    return line.strip(" ") == ""


def is_comment(line: str) -> bool:
    """整行是一条注释（行首空格之后就是 `#`）。"""
    return line.lstrip(" ").startswith("#")


def plugin_path(plugin: str, layer_id: str) -> list[str]:
    """插件名 + 层标识 → 以插件名为末段的 YAML 路径。

    插件配置只有两层：`global` 之下是全局默认，`controllers.<MAC>` 之下是
    单只手柄的覆盖；两层都先经过 `plugins:` 再按插件名分组。
    """
    if not TOKEN_RE.match(plugin):
        raise LayerError(f"插件名不是合法标识 {plugin!r}")
    if layer_id == "global":
        return ["global", "plugins", plugin]
    prefix = "controllers."
    if layer_id.startswith(prefix) and len(layer_id) > len(prefix):
        return ["controllers", layer_id[len(prefix) :], "plugins", plugin]
    raise LayerError(f"不认识的层 {layer_id}")


def _map_entry(node, key: str):
    """在一个映射节点里按键名找一对 (键节点, 值节点)。"""
    if not isinstance(node, MappingNode):
        return None
    for key_node, value_node in node.value:
        if isinstance(key_node, ScalarNode) and key_node.value == key:
            return key_node, value_node
    return None


def _compose(document: str):
    """整份文档的节点树；语法错误变成 LayerError。"""
    try:
        return yaml.compose(document, Loader=yaml.SafeLoader)
    except yaml.YAMLError as error:
        raise LayerError(f"YAML 读不通：{error}") from error


def _measure(node, key_line: int, key_indent: int, lines: list[str]) -> tuple[bool, int, int]:
    """量一个已存在节点：有没有子行、子层缩进、块的最后一行。

    块状映射的 end_mark 指向下一个同级 token（或文件末尾之后）。末尾带换行时
    它停在下一行的行首（column 0），最后一个子行就是 end_mark.line - 1；文件
    没有末尾换行时它停在最后那个字符之后，行号本身就是最后一个子行。再往回剪
    掉空行与不深于键的行（节与节之间的空行、文件末尾的注释都不属于这一节）。
    注释行不参与缩进判断：YAML 里注释的列号没有意义，`#` 行可以出现在任何缩进，
    回写时也原样留在它那一列。
    """
    if node is None:
        return False, -1, key_line
    if isinstance(node, ScalarNode):
        if node.value.strip() != "":
            raise LayerError(
                "这一节写成了一个标量；页面只能编辑缩进的键值行，请先在文件里改"
                "成映射"
            )
        return False, -1, key_line
    if isinstance(node, SequenceNode):
        raise LayerError("这一节写成了列表；页面只能编辑缩进的键值行")
    if not isinstance(node, MappingNode):
        raise LayerError("这一节不是映射；页面只能编辑缩进的键值行")
    if node.flow_style:
        raise LayerError(
            "这一节写成了行内形态（`{…}`）；页面只能整段编辑块状映射，请先在"
            "文件里展开成缩进的键值行"
        )
    if not node.value:
        return False, -1, key_line

    child_indent = min(key.start_mark.column for key, _ in node.value)
    for key_node, _ in node.value:
        if key_node.start_mark.line <= key_line:
            raise LayerError(
                "这一节里有别名（或行内写法）指向别处，找不到它自己的行范围"
            )

    end = node.end_mark.line - (1 if node.end_mark.column == 0 else 0)
    end = min(end, len(lines) - 1)
    if end < key_line:
        end = key_line
    while end > key_line and (
        is_blank(lines[end]) or indent_of(lines[end]) <= key_indent
    ):
        end -= 1

    for index in range(key_line + 1, end + 1):
        line = lines[index]
        if is_blank(line) or is_comment(line):
            continue
        if indent_of(line) < child_indent:
            raise LayerError(
                "这一节里有比子层更浅的续行；页面按整段回写，无法保持原缩进，"
                "请先在文件里对齐"
            )
    return True, child_indent, end


def analyze(document: str, path: list[str]) -> Analysis:
    """定位一条 YAML 路径，并判定它能不能编辑。"""
    lines, _ = split_document(document)
    root = _compose(document) if document.strip() != "" else None

    node = root
    depth = 0
    key_line = -1
    key_indent = -1
    key_end = -1
    for segment in path:
        found = _map_entry(node, segment)
        if found is None:
            break
        key_node, value_node = found
        key_line = key_node.start_mark.line
        key_indent = key_node.start_mark.column
        key_end = key_node.end_mark.column
        if key_line < 0 or key_line >= len(lines) or key_indent < 0:
            raise LayerError("这一节里有别名指向别处，找不到它自己的行范围")
        node = value_node
        depth += 1

    result = Analysis()
    if depth == len(path):
        result.present = True
        result.key_line = key_line
        result.key_indent = key_indent
        result.key_end = key_end
        if isinstance(node, ScalarNode):
            result.end_line = key_line
            result.scalar_start = node.start_mark.column
            result.scalar_end = node.end_mark.column
            result.scalar_style = node.style
            if node.start_mark.line != key_line or node.end_mark.line != key_line:
                result.editable = False
                result.reason = "这一项写成跨多行的值；页面只改单行标量"
            return result
        try:
            has_children, child_indent, end_line = _measure(
                node, key_line, key_indent, lines
            )
        except LayerError as error:
            result.editable = False
            result.reason = str(error)
            return result
        result.has_children = has_children
        result.child_indent = child_indent
        result.end_line = end_line
        return result

    # 这一层还不存在：从最深的既有祖先块尾补齐缺少的键。controllers 里还没有
    # 这只手柄时也一样——页面会补出 `controllers.<MAC>:` 这一串，已连接但用户
    # 还没配过的手柄因此也能直接编辑。
    if depth == 0:
        if root is not None and not isinstance(root, MappingNode):
            result.editable = False
            result.reason = "整份配置的根不是映射，无法追加新键"
            return result
        if isinstance(root, MappingNode) and root.flow_style:
            result.editable = False
            result.reason = "整份配置写成了行内形态，无法追加新键"
            return result
        result.insert_at = len(lines)
        result.insert_indent = 0
    else:
        try:
            has_children, child_indent, end_line = _measure(
                node, key_line, key_indent, lines
            )
        except LayerError as error:
            result.editable = False
            result.reason = str(error)
            return result
        result.insert_at = end_line + 1
        result.insert_indent = child_indent if has_children else key_indent + 2
        if isinstance(node, MappingNode) and node.flow_style:
            result.editable = False
            result.reason = f"路径上的 {path[depth - 1]} 写成了行内形态，无法追加新键"
            return result

    result.missing_keys = path[depth:]
    return result


def read_block(document: str, path: list[str]) -> dict:
    """取出路径指向的那一节之下的原文（已去掉子层缩进）。

    @note 注释行原样带回、不剥缩进：它的列号没有语义，剥了反而会把 `#` 切掉，
    回写时就对不上原文件了。
    """
    lines, _ = split_document(document)
    analysis = analyze(document, path)
    out = {
        "present": analysis.present,
        "editable": analysis.editable,
        "reason": analysis.reason,
        "text": "",
    }
    if not analysis.present or not analysis.editable or not analysis.has_children:
        return out
    block = []
    for index in range(analysis.key_line + 1, analysis.end_line + 1):
        line = lines[index]
        if is_blank(line):
            block.append("")
        elif is_comment(line):
            block.append(line)
        else:
            block.append(line[analysis.child_indent :])
    out["text"] = "\n".join(block)
    return out


def _indent_block_line(line: str, indent: int) -> str:
    """块里的一行摆进文档：空行留空，注释按原列号，其余按子层缩进。"""
    if is_blank(line):
        return ""
    if is_comment(line):
        return line
    return " " * indent + line


def write_block(document: str, path: list[str], block: str) -> str:
    """把一段新的子树写回路径指向的那一节，其余行逐字节不动。"""
    lines, trailing_newline = split_document(document)
    analysis = analyze(document, path)
    if not analysis.editable:
        raise LayerError(analysis.reason)

    block_lines = block.split("\n")
    while block_lines and is_blank(block_lines[-1]):
        block_lines.pop()
    while block_lines and is_blank(block_lines[0]):
        block_lines.pop(0)

    if analysis.present:
        if not block_lines:
            # 空节不能写成 `buttons:`：宿主要求插件名之下是非空映射。
            merged = lines[: analysis.key_line] + lines[analysis.end_line + 1 :]
            return join_document(merged, trailing_newline)
        indent = analysis.child_indent if analysis.has_children else analysis.key_indent + 2
        children = [_indent_block_line(line, indent) for line in block_lines]
        merged = (
            lines[: analysis.key_line]
            + [lines[analysis.key_line]]
            + children
            + lines[analysis.end_line + 1 :]
        )
        return join_document(merged, trailing_newline)

    if not block_lines:
        return document
    added = []
    indent = analysis.insert_indent
    for key in analysis.missing_keys:
        added.append(" " * indent + key + ":")
        indent += 2
    added.extend(_indent_block_line(line, indent) for line in block_lines)
    merged = lines[: analysis.insert_at] + added + lines[analysis.insert_at :]
    added_at_end = analysis.insert_at >= len(lines)
    return join_document(merged, trailing_newline or (added_at_end and bool(added)))


#: 能裸写成 YAML 纯量的形态；其余一律加引号。
_PLAIN_SCALAR_RE = re.compile(r"[A-Za-z0-9_./+@-][A-Za-z0-9_./:+@-]*\Z")
#: YAML 1.1 会把它们读成布尔或空值；写值时加引号，保住"这是一段文本"。
#: `true`/`false` 不在此列：基础配置里的布尔值本来就该写成布尔。
_RESERVED_SCALARS = {"yes", "no", "on", "off", "null", "~"}


def render_scalar(value: str) -> str:
    """把一个值写成单行 YAML 标量：能裸写就裸写，否则用双引号。"""
    if value == "":
        raise LayerError("值不能为空；要取消这一项就删掉这个键")
    if "\n" in value or "\r" in value:
        raise LayerError("值不能换行")
    if (
        value.lower() in _RESERVED_SCALARS
        or value.endswith(":")
        or not _PLAIN_SCALAR_RE.match(value)
    ):
        return json.dumps(value, ensure_ascii=False)
    return value


def _comment_tail(line: str, start: int) -> str:
    """取一行里 start 之后的行尾注释（含缩进）；没有注释时返回空串。"""
    rest = line[start:]
    index = rest.find("#")
    while index >= 0:
        if index == 0 or rest[index - 1] in " \t":
            return "  " + rest[index:]
        index = rest.find("#", index + 1)
    return ""


def set_scalar(document: str, path: list[str], value: str) -> str:
    """把路径末段设成一个标量：只换值本身，键名、缩进与行尾注释不动。

    @param document 整份配置原文
    @param path 以标量为末段的 YAML 路径
    @param value 新的取值文本（原样写成标量，必要时加引号）
    @return 改过的新全文；路径上的键不存在时在父块末尾补出这一串键
    @note 标量必须是单行；写成引号或块状的值在定位阶段就被判不可编辑
    """
    rendered = render_scalar(value)
    lines, trailing_newline = split_document(document)
    analysis = analyze(document, path)
    if not analysis.editable:
        raise LayerError(analysis.reason)

    if analysis.present:
        if analysis.scalar_start < 0:
            raise LayerError("这一项不是单个值；页面只改键与标量")
        line = lines[analysis.key_line]
        if analysis.scalar_start == analysis.scalar_end:
            # `键:` 后面没有值：在冒号后补值，原有的行尾注释留着。
            new_line = (
                line[: analysis.key_end + 1]
                + " "
                + rendered
                + _comment_tail(line, analysis.key_end + 1)
            )
        else:
            start = analysis.scalar_start
            end = analysis.scalar_end
            if analysis.scalar_style in ('"', "'"):
                start -= 1
                end += 1
            new_line = line[:start] + rendered + line[end:]
        lines[analysis.key_line] = new_line
        return join_document(lines, trailing_newline)

    added = []
    indent = analysis.insert_indent
    for key in analysis.missing_keys[:-1]:
        added.append(" " * indent + key + ":")
        indent += 2
    added.append(" " * indent + analysis.missing_keys[-1] + ": " + rendered)
    merged = lines[: analysis.insert_at] + added + lines[analysis.insert_at :]
    added_at_end = analysis.insert_at >= len(lines)
    return join_document(
        merged, trailing_newline or (added_at_end and bool(added))
    )


def delete_path(document: str, path: list[str]) -> str:
    """删掉路径末段这个键；跟着空掉的父节也一并删掉，其余行不动。

    @param document 整份配置原文
    @param path 要删的键的 YAML 路径
    @return 改过的新全文；路径本来就不存在时原文返回
    @note 路径末段必须是单行标量或空节；带子键的节要整段改写，不走这里
    """
    analysis = analyze(document, path)
    if not analysis.present:
        return document
    if analysis.has_children:
        raise LayerError("这一项下面还有内容；页面只删单个键")

    updated = document
    current = list(path)
    try:
        while current:
            analysis = analyze(updated, current)
            if (
                not analysis.present
                or not analysis.editable
                or analysis.has_children
            ):
                break
            lines, trailing_newline = split_document(updated)
            updated = join_document(
                lines[: analysis.key_line] + lines[analysis.end_line + 1 :],
                trailing_newline,
            )
            current.pop()
    except LayerError:
        pass
    return updated


def ensure_controller(document: str, mac: str) -> str:
    """确保 `controllers.<MAC>` 这个条目存在；已经有就原样返回。

    @param document 整份配置原文
    @param mac 手柄 MAC；大小写都收，写回时按大写
    @return 改过的新全文；只补一行 `MAC:`，其余行逐字节不动
    @note 只建空条目，不写任何手柄级键；取值由随后的 scalar 修改项补齐
    """
    mac = mac.strip().upper()
    if not MAC_RE.match(mac):
        raise LayerError(f"MAC 要写成 XX:XX:XX:XX:XX:XX，收到 {mac!r}")
    root = _compose(document) if document.strip() != "" else None
    controllers = _map_entry(root, "controllers")
    lines, trailing_newline = split_document(document)
    if controllers is None:
        lines.append("controllers:")
        lines.append("  " + mac + ":")
        return join_document(lines, True)
    node = controllers[1]
    if not isinstance(node, MappingNode):
        raise LayerError("controllers 不是映射；页面不能往里加条目")
    # MAC 不区分大小写：文件里写成小写也算同一个手柄。
    if any(
        isinstance(key, ScalarNode) and key.value.strip().upper() == mac
        for key, _ in node.value
    ):
        return document
    section = analyze(document, ["controllers"])
    if not section.editable:
        raise LayerError(section.reason)
    indent = section.child_indent if section.has_children else section.key_indent + 2
    insert_at = section.end_line + 1
    lines.insert(insert_at, " " * indent + mac + ":")
    return join_document(lines, trailing_newline or insert_at >= len(lines))


def _mac_key(text: str) -> str:
    """MAC 的比较口径：大小写不敏感。"""
    return str(text or "").strip().upper()


def _controller_mac_of(layer_id: str) -> str | None:
    """`controllers.<MAC>` → 大写 MAC；别的层标识返回 None。"""
    prefix = "controllers."
    if not layer_id.startswith(prefix):
        return None
    mac = _mac_key(layer_id[len(prefix) :])
    return mac if MAC_RE.match(mac) else None


def layer_entries(document: str, pads: list[dict] | None = None) -> list[dict]:
    """配置层清单：全局、文件里的每只手柄、已连接但还没写进文件的手柄。

    @param document 整份配置原文
    @param pads `discover_pads()` 的结果；None 表示只列文件里的条目
    @return 每项 `{id, mac, configured, connected, model}`，global 在第一项
    @note 文件里的键保持原样大小写；由已连接手柄补出来的条目一律大写
    """
    entries = [
        {
            "id": "global",
            "mac": "",
            "configured": True,
            "connected": False,
            "model": "",
        }
    ]
    try:
        root = _compose(document) if document.strip() != "" else None
    except LayerError:
        root = None
    models: dict[str, str] = {}
    connected: set[str] = set()
    for pad in pads or []:
        mac = _mac_key(pad.get("mac", ""))
        if not MAC_RE.match(mac):
            continue
        connected.add(mac)
        models.setdefault(mac, str(pad.get("model", "")))
    seen: set[str] = set()
    for mac in _controller_macs(root):
        seen.add(_mac_key(mac))
        entries.append(
            {
                "id": "controllers." + mac,
                "mac": mac,
                "configured": True,
                "connected": _mac_key(mac) in connected,
                "model": models.get(_mac_key(mac), ""),
            }
        )
    for mac in sorted(connected - seen):
        entries.append(
            {
                "id": "controllers." + mac,
                "mac": mac,
                "configured": False,
                "connected": True,
                "model": models.get(mac, ""),
            }
        )
    return entries


def list_layers(
    document: str, plugin: str, connected_macs: list[str] | tuple[str, ...] = ()
) -> list[dict]:
    """列出某个插件可切换的层：global、配置里的每只手柄、已连接的手柄。

    `global` 永远在列表里（空文件也要能从这里开始写）；`controllers.<MAC>`
    按文件里已有的条目列出，再把 `connected_macs` 里文件还没有的补在后面——
    已连接的手柄即使还没配过也能选中，保存时才写出这一层。`present` 只说明
    这个插件在这一层有没有配置，与"手柄在不在文件里"无关。
    """
    entries = layer_entries(
        document, [{"mac": mac} for mac in connected_macs]
    )

    def present(layer_id: str) -> bool:
        try:
            return analyze(document, plugin_path(plugin, layer_id)).present
        except LayerError:
            return False

    layers = []
    for entry in entries:
        if entry["id"] == "global":
            label = "全局（所有手柄的默认值）"
        else:
            label = "手柄 " + entry["mac"]
        layers.append(
            {
                "id": entry["id"],
                "label": label,
                "present": present(entry["id"]),
                "configured": entry["configured"],
                "connected": entry["connected"],
            }
        )
    return layers


def flatten_section(document: str, path: list[str]) -> list[list[str]] | None:
    """一节里的 (点分键路径, 标量原文) 序列；不能按行编辑时返回 None。

    通用面板要的是"键与值"，不需要知道任何语法；顺序就是文件中出现的顺序。
    """
    try:
        composed = _compose(document) if document.strip() != "" else None
    except LayerError:
        return None
    node = composed
    for segment in path:
        found = _map_entry(node, segment)
        if found is None:
            return []
        node = found[1]
    if node is None:
        return []
    if isinstance(node, ScalarNode):
        return [] if node.value.strip() == "" else None
    if not isinstance(node, MappingNode) or node.flow_style:
        return None
    rows: list[tuple[list[str], str]] = []
    try:
        _flatten(node, [], rows)
    except LayerError:
        return None
    return [[".".join(key), value] for key, value in rows]


# ---------------------------------------------------------------------------
# 模型：页面看到的是结构化的规则，不是 YAML 文本
# ---------------------------------------------------------------------------


def _scalar_text(node) -> str:
    """节点若不是标量就报错；返回标量原文。"""
    if not isinstance(node, ScalarNode):
        raise LayerError(
            "这一节里有列表或嵌套过深的值；页面只编辑键与标量，请先在文件里改"
            "成一行一个值"
        )
    return node.value


def _flatten(node, prefix: list[str], out: list[tuple[list[str], str]]) -> None:
    """把映射摊平成 (路径, 标量原文) 序列；列表与复杂键直接判不可编辑。"""
    if not isinstance(node, MappingNode):
        raise LayerError(
            "这一节里有列表或非映射的值；页面只编辑键与标量，请先在文件里改成"
            "一行一个值"
        )
    for key_node, value_node in node.value:
        if not isinstance(key_node, ScalarNode) or key_node.value == "":
            raise LayerError("这一节里有空的或非标量的键；页面不能编辑它")
        path = prefix + [key_node.value]
        if len(path) > 8:
            raise LayerError("这一节的嵌套超过 8 层；页面只编辑配置层允许的层数")
        if isinstance(value_node, MappingNode):
            _flatten(value_node, path, out)
        else:
            if isinstance(value_node, SequenceNode):
                raise LayerError(
                    "这一节里有列表；页面只编辑键与标量，请先在文件里改成一"
                    "行一个值"
                )
            out.append((path, _scalar_text(value_node)))


def _parse_trigger(text: str):
    """触发串 → {terms, timing, ms}；读不通返回 None。"""
    start = None
    for index, char in enumerate(text):
        if char in "~*>":
            start = index
            break
    terms_text = text
    timing = "chord"
    ms = None
    if start is not None:
        if any(char in "~*>" for char in text[start + 1 :]):
            return None
        terms_text = text[:start]
        suffix = text[start:].strip()
        if "+" in suffix:
            return None
        if suffix == "~":
            timing, ms = "tap", DEFAULT_TAP_MS
        elif suffix == "*2":
            timing, ms = "double", DEFAULT_DOUBLE_MS
        elif suffix.startswith(">"):
            digits = suffix[1:].strip()
            if not DECIMAL_RE.match(digits):
                return None
            ms = int(digits)
            if not LONG_MIN_MS <= ms <= LONG_MAX_MS:
                return None
            timing = "long"
        else:
            return None
    terms = [piece.strip() for piece in terms_text.split("+")]
    if not terms or any(term not in SOURCE_TOKENS for term in terms):
        return None
    return {"terms": terms, "timing": timing, "ms": ms}


def _trigger_text(terms: list[str], timing: str, ms) -> str:
    """{terms, timing, ms} → 触发串。"""
    base = "+".join(terms)
    if timing == "tap":
        return base + "~"
    if timing == "double":
        return base + "*2"
    if timing == "long":
        return base + ">" + str(ms)
    return base


def _parse_rule(value: str):
    """`触发串 : 结果` → (触发, 结果原文)；切不开返回 None。"""
    colon = value.find(":")
    if colon < 0:
        return None
    rule = _parse_trigger(value[:colon])
    if rule is None:
        return None
    return rule, value[colon + 1 :].strip()


def _classify(path: list[str], value: str) -> dict:
    """一行配置 → 一条模型项；认不出的形态留在 `other` 里原样编辑。"""
    if len(path) == 2:
        group, leaf = path
        if group == "remap" and leaf in SOURCE_TOKENS:
            text = value.strip()
            if text == "disable":
                return {
                    "kind": "remap",
                    "path": path,
                    "src": leaf,
                    "target": "",
                    "keep": False,
                    "disable": True,
                }
            pieces = [piece.strip() for piece in text.split("+")]
            if len(pieces) == 1:
                target, keep = pieces[0], False
            elif len(pieces) == 2 and pieces[1] == "keep":
                target, keep = pieces[0], True
            else:
                return {"kind": "other", "path": path, "value": value}
            if target not in WIRE_TOKENS:
                return {"kind": "other", "path": path, "value": value}
            return {
                "kind": "remap",
                "path": path,
                "src": leaf,
                "target": target,
                "keep": keep,
                "disable": False,
            }
        if group == "turbo" and leaf in SOURCE_TOKENS:
            text = value.strip()
            if DECIMAL_RE.match(text):
                return {
                    "kind": "turbo",
                    "path": path,
                    "src": leaf,
                    "hz": int(text),
                }
            return {"kind": "other", "path": path, "value": value}
        if group in ("shortcut", "macro") and leaf != "":
            parsed = _parse_rule(value)
            if parsed is not None:
                trigger, result = parsed
                if group == "shortcut" and result == "power_off":
                    return {
                        "kind": "shortcut",
                        "path": path,
                        "id": leaf,
                        "terms": trigger["terms"],
                        "timing": trigger["timing"],
                        "ms": trigger["ms"],
                    }
                if group == "macro":
                    targets = [piece.strip() for piece in result.split("+")]
                    if targets and all(t in WIRE_TOKENS for t in targets):
                        return {
                            "kind": "macro",
                            "path": path,
                            "id": leaf,
                            "terms": trigger["terms"],
                            "timing": trigger["timing"],
                            "ms": trigger["ms"],
                            "targets": targets,
                        }
            return {"kind": "other", "path": path, "value": value}
        if group == "axis":
            if leaf.startswith("invert_") and leaf[len("invert_") :] in AXIS_TOKENS:
                text = value.strip()
                if text in ("true", "false"):
                    return {
                        "kind": "axis_invert",
                        "path": path,
                        "axis": leaf[len("invert_") :],
                        "invert": text == "true",
                    }
            elif leaf.startswith("center_") and leaf[len("center_") :] in STICK_TOKENS:
                if value.strip() == "always":
                    return {
                        "kind": "axis_center",
                        "path": path,
                        "stick": leaf[len("center_") :],
                    }
            elif leaf == "swap_lstick_rstick" and value.strip() in ("both", "x", "y"):
                return {
                    "kind": "axis_swap",
                    "path": path,
                    "value": value.strip(),
                }
    return {"kind": "other", "path": path, "value": value}


def parse_model(block: str) -> dict:
    """一段 `buttons` 子树 → 页面模型。"""
    if block.strip() == "":
        return {"structured": True, "reason": "", "items": []}
    # 用与真配置同形的缩进重新组合一次，读的就是宿主会交给插件的那棵树。
    document = "buttons:\n" + "\n".join(
        " " * 2 + line if line.strip() else "" for line in block.split("\n")
    ) + "\n"
    try:
        root = _compose(document)
        buttons = _map_entry(root, "buttons")
        if buttons is None or not isinstance(buttons[1], MappingNode):
            return {
                "structured": False,
                "reason": "这一节不是块状映射，页面不能按行编辑",
                "items": [],
            }
        flat: list[tuple[list[str], str]] = []
        _flatten(buttons[1], [], flat)
    except LayerError as error:
        return {"structured": False, "reason": str(error), "items": []}
    items = [_classify(path, value) for path, value in flat]
    return {"structured": True, "reason": "", "items": items}


class ModelError(Exception):
    """页面交回的模型不是服务器能安全写出去的那一种。"""


def _need_string(value, name: str, maximum: int = 64) -> str:
    """取一个非空、无控制字符、长度受限的字符串。"""
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise ModelError(f"{name} 不是合法文本")
    if any(char in value for char in "\r\n\x00"):
        raise ModelError(f"{name} 不能包含换行或 NUL")
    return value


def _need_key_path(value) -> list[str]:
    """取一条页面交回的键路径；每段只允许配置键会出现的字符。"""
    if not isinstance(value, list) or not value or len(value) > 10:
        raise UserError("路径不是合法的键序列")
    path = []
    for segment in value:
        if not isinstance(segment, str) or not KEY_RE.match(segment):
            raise UserError(f"路径里的 {segment!r} 不是合法的键名")
        path.append(segment)
    return path


def _need_bool(value, name: str) -> bool:
    """取一个布尔值。"""
    if not isinstance(value, bool):
        raise ModelError(f"{name} 不是布尔值")
    return value


def _need_int(value, name: str, low: int, high: int) -> int:
    """取一个区间内的整数（bool 不算整数）。"""
    if isinstance(value, bool) or not isinstance(value, int):
        raise ModelError(f"{name} 不是整数")
    if not low <= value <= high:
        raise ModelError(f"{name} 超出 {low}..{high}")
    return value


def _need_terms(value, name: str, allowed: list[str]) -> list[str]:
    """取一串源 token。"""
    if not isinstance(value, list) or not value or len(value) > 16:
        raise ModelError(f"{name} 必须是 1..16 个键")
    terms = []
    for raw in value:
        token = _need_string(raw, name)
        if token not in allowed:
            raise ModelError(f"{name} 里的 {token} 不是可用的键")
        terms.append(token)
    return terms


def _need_timing(item: dict) -> tuple[str, int | None]:
    """取时序与阈值。"""
    timing = item.get("timing")
    if timing not in ("chord", "tap", "double", "long"):
        raise ModelError("时序不是 chord/tap/double/long")
    if timing == "long":
        return timing, _need_int(item.get("ms"), "长按毫秒", LONG_MIN_MS, LONG_MAX_MS)
    return timing, None


def _normalize_items(items) -> list[dict]:
    """校验页面交回的模型，并按字段重建路径（不信任客户端给的路径）。"""
    if not isinstance(items, list) or len(items) > MAX_ITEMS:
        raise ModelError("模型必须是数量受限的数组")
    normalized = []
    for item in items:
        if not isinstance(item, dict):
            raise ModelError("模型项不是对象")
        kind = item.get("kind")
        if kind == "remap":
            src = _need_string(item.get("src"), "映射源")
            if src not in SOURCE_TOKENS:
                raise ModelError(f"映射源 {src} 不认识")
            disable = _need_bool(item.get("disable", False), "disable")
            keep = _need_bool(item.get("keep", False), "keep")
            target = item.get("target", "")
            if not disable:
                target = _need_string(target, "映射目标")
                if target not in WIRE_TOKENS:
                    raise ModelError(f"映射目标 {target} 不是线上键")
            else:
                target, keep = "", False
            normalized.append(
                {
                    "kind": "remap",
                    "path": ["remap", src],
                    "src": src,
                    "target": target,
                    "keep": keep,
                    "disable": disable,
                }
            )
        elif kind == "turbo":
            src = _need_string(item.get("src"), "连点源")
            if src not in SOURCE_TOKENS:
                raise ModelError(f"连点源 {src} 不认识")
            hz = _need_int(item.get("hz"), "连点频率", 0, 100000)
            normalized.append(
                {"kind": "turbo", "path": ["turbo", src], "src": src, "hz": hz}
            )
        elif kind in ("shortcut", "macro"):
            leaf = _need_string(item.get("id"), "规则编号", 32)
            terms = _need_terms(item.get("terms"), "触发键", SOURCE_TOKENS)
            timing, ms = _need_timing(item)
            if kind == "shortcut":
                normalized.append(
                    {
                        "kind": "shortcut",
                        "path": ["shortcut", leaf],
                        "id": leaf,
                        "terms": terms,
                        "timing": timing,
                        "ms": ms,
                    }
                )
            else:
                targets = _need_terms(item.get("targets"), "宏目标", WIRE_TOKENS)
                normalized.append(
                    {
                        "kind": "macro",
                        "path": ["macro", leaf],
                        "id": leaf,
                        "terms": terms,
                        "timing": timing,
                        "ms": ms,
                        "targets": targets,
                    }
                )
        elif kind == "axis_invert":
            axis = _need_string(item.get("axis"), "轴")
            if axis not in AXIS_TOKENS:
                raise ModelError(f"轴 {axis} 不认识")
            normalized.append(
                {
                    "kind": "axis_invert",
                    "path": ["axis", "invert_" + axis],
                    "axis": axis,
                    "invert": _need_bool(item.get("invert"), "invert"),
                }
            )
        elif kind == "axis_center":
            stick = _need_string(item.get("stick"), "摇杆")
            if stick not in STICK_TOKENS:
                raise ModelError(f"摇杆 {stick} 不认识")
            normalized.append(
                {
                    "kind": "axis_center",
                    "path": ["axis", "center_" + stick],
                    "stick": stick,
                }
            )
        elif kind == "axis_swap":
            value = item.get("value")
            if value not in ("both", "x", "y"):
                raise ModelError("对调范围不是 both/x/y")
            normalized.append(
                {"kind": "axis_swap", "path": ["axis", "swap_lstick_rstick"], "value": value}
            )
        elif kind == "other":
            path = item.get("path")
            if not isinstance(path, list) or not 1 <= len(path) <= 8:
                raise ModelError("其他项的路径必须是 1..8 段")
            segments = [_need_string(segment, "路径段") for segment in path]
            value = item.get("value", "")
            if not isinstance(value, str) or len(value) > MAX_VALUE:
                raise ModelError("其他项的值不是长度受限的文本")
            normalized.append({"kind": "other", "path": segments, "value": value})
        else:
            raise ModelError(f"不认识的模型项 {kind!r}")
    return normalized


def _item_leaf(item: dict) -> tuple[list[str], object]:
    """一条模型项 → (路径, 要写进 YAML 的值)。"""
    kind = item["kind"]
    if kind == "remap":
        if item["disable"]:
            return ["remap", item["src"]], "disable"
        value = item["target"] + ("+keep" if item["keep"] else "")
        return ["remap", item["src"]], value
    if kind == "turbo":
        return ["turbo", item["src"]], item["hz"]
    if kind == "shortcut":
        return (
            ["shortcut", item["id"]],
            _trigger_text(item["terms"], item["timing"], item["ms"]) + ": power_off",
        )
    if kind == "macro":
        return (
            ["macro", item["id"]],
            _trigger_text(item["terms"], item["timing"], item["ms"])
            + ": "
            + "+".join(item["targets"]),
        )
    if kind == "axis_invert":
        return ["axis", "invert_" + item["axis"]], item["invert"]
    if kind == "axis_center":
        return ["axis", "center_" + item["stick"]], "always"
    if kind == "axis_swap":
        return ["axis", "swap_lstick_rstick"], item["value"]
    return item["path"], item["value"]


def _insert_nested(root: dict, path: list[str], value) -> None:
    """按路径把一个值放进嵌套字典；同名标量被后出现的项覆盖。"""
    node = root
    for segment in path[:-1]:
        child = node.get(segment)
        if not isinstance(child, dict):
            child = {}
            node[segment] = child
        node = child
    node[path[-1]] = value


def serialize_model(items) -> str:
    """模型 → `buttons` 之下的 YAML 文本（缩进 2，作为块文本交回）。"""
    normalized = _normalize_items(items)
    if not normalized:
        return ""
    grouped: dict[str, list[dict]] = {}
    for item in normalized:
        grouped.setdefault(item["path"][0], []).append(item)
    ordered_groups = [group for group in GROUP_ORDER if group in grouped]
    ordered_groups += [group for group in grouped if group not in GROUP_ORDER]

    tree: dict = {}
    for group in ordered_groups:
        for item in grouped[group]:
            path, value = _item_leaf(item)
            _insert_nested(tree, path, value)
    try:
        text = yaml.safe_dump(
            tree,
            allow_unicode=True,
            sort_keys=False,
            default_flow_style=False,
            width=1_000_000,
        )
    except yaml.YAMLError as error:
        raise ModelError(f"生成 YAML 失败：{error}") from error
    return text.rstrip("\n")


# ---------------------------------------------------------------------------
# 权威校验：调用 buttons_check
# ---------------------------------------------------------------------------


class ToolError(Exception):
    """调用校验器失败（找不到、超时、输出读不懂）。"""


def find_checker(explicit: str | None, script_dir: Path) -> str | None:
    """找 buttons_check：显式参数、环境变量、常见构建目录、PATH。"""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("DS5DUMMY_BUTTONS_CHECK")
    if env:
        candidates.append(env)
    repo = script_dir.parent
    candidates += [
        str(repo / "build" / "buttons_check"),
        str(repo / "build-tools" / "buttons_check"),
        str(repo / "build" / "tools" / "buttons_check"),
    ]
    found = shutil.which("buttons_check")
    if found:
        candidates.append(found)
    for candidate in candidates:
        path = Path(candidate).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    return None


def _run_checker(
    checker: str, arguments: list[str], stdin_text: str | None = None
) -> dict:
    """跑一次 buttons_check --json，返回它解析后的对象。"""
    try:
        completed = subprocess.run(
            [checker, "--json", *arguments],
            input=stdin_text,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
    except FileNotFoundError as error:
        raise ToolError(f"找不到校验器 {checker}") from error
    except subprocess.TimeoutExpired as error:
        raise ToolError("buttons_check 超时") from error
    stdout = completed.stdout.strip()
    if stdout:
        try:
            return json.loads(stdout)
        except json.JSONDecodeError as error:
            raise ToolError(f"buttons_check 输出读不懂：{error}") from error
    detail = completed.stderr.strip() or f"退出码 {completed.returncode}"
    raise ToolError(f"buttons_check 没有给出结果：{detail}")


def check_layer(checker: str, layer_id: str, block: str) -> dict:
    """按层校验一段 `buttons` 子树。"""
    data = _run_checker(checker, ["--layer", layer_id], stdin_text=block + "\n")
    yaml_ok = bool(data.get("yaml_ok"))
    report = data.get("report") or {}
    return {
        "available": True,
        "yaml_ok": yaml_ok,
        "yaml_error": data.get("yaml_error", "") if not yaml_ok else "",
        "entries": report.get("entries", []),
        "rejected": report.get("rejected", 0),
        "ignored": report.get("ignored", 0),
        "armed": bool(report.get("armed")),
        "summary": report.get("summary", ""),
    }


def unavailable(error: str) -> dict:
    """校验器缺席时的结果；保存会被拒绝。"""
    return {
        "available": False,
        "yaml_ok": True,
        "yaml_error": "",
        "error": error,
        "entries": [],
        "rejected": 0,
        "ignored": 0,
        "armed": False,
        "summary": "",
    }


def check_document(checker: str, text: str, layer_id: str) -> str | None:
    """整份新文档过一遍 daemon 的解析器；出错返回原因。"""
    directory = Path(tempfile.gettempdir())
    handle, temporary = tempfile.mkstemp(prefix="ds5dummy-buttons-", suffix=".yaml", dir=directory)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="") as stream:
            stream.write(text)
        arguments = [temporary]
        if layer_id.startswith("controllers."):
            arguments += ["--mac", layer_id[len("controllers.") :]]
        try:
            completed = subprocess.run(
                [checker, "--json", *arguments],
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as error:
            raise ToolError(f"校验整份配置失败：{error}") from error
        if completed.returncode != 2:
            return None
        try:
            data = json.loads(completed.stdout)
            message = str(data.get("error", "整份配置读不通"))
            # 解析器的报错带临时文件名；页面上拿不到也没必要看到它。
            if message.startswith(temporary + ": "):
                message = message[len(temporary) + 2 :]
            return message
        except (json.JSONDecodeError, AttributeError):
            return completed.stderr.strip() or "整份配置读不通"
    finally:
        try:
            os.unlink(temporary)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 文件读写与本地服务
# ---------------------------------------------------------------------------


class UserError(Exception):
    """可以直接给页面看的 4xx 错误。"""


class StaleError(Exception):
    """磁盘上的文件与页面打开时不是同一份。"""


def default_config_path() -> Path:
    """`$XDG_CONFIG_HOME/ds5dummy/config.yaml`，没有 XDG 时用 `~/.config`。"""
    base = os.environ.get("XDG_CONFIG_HOME")
    if base:
        return Path(base) / "ds5dummy" / "config.yaml"
    return Path.home() / ".config" / "ds5dummy" / "config.yaml"


def _hash(text: str) -> str:
    """页面用它判断磁盘上的文件有没有被别处改过。"""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _atomic_write(path: Path, text: str) -> None:
    """同目录临时文件 + fsync + rename；沿用原文件权限，新文件 0600。"""
    mode = 0o600
    if path.exists():
        mode = path.stat().st_mode & 0o777
    directory = path.parent
    handle, temporary = tempfile.mkstemp(prefix=".config.yaml.", dir=directory)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
        directory_fd = os.open(directory, os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


# ---------------------------------------------------------------------------
# 基础配置、系统配置与运行状态：页面读的那几样东西
# ---------------------------------------------------------------------------

#: 用户配置根级里这份工具能改的键。
ROOT_KEYS = ("enabled", "bt_addr", "log_level")
#: `global:` 与每个 `controllers.<MAC>` 条目共用的六项。
HANDHELD_KEYS = (
    "bt_wait_s",
    "audio",
    "haptics_gain",
    "speaker_gain",
    "usb_report_interval_ms",
    "usb_model",
)
#: 系统配置的固定路径，只读展示。
SYSTEM_CONFIG_PATH = Path("/etc/ds5dummy/config.yaml")
#: 认得的 DualSense HID 身份：(VID, PID) → 型号。
PAD_IDENTITIES = {
    ("054c", "0ce6"): "DualSense",
    ("054c", "0df2"): "DualSense Edge",
    ("1998", "1221"): "DualSense（hidraw 合成）",
    ("1998", "1103"): "DualSense Edge（hidraw 合成）",
}

#: 结构化编辑器的注册表：插件名 → 服务端模型 ↔ 节文本的编解码。
#: 没有登记的插件走通用键值面板，不在这里加代码。
CODECS = {
    "buttons": {
        "parse": parse_model,
        "serialize": serialize_model,
        "catalog": catalog,
    },
}


@dataclass
class Panel:
    """插件目录里的一个 `.so`，以及它旁边可选的面板脚本。"""

    id: str
    so: Path
    script: Path | None

    def view(self) -> dict:
        """页面要的那几项。"""
        return {
            "id": self.id,
            "so": str(self.so),
            "has_panel": self.script is not None,
        }


def default_plugin_dirs() -> list[Path]:
    """宿主的两个插件目录：系统目录在前、用户目录覆盖。"""
    dirs = [Path("/usr/lib/ds5dummy/plugins")]
    home = os.environ.get("HOME")
    if home:
        dirs.append(Path(home) / ".config" / "ds5dummy" / "plugins")
    return dirs


def discover_panels(dirs: list[Path]) -> list[Panel]:
    """按宿主的口径列出插件；用户目录的同名条目覆盖系统目录。"""
    found: dict[str, Panel] = {}
    for directory in dirs:
        try:
            entries = sorted(directory.iterdir(), key=lambda item: item.name)
        except OSError:
            continue
        for entry in entries:
            if entry.suffix != ".so" or not TOKEN_RE.match(entry.stem):
                continue
            try:
                if not entry.is_file():
                    continue
            except OSError:
                continue
            script = entry.with_name(entry.stem + ".panel.js")
            found[entry.stem] = Panel(
                id=entry.stem,
                so=entry,
                script=script if script.is_file() else None,
            )
    return [found[name] for name in sorted(found)]


def _systemctl_state(unit: str) -> dict:
    """`systemctl is-active/is-enabled` 的两行结果；跑不起来就记原因。"""
    out = {"active": "", "enabled": "", "error": ""}
    for key, verb in (("active", "is-active"), ("enabled", "is-enabled")):
        try:
            completed = subprocess.run(
                ["systemctl", verb, unit],
                capture_output=True,
                text=True,
                timeout=3,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            out["error"] = str(error)
            return out
        out[key] = completed.stdout.strip() or completed.stderr.strip()
    return out


def discover_pads() -> list[dict]:
    """从 `/sys/class/hidraw` 认出系统里的 DualSense。"""
    pads = []
    try:
        nodes = sorted(Path("/sys/class/hidraw").iterdir())
    except OSError:
        return pads
    for node in nodes:
        try:
            text = (node / "device" / "uevent").read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        fields = {}
        for line in text.splitlines():
            key, _, value = line.partition("=")
            fields[key] = value
        identity = fields.get("HID_ID", "").split(":")
        if len(identity) != 3:
            continue
        vendor = identity[1][-4:].lower()
        product = identity[2][-4:].lower()
        model = PAD_IDENTITIES.get((vendor, product))
        if model is None:
            continue
        pads.append(
            {"node": node.name, "mac": fields.get("HID_UNIQ", ""), "model": model}
        )
    return pads


def list_udc() -> list[str]:
    """虚拟 UDC 的名字；读不到时为空。"""
    try:
        return sorted(path.name for path in Path("/sys/class/udc").iterdir())
    except OSError:
        return []


def system_view() -> dict:
    """系统配置的只读展示；文件缺失或读不了时带上原因。"""
    view = {
        "path": str(SYSTEM_CONFIG_PATH),
        "present": False,
        "values": {},
        "error": "",
    }
    try:
        text = SYSTEM_CONFIG_PATH.read_text(encoding="utf-8")
    except FileNotFoundError:
        return view
    except (OSError, UnicodeDecodeError) as error:
        view["error"] = str(error)
        return view
    view["present"] = True
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as error:
        view["error"] = f"YAML 读不通：{error}"
        return view
    if not isinstance(data, dict):
        view["error"] = "系统配置的根不是映射"
        return view
    view["values"] = data
    return view


def _path_text(root, path: list[str]) -> str | None:
    """在已组合的整份文档里取一条路径的标量文本；缺失或非标量时 None。"""
    node = root
    for segment in path:
        found = _map_entry(node, segment)
        if found is None:
            return None
        node = found[1]
    return node.value if isinstance(node, ScalarNode) else None


def _controller_macs(root) -> list[str]:
    """`controllers` 下已有的条目名，按文本排序。"""
    found = _map_entry(root, "controllers")
    if found is None or not isinstance(found[1], MappingNode):
        return []
    names = [
        key.value for key, _ in found[1].value if isinstance(key, ScalarNode)
    ]
    return sorted(names)


def basic_view(document: str) -> dict:
    """根级、`global:` 与 `controllers` 条目里的原始取值。"""
    view = {
        "root": {key: None for key in ROOT_KEYS},
        "global": {key: None for key in HANDHELD_KEYS},
        "controllers": [],
        "error": "",
    }
    try:
        root = _compose(document) if document.strip() != "" else None
    except LayerError as error:
        view["error"] = str(error)
        return view
    for key in ROOT_KEYS:
        view["root"][key] = _path_text(root, [key])
    for key in HANDHELD_KEYS:
        view["global"][key] = _path_text(root, ["global", key])
    for mac in _controller_macs(root):
        values = {
            key: _path_text(root, ["controllers", mac, key])
            for key in HANDHELD_KEYS
        }
        view["controllers"].append({"mac": mac, "values": values})
    controllers = _map_entry(root, "controllers")
    if controllers is not None and not isinstance(controllers[1], MappingNode):
        view["error"] = "controllers 不是映射"
    return view


def layer_id_of(path: list[str]) -> str:
    """插件节路径 → `buttons_check --layer` 认的层标识。"""
    if path[:2] == ["global", "plugins"]:
        return "global"
    if len(path) == 4 and path[0] == "controllers" and path[2] == "plugins":
        return "controllers." + path[1]
    raise LayerError("不是插件节的路径")


class App:
    """一次运行的全部状态：配置路径、页面、校验器与插件目录。"""

    def __init__(
        self,
        config_path: Path,
        checker: str | None,
        html: str,
        script_dir: Path,
        plugin_dirs: list[Path],
        verbose: bool,
    ):
        self.config_path = config_path
        self.checker = checker
        self.html = html
        self.script_dir = script_dir
        self.plugin_dirs = plugin_dirs
        self.verbose = verbose
        self.lock = threading.Lock()
        self.panels: dict[str, Panel] = {}
        self.refresh_panels()

    # -- 读 -----------------------------------------------------------------

    def read_document(self) -> str:
        """读用户配置；不存在等于空文档，符号链接与超限文件直接拒绝。"""
        path = self.config_path
        if not path.exists():
            return ""
        if path.is_symlink():
            raise UserError("配置文件是符号链接；页面不跟着链接写别处的文件")
        if not path.is_file():
            raise UserError(f"{path} 不是普通文件")
        size = path.stat().st_size
        if size > MAX_CONFIG_BYTES:
            raise UserError("配置文件超过 1 MiB；这不是页面该编辑的文件")
        try:
            return path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            raise UserError(f"配置文件不是 UTF-8：{error}") from error
        except OSError as error:
            raise UserError(f"读不了配置文件：{error}") from error

    def refresh_panels(self) -> None:
        """重新扫一遍插件目录；页面每次取状态都会走到这里。"""
        self.panels = {panel.id: panel for panel in discover_panels(self.plugin_dirs)}

    def panel_script(self, name: str) -> Path | None:
        """面板脚本的路径：`core` 随工具走，其余与 `.so` 同目录。"""
        if name == "core":
            path = self.script_dir / "panels" / "core.js"
            return path if path.is_file() else None
        panel = self.panels.get(name)
        return panel.script if panel is not None else None

    def state(self) -> dict:
        """页面要的一整份状态：基础配置、层清单、运行事实与插件面板清单。"""
        self.refresh_panels()
        document = self.read_document()
        pads = discover_pads()
        return {
            "config": str(self.config_path),
            "exists": self.config_path.exists(),
            "hash": _hash(document),
            "restart": "sudo systemctl restart ds5dummy",
            "base": basic_view(document),
            "layers": layer_entries(document, pads),
            "runtime": {
                "service": _systemctl_state("ds5dummy"),
                "pads": pads,
                "udc": list_udc(),
            },
            "system": system_view(),
            "panels": [panel.view() for panel in self.panels.values()],
        }

    def plugin_state(self, name: str, layer_id: str) -> dict:
        """一个插件在某一层的一节：原文、模型、通用键值与校验结果。

        手柄层另回 `inherited`（global 同节的扁平键值），面板用它说明
        "这个键留空等于继承到多少"。登记了结构化编辑器的插件还带回自己的
        静态词表 `catalog`：面板只消费这一份 payload，不再自己发请求。
        """
        if not TOKEN_RE.match(name):
            raise UserError(f"插件名不是合法标识 {name!r}")
        self.refresh_panels()
        document = self.read_document()
        connected = [pad.get("mac", "") for pad in discover_pads() if pad.get("mac")]
        layers = list_layers(document, name, connected)
        ids = [layer["id"] for layer in layers]
        if layer_id in (None, ""):
            layer_id = ids[0]
        if layer_id not in ids:
            # 页面上手写的 MAC：层清单里没有也算数，保存时才写出这一节。
            mac = _controller_mac_of(layer_id)
            if mac is None:
                raise UserError(f"不认识的层 {layer_id}")
            layer_id = "controllers." + mac
            layers.append(
                {
                    "id": layer_id,
                    "label": "手柄 " + mac,
                    "present": False,
                    "configured": False,
                    "connected": False,
                }
            )
        path = plugin_path(name, layer_id)
        block = read_block(document, path)
        # 手柄层没写的键由 global 同名的扁平键继承；面板用它显示"留空等于多少"。
        inherited = (
            []
            if layer_id == "global"
            else (flatten_section(document, plugin_path(name, "global")) or [])
        )
        codec = CODECS.get(name)
        model = None
        validation = None
        catalog_view = None
        if codec is not None:
            model = (
                codec["parse"](block["text"])
                if block["editable"]
                else {
                    "structured": False,
                    "reason": block["reason"],
                    "items": [],
                }
            )
            validation = self.validate(name, layer_id, block["text"])
            builder = codec.get("catalog")
            if builder is not None:
                catalog_view = builder()
        return {
            "name": name,
            "layers": layers,
            "layer": layer_id,
            "path": path,
            "present": block["present"],
            "editable": block["editable"],
            "reason": block["reason"],
            "block": block["text"],
            "model": model,
            "flat": flatten_section(document, path),
            "inherited": inherited,
            "validation": validation,
            "catalog": catalog_view,
        }

    def validate(self, name: str, layer_id: str, block: str) -> dict:
        """跑一次按层校验；只有登记了编解码的插件有随包校验器。"""
        if CODECS.get(name) is None:
            return {
                "available": False,
                "error": "这个插件没有随包校验器；保存时由 daemon 解析器整份检查",
            }
        if self.checker is None:
            return unavailable(
                "找不到 buttons_check；先构建它：cmake --build build --target "
                "buttons_check，或给 --check 指路"
            )
        try:
            return check_layer(self.checker, layer_id, block)
        except ToolError as error:
            return unavailable(str(error))

    # -- 写 -----------------------------------------------------------------

    def save(self, edits, base_hash: str) -> dict:
        """按修改项改整份配置，校验后原子替换；返回新的哈希。"""
        with self.lock:
            if self.checker is None:
                raise UserError(
                    "找不到 buttons_check，不能写入未校验的配置；先构建它："
                    "cmake --build build --target buttons_check"
                )
            document = self.read_document()
            if base_hash and _hash(document) != base_hash:
                raise StaleError("配置文件在磁盘上改过了；先重新读一遍再保存")
            if not isinstance(edits, list) or not edits:
                raise UserError("没有要保存的修改")
            if len(edits) > MAX_EDITS:
                raise UserError("修改项太多")

            updated = document
            layer_hint = ""
            for edit in edits:
                if not isinstance(edit, dict):
                    raise UserError("修改项不是对象")
                kind = edit.get("kind")
                if kind == "plugin":
                    name = _need_string(edit.get("name"), "插件名", 32)
                    if not TOKEN_RE.match(name):
                        raise UserError(f"插件名不是合法标识 {name!r}")
                    codec = CODECS.get(name)
                    if codec is None:
                        raise UserError(f"插件 {name} 没有结构化编辑器")
                    layer = _need_string(edit.get("layer"), "层", 128)
                    try:
                        block = codec["serialize"](edit.get("items"))
                    except ModelError as error:
                        raise UserError(str(error)) from error
                    checked = self.validate(name, layer, block)
                    if checked.get("available") and not checked.get("yaml_ok"):
                        raise UserError(
                            checked.get("yaml_error")
                            or f"plugins.{name} 这一段还不能被宿主读进去"
                        )
                    try:
                        if layer.startswith("controllers."):
                            updated = ensure_controller(
                                updated, layer[len("controllers.") :]
                            )
                        updated = write_block(
                            updated, plugin_path(name, layer), block
                        )
                    except LayerError as error:
                        raise UserError(str(error)) from error
                    layer_hint = layer
                elif kind == "controller":
                    mac = _need_string(edit.get("mac"), "MAC", 64)
                    try:
                        updated = ensure_controller(updated, mac)
                    except LayerError as error:
                        raise UserError(str(error)) from error
                elif kind in ("scalar", "remove"):
                    path = _need_key_path(edit.get("path"))
                    try:
                        if kind == "scalar":
                            updated = set_scalar(
                                updated, path, _need_string(edit.get("value"), "取值", 128)
                            )
                        else:
                            updated = delete_path(updated, path)
                    except LayerError as error:
                        raise UserError(str(error)) from error
                else:
                    raise UserError(f"不认识的修改类型 {kind!r}")

            whole = check_document(self.checker, updated, layer_hint)
            if whole is not None:
                raise UserError("改完的整份配置读不通：" + whole)

            self.config_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            try:
                _atomic_write(self.config_path, updated)
            except OSError as error:
                raise UserError(f"写不进去：{error}") from error
            return {
                "ok": True,
                "hash": _hash(updated),
                "restart": "sudo systemctl restart ds5dummy",
            }


class Handler(BaseHTTPRequestHandler):
    """一个极小路由：页面、state、plugin、validate、save。"""

    server_version = "ds5dummy-buttons"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002 - 父类签名
        if getattr(self.server, "verbose", False):
            sys.stderr.write("%s - %s\n" % (self.address_string(), format % args))

    # -- 工具 ---------------------------------------------------------------

    @property
    def app(self) -> App:
        return self.server.app

    def _allowed_hosts(self) -> set[str]:
        port = self.server.server_address[1]
        return {f"127.0.0.1:{port}", f"localhost:{port}"}

    def _host_ok(self) -> bool:
        host = self.headers.get("Host")
        return not host or host in self._allowed_hosts()

    def _send(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'none'; style-src 'unsafe-inline'; "
            "script-src 'self' 'unsafe-inline'; connect-src 'self'; img-src data:; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
        )
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    def _error(self, status: int, message: str) -> None:
        self._json(status, {"ok": False, "error": message})

    def _read_json(self) -> dict:
        if self.headers.get("Transfer-Encoding"):
            raise UserError("不接受分块请求")
        length_text = self.headers.get("Content-Length")
        if length_text is None:
            raise UserError("缺少 Content-Length")
        try:
            length = int(length_text)
        except ValueError as error:
            raise UserError("Content-Length 不是数字") from error
        if length < 0 or length > MAX_CONFIG_BYTES:
            raise UserError("请求太大")
        body = self.rfile.read(length)
        try:
            data = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise UserError(f"请求不是 JSON：{error}") from error
        if not isinstance(data, dict):
            raise UserError("请求必须是 JSON 对象")
        return data

    # -- 路由 ---------------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802 - 父类约定
        if not self._host_ok():
            self._error(403, "Host 不对")
            return
        parsed = urlsplit(self.path)
        route = parsed.path.lstrip("/")
        if route == "":
            body = self.app.html.encode("utf-8")
            self._send(200, body, "text/html; charset=utf-8")
            return
        if route == "api/state":
            try:
                self._json(200, self.app.state())
            except (LayerError, UserError) as error:
                self._error(400, str(error))
            return
        if route == "api/plugin":
            query = parse_qs(parsed.query)
            name = query.get("name", [""])[0]
            layer = query.get("layer", [""])[0]
            try:
                self._json(200, self.app.plugin_state(name, layer))
            except (LayerError, UserError) as error:
                self._error(400, str(error))
            return
        if route.startswith("panel/") and route.endswith(".js"):
            name = route[len("panel/") : -len(".js")]
            self._send_panel(name)
            return
        self._error(404, "没找到")

    def _send_panel(self, name: str) -> None:
        """把一份面板脚本发给页面；只发插件目录里登记过的那几个文件。"""
        if not TOKEN_RE.match(name):
            self._error(404, "没找到")
            return
        path = self.app.panel_script(name)
        if path is None:
            self._error(404, f"没有插件 {name} 的面板")
            return
        try:
            body = path.read_bytes()
        except OSError as error:
            self._error(404, f"读不了面板 {name}：{error}")
            return
        if len(body) > MAX_PANEL_BYTES:
            self._error(413, f"面板 {name} 太大")
            return
        self._send(200, body, "text/javascript; charset=utf-8")

    def do_POST(self) -> None:  # noqa: N802 - 父类约定
        if not self._host_ok():
            self._error(403, "Host 不对")
            return
        origin = self.headers.get("Origin")
        if origin and origin not in {
            "http://" + host for host in self._allowed_hosts()
        }:
            self._error(403, "Origin 不对")
            return
        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith("application/json"):
            self._error(415, "只接受 application/json")
            return
        parsed = urlsplit(self.path)
        route = parsed.path.lstrip("/")
        try:
            request = self._read_json()
        except UserError as error:
            self._error(400, str(error))
            return

        if route == "api/validate":
            try:
                name = _need_string(request.get("name"), "插件名", 32)
                layer = _need_string(request.get("layer"), "层", 128)
                codec = CODECS.get(name)
                if codec is None:
                    raise UserError(f"插件 {name} 没有结构化校验器")
                block = codec["serialize"](request.get("items"))
                validation = self.app.validate(name, layer, block)
            except (ModelError, UserError) as error:
                self._error(400, str(error))
                return
            self._json(200, {"ok": True, "block": block, "validation": validation})
            return

        if route == "api/save":
            base_hash = request.get("hash", "")
            try:
                result = self.app.save(request.get("edits"), str(base_hash))
            except StaleError as error:
                self._error(409, str(error))
                return
            except (ModelError, LayerError, UserError) as error:
                self._error(400, str(error))
                return
            self._json(200, result)
            return
        self._error(404, "没找到")


class Server(ThreadingHTTPServer):
    """回环地址上的多线程服务；保存由 App.lock 串行化。"""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, app: App, verbose: bool):
        super().__init__(address, handler)
        self.app = app
        self.verbose = verbose


def main(argv: list[str] | None = None) -> int:
    """命令行入口：解析参数、开监听、打印地址、打开浏览器。"""
    parser = argparse.ArgumentParser(
        description="DS5Dummy 用户配置的本地页面（只监听 127.0.0.1）"
    )
    parser.add_argument(
        "--config",
        default=None,
        help="要编辑的用户配置（缺省 $XDG_CONFIG_HOME/ds5dummy/config.yaml）",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"监听端口（缺省 {DEFAULT_PORT}：页面就是这个固定地址）",
    )
    parser.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    parser.add_argument("--check", default=None, help="buttons_check 的路径")
    parser.add_argument(
        "--plugins-dir",
        action="append",
        default=[],
        help="额外的插件目录（可重复；给了就只扫这些，不扫宿主的那两个）",
    )
    parser.add_argument("--verbose", action="store_true", help="把每个请求打到 stderr")
    args = parser.parse_args(argv)

    if not 0 <= args.port <= 65535:
        print("buttons_web: --port 要在 0..65535 之间", file=sys.stderr)
        return 2

    script_dir = Path(__file__).resolve().parent
    page = script_dir / "buttons_web.html"
    if not page.is_file():
        print(f"buttons_web: 找不到页面 {page}", file=sys.stderr)
        return 2
    html = page.read_text(encoding="utf-8")

    config = Path(args.config).expanduser() if args.config else default_config_path()
    checker = find_checker(args.check, script_dir)
    if checker is None:
        print(
            "buttons_web: 没找到 buttons_check，页面可以浏览，但保存会被拒绝；"
            "先构建：cmake --build build --target buttons_check",
            file=sys.stderr,
        )
    if args.plugins_dir:
        plugin_dirs = [Path(item).expanduser() for item in args.plugins_dir]
    else:
        plugin_dirs = default_plugin_dirs()

    app = App(config, checker, html, script_dir, plugin_dirs, args.verbose)
    try:
        server = Server(("127.0.0.1", args.port), Handler, app, args.verbose)
    except OSError as error:
        print(f"buttons_web: 监听 127.0.0.1:{args.port} 失败：{error}", file=sys.stderr)
        return 1

    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/"
    print(f"配置: {config}")
    if checker:
        print(f"校验器: {checker}")
    print("插件目录: " + " · ".join(str(item) for item in plugin_dirs))
    print(f"页面: {url}")
    print("按 Ctrl-C 结束。", flush=True)
    if not args.no_open:
        threading.Thread(target=webbrowser.open, args=(url,), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
