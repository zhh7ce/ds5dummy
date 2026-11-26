#!/usr/bin/env python3
"""`decky/pack.py` 的 ZIP 形状、版本号与插件内布局。

不跑 npm、不跑 cmake：`stage_plugin` 只读 `decky/` 下的插件源码、前端产物和一份
已经铺好的树，所以这里把两者都指到临时搭的目录上，验证 loader 真正认的那几样
东西——入口文件名、版本号所在的 `package.json`、`py_modules/` 的位置、整棵便携树
嵌在 `bin/ds5dummy/` 下——以及同样的输入压出的 ZIP 逐字节相同。
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import tempfile
import unittest
import zipfile
from pathlib import Path

import support  # 把 decky/ 放进 sys.path，下面才能 import pack
import pack


class VersionTest(unittest.TestCase):
    """release tag 到版本号的换算：去掉 `v`，其余字符必须能进文件名。"""

    def test_strips_v_prefix(self) -> None:
        """`v` 前缀只是 tag 的写法，版本号里不带它。"""
        self.assertEqual(pack.normalize_version("v0.2.0"), "0.2.0")

    def test_accepts_bare_version(self) -> None:
        """已经是版本号的输入原样通过。"""
        self.assertEqual(pack.normalize_version("0.2.0"), "0.2.0")

    def test_rejects_empty(self) -> None:
        """只有一个 `v` 的 tag 没有版本号可用。"""
        with self.assertRaises(SystemExit):
            pack.normalize_version("v")

    def test_rejects_path_characters(self) -> None:
        """斜杠与 `..` 会破坏 ZIP 内路径，直接拒绝。"""
        with self.assertRaises(SystemExit):
            pack.normalize_version("v1.0/../x")


class _StagedTree(unittest.TestCase):
    """搭一份最小的 `decky/` 与一棵最小的便携树，供 staging 测试共用。"""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.decky = root / "decky"
        self.tree = root / "tree"

        (self.decky / "ds5dummy").mkdir(parents=True)
        (self.decky / "ds5dummy" / "__init__.py").write_text("", encoding="utf-8")
        (self.decky / "ds5dummy" / "__pycache__").mkdir()
        (self.decky / "ds5dummy" / "__pycache__" / "junk.pyc").write_bytes(b"x")
        (self.decky / "plugin.json").write_text(
            '{"name": "DS5Dummy", "author": "t", "publish": {"description": "d"}}\n',
            encoding="utf-8",
        )
        (self.decky / "main.py").write_text("", encoding="utf-8")
        (self.decky / "README.md").write_text("", encoding="utf-8")
        (self.decky / "dist").mkdir()
        (self.decky / "dist" / "index.js").write_text("// bundle\n", encoding="utf-8")

        (self.tree / "bin").mkdir(parents=True)
        binary = self.tree / "bin" / "ds5dummyd"
        binary.write_bytes(b"\x7fELF")
        binary.chmod(0o755)
        for name in ("buttons_check", "ds5dummy-config"):
            script = self.tree / "bin" / name
            script.write_bytes(b"\x7fELF")
            script.chmod(0o755)
        (self.tree / "lib").mkdir()
        (self.tree / "lib" / "libyaml-cpp.so.0.9").write_bytes(b"")
        (self.tree / "plugins").mkdir()
        (self.tree / "plugins" / "idle.so").write_bytes(b"")
        (self.tree / "etc").mkdir()
        (self.tree / "etc" / "config.yaml").write_text("", encoding="utf-8")
        docs = self.tree / "share" / "doc" / "ds5dummy"
        docs.mkdir(parents=True)
        (docs / "buttons.md").write_text("", encoding="utf-8")

        self._saved = pack.DECKY_DIR
        pack.DECKY_DIR = self.decky

    def tearDown(self) -> None:
        pack.DECKY_DIR = self._saved


class StageTest(_StagedTree):
    """ZIP 里的目录形状：loader 认的入口名、版本位置、`py_modules/`、嵌套的树。"""

    def _stage(self) -> Path:
        staging = Path(self._tmp.name) / "staging"
        staging.mkdir()
        return pack.stage_plugin(staging, self.tree, "1.2.3")

    def test_top_directory_comes_from_plugin_json(self) -> None:
        """官方 CLI 用 plugin.json 的 name 作顶层目录名，loader 按它认插件。"""
        self.assertEqual(self._stage().name, "DS5Dummy")

    def test_entry_and_metadata(self) -> None:
        """loader 写死的入口名与它读版本号的位置都是承重的。"""
        plugin = self._stage()
        self.assertTrue((plugin / "main.py").is_file())
        self.assertTrue((plugin / "LICENSE").is_file())
        package = json.loads((plugin / "package.json").read_text(encoding="utf-8"))
        self.assertEqual(package["version"], "1.2.3")
        self.assertEqual(package["type"], "module")
        self.assertEqual((plugin / "VERSION").read_text(encoding="utf-8").strip(), "1.2.3")

    def test_python_package_goes_under_py_modules(self) -> None:
        """loader 只把 `py_modules` 加进 `sys.path`。"""
        plugin = self._stage()
        self.assertTrue((plugin / "py_modules" / "ds5dummy" / "__init__.py").is_file())
        self.assertFalse((plugin / "py_modules" / "ds5dummy" / "__pycache__").exists())

    def test_frontend_and_nested_tree(self) -> None:
        """前端在 `dist/`，整棵便携树在 `bin/ds5dummy/` 且形状不变。"""
        plugin = self._stage()
        self.assertTrue((plugin / "dist" / "index.js").is_file())
        tree = plugin / "bin" / "ds5dummy"
        self.assertTrue(os.access(tree / "bin" / "ds5dummyd", os.X_OK))
        self.assertTrue((tree / "lib" / "libyaml-cpp.so.0.9").is_file())
        self.assertTrue((tree / "plugins" / "idle.so").is_file())
        self.assertTrue((tree / "etc" / "config.yaml").is_file())
        self.assertTrue((tree / "share" / "doc" / "ds5dummy" / "buttons.md").is_file())

    def test_missing_frontend_fails(self) -> None:
        """没有 `dist/index.js` 就点名失败，不打出一个没面板的 ZIP。"""
        (self.decky / "dist" / "index.js").unlink()
        with self.assertRaises(SystemExit):
            self._stage()


class ZipTest(_StagedTree):
    """权限位带进 ZIP，且同样的输入压出逐字节相同的文件。"""

    def test_modes_and_determinism(self) -> None:
        """权限位取自文件本身；同样的输入压出逐字节相同的 ZIP。"""
        staging = Path(self._tmp.name) / "staging"
        staging.mkdir()
        plugin = pack.stage_plugin(staging, self.tree, "1.2.3")
        first = Path(self._tmp.name) / "a.zip"
        second = Path(self._tmp.name) / "b.zip"
        pack.write_zip(plugin, first)
        pack.write_zip(plugin, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

        with zipfile.ZipFile(first) as bundle:
            info = bundle.getinfo("DS5Dummy/bin/ds5dummy/bin/ds5dummyd")
            self.assertEqual(stat.S_IMODE(info.external_attr >> 16), 0o755)
            self.assertIn("DS5Dummy/dist/index.js", bundle.namelist())


class CheckTreeTest(_StagedTree):
    """便携性校验：不合格的树不许打成 ZIP。"""

    def test_rejects_missing_yaml_cpp(self) -> None:
        """没有随包 yaml-cpp 的树在别的机器上起不来。"""
        shutil.rmtree(self.tree / "lib")
        (self.tree / "lib").mkdir()
        with self.assertRaises(SystemExit):
            pack.check_tree(self.tree)

    def test_rejects_non_elf_binary(self) -> None:
        """`readelf` 读不动说明这不是便携构建（或二进制被换过）。"""
        (self.tree / "bin" / "ds5dummyd").write_text("not an elf", encoding="utf-8")
        with self.assertRaises(SystemExit):
            pack.check_tree(self.tree)


if __name__ == "__main__":
    unittest.main()
