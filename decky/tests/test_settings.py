#!/usr/bin/env python3
"""设置的解析、夹取与原子写回（C-05、L-02、E-02）。"""

from __future__ import annotations

import json
import unittest

import support
from ds5dummy import settings


class ParseTest(unittest.TestCase):
    """坏字段按缺省值处理，不让面板因为一份坏 JSON 消失。"""

    def test_defaults(self) -> None:
        parsed, warnings = settings.parse({})
        self.assertFalse(parsed.enabled)
        self.assertEqual(parsed.udc_num, settings.UDC_DEFAULT)
        self.assertEqual(warnings, [])

    def test_clamps_udc(self) -> None:
        parsed, warnings = settings.parse({"udc_num": 99})
        self.assertEqual(parsed.udc_num, settings.UDC_MAX)
        self.assertEqual(len(warnings), 1)

        parsed, _ = settings.parse({"udc_num": 0})
        self.assertEqual(parsed.udc_num, settings.UDC_MIN)

    def test_rejects_wrong_types(self) -> None:
        parsed, warnings = settings.parse({"enabled": "yes", "udc_num": "three"})
        self.assertFalse(parsed.enabled)
        self.assertEqual(parsed.udc_num, settings.UDC_DEFAULT)
        self.assertEqual(len(warnings), 2)

    def test_non_object(self) -> None:
        parsed, warnings = settings.parse([1, 2, 3])
        self.assertFalse(parsed.enabled)
        self.assertEqual(len(warnings), 1)


class LoadSaveTest(unittest.TestCase):
    """文件缺失是首装的正常状态；保存走临时文件再改名。"""

    def test_missing_file_is_silent(self) -> None:
        fake = support.FakeSystem()
        parsed, warnings = settings.load(fake, "/settings/settings.json")
        self.assertFalse(parsed.enabled)
        self.assertEqual(warnings, [])

    def test_bad_json_warns(self) -> None:
        fake = support.FakeSystem()
        fake.files["/settings/settings.json"] = "{not json"
        parsed, warnings = settings.load(fake, "/settings/settings.json")
        self.assertFalse(parsed.enabled)
        self.assertEqual(len(warnings), 1)

    def test_round_trip(self) -> None:
        fake = support.FakeSystem()
        original = settings.Settings(enabled=True, udc_num=5)
        settings.save(fake, "/settings/settings.json", original)

        self.assertEqual(
            json.loads(fake.files["/settings/settings.json"]),
            {"enabled": True, "udc_num": 5},
        )
        self.assertNotIn("/settings/settings.json.tmp", fake.files)
        loaded, warnings = settings.load(fake, "/settings/settings.json")
        self.assertEqual(loaded, original)
        self.assertEqual(warnings, [])


if __name__ == "__main__":
    unittest.main()
