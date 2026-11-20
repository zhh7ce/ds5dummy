#!/usr/bin/env python3
"""解析顺序、三种失败与缓存维护（M-01～M-09）。"""

from __future__ import annotations

import importlib.util
import os
import sys
import unittest
from pathlib import Path

import support
from ds5dummy import elf
from ds5dummy import resolve as module
from ds5dummy.system import DownloadHttpError, DownloadNetworkError


class PathTest(unittest.TestCase):
    """地址与缓存路径是设备端唯一的两处拼法。"""

    def test_url(self) -> None:
        self.assertEqual(
            module.module_url("6.1.0-x", "jupiter-3.10"),
            "https://github.com/zhh7ce/ds5dummy/releases/download/jupiter-3.10/6.1.0-x.ko",
        )

    def test_cache_path(self) -> None:
        self.assertEqual(
            module.cache_path("/data/kmod", "6.1.0-x"),
            os.path.join("/data/kmod", "6.1.0-x", "6.1.0-x.ko"),
        )


class ChannelListTest(unittest.TestCase):
    """设备侧与发布侧的频道清单必须同集：发布侧加频道时，设备端不能还试旧名单。"""

    def test_matches_publish_side(self) -> None:
        scripts = Path(__file__).resolve().parents[2] / "scripts"
        path = scripts / "kmod_plan.py"
        sys.path.insert(0, str(scripts))
        try:
            spec = importlib.util.spec_from_file_location("kmod_plan_channels", path)
            self.assertIsNotNone(spec)
            publisher = importlib.util.module_from_spec(spec)
            # 模块里有 dataclass（字符串注解要在 sys.modules 里找回自己）。
            sys.modules[spec.name] = publisher
            try:
                spec.loader.exec_module(publisher)  # type: ignore[union-attr]
            finally:
                del sys.modules[spec.name]
        finally:
            sys.path.remove(str(scripts))
        self.assertEqual(
            set(module.CHANNEL_TAGS), set(publisher.DISCOVERY_CHANNELS)
        )


class ResolveTest(unittest.TestCase):
    """缓存优先，远端兜底，失败三分为网络 / 404 / 文件不对。"""

    def setUp(self) -> None:
        self.fake = support.FakeSystem()
        self.root = "/data/kmod"
        self.release = self.fake.release

    def test_cache_hit_does_not_download(self) -> None:
        path = os.path.join(self.root, self.release, self.release + ".ko")
        self.fake.blobs[path] = support.make_elf()
        self.fake.mkdirs(os.path.dirname(path))

        resolved = module.resolve(self.fake, self.root, self.release)

        self.assertEqual(resolved.path, path)
        self.assertEqual(resolved.source, module.SOURCE_CACHE)
        self.assertEqual(self.fake.commands, [])

    def test_bad_cache_entry_is_dropped_and_refetched(self) -> None:
        path = os.path.join(self.root, self.release, self.release + ".ko")
        self.fake.blobs[path] = b"garbage" * 16
        self.fake.mkdirs(os.path.dirname(path))
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        self.fake.downloads[url] = support.make_elf()

        resolved = module.resolve(self.fake, self.root, self.release)

        self.assertEqual(resolved.source, module.SOURCE_REMOTE)
        self.assertEqual(self.fake.blobs[path], support.make_elf())

    def test_404_is_no_artifact(self) -> None:
        for tag in module.CHANNEL_TAGS:
            self.fake.downloads[module.module_url(self.release, tag)] = (
                DownloadHttpError(404)
            )
        with self.assertRaises(module.Unavailable) as caught:
            module.resolve(self.fake, self.root, self.release)
        self.assertEqual(caught.exception.reason, module.REASON_NO_ARTIFACT)
        # 说明里要能看出"所有频道都查过"，而不是只报了第一个 tag。
        self.assertIn(str(len(module.CHANNEL_TAGS)), caught.exception.detail)
        part = os.path.join(self.root, ".tmp", self.release + ".ko.part")
        self.assertFalse(self.fake.exists(part))

    def test_falls_back_to_later_channel(self) -> None:
        first, second = module.CHANNEL_TAGS[0], module.CHANNEL_TAGS[1]
        self.fake.downloads[module.module_url(self.release, first)] = (
            DownloadHttpError(404)
        )
        self.fake.downloads[module.module_url(self.release, second)] = (
            support.make_elf()
        )

        resolved = module.resolve(self.fake, self.root, self.release)

        self.assertEqual(resolved.source, module.SOURCE_REMOTE)
        meta = os.path.join(self.root, self.release, "meta.json")
        self.assertIn(module.module_url(self.release, second), self.fake.files[meta])

    def test_other_http_is_network(self) -> None:
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        self.fake.downloads[url] = DownloadHttpError(503)
        with self.assertRaises(module.Unavailable) as caught:
            module.resolve(self.fake, self.root, self.release)
        self.assertEqual(caught.exception.reason, module.REASON_NETWORK)

    def test_connection_failure_is_network(self) -> None:
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        self.fake.downloads[url] = DownloadNetworkError("timed out")
        with self.assertRaises(module.Unavailable) as caught:
            module.resolve(self.fake, self.root, self.release)
        self.assertEqual(caught.exception.reason, module.REASON_NETWORK)

    def test_bad_download_is_bad_artifact(self) -> None:
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        self.fake.downloads[url] = support.make_elf(
            {elf.VERMAGIC_KEY: "6.1.0-other SMP preempt mod_unload"}
        )
        with self.assertRaises(module.Unavailable) as caught:
            module.resolve(self.fake, self.root, self.release)
        self.assertEqual(caught.exception.reason, module.REASON_BAD_ARTIFACT)
        target = os.path.join(self.root, self.release, self.release + ".ko")
        self.assertFalse(self.fake.exists(target))
        part = os.path.join(self.root, ".tmp", self.release + ".ko.part")
        self.assertFalse(self.fake.exists(part))

    def test_force_remote_ignores_cache(self) -> None:
        path = os.path.join(self.root, self.release, self.release + ".ko")
        self.fake.blobs[path] = support.make_elf()
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        replacement = support.make_elf({elf.VERMAGIC_KEY: support.VERMAGIC, "marker": "new"})
        self.fake.downloads[url] = replacement

        resolved = module.resolve(
            self.fake, self.root, self.release, force_remote=True
        )

        self.assertEqual(resolved.source, module.SOURCE_REMOTE)
        self.assertEqual(self.fake.blobs[path], replacement)
        meta = os.path.join(self.root, self.release, "meta.json")
        self.assertIn("fetched_at", self.fake.files[meta])

    def test_cancel_aborts_without_leaving_part(self) -> None:
        url = module.module_url(self.release, module.CHANNEL_TAGS[0])
        self.fake.downloads[url] = support.make_elf()

        def cancel() -> None:
            raise module.Aborted()

        with self.assertRaises(module.Aborted):
            module.resolve(self.fake, self.root, self.release, cancel=cancel)
        part = os.path.join(self.root, ".tmp", self.release + ".ko.part")
        self.assertFalse(self.fake.exists(part))


class CleanCacheTest(unittest.TestCase):
    """清理只删当前内核以外的条目，`.tmp` 也不算条目（M-07）。"""

    def test_keeps_current_and_tmp(self) -> None:
        fake = support.FakeSystem()
        for name in ("old-a", "old-b", fake.release, ".tmp"):
            fake.mkdirs(os.path.join("/data/kmod", name))

        removed = module.clean_cache(fake, "/data/kmod", fake.release)

        self.assertEqual(removed, ["old-a", "old-b"])
        self.assertTrue(fake.exists(os.path.join("/data/kmod", fake.release)))
        self.assertTrue(fake.exists(os.path.join("/data/kmod", ".tmp")))

    def test_missing_root(self) -> None:
        fake = support.FakeSystem()
        self.assertEqual(module.clean_cache(fake, "/data/kmod", "x"), [])


if __name__ == "__main__":
    unittest.main()
