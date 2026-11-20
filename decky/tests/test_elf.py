#!/usr/bin/env python3
"""ELF `.modinfo` 解析与 vermagic 比较（A-04）。"""

from __future__ import annotations

import unittest

import support
from ds5dummy import elf


class ModuleReleaseTest(unittest.TestCase):
    """P-13 的查找键：vermagic 的第一个词就是 release。"""

    def test_takes_first_word(self) -> None:
        self.assertEqual(
            elf.module_release(support.VERMAGIC), support.RELEASE
        )

    def test_blank_input_is_empty(self) -> None:
        self.assertEqual(elf.module_release("   "), "")


class ReadModinfoTest(unittest.TestCase):
    """从自造的 ELF64 里读出 `.modinfo`。"""

    def test_reads_fields(self) -> None:
        fields = elf.read_modinfo(support.make_elf())
        self.assertEqual(fields[elf.VERMAGIC_KEY], support.VERMAGIC)
        self.assertEqual(fields["name"], "hid_playstation_ds5dummy")

    def test_custom_entries(self) -> None:
        blob = support.make_elf({elf.VERMAGIC_KEY: "9.9.9-test SMP"})
        self.assertEqual(
            elf.module_release(elf.read_modinfo(blob)[elf.VERMAGIC_KEY]), "9.9.9-test"
        )

    def test_rejects_non_elf(self) -> None:
        with self.assertRaises(elf.ElfError):
            elf.read_modinfo(b"definitely not an elf file" * 8)

    def test_rejects_truncated(self) -> None:
        with self.assertRaises(elf.ElfError):
            elf.read_modinfo(support.make_elf()[:40])

    def test_rejects_missing_modinfo(self) -> None:
        # 把节名表里的 ".modinfo"（第 11 字节起）改成别的名字，节还在但名字对不上。
        blob = bytearray(support.make_elf())
        index = blob.find(b".modinfo")
        self.assertGreater(index, 0)
        blob[index : index + 8] = b".otherinfo"[:8]
        with self.assertRaises(elf.ElfError):
            elf.read_modinfo(bytes(blob))


if __name__ == "__main__":
    unittest.main()
