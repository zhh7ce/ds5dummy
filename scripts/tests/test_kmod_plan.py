#!/usr/bin/env python3
"""scripts/kmod_plan.py 的单测：发现规则、seen 合并、产物判定与头文件树核对。

判断全是纯函数；`discover` 子命令的端到端用例走 `file://` 假镜像，不联网。跑法：
python3 scripts/tests/test_kmod_plan.py
"""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))

import kmod_plan as plan  # noqa: E402
import steamos_repo as repo  # noqa: E402

#: 一个真实形状的内核 release：包版本里的点变成横线，尾部带 localversion。
RELEASE = "6.18.50-valve2-1-neptune-618-gc7289a96b14d"
PACKAGE_VERSION = "6.18.50.valve2-1"
HEADER_NAME = "linux-neptune-618-headers"

#: 本轮的 `kmod/` 内容指纹；与 seen 条目里的同值时表示"编它的还是这份补丁"。
FINGERPRINT = "9a8244b2362c02a96056be0e8728668316d083881abeb1ec6ecb33f1afea00b9"


def package(name: str, version: str = PACKAGE_VERSION, **kwargs: object) -> repo.Package:
    """造一条头文件包记录。"""
    return repo.Package(
        repo=str(kwargs.get("repo", "jupiter-3.8")),
        name=name,
        version=version,
        filename=str(kwargs.get("filename", f"{name}-{version}-x86_64.pkg.tar.zst")),
        depends=(),
        provides=(),
    )


def seen_with(release: str | None = RELEASE, version: str = PACKAGE_VERSION,
              fingerprint: str = FINGERPRINT) -> dict[str, object]:
    """造一份只记了一个头文件包的 seen 结构。"""
    document = plan.empty_seen()
    document["packages"] = {
        plan.seen_key("jupiter-3.8", HEADER_NAME): {
            "channel": "jupiter-3.8",
            "name": HEADER_NAME,
            "version": version,
            "filename": f"{HEADER_NAME}-{version}-x86_64.pkg.tar.zst",
            "release": release or "",
            "patch_fingerprint": fingerprint,
        }
    }
    return document


def assets_with(channel: str = "jupiter-3.8", *names: str) -> dict[str, set[str]]:
    """造一份"某频道 Release 里已有这些资产"的清单。"""
    return {channel: set(names)}


class NamingTest(unittest.TestCase):
    """频道映射、头文件包识别与资产名。"""

    def test_toolchain_repos_for_every_channel(self) -> None:
        for channel in plan.DISCOVERY_CHANNELS:
            suffix = channel[len("jupiter-"):]
            self.assertEqual(
                plan.toolchain_repos(channel), (f"core-{suffix}", f"extra-{suffix}")
            )

    def test_toolchain_repos_rejects_other_repos(self) -> None:
        for name in ("core-3.8", "jupiter-", ""):
            with self.assertRaises(repo.SteamosError):
                plan.toolchain_repos(name)

    def test_header_package_shapes(self) -> None:
        for name in (
            "linux-neptune-headers",
            "linux-neptune-618-headers",
            "linux-neptune-618-drm-exec-headers",
            "linux-neptune-618-kasan-headers",
        ):
            self.assertTrue(plan.is_header_package(name), name)
        for name in (
            "linux-neptune-618",
            "linux-neptune-618-headers-dbg",
            "linux-headers",
            "linux-neptune618-headers",
        ):
            self.assertFalse(plan.is_header_package(name), name)

    def test_asset_and_seen_keys(self) -> None:
        self.assertEqual(plan.asset_name(RELEASE), f"{RELEASE}.ko")
        self.assertEqual(
            plan.seen_key("jupiter-3.8", HEADER_NAME),
            f"jupiter-3.8/{HEADER_NAME}",
        )


class AssetsDirTest(unittest.TestCase):
    """每频道一份资产清单：目录里的 `<频道>.txt`，一行一个名字。"""

    def test_reads_one_file_per_channel(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "jupiter-3.8.txt").write_text("a.ko\nb.ko\n\n", encoding="utf-8")
            (directory / "jupiter-3.9.txt").write_text("a.ko\n", encoding="utf-8")
            self.assertEqual(
                plan.read_assets(directory),
                {"jupiter-3.8": {"a.ko", "b.ko"}, "jupiter-3.9": {"a.ko"}},
            )

    def test_missing_directory_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(plan.read_assets(Path(tmp) / "absent"), {})


class ChannelsCommandTest(unittest.TestCase):
    """`channels` 打印的就是 discover 轮询的那份清单，tag 与频道名同值。"""

    def test_prints_every_channel(self) -> None:
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(plan.main(["channels"]), 0)
        self.assertEqual(
            buffer.getvalue().split(), list(plan.DISCOVERY_CHANNELS)
        )


class SeenFileTest(unittest.TestCase):
    """seen.json 的读写与合并。"""

    def test_missing_file_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(
                plan.load_seen(Path(tmp) / "seen.json"), plan.empty_seen()
            )

    def test_bad_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "seen.json"
            path.write_text(json.dumps({"version": 99, "packages": {}}))
            with self.assertRaises(repo.SteamosError):
                plan.load_seen(path)

    def test_corrupt_json_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "seen.json"
            path.write_text("{not json")
            with self.assertRaises(repo.SteamosError):
                plan.load_seen(path)

    def test_apply_results_merges_and_keeps_others(self) -> None:
        seen = plan.empty_seen()
        seen["packages"] = {"jupiter-3.9/linux-neptune-headers": {"release": "x"}}
        merged = plan.apply_results(
            seen,
            [
                {
                    "channel": "jupiter-3.8",
                    "name": HEADER_NAME,
                    "version": PACKAGE_VERSION,
                    "filename": "h.pkg.tar.zst",
                    "release": RELEASE,
                }
            ],
            FINGERPRINT,
        )
        entry = merged["packages"][f"jupiter-3.8/{HEADER_NAME}"]
        self.assertEqual(entry["release"], RELEASE)
        self.assertEqual(entry["patch_fingerprint"], FINGERPRINT)
        self.assertIn("jupiter-3.9/linux-neptune-headers", merged["packages"])

    def test_apply_results_requires_a_release(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.apply_results(
                plan.empty_seen(),
                [
                    {
                        "channel": "jupiter-3.8",
                        "name": HEADER_NAME,
                        "version": PACKAGE_VERSION,
                        "filename": "h.pkg.tar.zst",
                        "release": "",
                    }
                ],
                FINGERPRINT,
            )

    def test_apply_results_requires_a_fingerprint(self) -> None:
        # 没有指纹就写不出"这份产物是用哪份补丁编的"，下一轮会把它当成新产物重建。
        with self.assertRaises(repo.SteamosError):
            plan.apply_results(plan.empty_seen(), [], "")


class ScheduledPlanTest(unittest.TestCase):
    """定时轮次：已有产物的内核跳过，未知或版本变化的要构建。"""

    def test_skips_when_asset_exists(self) -> None:
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]},
            seen_with(),
            assets_with("jupiter-3.8", plan.asset_name(RELEASE)),
            rebuild_all=False,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual(tasks, [])

    def test_asset_in_another_channel_does_not_skip(self) -> None:
        # 每个频道有自己的 Release：同一个内核在 3.9 发布过，不代表 3.8 也有了。
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]},
            seen_with(),
            assets_with("jupiter-3.9", plan.asset_name(RELEASE)),
            rebuild_all=False,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual([(t.channel, t.release) for t in tasks],
                         [("jupiter-3.8", RELEASE)])

    def test_builds_when_asset_missing(self) -> None:
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]}, seen_with(), {}, False,
            FINGERPRINT,
        )
        self.assertEqual(len(tasks), 1)
        task = tasks[0]
        self.assertEqual((task.channel, task.core_repo, task.extra_repo),
                         ("jupiter-3.8", "core-3.8", "extra-3.8"))
        self.assertEqual(task.release, RELEASE)
        self.assertEqual(task.reason, "new")

    def test_version_change_drops_the_trusted_release(self) -> None:
        # 包被重打过：旧记录里的 release 不能代表这份新包，要重新解出真值。
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME, version="6.18.51.valve2-1")]},
            seen_with(),
            assets_with("jupiter-3.8", plan.asset_name(RELEASE)),
            rebuild_all=False,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual(len(tasks), 1)
        self.assertEqual(tasks[0].release, "")

    def test_unknown_package_is_built(self) -> None:
        tasks = plan.plan({"jupiter-3.8": [package(HEADER_NAME)]},
                          plan.empty_seen(), {}, False, FINGERPRINT)
        self.assertEqual([t.release for t in tasks], [""])

    def test_non_header_packages_are_ignored(self) -> None:
        tasks = plan.plan(
            {"jupiter-3.8": [package("linux-neptune-618"), package("pahole")]},
            plan.empty_seen(),
            {},
            False,
            FINGERPRINT,
        )
        self.assertEqual(tasks, [])

    def test_order_is_deterministic(self) -> None:
        channels = {"jupiter-3.9": [package(HEADER_NAME)], "jupiter-3.8": [package(HEADER_NAME)]}
        first = plan.plan(channels, plan.empty_seen(), {}, False, FINGERPRINT)
        second = plan.plan(channels, plan.empty_seen(), {}, False, FINGERPRINT)
        self.assertEqual([t.channel for t in first], ["jupiter-3.8", "jupiter-3.9"])
        self.assertEqual(first, second)


class RebuildPlanTest(unittest.TestCase):
    """重建轮次：seen 里的已发布内核全部重建，新包另加。"""

    def test_rebuild_ignores_existing_assets(self) -> None:
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]},
            seen_with(),
            assets_with("jupiter-3.8", plan.asset_name(RELEASE)),
            rebuild_all=True,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual(len(tasks), 1)
        self.assertEqual((tasks[0].reason, tasks[0].release), ("rebuild", RELEASE))

    def test_rebuild_keeps_kernels_no_longer_in_the_db(self) -> None:
        tasks = plan.plan({"jupiter-3.8": []}, seen_with(), {}, True, FINGERPRINT)
        self.assertEqual([t.release for t in tasks], [RELEASE])

    def test_new_version_adds_a_second_task(self) -> None:
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME, version="6.18.51.valve2-1")]},
            seen_with(),
            {},
            rebuild_all=True,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual(
            sorted((t.reason, t.version) for t in tasks),
            [("new", "6.18.51.valve2-1"), ("rebuild", PACKAGE_VERSION)],
        )

    def test_seen_entry_without_release_is_new(self) -> None:
        tasks = plan.plan({"jupiter-3.8": []}, seen_with(release=None), {}, True,
                          FINGERPRINT)
        self.assertEqual([t.reason for t in tasks], ["new"])


class MatrixTest(unittest.TestCase):
    """矩阵结构：GHA 只收字符串值。"""

    def test_shape(self) -> None:
        tasks = plan.plan({"jupiter-3.8": [package(HEADER_NAME)]},
                          plan.empty_seen(), {}, False, FINGERPRINT)
        document = plan.matrix(tasks)
        self.assertEqual(list(document), ["include"])
        row = document["include"][0]
        self.assertTrue(all(isinstance(value, str) for value in row.values()))
        self.assertEqual(row["name"], HEADER_NAME)
        self.assertEqual(row["release"], "")


def elf_bytes(size: int = 64) -> bytes:
    """造一个开头像 ELF64 的文件内容；其余字节填零。"""
    return b"\x7fELF\x02" + b"\x00" * (size - 5)


class ModuleVerdictTest(unittest.TestCase):
    """产物判定：大小、ELF64、vermagic 逐字相等、srcversion 可读。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.module = Path(self.tmp.name) / "module.ko"
        self.module.write_bytes(elf_bytes())

    @staticmethod
    def modinfo(**kwargs: str) -> str:
        fields = {"vermagic": f"{RELEASE} SMP preempt mod_unload", "srcversion": "ABCDEF"}
        fields.update(kwargs)
        return "".join(
            f"{key}:    {value}\n" for key, value in fields.items() if value is not None
        )

    def test_accepts_a_good_module(self) -> None:
        plan.check_module(self.module, RELEASE, self.modinfo())

    def test_rejects_empty_file(self) -> None:
        self.module.write_bytes(b"")
        with self.assertRaises(repo.SteamosError):
            plan.check_module(self.module, RELEASE, self.modinfo())

    def test_rejects_oversized_file(self) -> None:
        with open(self.module, "wb") as handle:
            handle.truncate(plan.MAX_MODULE_BYTES + 1)
        with self.assertRaises(repo.SteamosError) as caught:
            plan.check_module(self.module, RELEASE, self.modinfo())
        self.assertIn("上限", str(caught.exception))

    def test_rejects_non_elf_and_elf32(self) -> None:
        for content in (b"not an elf at all", b"\x7fELF\x01" + b"\x00" * 32):
            self.module.write_bytes(content)
            with self.assertRaises(repo.SteamosError):
                plan.check_module(self.module, RELEASE, self.modinfo())

    def test_rejects_vermagic_mismatch(self) -> None:
        with self.assertRaises(repo.SteamosError) as caught:
            plan.check_module(
                self.module,
                "6.18.53-valve2-1-neptune-619-gdeadbeef",
                self.modinfo(),
            )
        self.assertIn("vermagic", str(caught.exception))

    def test_rejects_missing_srcversion(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.check_module(self.module, RELEASE, self.modinfo(srcversion=""))

    def test_parse_modinfo_keeps_first_value(self) -> None:
        fields = plan.parse_modinfo("vermagic: a b\nparm: x:1\nparm: y:2\n")
        self.assertEqual(fields["vermagic"], "a b")
        self.assertEqual(fields["parm"], "x:1")


class KernelTreeTest(unittest.TestCase):
    """头文件树自证：Makefile 版本字段与 kernel.release 对齐。"""

    MAKEFILE = "VERSION = 6\nPATCHLEVEL = 18\nSUBLEVEL = 50\nEXTRAVERSION = -valve2-1\n"

    def test_release_from_file(self) -> None:
        self.assertEqual(plan.kernel_release_from_file(f"{RELEASE}\n"), RELEASE)
        with self.assertRaises(repo.SteamosError):
            plan.kernel_release_from_file("  \n")

    def test_consistent_tree(self) -> None:
        plan.check_kernel_tree(RELEASE, self.MAKEFILE)
        # 没有前导 '-' 的写法同样成立（Makefile 习惯两种都有）。
        plan.check_kernel_tree(RELEASE, self.MAKEFILE.replace("-valve2-1", "valve2-1"))

    def test_numeric_mismatch(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.check_kernel_tree(RELEASE, self.MAKEFILE.replace("SUBLEVEL = 50", "SUBLEVEL = 51"))

    def test_extraversion_mismatch(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.check_kernel_tree(RELEASE, self.MAKEFILE.replace("-valve2-1", "-valve3"))

    def test_missing_fields(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.check_kernel_tree(RELEASE, "SUBLEVEL = 50\n")

    def test_empty_extraversion_is_unconstrained(self) -> None:
        plan.check_kernel_tree(RELEASE, self.MAKEFILE.replace("-valve2-1", ""))


class DiscoverCommandTest(unittest.TestCase):
    """`discover` 端到端：file:// 假镜像 + seen + assets → 矩阵文件。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.mirror = self.root / "mirror"
        self.write_db(
            "jupiter-3.8",
            [
                (HEADER_NAME, PACKAGE_VERSION),
                ("linux-neptune-618-drm-exec-headers", "6.18.50.valve2-1"),
                ("linux-neptune-618", "6.18.50.valve2-1"),
                # SteamOS 3.0/3.1 时代的 5.13：低于支持下限，永远不该进矩阵。
                ("linux-neptune-51-headers", "5.13.0.valve37-1"),
            ],
        )

    def write_db(self, channel: str, packages: list[tuple[str, str]]) -> None:
        """写一份最小 `.db`：每个包一个 `desc` 成员。"""
        directory = self.mirror / channel / "os" / "x86_64"
        directory.mkdir(parents=True, exist_ok=True)
        with tarfile.open(directory / f"{channel}.db", "w:xz") as tar:
            for name, version in packages:
                text = (
                    f"%FILENAME%\n{name}-{version}-x86_64.pkg.tar.zst\n\n"
                    f"%NAME%\n{name}\n\n%VERSION%\n{version}\n\n"
                ).encode()
                info = tarfile.TarInfo(f"{name}-{version}/desc")
                info.size = len(text)
                tar.addfile(info, io.BytesIO(text))

    def run_discover(self, *extra: str, seen: dict[str, object] | None = None,
                     assets: dict[str, list[str]] | None = None,
                     fingerprint: str = FINGERPRINT) -> dict[str, object]:
        """跑 discover 子命令，返回写出的矩阵。

        `assets` 的键是频道名：每个键写成目录里同名的 `<频道>.txt`，形状与工作流里
        为每个频道取资产列表的结果一致。
        """
        seen_path = self.root / "seen.json"
        assets_dir = self.root / "assets"
        output = self.root / "matrix.json"
        if seen is not None:
            seen_path.write_text(json.dumps(seen), encoding="utf-8")
        for channel, names in (assets or {}).items():
            assets_dir.mkdir(exist_ok=True)
            (assets_dir / f"{channel}.txt").write_text(
                "\n".join(names) + "\n", encoding="utf-8"
            )
        code = plan.main([
            "discover",
            "--seen", str(seen_path),
            "--assets-dir", str(assets_dir),
            "--output", str(output),
            "--channel", "jupiter-3.8",
            "--mirror", self.mirror.as_uri(),
            "--patch-fingerprint", fingerprint,
            *extra,
        ])
        self.assertEqual(code, 0)
        return json.loads(output.read_text(encoding="utf-8"))

    def test_discovers_all_header_variants(self) -> None:
        document = self.run_discover()
        names = [row["name"] for row in document["include"]]
        # plan() 按 (channel, name, ...) 排序，"-drm-exec-..." 排在 "-headers" 之前
        self.assertEqual(
            names, ["linux-neptune-618-drm-exec-headers", HEADER_NAME]
        )
        self.assertTrue(all(row["reason"] == "new" for row in document["include"]))
        self.assertTrue(all(row["release"] == "" for row in document["include"]))

    def test_kernels_below_the_floor_are_skipped(self) -> None:
        buffer = io.StringIO()
        with contextlib.redirect_stderr(buffer):
            document = self.run_discover()
        self.assertNotIn(
            "linux-neptune-51-headers", [row["name"] for row in document["include"]]
        )
        self.assertIn("跳过 1 个头文件包（低于支持下限 6.0）", buffer.getvalue())

    def test_skips_known_and_published(self) -> None:
        document = self.run_discover(
            seen=seen_with(),
            assets={"jupiter-3.8": [plan.asset_name(RELEASE)]},
        )
        self.assertEqual(
            [row["name"] for row in document["include"]],
            ["linux-neptune-618-drm-exec-headers"],
        )

    def test_assets_of_other_channels_are_ignored(self) -> None:
        document = self.run_discover(
            seen=seen_with(),
            assets={"jupiter-3.9": [plan.asset_name(RELEASE)]},
        )
        self.assertIn(HEADER_NAME, [row["name"] for row in document["include"]])

    def test_changed_fingerprint_rebuilds_only_that_kernel(self) -> None:
        document = self.run_discover(
            seen=seen_with(fingerprint="old"),
            assets={"jupiter-3.8": [plan.asset_name(RELEASE)]},
        )
        rows = {row["name"]: row for row in document["include"]}
        self.assertEqual(
            (rows[HEADER_NAME]["reason"], rows[HEADER_NAME]["release"]),
            ("rebuild", RELEASE),
        )
        # 没有记录的包照常按新内核处理，不受这一条旧指纹影响
        self.assertEqual(
            rows["linux-neptune-618-drm-exec-headers"]["reason"], "new"
        )

    def test_rebuild_all_covers_seen(self) -> None:
        document = self.run_discover(
            "--rebuild-all", seen=seen_with(),
            assets={"jupiter-3.8": [plan.asset_name(RELEASE)]},
        )
        reasons = {row["name"]: (row["reason"], row["release"])
                   for row in document["include"]}
        self.assertEqual(reasons[HEADER_NAME], ("rebuild", RELEASE))
        self.assertEqual(
            reasons["linux-neptune-618-drm-exec-headers"], ("new", "")
        )


class FetchHeaderCommandTest(unittest.TestCase):
    """`fetch-header` 端到端：file:// 假镜像 → 复核 → 下载 → 剥层解出。"""

    def setUp(self) -> None:
        if shutil.which("bsdtar") is None:
            self.skipTest("需要 bsdtar 才能验证解包")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.mirror = self.root / "mirror"
        self.into = self.root / "pkgs"
        self.extract = self.root / "kernel"
        self.into.mkdir()
        self.extract.mkdir()
        self.filename = f"{HEADER_NAME}-{PACKAGE_VERSION}-x86_64.pkg.tar.zst"
        self.write_package(self.filename, PACKAGE_VERSION)

    def write_package(self, filename: str, version: str) -> None:
        """造一个头文件包（tar.gz；bsdtar 不看扩展名）与它的 db 条目。"""
        directory = self.mirror / "jupiter-3.8" / "os" / "x86_64"
        directory.mkdir(parents=True, exist_ok=True)
        with tarfile.open(directory / filename, "w:gz") as tar:
            for path, content in {
                ".PKGINFO": "pkgname = %s\n" % HEADER_NAME,
                "usr/lib/modules/%s/build/include/config/kernel.release" % RELEASE:
                    RELEASE + "\n",
            }.items():
                payload = content.encode()
                info = tarfile.TarInfo(path)
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        text = (
            f"%FILENAME%\n{filename}\n\n%NAME%\n{HEADER_NAME}\n\n"
            f"%VERSION%\n{version}\n\n"
        ).encode()
        with tarfile.open(directory / "jupiter-3.8.db", "w:xz") as db:
            info = tarfile.TarInfo(f"{HEADER_NAME}-{version}/desc")
            info.size = len(text)
            db.addfile(info, io.BytesIO(text))

    def run_fetch(self, *extra: str) -> int:
        """跑 fetch-header 子命令；默认参数指向假镜像里的那一份。"""
        return plan.main([
            "fetch-header",
            "--channel", "jupiter-3.8",
            "--name", HEADER_NAME,
            "--into", str(self.into),
            "--extract", str(self.extract),
            "--mirror", self.mirror.as_uri(),
            *extra,
        ])

    def test_downloads_and_extracts(self) -> None:
        self.assertEqual(
            self.run_fetch("--version", PACKAGE_VERSION, "--filename", self.filename),
            0,
        )
        self.assertTrue((self.into / self.filename).is_file())
        # 只取 usr/ 并剥掉这一层：KDIR 因此位于 <extract>/lib/modules/<release>/build。
        tree = self.extract / "lib" / "modules" / RELEASE / "build"
        self.assertTrue((tree / "include/config/kernel.release").is_file())
        self.assertFalse((self.extract / ".PKGINFO").exists())

    def test_version_mismatch_fails(self) -> None:
        self.assertEqual(self.run_fetch("--version", "6.18.53.valve2-1"), 1)
        self.assertFalse((self.into / self.filename).exists())

    def test_filename_mismatch_fails(self) -> None:
        self.assertEqual(self.run_fetch("--filename", "other.pkg.tar.zst"), 1)

    def test_unknown_name_fails(self) -> None:
        code = plan.main([
            "fetch-header",
            "--channel", "jupiter-3.8",
            "--name", "linux-neptune-999-headers",
            "--into", str(self.into),
            "--mirror", self.mirror.as_uri(),
        ])
        self.assertEqual(code, 1)


class KernelFingerprintTest(unittest.TestCase):
    """补丁指纹记在每份产物上：只重建指纹对不上的那一个内核（P-25）。"""

    def test_entry_without_fingerprint_rebuilds_once(self) -> None:
        # 旧缓存里的条目没有这个键：当作"不是本指纹编的"，重建一次后记上新值。
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]},
            seen_with(fingerprint=""),
            assets_with("jupiter-3.8", plan.asset_name(RELEASE)),
            False,
            FINGERPRINT,
        )
        self.assertEqual([(t.reason, t.release) for t in tasks],
                         [("rebuild", RELEASE)])

    def test_empty_current_fingerprint_never_rebuilds(self) -> None:
        # 工作流没算出指纹（空串）时不能因此每轮都重建。
        tasks = plan.plan(
            {"jupiter-3.8": [package(HEADER_NAME)]},
            seen_with(fingerprint="old"),
            assets_with("jupiter-3.8", plan.asset_name(RELEASE)),
            False,
            "",
        )
        self.assertEqual(tasks, [])

    def test_failed_kernel_is_retried_next_round(self) -> None:
        # 两个内核的产物都用旧指纹编过；本轮只有主包构建成功并记下新指纹，
        # 另一个留着旧指纹，于是下一轮只重试它——这正是整份文件只有一个指纹时
        # 会漏掉的那种情况。
        drm = "linux-neptune-618-drm-exec-headers"
        drm_release = RELEASE + "-drm"
        seen = seen_with(fingerprint="old")
        seen["packages"][plan.seen_key("jupiter-3.8", drm)] = {
            "channel": "jupiter-3.8",
            "name": drm,
            "version": PACKAGE_VERSION,
            "filename": f"{drm}-{PACKAGE_VERSION}-x86_64.pkg.tar.zst",
            "release": drm_release,
            "patch_fingerprint": "old",
        }
        channels = {"jupiter-3.8": [package(HEADER_NAME), package(drm)]}
        assets = {"jupiter-3.8": {
            plan.asset_name(RELEASE), plan.asset_name(drm_release),
        }}
        updated = plan.apply_results(
            seen,
            [{
                "channel": "jupiter-3.8",
                "name": HEADER_NAME,
                "version": PACKAGE_VERSION,
                "filename": "h.pkg.tar.zst",
                "release": RELEASE,
            }],
            FINGERPRINT,
        )
        tasks = plan.plan(channels, updated, assets, False, FINGERPRINT)
        self.assertEqual([(t.name, t.reason) for t in tasks], [(drm, "rebuild")])


class KernelFloorTest(unittest.TestCase):
    """支持下限：低于 `MIN_KERNEL_VERSION` 的内核不进矩阵。"""

    def test_parses_major_and_minor(self) -> None:
        self.assertEqual(plan.kernel_version("6.18.50.valve2-1"), (6, 18))
        self.assertEqual(plan.kernel_version("5.13.0.valve37-1"), (5, 13))

    def test_unparsable_versions(self) -> None:
        for version in ("6", "", "valve-1"):
            self.assertIsNone(plan.kernel_version(version), version)

    def test_floor(self) -> None:
        self.assertFalse(plan.is_supported_kernel("5.13.0.valve37-1"))
        self.assertTrue(plan.is_supported_kernel("6.0.0.valve1-1"))
        self.assertTrue(plan.is_supported_kernel("6.1.52.valve19-1"))
        # 形状认不出的版本保留：交给构建阶段暴露，而不是静默丢掉
        self.assertTrue(plan.is_supported_kernel("valve"))

    def test_selection_drops_old_kernels(self) -> None:
        selected = plan.select_header_packages({"jupiter-3.8": [
            package("linux-neptune-51-headers", "5.13.0.valve37-1"),
            package(HEADER_NAME),
        ]})
        self.assertEqual([item.name for item in selected["jupiter-3.8"]],
                         [HEADER_NAME])

    def test_rebuild_all_skips_old_kernels(self) -> None:
        seen = seen_with()
        seen["packages"][plan.seen_key("jupiter-3.8", "linux-neptune-51-headers")] = {
            "channel": "jupiter-3.8",
            "name": "linux-neptune-51-headers",
            "version": "5.13.0.valve37-1",
            "filename": "old.pkg.tar.zst",
            "release": "5.13.0-valve37-1",
            "patch_fingerprint": FINGERPRINT,
        }
        tasks = plan.plan({"jupiter-3.8": []}, seen, {}, True, FINGERPRINT)
        self.assertEqual([task.release for task in tasks], [RELEASE])


class RecordCommandTest(unittest.TestCase):
    """`record`：合并成功产物，每份记下编它用的补丁指纹。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.seen = self.root / "seen.json"
        self.metas = self.root / "assets"
        self.metas.mkdir()

    def write_meta(self, directory: str, **fields: str) -> None:
        target = self.metas / directory
        target.mkdir()
        (target / "meta.json").write_text(json.dumps(fields), encoding="utf-8")

    def test_merges(self) -> None:
        self.write_meta(
            "a", channel="jupiter-3.8", name=HEADER_NAME, version=PACKAGE_VERSION,
            filename="x.pkg.tar.zst", release=RELEASE,
        )
        self.assertEqual(
            plan.main([
                "record", "--seen", str(self.seen), "--metas", str(self.metas),
                "--patch-fingerprint", FINGERPRINT, "--output", str(self.seen),
            ]),
            0,
        )
        document = json.loads(self.seen.read_text(encoding="utf-8"))
        self.assertNotIn("patch", document)  # 指纹不再记在整份文件上
        entry = document["packages"][f"jupiter-3.8/{HEADER_NAME}"]
        self.assertEqual(entry["release"], RELEASE)
        self.assertEqual(entry["patch_fingerprint"], FINGERPRINT)

    def test_empty_metas_is_not_an_error(self) -> None:
        self.assertEqual(
            plan.main([
                "record", "--seen", str(self.seen), "--metas", str(self.metas),
                "--patch-fingerprint", FINGERPRINT,
                "--output", str(self.seen),
            ]),
            0,
        )
        self.assertEqual(json.loads(self.seen.read_text())["packages"], {})

    def test_meta_without_channel_is_rejected(self) -> None:
        # 构建 job 写的是 `channel`；键名对不上就说明两边已经错开，宁可失败也不写。
        self.write_meta(
            "a", repo="jupiter-3.8", name=HEADER_NAME, version=PACKAGE_VERSION,
            filename="x.pkg.tar.zst", release=RELEASE,
        )
        self.assertEqual(
            plan.main([
                "record", "--seen", str(self.seen), "--metas", str(self.metas),
                "--patch-fingerprint", FINGERPRINT, "--output", str(self.seen),
            ]),
            1,
        )


class NoteLineTest(unittest.TestCase):
    """P-27 的构建记录行。"""

    META = {
        "release": RELEASE,
        "channel": "jupiter-3.8",
        "name": HEADER_NAME,
        "version": PACKAGE_VERSION,
        "patch_fingerprint": (
            "3f9c1ab2d4e5f6a7b8c9d0e1f2a3b4c5"
            "d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1"
        ),
        "gcc": "15.1.1",
        "pahole": "1.30",
        "built_at": "2026-11-13T09:12Z",
        "reason": "new",
    }

    def test_line(self) -> None:
        self.assertEqual(
            plan.note_line(self.META),
            f"- `{RELEASE}` · jupiter-3.8 · {HEADER_NAME} {PACKAGE_VERSION} · "
            "patch 3f9c1ab2d4e5 · gcc 15.1.1 · pahole 1.30 · 2026-11-13T09:12Z",
        )

    def test_rebuild_is_marked(self) -> None:
        line = plan.note_line({**self.META, "reason": "rebuild"})
        self.assertIn("重建", line)

    def test_pahole_is_optional(self) -> None:
        line = plan.note_line({**self.META, "pahole": ""})
        self.assertNotIn("pahole", line)

    def test_missing_field_fails(self) -> None:
        with self.assertRaises(repo.SteamosError):
            plan.note_line({**self.META, "patch_fingerprint": ""})


if __name__ == "__main__":
    unittest.main()
