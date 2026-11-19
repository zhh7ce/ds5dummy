#!/usr/bin/env python3
"""scripts/kernel_source.py 的单测：版本→标签换算、地址拼装与严格打补丁。

联网部分（`download`）只是 `steamos_repo.fetch` 的循环，不在这里造网络；这里
覆盖的是能决定成败的两件事——标签算得对不对、补丁贴得严不严。跑法：
python3 scripts/tests/test_kernel_source.py
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))

import kernel_source as source  # noqa: E402


class UpstreamTagTest(unittest.TestCase):
    """头文件包版本到 kernel.org 标签的换算。"""

    def test_stable_point_release(self) -> None:
        self.assertEqual(source.upstream_tag("6.1.52.valve19-1"), "v6.1.52")
        self.assertEqual(source.upstream_tag("6.11.11.valve28-1"), "v6.11.11")
        self.assertEqual(source.upstream_tag("6.18.53-1"), "v6.18.53")
        self.assertEqual(source.upstream_tag("7.2.7.valve2-2"), "v7.2.7")
        self.assertEqual(source.upstream_tag("7.3.1.valve3-1"), "v7.3.1")

    def test_release_tag_has_two_segments(self) -> None:
        # 稳定树没有 v6.5.0 / v5.13.0 这两个标签，发布标签是两段式。
        self.assertEqual(source.upstream_tag("6.5.0.valve23-1"), "v6.5")
        self.assertEqual(source.upstream_tag("5.13.0.valve37-1"), "v5.13")

    def test_suffix_variants_use_base_version(self) -> None:
        self.assertEqual(source.upstream_tag("6.16.12.valve4.drmexec3-1"), "v6.16.12")
        self.assertEqual(source.upstream_tag("6.16.12.valve24.5-4"), "v6.16.12")

    def test_epoch_is_ignored(self) -> None:
        self.assertEqual(source.upstream_tag("1:6.18.53-1"), "v6.18.53")

    def test_unparseable_version(self) -> None:
        with self.assertRaises(source.KernelSourceError):
            source.upstream_tag("linux-neptune-72-headers")

    def test_source_url(self) -> None:
        self.assertEqual(
            source.source_url("drivers/hid/hid-ids.h", "v7.2.7", "https://example.test/mirror"),
            "https://example.test/mirror/v7.2.7/drivers/hid/hid-ids.h",
        )


class ApplyPatchTest(unittest.TestCase):
    """`patch -p1 --fuzz=0` 的成败边界。"""

    #: 三段上下文的最小补丁；与真实补丁同样带 `a/`、`b/` 前缀。
    PATCH = (
        "--- a/hid-playstation.c\n"
        "+++ b/hid-playstation.c\n"
        "@@ -1,3 +1,4 @@\n"
        " alpha\n"
        "+inserted\n"
        " beta\n"
        " gamma\n"
    )

    def _root(self) -> tempfile.TemporaryDirectory[str]:
        return tempfile.TemporaryDirectory(prefix="kernel-source-test-")

    def _write_patch(self, root: Path) -> Path:
        patch_file = root / "ds5dummy.patch"
        patch_file.write_text(self.PATCH, encoding="utf-8")
        return patch_file

    def test_applies_exact_context(self) -> None:
        with self._root() as tmp:
            root = Path(tmp)
            target = root / "hid-playstation.c"
            target.write_text("alpha\nbeta\ngamma\n", encoding="utf-8")
            source.apply_patch(root, self._write_patch(root))
            self.assertEqual(target.read_text(encoding="utf-8"),
                             "alpha\ninserted\nbeta\ngamma\n")

    def test_rejects_mismatched_context(self) -> None:
        # 上下文对不上时 --fuzz=0 直接失败：这正是上游漂移的报警方式。
        with self._root() as tmp:
            root = Path(tmp)
            (root / "hid-playstation.c").write_text("alpha\nBETA\ngamma\n", encoding="utf-8")
            with self.assertRaises(source.KernelSourceError):
                source.apply_patch(root, self._write_patch(root))

    def test_rejects_already_applied(self) -> None:
        # 重试前必须重新下载覆盖（见 download 的文档）；第二次直接打会失败。
        with self._root() as tmp:
            root = Path(tmp)
            (root / "hid-playstation.c").write_text("alpha\nbeta\ngamma\n", encoding="utf-8")
            patch_file = self._write_patch(root)
            source.apply_patch(root, patch_file)
            with self.assertRaises(source.KernelSourceError):
                source.apply_patch(root, patch_file)


class MainTest(unittest.TestCase):
    """命令行入口的离线路径。"""

    def test_dry_run_prints_tag_and_urls(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = source.main([
                "--version", "6.5.0.valve23-1",
                "--patch", "unused.patch",
                "--into", "/tmp/module",
                "--dry-run",
            ])
        self.assertEqual(code, 0)
        text = out.getvalue()
        self.assertIn("6.5.0.valve23-1 -> v6.5", text)
        self.assertIn("/v6.5/drivers/hid/hid-playstation.c", text)
        self.assertIn("/v6.5/drivers/hid/hid-ids.h", text)

    def test_missing_patch_file_fails(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = source.main([
                "--version", "7.2.7.valve2-2",
                "--patch", "/nonexistent/ds5dummy.patch",
                "--into", "/tmp/module",
            ])
        self.assertEqual(code, 2)
        self.assertIn("补丁文件不存在", err.getvalue())


if __name__ == "__main__":
    unittest.main()
