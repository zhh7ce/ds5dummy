#!/usr/bin/env python3
"""`decky/pack.py` 的 ZIP 形状、版本号与解包路径（T-02）。

不解真的 pacman 包、也不跑 npm：`stage_plugin` 只读 `DECKY_DIR` 下的插件源码与前端
产物，所以这里把 `DECKY_DIR` 指到临时搭的树上，验证 loader 真正认的那几样东西——
入口文件名、版本号所在的 `package.json`、`py_modules/` 的位置、`bin/` 的执行位——
以及同样的输入压出的 ZIP 逐字节相同。

`extract_package` 用 `bsdtar`，本机没有就跳过那一项；它是打包脚本声明的依赖，不是
测试环境该自备的东西。
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import tarfile
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
    """搭一棵最小的 `DECKY_DIR` 与便携分发树，供 staging 测试共用。"""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.decky = root / "decky"
        self.payload = root / "usr" / "lib" / "ds5dummy"

        (self.decky / "ds5dummy").mkdir(parents=True)
        (self.decky / "ds5dummy" / "__init__.py").write_text("", encoding="utf-8")
        (self.decky / "ds5dummy" / "__pycache__").mkdir()
        (self.decky / "ds5dummy" / "__pycache__" / "junk.pyc").write_bytes(b"x")
        (self.decky / "plugin.json").write_text('{"name": "x"}\n', encoding="utf-8")
        (self.decky / "main.py").write_text("", encoding="utf-8")
        (self.decky / "README.md").write_text("", encoding="utf-8")
        (self.decky / "dist").mkdir()
        (self.decky / "dist" / "index.js").write_text("// bundle\n", encoding="utf-8")

        (self.payload / "bin").mkdir(parents=True)
        binary = self.payload / "bin" / "ds5dummyd"
        binary.write_bytes(b"\x7fELF")
        binary.chmod(0o755)
        (self.payload / "lib").mkdir()
        (self.payload / "lib" / "libfoo.so").write_bytes(b"")
        (self.payload / "plugins").mkdir()
        (self.payload / "plugins" / "idle.so").write_bytes(b"")
        docs = self.payload / "share" / "doc" / "ds5dummy"
        docs.mkdir(parents=True)
        (docs / "buttons.md").write_text("", encoding="utf-8")

        self._saved = pack.DECKY_DIR
        pack.DECKY_DIR = self.decky

    def tearDown(self) -> None:
        pack.DECKY_DIR = self._saved


class StageTest(_StagedTree):
    """ZIP 里的目录形状：loader 认的入口名、版本位置、`py_modules/`。"""

    def _stage(self) -> Path:
        staging = Path(self._tmp.name) / "staging"
        staging.mkdir()
        return pack.stage_plugin(staging, self.payload, "1.2.3")

    def test_entry_and_metadata(self) -> None:
        """loader 写死的入口名与它读版本号的位置都是承重的。"""
        plugin = self._stage()
        self.assertTrue((plugin / "main.py").is_file())
        package = json.loads((plugin / "package.json").read_text(encoding="utf-8"))
        self.assertEqual(package["version"], "1.2.3")
        self.assertEqual(package["type"], "module")
        self.assertEqual((plugin / "VERSION").read_text(encoding="utf-8").strip(), "1.2.3")

    def test_python_package_goes_under_py_modules(self) -> None:
        """loader 只把 `py_modules` 加进 `sys.path`。"""
        plugin = self._stage()
        self.assertTrue((plugin / "py_modules" / "ds5dummy" / "__init__.py").is_file())
        self.assertFalse((plugin / "py_modules" / "ds5dummy" / "__pycache__").exists())

    def test_frontend_and_payload(self) -> None:
        """前端 bundle、执行位与便携分发都要落到插件根下。"""
        plugin = self._stage()
        self.assertTrue((plugin / "dist" / "index.js").is_file())
        self.assertTrue(os.access(plugin / "bin" / "ds5dummyd", os.X_OK))
        self.assertTrue((plugin / "plugins" / "idle.so").is_file())
        self.assertTrue((plugin / "share" / "doc" / "ds5dummy" / "buttons.md").is_file())

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
        plugin = pack.stage_plugin(staging, self.payload, "1.2.3")
        first = Path(self._tmp.name) / "a.zip"
        second = Path(self._tmp.name) / "b.zip"
        pack.write_zip(plugin, first)
        pack.write_zip(plugin, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

        with zipfile.ZipFile(first) as bundle:
            info = bundle.getinfo("ds5dummy/bin/ds5dummyd")
            self.assertEqual(stat.S_IMODE(info.external_attr >> 16), 0o755)
            self.assertIn("ds5dummy/dist/index.js", bundle.namelist())


class CollectTest(_StagedTree):
    """输入包缺东西时点名失败，不让坏包静默打成半个 ZIP。"""

    def test_accepts_complete_payload(self) -> None:
        """三样都在、二进制可执行时放行。"""
        self.assertEqual(pack.collect_payload(self.payload.parent.parent.parent), self.payload)

    def test_rejects_missing_binary(self) -> None:
        """没有 `bin/ds5dummyd` 说明输入包不是便携构建。"""
        (self.payload / "bin" / "ds5dummyd").unlink()
        with self.assertRaises(SystemExit):
            pack.collect_payload(self.payload.parent.parent.parent)

    def test_rejects_binary_without_exec_bit(self) -> None:
        """二进制没有执行位时装到 Deck 上也起不来。"""
        (self.payload / "bin" / "ds5dummyd").chmod(0o644)
        with self.assertRaises(SystemExit):
            pack.collect_payload(self.payload.parent.parent.parent)


class ExtractTest(unittest.TestCase):
    """相对路径的输入包：`bsdtar` 以解包目录为工作目录跑，路径必须先解析。"""

    @unittest.skipUnless(shutil.which("bsdtar"), "需要 bsdtar")
    def test_relative_package_path(self) -> None:
        """调用方给的相对路径必须按当前目录解析，而不是按解包目录。"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "sample.bin"
            source.write_bytes(b"payload")
            package = root / "sample.pkg.tar"
            with tarfile.open(package, "w") as archive:
                archive.add(source, arcname="bin/sample.bin")
            destination = root / "out"
            destination.mkdir()

            previous = os.getcwd()
            os.chdir(root)
            try:
                pack.extract_package(Path("sample.pkg.tar"), destination)
            finally:
                os.chdir(previous)
            self.assertTrue((destination / "bin" / "sample.bin").is_file())


if __name__ == "__main__":
    unittest.main()
