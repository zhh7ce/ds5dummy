#!/usr/bin/env python3
"""tools/buttons_web.py 的单测：层几何、模型往返、词表对账与回环服务。

全部用例不需要 root、手柄或 PipeWire，也不读真实的用户配置目录：配置一律写在
临时目录里，服务监听 127.0.0.1 的内核分配端口。需要 PyYAML——与页面服务本身
同一个依赖。

跑法：python3 tools/test_buttons_web.py
"""

from __future__ import annotations

import ast
import http.client
import json
import re
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import yaml

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import buttons_web as web  # noqa: E402


#: 一份两层的样例：global 是默认，controllers 里的那个 MAC 覆盖写了的键。
#: 这一节之外的注释与空行必须原样保留。
SAMPLE = """\
# 头部注释：这一段必须逐字节保留
enabled: true
bt_addr: auto

global:
  plugins:
    buttons:
      remap:
        square: triangle
        l1: r1+keep
        cross: triangle
      turbo:
        l3: 20

controllers:
  "aa:bb:cc:dd:ee:ff":
    usb_model: edge
    plugins:
      buttons:
        remap:
          circle: cross
"""

CONTROLLER = "controllers.aa:bb:cc:dd:ee:ff"

#: 每一项：层、层内的原文、以及只改这一处之后的文本。
CHANGE_CASES = [
    ("global", "square: triangle", "square: cross"),
    (CONTROLLER, "circle: cross", "circle: square"),
]


def layer_path(layer_id: str) -> list[str]:
    """`buttons` 插件的一个层标识 → YAML 路径。"""
    return web.plugin_path("buttons", layer_id)


def outside(document: str, layer_id: str) -> tuple[list[str], list[str]]:
    """这一层之外的行：键行之前、块尾之后。"""
    lines, _ = web.split_document(document)
    analysis = web.analyze(document, layer_path(layer_id))
    return lines[: analysis.key_line], lines[analysis.end_line + 1 :]


class LayerGeometryTest(unittest.TestCase):
    """按行读写一层 `plugins.buttons`。"""

    def test_plugin_path_covers_the_two_layers(self):
        self.assertEqual(
            layer_path("global"), ["global", "plugins", "buttons"]
        )
        self.assertEqual(
            layer_path(CONTROLLER),
            ["controllers", "aa:bb:cc:dd:ee:ff", "plugins", "buttons"],
        )

    def test_plugin_path_rejects_bad_names_and_layers(self):
        for plugin, layer in (
            ("../evil", "global"),
            ("", "global"),
            ("buttons", "plugins"),
            ("buttons", "controllers"),
            ("buttons", "defaults"),
        ):
            with self.assertRaises(web.LayerError, msg=(plugin, layer)):
                web.plugin_path(plugin, layer)

    def test_reads_both_layers_indented(self):
        self.assertEqual(
            web.read_block(SAMPLE, layer_path("global"))["text"],
            "remap:\n  square: triangle\n  l1: r1+keep\n  cross: triangle\n"
            "turbo:\n  l3: 20",
        )
        self.assertEqual(
            web.read_block(SAMPLE, layer_path(CONTROLLER))["text"],
            "remap:\n  circle: cross",
        )

    def test_absent_layer_reads_empty(self):
        block = web.read_block("enabled: true\n", layer_path("global"))
        self.assertFalse(block["present"])
        self.assertTrue(block["editable"])
        self.assertEqual(block["text"], "")

    def test_same_text_leaves_the_document_byte_identical(self):
        for layer_id in ("global", CONTROLLER):
            block = web.read_block(SAMPLE, layer_path(layer_id))["text"]
            self.assertEqual(
                web.write_block(SAMPLE, layer_path(layer_id), block), SAMPLE
            )

    def test_a_change_stays_inside_the_section(self):
        for layer_id, old, new in CHANGE_CASES:
            block = web.read_block(SAMPLE, layer_path(layer_id))["text"]
            self.assertIn(old, block)
            updated = web.write_block(
                SAMPLE, layer_path(layer_id), block.replace(old, new)
            )
            self.assertIn(new, updated)
            self.assertEqual(
                outside(updated, layer_id), outside(SAMPLE, layer_id)
            )

    def test_empty_block_deletes_the_key_only(self):
        updated = web.write_block(SAMPLE, layer_path("global"), "")
        self.assertFalse(web.analyze(updated, layer_path("global")).present)
        self.assertEqual(
            web.read_block(updated, layer_path(CONTROLLER))["text"],
            web.read_block(SAMPLE, layer_path(CONTROLLER))["text"],
        )
        # 空的 `plugins:` 是"本层不覆盖"，宿主接受，所以不删祖先键行。
        self.assertIn("global:\n  plugins:\n", updated)

    def test_missing_global_chain_is_created(self):
        document = "enabled: true\n"
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            "enabled: true\n"
            "global:\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n",
        )
        self.assertEqual(
            web.read_block(updated, layer_path("global"))["text"],
            "remap:\n  circle: cross",
        )

    def test_missing_plugins_chain_goes_inside_global(self):
        document = "global:\n  audio: true\n"
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            "global:\n"
            "  audio: true\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n",
        )

    def test_missing_controller_is_created(self):
        """已连接但还没配过的手柄：这一层还不在文件里，也能直接写。"""
        layer = "controllers.11:22:33:44:55:66"
        updated = web.write_block(
            SAMPLE, layer_path(layer), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            SAMPLE + "  11:22:33:44:55:66:\n"
            "    plugins:\n"
            "      buttons:\n"
            "        remap:\n"
            "          circle: cross\n",
        )
        self.assertEqual(
            web.read_block(updated, layer_path(layer))["text"],
            "remap:\n  circle: cross",
        )
        # 原来的两层一字未动。
        self.assertEqual(
            web.read_block(updated, layer_path("global"))["text"],
            web.read_block(SAMPLE, layer_path("global"))["text"],
        )

    def test_flow_style_is_refused(self):
        document = (
            "global:\n  plugins:\n    buttons: {remap: {circle: cross}}\n"
        )
        analysis = web.analyze(document, layer_path("global"))
        self.assertFalse(analysis.editable)
        self.assertIn("行内", analysis.reason)
        with self.assertRaises(web.LayerError):
            web.write_block(
                document, layer_path("global"), "remap:\n  circle: cross"
            )

    def test_flow_root_cannot_gain_keys(self):
        document = "{enabled: true}\n"
        self.assertFalse(
            web.analyze(document, layer_path("global")).editable
        )
        with self.assertRaises(web.LayerError):
            web.write_block(
                document, layer_path("global"), "remap:\n  circle: cross"
            )

    def test_sequence_section_is_refused(self):
        document = "global:\n  plugins:\n    buttons:\n      - remap\n"
        self.assertFalse(
            web.analyze(document, layer_path("global")).editable
        )

    def test_comments_are_not_part_of_the_indentation(self):
        """注释的列号没有语义：夹在中间、缩进不齐的 `#` 行不挡住编辑。"""
        document = (
            "global:\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n"
            "     # 夹在两个子键之间、缩进不齐的注释\n"
            "      turbo:\n"
            "        l3: 20\n"
        )
        analysis = web.analyze(document, layer_path("global"))
        self.assertTrue(analysis.editable)
        block = web.read_block(document, layer_path("global"))["text"]
        self.assertIn("缩进不齐的注释", block)
        self.assertEqual(
            web.write_block(document, layer_path("global"), block), document
        )

    def test_template_comments_under_global_do_not_block_editing(self):
        """随包模板把说明写在 `global:` 下的第 0 列；这一节必须可编辑。"""
        document = (
            "enabled: true\n"
            "bt_addr: auto\n"
            "\n"
            "global:\n"
            "#   audio: true\n"
            "#   bt_wait_s: 60\n"
            "  usb_report_interval_ms: 4\n"
            "\n"
            "# controllers:\n"
            '#   "AA:BB:CC:DD:EE:FF":\n'
            "#     audio: false\n"
        )
        section = web.analyze(document, ["global"])
        self.assertTrue(section.editable)
        self.assertEqual(section.child_indent, 2)
        analysis = web.analyze(document, layer_path("global"))
        self.assertTrue(analysis.editable)
        self.assertEqual(
            web.read_block(document, ["global"])["text"],
            "#   audio: true\n#   bt_wait_s: 60\nusb_report_interval_ms: 4",
        )
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            "enabled: true\n"
            "bt_addr: auto\n"
            "\n"
            "global:\n"
            "#   audio: true\n"
            "#   bt_wait_s: 60\n"
            "  usb_report_interval_ms: 4\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n"
            "\n"
            "# controllers:\n"
            '#   "AA:BB:CC:DD:EE:FF":\n'
            "#     audio: false\n",
        )

    def test_comment_only_global_still_gains_the_plugins_chain(self):
        """`global:` 下一个键都没写、只剩模板注释时，新子树插在键行之后。"""
        document = "global:\n#   audio: true\n"
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            "global:\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n"
            "#   audio: true\n",
        )

    def test_empty_section_can_gain_children(self):
        document = "global:\n  plugins:\n    buttons:\n"
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  circle: cross"
        )
        self.assertEqual(
            updated,
            "global:\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n",
        )

    def test_trailing_newline_is_preserved_both_ways(self):
        document = SAMPLE.rstrip("\n")
        updated = web.write_block(
            document, layer_path("global"), "remap:\n  square: cross"
        )
        self.assertTrue(updated.endswith("circle: cross"))
        self.assertFalse(updated.endswith("\n"))

        updated = web.write_block(
            SAMPLE, layer_path("global"), "remap:\n  square: cross"
        )
        self.assertTrue(updated.endswith("circle: cross\n"))

    def test_list_layers_orders_controllers_by_mac(self):
        document = SAMPLE.replace(
            'controllers:\n  "aa:bb:cc:dd:ee:ff":',
            'controllers:\n  "ff:ee:dd:cc:bb:aa":\n    usb_model: auto\n'
            '    plugins:\n      buttons:\n        remap:\n          circle: cross\n'
            '  "aa:bb:cc:dd:ee:ff":',
        )
        layers = web.list_layers(document, "buttons")
        self.assertEqual(
            [layer["id"] for layer in layers],
            ["global", "controllers.aa:bb:cc:dd:ee:ff",
             "controllers.ff:ee:dd:cc:bb:aa"],
        )
        self.assertTrue(all(layer["present"] for layer in layers))

    def test_list_layers_lists_every_controller_in_the_file(self):
        layers = web.list_layers(SAMPLE, "idle")
        self.assertEqual([layer["id"] for layer in layers], ["global", CONTROLLER])
        self.assertFalse(layers[0]["present"])
        self.assertFalse(layers[1]["present"])

    def test_list_layers_marks_controllers_without_this_plugin(self):
        document = SAMPLE.replace(
            'controllers:\n  "aa:bb:cc:dd:ee:ff":',
            'controllers:\n  "11:22:33:44:55:66":\n    audio: false\n'
            '  "aa:bb:cc:dd:ee:ff":',
        )
        layers = web.list_layers(document, "buttons")
        self.assertEqual(
            [layer["id"] for layer in layers],
            ["global", "controllers.11:22:33:44:55:66", CONTROLLER],
        )
        self.assertEqual([layer["present"] for layer in layers],
                         [True, False, True])

    def test_list_layers_adds_connected_pads_missing_from_the_file(self):
        """连着但文件里没有的手柄也要能选中；保存时才写出这一层。"""
        layers = web.list_layers(
            SAMPLE, "buttons", ["11:22:33:44:55:66", "aa:bb:cc:dd:ee:ff"]
        )
        self.assertEqual(
            [layer["id"] for layer in layers],
            ["global", CONTROLLER, "controllers.11:22:33:44:55:66"],
        )
        self.assertEqual(
            [(layer["configured"], layer["connected"]) for layer in layers],
            [(True, False), (True, True), (False, True)],
        )
        self.assertFalse(layers[2]["present"])

    def test_list_layers_skips_connected_garbage(self):
        layers = web.list_layers(SAMPLE, "idle", ["", "not-a-mac"])
        self.assertEqual([layer["id"] for layer in layers], ["global", CONTROLLER])

    def test_layer_entries_reports_connect_and_configure(self):
        entries = web.layer_entries(
            SAMPLE,
            [
                {"mac": "aa:bb:cc:dd:ee:ff", "model": "DualSense Edge"},
                {"mac": "11:22:33:44:55:66", "model": "DualSense"},
            ],
        )
        self.assertEqual(
            [(entry["id"], entry["configured"], entry["connected"],
              entry["model"]) for entry in entries],
            [
                ("global", True, False, ""),
                (CONTROLLER, True, True, "DualSense Edge"),
                ("controllers.11:22:33:44:55:66", False, True, "DualSense"),
            ],
        )

    def test_controller_mac_of_normalizes_and_rejects(self):
        self.assertEqual(
            web._controller_mac_of("controllers.aa:bb:cc:dd:ee:ff"),
            "AA:BB:CC:DD:EE:FF",
        )
        for layer_id in ("global", "controllers", "controllers.nope",
                         "controllers.aa:bb:cc:dd:ee"):
            self.assertIsNone(web._controller_mac_of(layer_id), layer_id)

    def test_unknown_layer_id_is_rejected(self):
        for layer_id in ("plugins", "controllers"):
            with self.assertRaises(web.LayerError):
                layer_path(layer_id)


class ModelTest(unittest.TestCase):
    """页面看到的规则模型：解析、序列化与往返。"""

    BLOCK = """\
remap:
  cross: triangle
  l1: r1+keep
  dpad_up: disable
turbo:
  l3: 20
shortcut:
  '1': 'l3+r3: power_off'
  '2': 'create~: power_off'
  '3': 'l1*2: power_off'
  '4': 'r1>500: power_off'
macro:
  '5': 'swipe_up: cross+circle'
axis:
  invert_lstick_x: true
  center_rstick: always
  swap_lstick_rstick: x
typo:
  key: value
"""

    def test_parse_model_classifies_every_group(self):
        items = web.parse_model(self.BLOCK)["items"]
        by_path = {".".join(item["path"]): item for item in items}
        self.assertEqual(len(items), 13)

        remap = by_path["remap.cross"]
        self.assertEqual(
            (remap["kind"], remap["src"], remap["target"], remap["keep"],
             remap["disable"]),
            ("remap", "cross", "triangle", False, False),
        )
        self.assertTrue(by_path["remap.l1"]["keep"])
        self.assertTrue(by_path["remap.dpad_up"]["disable"])

        self.assertEqual(by_path["turbo.l3"]["hz"], 20)

        self.assertEqual(by_path["shortcut.1"]["timing"], "chord")
        self.assertEqual(by_path["shortcut.1"]["terms"], ["l3", "r3"])
        self.assertEqual(by_path["shortcut.2"]["timing"], "tap")
        self.assertEqual(by_path["shortcut.3"]["timing"], "double")
        self.assertEqual(by_path["shortcut.4"]["timing"], "long")
        self.assertEqual(by_path["shortcut.4"]["ms"], 500)

        macro = by_path["macro.5"]
        self.assertEqual(macro["terms"], ["swipe_up"])
        self.assertEqual(macro["targets"], ["cross", "circle"])

        self.assertTrue(by_path["axis.invert_lstick_x"]["invert"])
        self.assertEqual(by_path["axis.center_rstick"]["stick"], "rstick")
        self.assertEqual(by_path["axis.swap_lstick_rstick"]["value"], "x")

        other = by_path["typo.key"]
        self.assertEqual((other["kind"], other["path"], other["value"]),
                         ("other", ["typo", "key"], "value"))

    def test_model_round_trip_is_stable(self):
        items = web.parse_model(self.BLOCK)["items"]
        text = web.serialize_model(items)
        again = web.parse_model(text)["items"]
        self.assertEqual(again, items)
        self.assertEqual(web.serialize_model(again), text)

    def test_empty_model_serializes_to_empty_text(self):
        self.assertEqual(web.serialize_model([]), "")
        self.assertEqual(
            web.parse_model(""), {"structured": True, "reason": "", "items": []}
        )

    def test_serialize_orders_groups(self):
        text = web.serialize_model(
            [
                {"kind": "axis_swap", "value": "both"},
                {"kind": "remap", "src": "circle", "target": "cross"},
            ]
        )
        self.assertLess(text.index("remap:"), text.index("axis:"))

    def test_duplicate_source_keeps_the_last_value(self):
        text = web.serialize_model(
            [
                {"kind": "remap", "src": "circle", "target": "cross"},
                {"kind": "remap", "src": "circle", "target": "square"},
            ]
        )
        self.assertIn("circle: square", text)
        self.assertNotIn("circle: cross", text)

    def test_unknown_forms_stay_editable_text(self):
        items = web.parse_model(
            "remap:\n  circle: lstick_up\nturbo:\n  l3: fast\n"
        )["items"]
        self.assertTrue(all(item["kind"] == "other" for item in items))

    def test_non_mapping_block_is_not_structured(self):
        self.assertFalse(web.parse_model("- remap\n- turbo")["structured"])

    def test_trigger_suffixes_round_trip(self):
        cases = [
            (["l1", "r1"], "chord", None, "l1+r1"),
            (["l3"], "tap", web.DEFAULT_TAP_MS, "l3~"),
            (["create"], "double", web.DEFAULT_DOUBLE_MS, "create*2"),
            (["r1"], "long", 750, "r1>750"),
        ]
        for terms, timing, ms, text in cases:
            parsed = web._parse_trigger(text)
            self.assertEqual(
                (parsed["terms"], parsed["timing"], parsed["ms"]),
                (terms, timing, ms),
            )
            self.assertEqual(web._trigger_text(terms, timing, ms), text)

    def test_trigger_rejects_bad_suffixes(self):
        for text in ("l3+", "nope", "l1>10", "l1>abc", "l1~+r1", "l1*3", ""):
            self.assertIsNone(web._parse_trigger(text), text)

    def test_model_rejects_unknown_tokens_and_ranges(self):
        bad_items = [
            {"kind": "remap", "src": "nope", "target": "cross"},
            {"kind": "remap", "src": "circle", "target": "lstick_up"},
            {"kind": "turbo", "src": "l3", "hz": True},
            {"kind": "shortcut", "id": "1", "terms": ["l3"], "timing": "long",
             "ms": 10},
            {"kind": "macro", "id": "1", "terms": ["l3"], "timing": "chord",
             "targets": ["pad_left"]},
            {"kind": "axis_invert", "axis": "l3", "invert": True},
            {"kind": "axis_swap", "value": "z"},
            {"kind": "other", "path": ["a"] * 9, "value": "x"},
            {"kind": "nope"},
        ]
        for item in bad_items:
            with self.assertRaises(web.ModelError):
                web.serialize_model([item])


def cxx_tokens(source: str, name: str) -> list[str]:
    """从一个 `constexpr std::string_view <name>[] = {...};` 表里取字符串。"""
    match = re.search(
        r"constexpr std::string_view " + re.escape(name) + r"\[\] = \{(.*?)\};",
        source,
        re.S,
    )
    if match is None:
        raise AssertionError(f"ids.cpp 里找不到 {name}")
    return re.findall(r'"([^"]*)"', match.group(1))


class CatalogTest(unittest.TestCase):
    """页面词表必须与 plugin/buttons/ids.cpp 的 token 表同序、同拼写。"""

    @classmethod
    def setUpClass(cls):
        cls.ids = (REPO / "plugin" / "buttons" / "ids.cpp").read_text(
            encoding="utf-8"
        )

    def test_source_tokens_match_ids_cpp(self):
        self.assertEqual(
            web.SOURCE_TOKENS, cxx_tokens(self.ids, "kSourceTokens")
        )
        self.assertEqual(len(web.SOURCE_TOKENS), 40)

    def test_axis_and_stick_tokens_match_ids_cpp(self):
        self.assertEqual(web.AXIS_TOKENS, cxx_tokens(self.ids, "kAxisTokens"))
        self.assertEqual(web.STICK_TOKENS, cxx_tokens(self.ids, "kStickTokens"))

    def test_derived_slices_follow_the_id_space(self):
        self.assertEqual(web.WIRE_TOKENS, web.SOURCE_TOKENS[:23])
        self.assertEqual(web.REMAP_SOURCE_TOKENS, web.SOURCE_TOKENS[:36])
        self.assertEqual(
            web.TURBO_TOKENS,
            [token for token in web.WIRE_TOKENS
             if token not in ("l2", "r2", "pad")],
        )

    def test_catalog_flags_match_the_slices(self):
        data = web.catalog()
        sources = data["sources"]
        self.assertEqual([source["token"] for source in sources],
                         web.SOURCE_TOKENS)
        self.assertEqual(
            [source["token"] for source in sources if source["wire"]],
            web.WIRE_TOKENS,
        )
        self.assertEqual(
            [source["token"] for source in sources if source["turbo"]],
            web.TURBO_TOKENS,
        )
        self.assertEqual(
            [source["token"] for source in sources if source["remap_source"]],
            web.REMAP_SOURCE_TOKENS,
        )
        self.assertEqual(
            [source["token"] for source in sources if source["swipe"]],
            web.SOURCE_TOKENS[36:],
        )
        self.assertEqual(data["ranges"]["turbo"], [1, 60])
        self.assertEqual(data["ranges"]["long_ms"], [50, 10000])
        self.assertEqual([timing["id"] for timing in data["timings"]],
                         ["chord", "tap", "double", "long"])


#: 插件目录里的按键映射面板；它带着 63 字节解码表的第二处写法。
PANEL_JS = REPO / "plugin" / "buttons" / "buttons.panel.js"


def page_script() -> str:
    """读面板脚本：它是 63 字节解码表的第二处写法。"""
    return PANEL_JS.read_text(encoding="utf-8")


def js_block(script: str, name: str) -> str:
    """一个 `const <name> = [ ... ];` 表的内容。"""
    match = re.search(
        r"const\s+" + re.escape(name) + r"\s*=\s*\[(.*?)\n\];", script, re.S
    )
    if match is None:
        raise AssertionError(f"buttons.panel.js 里找不到 {name}")
    return match.group(1)


def js_const(script: str, name: str) -> int:
    """一个 `const <name> = <整数>;` 常量。"""
    match = re.search(
        r"const\s+" + re.escape(name) + r"\s*=\s*(-?\d+)\s*;", script
    )
    if match is None:
        raise AssertionError(f"buttons.panel.js 里找不到 {name}")
    return int(match.group(1))


def js_hat_tokens(script: str) -> list[list[str]]:
    """页面的 hat 表：每个 hat 值对应哪几个方向 token。"""
    block = js_block(script, "HAT_TOKENS")
    return [
        re.findall(r"'([^']*)'", row)
        for row in re.findall(r"\[([^\]]*)\]", block)
    ]


def js_wire_bits(script: str) -> list[tuple[str, int, int]]:
    """页面的线上位表：token、状态体偏移、掩码。"""
    rows = re.findall(
        r"\['([^']+)',\s*(\d+),\s*(0x[0-9A-Fa-f]+)\]",
        js_block(script, "WIRE_BITS"),
    )
    return [(token, int(offset), int(mask, 16)) for token, offset, mask in rows]


def js_shape_tokens(script: str, name: str) -> list[str]:
    """`PAD_SHAPES` / `STICK_ARROWS` 那种带 `token:'…'` 的表。"""
    return re.findall(r"token:'([^']+)'", js_block(script, name))


def js_string_list(script: str, name: str) -> list[str]:
    """`TOUCH_TOKENS` 那种纯字符串表。"""
    return re.findall(r"'([^']+)'", js_block(script, name))


def js_stick_axes(script: str) -> list[tuple[str, int, int]]:
    """`STICK_AXES`：摇杆名与它 X/Y 轴在状态体里的下标。"""
    rows = re.findall(
        r"\{name:'([^']+)',\s*x:(\d+),\s*y:(\d+)\}",
        js_block(script, "STICK_AXES"),
    )
    return [(name, int(x), int(y)) for name, x, y in rows]


def cxx_enum(source: str, opening: str) -> list[str]:
    """一个 `enum class ... {` 里的枚举名，按声明顺序（注释里的名字不算）。"""
    start = source.index(opening)
    end = source.index("\n};", start)
    names = []
    for line in source[start:end].splitlines():
        match = re.match(r"\s*(k[A-Z]\w*)\s*,", line)
        if match is not None:
            names.append(match.group(1))
    return names


_INT_OPS = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a // b,  # 只出现在正数常量上，与 C++ 的截断同值
    ast.Mod: lambda a, b: a % b,
    ast.LShift: lambda a, b: a << b,
    ast.RShift: lambda a, b: a >> b,
    ast.BitOr: lambda a, b: a | b,
    ast.BitAnd: lambda a, b: a & b,
    ast.BitXor: lambda a, b: a ^ b,
}

_INT_UNARY = {
    ast.USub: lambda a: -a,
    ast.UAdd: lambda a: a,
    ast.Invert: lambda a: ~a,
}


def cxx_int(expr: str) -> int:
    """求值 ids.h 里的整数常量表达式；只认整数常量与位运算。"""
    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in _INT_OPS:
            return _INT_OPS[type(node.op)](walk(node.left), walk(node.right))
        if isinstance(node, ast.UnaryOp) and type(node.op) in _INT_UNARY:
            return _INT_UNARY[type(node.op)](walk(node.operand))
        raise AssertionError(f"不认识的整数常量表达式：{expr}")

    return walk(ast.parse(expr, mode="eval"))


def cxx_constants(source: str) -> dict[str, int]:
    """顺序求值一份头文件里的整数 `constexpr`，后一项可以引用前一项。"""
    values: dict[str, int] = {}
    pattern = re.compile(
        r"constexpr\s+[A-Za-z0-9_:<>\s]+?\s+(\w+)\s*=\s*([^;]+);"
    )
    for match in pattern.finditer(source):
        name, expr = match.group(1), match.group(2).strip()
        expr = re.sub(r"(\d)[uUlL]+\b", r"\1", expr)
        for known, value in values.items():
            expr = re.sub(r"\b" + re.escape(known) + r"\b", str(value), expr)
        if not re.fullmatch(r"[0-9a-fA-FxX+\-*/%() <>&|^~]+", expr):
            continue
        values[name] = cxx_int(expr)
    return values


def cxx_table_body(source: str, opening: str) -> str:
    """一张 `constexpr ... <name>[] = { ... };` 表的内容（花括号之间）。"""
    start = source.index(opening) + len(opening)
    end = source.index("\n};", start)
    return source[start:end]


class PageTablesTest(unittest.TestCase):
    """页面那份 63 字节解码表必须与 plugin/buttons 的 C++ 表逐项一致。"""

    @classmethod
    def setUpClass(cls):
        cls.script = page_script()
        cls.ids_cpp = (REPO / "plugin" / "buttons" / "ids.cpp").read_text(
            encoding="utf-8"
        )
        cls.ids_h = (REPO / "plugin" / "buttons" / "ids.h").read_text(
            encoding="utf-8"
        )
        cls.constants = {
            **cxx_constants(cls.ids_h),
            **cxx_constants(cls.ids_cpp),
        }
        cls.source_tokens = cxx_tokens(cls.ids_cpp, "kSourceTokens")
        names = cxx_enum(cls.ids_h, "enum class Source : uint8_t {")
        cls.token_of_enum = dict(zip(names, cls.source_tokens))

    def test_source_enum_lines_up_with_the_token_table(self):
        names = cxx_enum(self.ids_h, "enum class Source : uint8_t {")
        self.assertEqual(len(names), 41)
        self.assertEqual(names[-1], "kCount")
        self.assertEqual(len(self.token_of_enum), 40)
        self.assertEqual(len(self.source_tokens), 40)

    def test_hat_tokens_match_the_hat_table(self):
        body = cxx_table_body(self.ids_cpp, "constexpr Mask kHatDirs[] = {")
        rows = [entry.strip() for entry in body.split(",") if entry.strip()]
        self.assertEqual(len(rows), 16)
        expected = [
            [self.token_of_enum[name] for name in re.findall(
                r"Source::(k\w+)", entry)]
            for entry in rows[:8]
        ]
        self.assertEqual(js_hat_tokens(self.script), expected)
        for entry in rows[8:]:
            self.assertEqual(re.findall(r"Source::(k\w+)", entry), [])
        # 页面用 `hat < 8` 判"未按下"，ids.cpp 的 kHatNone 必须是同一档。
        self.assertEqual(self.constants["kHatNone"], 8)
        self.assertIn("if (hat < 8)", self.script)

    def test_wire_bits_match_the_slot_table(self):
        body = cxx_table_body(self.ids_cpp, "constexpr Slot kSlots[] = {")

        def value_of(expr: str) -> int:
            expr = expr.strip()
            return self.constants[expr] if expr in self.constants else cxx_int(expr)

        slots = [
            (value_of(offset), value_of(mask))
            for offset, mask in re.findall(
                r"\{\s*([^,{}]+?)\s*,\s*([^,{}]+?)\s*\}", body)
        ]
        self.assertEqual(len(slots), 40)
        expected = [
            (token, offset, mask)
            for token, (offset, mask) in zip(self.source_tokens, slots)
            if mask != 0
        ]
        self.assertEqual(js_wire_bits(self.script), expected)
        # 十字键那四条不在单比特表里，走上面那张 hat 表。
        for token, (offset, mask) in zip(self.source_tokens[:4], slots[:4]):
            self.assertEqual((offset, mask), (0, 0), token)

    def test_stick_and_pad_geometry_match_ids_h(self):
        self.assertEqual(js_const(self.script, "STICK_CENTER"),
                         self.constants["kStickCenter"])
        self.assertEqual(js_const(self.script, "STICK_STEP"),
                         self.constants["kStickStep"])
        self.assertEqual(js_const(self.script, "STICK_HYST"),
                         self.constants["kStickHysteresis"])
        self.assertEqual(js_const(self.script, "PAD_MID_X"),
                         self.constants["kPadMidX"])
        self.assertEqual(js_const(self.script, "PAD_NEUTRAL_BAND"),
                         self.constants["kPadNeutralBand"])

    def test_pad_shapes_cover_exactly_the_wire_bits(self):
        shapes = js_shape_tokens(self.script, "PAD_SHAPES")
        self.assertEqual(len(shapes), len(set(shapes)))
        self.assertEqual(sorted(shapes), sorted(web.WIRE_TOKENS))

    def test_arrows_and_chips_cover_the_judged_forms(self):
        self.assertEqual(js_shape_tokens(self.script, "STICK_ARROWS"),
                         web.SOURCE_TOKENS[23:31])
        self.assertEqual(js_string_list(self.script, "TOUCH_TOKENS"),
                         web.SOURCE_TOKENS[31:40])

    def test_stick_axes_follow_the_axis_table(self):
        self.assertEqual([name for name, _, _ in js_stick_axes(self.script)],
                         web.STICK_TOKENS)
        for name, x, y in js_stick_axes(self.script):
            self.assertEqual(x, web.AXIS_TOKENS.index(name + "_x"))
            self.assertEqual(y, web.AXIS_TOKENS.index(name + "_y"))

    def test_touch_word_layout_matches_the_decoder(self):
        self.assertEqual(self.constants["kTouchOffset"], 32)
        self.assertEqual(self.constants["kContactNotTouchingMask"], 0x80)
        self.assertEqual(self.constants["kContactAxisMask"], 0xFFF)
        self.assertIn("for (const offset of [32, 36])", self.script)
        self.assertIn("word & 0x80", self.script)
        self.assertIn("(word >>> 8) & 0xfff", self.script)


class BasicConfigTest(unittest.TestCase):
    """基础配置的读写：改标量、删键、新手柄条目与只读视图。"""

    def test_set_scalar_keeps_the_comment(self):
        document = "enabled: true  # 这一行不许动\n"
        self.assertEqual(
            web.set_scalar(document, ["enabled"], "false"),
            "enabled: false  # 这一行不许动\n",
        )

    def test_set_scalar_quotes_only_when_needed(self):
        document = "bt_addr: auto\n"
        self.assertEqual(
            web.set_scalar(document, ["bt_addr"], "aa:bb"),
            "bt_addr: aa:bb\n",
        )
        self.assertEqual(
            web.set_scalar(document, ["bt_addr"], "on"),
            'bt_addr: "on"\n',
        )
        self.assertEqual(
            web.set_scalar(document, ["bt_addr"], "a b"),
            'bt_addr: "a b"\n',
        )
        with self.assertRaises(web.LayerError):
            web.set_scalar(document, ["bt_addr"], "")
        with self.assertRaises(web.LayerError):
            web.set_scalar(document, ["bt_addr"], "a\nb")

    def test_set_scalar_creates_the_missing_chain(self):
        updated = web.set_scalar("enabled: true\n", ["global", "audio"], "false")
        self.assertEqual(updated, "enabled: true\nglobal:\n  audio: false\n")

    def test_set_scalar_refuses_to_rewrite_a_section(self):
        with self.assertRaises(web.LayerError):
            web.set_scalar(SAMPLE, ["global", "plugins", "buttons"], "x")

    def test_delete_path_cascades_empty_parents(self):
        document = (
            "enabled: true\n"
            "global:\n"
            "  plugins:\n"
            "    buttons:\n"
            "      remap:\n"
            "        circle: cross\n"
        )
        updated = web.delete_path(
            document, ["global", "plugins", "buttons", "remap", "circle"]
        )
        self.assertNotIn("global:", updated)

    def test_delete_path_keeps_filled_parents(self):
        updated = web.delete_path(
            SAMPLE, ["controllers", "aa:bb:cc:dd:ee:ff", "usb_model"]
        )
        self.assertIn('"aa:bb:cc:dd:ee:ff":', updated)
        self.assertNotIn("usb_model", updated)

    def test_delete_path_of_a_missing_key_returns_the_original(self):
        self.assertEqual(web.delete_path(SAMPLE, ["nope"]), SAMPLE)

    def test_ensure_controller_appends_when_the_section_is_missing(self):
        updated = web.ensure_controller("enabled: true\n", "11:22:33:44:55:66")
        self.assertEqual(
            updated, "enabled: true\ncontrollers:\n  11:22:33:44:55:66:\n"
        )

    def test_ensure_controller_inserts_inside_an_existing_section(self):
        updated = web.ensure_controller(SAMPLE, "11:22:33:44:55:66")
        self.assertIn('"aa:bb:cc:dd:ee:ff":', updated)
        self.assertIn("  11:22:33:44:55:66:", updated)
        self.assertLess(
            updated.index("aa:bb:cc:dd:ee:ff"), updated.index("11:22")
        )

    def test_ensure_controller_is_idempotent_and_case_insensitive(self):
        self.assertEqual(
            web.ensure_controller(SAMPLE, "AA:BB:CC:DD:EE:FF"), SAMPLE
        )

    def test_ensure_controller_rejects_a_bad_mac(self):
        for mac in ("aa:bb", "aa-bb-cc-dd-ee-ff", "aa:bb:cc:dd:ee:fg", ""):
            with self.assertRaises(web.LayerError, msg=mac):
                web.ensure_controller(SAMPLE, mac)

    def test_basic_view_returns_the_raw_texts(self):
        view = web.basic_view(SAMPLE)
        self.assertEqual(view["error"], "")
        self.assertEqual(view["root"]["enabled"], "true")
        self.assertEqual(view["root"]["bt_addr"], "auto")
        self.assertIsNone(view["root"]["log_level"])
        self.assertEqual(
            view["global"], {key: None for key in web.HANDHELD_KEYS}
        )
        self.assertEqual(
            [item["mac"] for item in view["controllers"]],
            ["aa:bb:cc:dd:ee:ff"],
        )
        values = view["controllers"][0]["values"]
        self.assertEqual(values["usb_model"], "edge")
        self.assertIsNone(values["audio"])

    def test_basic_view_reports_broken_yaml(self):
        self.assertTrue(web.basic_view("plugins: [\n")["error"])


class PanelDiscoveryTest(unittest.TestCase):
    """插件目录扫描必须与宿主的装载口径一致。"""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def test_discovers_sidecar_panels_and_skips_other_files(self):
        (self.root / "buttons.so").write_bytes(b"")
        (self.root / "buttons.panel.js").write_text("// 面板\n", encoding="utf-8")
        (self.root / "idle.so").write_bytes(b"")
        (self.root / "notes.txt").write_text("x", encoding="utf-8")
        (self.root / "folder.so").mkdir()
        panels = web.discover_panels([self.root])
        self.assertEqual([panel.id for panel in panels], ["buttons", "idle"])
        self.assertEqual(
            panels[0].view(),
            {
                "id": "buttons",
                "so": str(self.root / "buttons.so"),
                "has_panel": True,
            },
        )
        self.assertFalse(panels[1].view()["has_panel"])

    def test_user_directory_overrides_the_system_one(self):
        system = self.root / "system"
        user = self.root / "user"
        system.mkdir()
        user.mkdir()
        (system / "buttons.so").write_bytes(b"")
        (system / "buttons.panel.js").write_text("// system\n", encoding="utf-8")
        (user / "buttons.so").write_bytes(b"")
        panels = web.discover_panels([system, user])
        self.assertEqual(len(panels), 1)
        self.assertEqual(panels[0].so, user / "buttons.so")
        self.assertIsNone(panels[0].script)

    def test_a_missing_directory_is_ignored(self):
        self.assertEqual(web.discover_panels([self.root / "nope"]), [])


class HttpTest(unittest.TestCase):
    """回环服务：页面、state、plugin、validate、save 与三道 POST 检查。"""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.config = self.root / "config.yaml"
        self.config.write_text(SAMPLE, encoding="utf-8")
        self.plugins = self.root / "plugins"
        self.plugins.mkdir()
        (self.plugins / "buttons.so").write_bytes(b"")
        (self.plugins / "buttons.panel.js").write_text(
            "// 按键面板\n", encoding="utf-8"
        )
        (self.plugins / "idle.so").write_bytes(b"")
        self.checker = web.find_checker(None, TOOLS)
        self.app = self.make_app(self.checker)
        self.server = web.Server(("127.0.0.1", 0), web.Handler, self.app, False)
        self.prefix = "/"
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True
        )
        self.thread.start()
        self.addCleanup(self.stop)

    def make_app(self, checker):
        return web.App(
            self.config,
            checker,
            "<html>页面</html>",
            TOOLS,
            [self.plugins],
            False,
        )

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)

    def request(self, method, path, payload=None, headers=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.port, timeout=15
        )
        try:
            body = None
            merged = {}
            if payload is not None:
                body = json.dumps(payload).encode("utf-8")
                merged["Content-Type"] = "application/json"
            merged.update(headers or {})
            connection.request(method, path, body=body, headers=merged)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def json_of(self, status_body):
        status, body = status_body
        return status, json.loads(body.decode("utf-8"))

    def plugin(self, name="buttons", layer=""):
        query = "name=" + name
        if layer:
            query += "&layer=" + layer
        return self.json_of(
            self.request("GET", self.prefix + "api/plugin?" + query)
        )

    def current_hash(self):
        status, state = self.json_of(
            self.request("GET", self.prefix + "api/state")
        )
        self.assertEqual(status, 200, state)
        return state["hash"]

    def test_page_is_served_at_the_root(self):
        status, body = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(body, "<html>页面</html>".encode("utf-8"))

    def test_no_one_shot_token_paths(self):
        self.assertEqual(self.request("GET", "/t/deadbeef/")[0], 404)
        self.assertEqual(self.request("GET", "/t/" + "0" * 32 + "/")[0], 404)

    def test_plugin_state_carries_its_catalog(self):
        """词表随插件节一起下发，面板不再自己发请求。"""
        status, data = self.plugin()
        self.assertEqual(status, 200, data)
        self.assertEqual(data["catalog"]["sources"][0]["token"], "dpad_up")
        status, data = self.plugin(name="idle")
        self.assertEqual(status, 200, data)
        self.assertIsNone(data["catalog"])
        self.assertEqual(
            self.request("GET", self.prefix + "api/catalog")[0], 404
        )

    def test_state_returns_basic_config_and_panels(self):
        status, data = self.json_of(
            self.request("GET", self.prefix + "api/state")
        )
        self.assertEqual(status, 200)
        self.assertEqual(
            [panel["id"] for panel in data["panels"]], ["buttons", "idle"]
        )
        self.assertEqual(
            [panel["has_panel"] for panel in data["panels"]], [True, False]
        )
        self.assertEqual(data["base"]["root"]["enabled"], "true")
        self.assertEqual(
            [item["mac"] for item in data["base"]["controllers"]],
            ["aa:bb:cc:dd:ee:ff"],
        )
        self.assertEqual(data["hash"], web._hash(SAMPLE))
        self.assertEqual(data["layers"][0]["id"], "global")
        self.assertIn(
            CONTROLLER, [entry["id"] for entry in data["layers"]]
        )
        self.assertIn("service", data["runtime"])
        self.assertIn("pads", data["runtime"])
        self.assertIn("udc", data["runtime"])
        self.assertIn("values", data["system"])
        self.assertIn("restart", data)

    def test_state_without_a_config_file_reads_empty(self):
        """用户配置还不存在时：层清单与空白插件节照样能读。"""
        self.config.unlink()
        status, state = self.json_of(
            self.request("GET", self.prefix + "api/state")
        )
        self.assertEqual(status, 200, state)
        self.assertFalse(state["exists"])
        self.assertEqual(state["layers"][0]["id"], "global")
        status, section = self.plugin()
        self.assertEqual(status, 200, section)
        self.assertFalse(section["present"])
        self.assertTrue(section["editable"])
        self.assertEqual(section["model"]["items"], [])

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_first_save_creates_the_config_file_and_its_directory(self):
        app = web.App(
            self.root / "deep" / "config.yaml",
            self.checker,
            "",
            TOOLS,
            [self.plugins],
            False,
        )
        app.save(
            [{"kind": "scalar", "path": ["enabled"], "value": "false"}], ""
        )
        created = self.root / "deep" / "config.yaml"
        self.assertEqual(
            yaml.safe_load(created.read_text(encoding="utf-8")),
            {"enabled": False},
        )
        self.assertEqual(created.parent.stat().st_mode & 0o777, 0o700)

    def test_plugin_state_serves_each_layer(self):
        status, data = self.plugin()
        self.assertEqual(status, 200, data)
        self.assertEqual(data["layer"], "global")
        self.assertEqual(data["path"], ["global", "plugins", "buttons"])
        self.assertTrue(data["present"])
        kinds = {item["kind"] for item in data["model"]["items"]}
        self.assertEqual(kinds, {"remap", "turbo"})
        self.assertEqual(
            [row[0] for row in data["flat"]],
            ["remap.square", "remap.l1", "remap.cross", "turbo.l3"],
        )
        # 全局层没有"继承"这回事：它自己就是默认值。
        self.assertEqual(data["inherited"], [])
        self.assertTrue(data["validation"]["yaml_ok"])

        status, data = self.plugin(layer=CONTROLLER)
        self.assertEqual(status, 200, data)
        self.assertEqual(
            data["path"],
            ["controllers", "aa:bb:cc:dd:ee:ff", "plugins", "buttons"],
        )
        self.assertEqual(data["model"]["items"][0]["src"], "circle")
        # 手柄层没写的扁平键由 global 同名键继承，面板用它显示"留空等于多少"。
        self.assertEqual(
            data["inherited"],
            [
                ["remap.square", "triangle"],
                ["remap.l1", "r1+keep"],
                ["remap.cross", "triangle"],
                ["turbo.l3", "20"],
            ],
        )

    def test_plugin_state_without_a_codec_is_flat_only(self):
        status, data = self.plugin(name="idle")
        self.assertEqual(status, 200, data)
        self.assertIsNone(data["model"])
        self.assertIsNone(data["validation"])
        self.assertFalse(data["present"])
        self.assertEqual(data["flat"], [])
        self.assertEqual(data["inherited"], [])
        self.assertEqual(data["path"], ["global", "plugins", "idle"])

    def test_plugin_state_rejects_unknown_layers_and_names(self):
        status, data = self.plugin(layer="nope")
        self.assertEqual(status, 400)
        self.assertFalse(data["ok"])
        status, data = self.plugin(name="../evil")
        self.assertEqual(status, 400)

    def test_plugin_state_accepts_a_handwritten_controller(self):
        """页面上手写的 MAC 不在文件里、也没连着，也要能读出一层空白。"""
        status, data = self.plugin(layer="controllers.de:ad:be:ef:00:01")
        self.assertEqual(status, 200, data)
        self.assertEqual(data["layer"], "controllers.DE:AD:BE:EF:00:01")
        self.assertEqual(
            data["path"],
            ["controllers", "DE:AD:BE:EF:00:01", "plugins", "buttons"],
        )
        self.assertFalse(data["present"])
        self.assertTrue(data["editable"])
        self.assertEqual(data["model"]["items"], [])
        self.assertIn(
            "controllers.DE:AD:BE:EF:00:01",
            [layer["id"] for layer in data["layers"]],
        )

    def test_panel_scripts_are_served_only_for_registered_names(self):
        status, body = self.request("GET", self.prefix + "panel/buttons.js")
        self.assertEqual(status, 200)
        self.assertEqual(body, "// 按键面板\n".encode("utf-8"))

        status, body = self.request("GET", self.prefix + "panel/core.js")
        self.assertEqual(status, 200)
        self.assertIn(b"DS5.define", body)

        self.assertEqual(
            self.request("GET", self.prefix + "panel/idle.js")[0], 404
        )
        self.assertEqual(
            self.request("GET", self.prefix + "panel/nope.js")[0], 404
        )

    def test_validate_serializes_and_reports(self):
        payload = {
            "name": "buttons",
            "layer": "global",
            "items": [
                {"kind": "remap", "src": "square", "target": "circle"},
                {"kind": "turbo", "src": "l3", "hz": 20},
            ],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/validate", payload)
        )
        self.assertEqual(status, 200)
        self.assertIn("square: circle", data["block"])
        self.assertIn("validation", data)

    def test_validate_rejects_a_bad_model_and_unknown_plugins(self):
        payload = {
            "name": "buttons",
            "layer": "global",
            "items": [{"kind": "remap", "src": "nope", "target": "cross"}],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/validate", payload)
        )
        self.assertEqual(status, 400)
        self.assertFalse(data["ok"])

        status, data = self.json_of(
            self.request(
                "POST",
                self.prefix + "api/validate",
                {"name": "idle", "layer": "global", "items": []},
            )
        )
        self.assertEqual(status, 400)

    def test_post_requires_json_and_same_origin(self):
        payload = {"name": "buttons", "layer": "global", "items": []}
        status, _ = self.request(
            "POST",
            self.prefix + "api/validate",
            payload,
            headers={"Content-Type": "text/plain"},
        )
        self.assertEqual(status, 415)

        status, _ = self.request(
            "POST",
            self.prefix + "api/validate",
            payload,
            headers={"Origin": "http://evil.example"},
        )
        self.assertEqual(status, 403)

        status, _ = self.request(
            "POST",
            self.prefix + "api/validate",
            payload,
            headers={"Host": "evil.example"},
        )
        self.assertEqual(status, 403)

    def test_save_refuses_without_a_checker(self):
        app = self.make_app(None)
        with self.assertRaises(web.UserError):
            app.save([], "")

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_save_without_final_newline_keeps_the_last_line(self):
        self.config.write_text(SAMPLE.rstrip("\n"), encoding="utf-8")
        payload = {
            "hash": self.current_hash(),
            "edits": [
                {
                    "kind": "plugin",
                    "name": "buttons",
                    "layer": CONTROLLER,
                    "items": [
                        {"kind": "remap", "src": "circle", "target": "square"}
                    ],
                }
            ],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/save", payload)
        )
        self.assertEqual(status, 200, data)
        saved = self.config.read_text(encoding="utf-8")
        self.assertTrue(saved.endswith("circle: square"))
        self.assertFalse(saved.endswith("\n"))

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_save_writes_only_its_section_and_detects_stale_hash(self):
        base_hash = self.current_hash()
        status, section = self.plugin()
        self.assertEqual(status, 200, section)
        items = section["model"]["items"]
        for item in items:
            if item["kind"] == "remap" and item["src"] == "square":
                item["target"] = "circle"

        payload = {
            "hash": base_hash,
            "edits": [
                {
                    "kind": "plugin",
                    "name": "buttons",
                    "layer": "global",
                    "items": items,
                }
            ],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/save", payload)
        )
        self.assertEqual(status, 200, data)
        self.assertTrue(data["ok"])

        saved = self.config.read_text(encoding="utf-8")
        self.assertIn("square: circle", saved)
        self.assertEqual(outside(saved, "global"), outside(SAMPLE, "global"))

        status, current = self.json_of(
            self.request("GET", self.prefix + "api/state")
        )
        self.assertEqual(current["hash"], data["hash"])

        status, data = self.json_of(
            self.request("POST", self.prefix + "api/save", payload)
        )
        self.assertEqual(status, 409)
        self.assertFalse(data["ok"])

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_save_applies_scalar_remove_and_controller_edits(self):
        payload = {
            "hash": self.current_hash(),
            "edits": [
                {"kind": "scalar", "path": ["enabled"], "value": "false"},
                {
                    "kind": "scalar",
                    "path": ["global", "haptics_gain"],
                    "value": "1.5",
                },
                {"kind": "controller", "mac": "11:22:33:44:55:66"},
                {
                    "kind": "scalar",
                    "path": ["controllers", "11:22:33:44:55:66", "usb_model"],
                    "value": "dualsense",
                },
                {
                    "kind": "remove",
                    "path": ["controllers", "aa:bb:cc:dd:ee:ff", "usb_model"],
                },
            ],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/save", payload)
        )
        self.assertEqual(status, 200, data)

        saved = yaml.safe_load(self.config.read_text(encoding="utf-8"))
        self.assertIs(saved["enabled"], False)
        self.assertEqual(saved["global"]["haptics_gain"], 1.5)
        self.assertEqual(
            saved["controllers"]["11:22:33:44:55:66"]["usb_model"], "dualsense"
        )
        self.assertNotIn(
            "usb_model", saved["controllers"]["aa:bb:cc:dd:ee:ff"]
        )
        self.assertIn("plugins", saved["controllers"]["aa:bb:cc:dd:ee:ff"])
        self.assertIn("# 头部注释：这一段必须逐字节保留",
                      self.config.read_text(encoding="utf-8"))

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_save_into_a_handwritten_controller_creates_the_entry(self):
        payload = {
            "hash": self.current_hash(),
            "edits": [
                {
                    "kind": "plugin",
                    "name": "buttons",
                    "layer": "controllers.11:22:33:44:55:66",
                    "items": [
                        {"kind": "remap", "src": "circle", "target": "cross"}
                    ],
                }
            ],
        }
        status, data = self.json_of(
            self.request("POST", self.prefix + "api/save", payload)
        )
        self.assertEqual(status, 200, data)

        saved = yaml.safe_load(self.config.read_text(encoding="utf-8"))
        self.assertEqual(
            saved["controllers"]["11:22:33:44:55:66"]["plugins"]["buttons"]
            ["remap"]["circle"],
            "cross",
        )
        # 原来那只手柄原样保留。
        self.assertEqual(
            saved["controllers"]["aa:bb:cc:dd:ee:ff"]["usb_model"], "edge"
        )

    @unittest.skipUnless(
        web.find_checker(None, TOOLS) is not None,
        "需要 buttons_check：cmake --build build --target buttons_check",
    )
    def test_save_rejects_bad_edits_without_touching_the_file(self):
        base_hash = self.current_hash()
        for edit in (
            {"kind": "nope"},
            {"kind": "controller", "mac": "aa:bb:cc"},
            {"kind": "scalar", "path": ["nope"], "value": "x"},
            {"kind": "scalar", "path": ["enabled"], "value": ""},
        ):
            status, data = self.json_of(
                self.request(
                    "POST",
                    self.prefix + "api/save",
                    {"hash": base_hash, "edits": [edit]},
                )
            )
            self.assertEqual(status, 400, (edit, data))
            self.assertEqual(
                self.config.read_text(encoding="utf-8"), SAMPLE
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
