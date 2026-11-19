#!/usr/bin/env python3
"""scripts/steamos_toolchain.py 的单测：依赖展开、gcc 定位、环境输出与一次离线取包。

除最后一个用例（走 `file://` 假镜像，且只在装了 `bsdtar` 时运行）外全部是纯函数
测试。不联网、不需要特权：假镜像里的"包"是随手造的几个 tar，用来证明 `usr/` 剥层
与依赖递归真的发生。

跑法：python3 scripts/tests/test_steamos_toolchain.py
"""

from __future__ import annotations

import contextlib
import io
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPTS))

import steamos_repo as repo  # noqa: E402
import steamos_toolchain as toolchain  # noqa: E402


def desc_text(**fields: object) -> str:
    """按 `%KEY%` 段拼一份 desc 文本；值可以是字符串或字符串列表。"""
    parts: list[str] = []
    for key, value in fields.items():
        parts.append(f"%{key.upper()}%")
        values = value if isinstance(value, (list, tuple)) else [value]
        parts.extend(str(item) for item in values)
        parts.append("")
    return "\n".join(parts)


class ResolvePackagesTest(unittest.TestCase):
    """依赖展开：递归、去重、顺序稳定，缺包时要报出依赖链。"""

    @staticmethod
    def build() -> repo.RepoIndex:
        index = repo.RepoIndex()
        for name, deps, provides in (
            ("gcc", ("binutils", "glibc"), ()),
            ("binutils", ("glibc",), ()),
            ("glibc", (), ("libc.so",)),
            ("pahole", (), ()),
        ):
            index.add(
                repo.Package(
                    repo="core-3.8",
                    name=name,
                    version="1-1",
                    filename=f"{name}-1-1-x86_64.pkg.tar.zst",
                    depends=deps,
                    provides=provides,
                )
            )
        return index

    def test_transitive_and_ordered(self) -> None:
        names = [
            p.name
            for p in toolchain.resolve_packages(self.build(), ["pahole", "gcc"])
        ]
        # 按宽度优先：两个根先出（gcc 在 pahole 前），再展开依赖；glibc 只出现一次。
        self.assertEqual(names, ["gcc", "pahole", "binutils", "glibc"])

    def test_resolves_through_provides(self) -> None:
        index = self.build()
        index.add(
            repo.Package(
                repo="core-3.8",
                name="needs-libc",
                version="1-1",
                filename="needs-libc-1-1-x86_64.pkg.tar.zst",
                depends=("libc.so",),
                provides=(),
            )
        )
        names = [p.name for p in toolchain.resolve_packages(index, ["needs-libc"])]
        self.assertEqual(names, ["needs-libc", "glibc"])

    def test_missing_dependency_names_the_chain(self) -> None:
        index = repo.RepoIndex()
        index.add(
            repo.Package(
                repo="core-3.8",
                name="gcc",
                version="1-1",
                filename="gcc-1-1-x86_64.pkg.tar.zst",
                depends=("glibc",),
                provides=(),
            )
        )
        with self.assertRaises(repo.SteamosError) as caught:
            toolchain.resolve_packages(index, ["gcc"])
        message = str(caught.exception)
        self.assertIn("glibc", message)
        self.assertIn("gcc", message)  # 依赖链里要看得见"谁要它"

    def test_exclude_drops_subtree(self) -> None:
        index = repo.RepoIndex()
        for name, deps in (
            ("gcc", ("binutils", "glibc")),
            ("binutils", ("glibc",)),
            ("glibc", ("tzdata",)),
            ("tzdata", ()),
        ):
            index.add(
                repo.Package(
                    repo="core-3.8", name=name, version="1-1",
                    filename=f"{name}-1-1-x86_64.pkg.tar.zst",
                    depends=deps, provides=(),
                )
            )
        names = [
            p.name
            for p in toolchain.resolve_packages(
                index, ["gcc"], frozenset({"glibc"})
            )
        ]
        # glibc 被跳过时它的依赖也跟着不取：否则前缀里会出现一份半截的基础运行时。
        self.assertEqual(names, ["gcc", "binutils"])


class ShortVersionTest(unittest.TestCase):
    """Release 说明里的版本号：去掉 epoch 与 pkgrel。"""

    def test_short_version(self) -> None:
        self.assertEqual(toolchain.short_version("15.1.1-1"), "15.1.1")
        self.assertEqual(toolchain.short_version("1:1.30-1"), "1.30")


class FindGccTest(unittest.TestCase):
    """gcc 内部目录的定位：唯一才接受。"""

    def test_single_gcc(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp)
            cc1 = prefix / "lib/gcc/x86_64-pc-linux-gnu/15.1.1/cc1"
            cc1.parent.mkdir(parents=True)
            cc1.write_text("stub")
            self.assertEqual(
                toolchain.find_gcc(prefix), ("x86_64-pc-linux-gnu", "15.1.1")
            )

    def test_missing_gcc_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(repo.SteamosError):
                toolchain.find_gcc(Path(tmp))

    def test_two_gccs_are_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp)
            for version in ("15.1.1", "16.2.1"):
                cc1 = prefix / "lib/gcc/x86_64-pc-linux-gnu" / version / "cc1"
                cc1.parent.mkdir(parents=True)
                cc1.write_text("stub")
            with self.assertRaises(repo.SteamosError) as caught:
                toolchain.find_gcc(prefix)
            self.assertIn("15.1.1", str(caught.exception))
            self.assertIn("16.2.1", str(caught.exception))


class EnvAssignmentsTest(unittest.TestCase):
    """给 eval 的环境赋值：PATH 追加、CC 带 -B、pahole 可选。"""

    def test_lines(self) -> None:
        prefix = Path("/opt/tc")
        lines = toolchain.env_assignments(prefix, "x86_64-pc-linux-gnu", "15.1.1", "1.30")
        self.assertEqual(
            lines,
            [
                "export PATH=/opt/tc/bin${PATH:+:$PATH}",
                "export LD_LIBRARY_PATH=/opt/tc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}",
                "export CC='/opt/tc/bin/gcc -B /opt/tc/lib/gcc/x86_64-pc-linux-gnu/15.1.1'",
                "export HOSTCC='/opt/tc/bin/gcc -B /opt/tc/lib/gcc/x86_64-pc-linux-gnu/15.1.1'",
                "export TOOLCHAIN_GCC_VERSION=15.1.1",
                "export TOOLCHAIN_PAHOLE_VERSION=1.30",
            ],
        )

    def test_no_pahole_line_without_pahole(self) -> None:
        lines = toolchain.env_assignments(
            Path("/opt/tc"), "x86_64-pc-linux-gnu", "15.1.1", None
        )
        self.assertFalse(any("PAHOLE" in line for line in lines))

    def test_prefix_with_space_is_quoted(self) -> None:
        lines = toolchain.env_assignments(
            Path("/opt/tool chain"), "x86_64-pc-linux-gnu", "15.1.1", None
        )
        self.assertTrue(lines[0].startswith("export PATH='/opt/tool chain/bin'"))
        self.assertIn("'", lines[1])


class FakeMirrorTest(unittest.TestCase):
    """走 `file://` 假镜像的端到端用例：db → 展开 → 下载 → 剥层 → 环境输出。"""

    def setUp(self) -> None:
        if shutil.which("bsdtar") is None:
            self.skipTest("需要 bsdtar 才能验证解包路径")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.mirror = self.root / "mirror"
        self.prefix = self.root / "prefix"

    def write_package(
        self,
        repo_name: str,
        name: str,
        version: str,
        members: dict[str, str],
        depends: tuple[str, ...] = (),
    ) -> None:
        """造一个包（tar.gz；bsdtar 不在乎压缩格式）与它对应的 desc 条目。"""
        directory = self.mirror / repo_name / "os" / "x86_64"
        directory.mkdir(parents=True, exist_ok=True)
        filename = f"{name}-{version}-x86_64.pkg.tar.gz"
        with tarfile.open(directory / filename, "w:gz") as tar:
            for path, content in {
                ".PKGINFO": f"pkgname = {name}\n",
                **{f"usr/{key}": value for key, value in members.items()},
            }.items():
                payload = content.encode()
                info = tarfile.TarInfo(path)
                info.size = len(payload)
                info.mode = 0o755 if "/bin/" in path or path.endswith("/cc1") else 0o644
                tar.addfile(info, io.BytesIO(payload))
        self.add_desc(repo_name, name, version, filename, depends)

    def add_desc(
        self,
        repo_name: str,
        name: str,
        version: str,
        filename: str,
        depends: tuple[str, ...] = (),
    ) -> None:
        """把一个 desc 成员追加进该仓库的 `.db`（xz tar，成员名带包版本目录）。"""
        directory = self.mirror / repo_name / "os" / "x86_64"
        directory.mkdir(parents=True, exist_ok=True)
        db = directory / f"{repo_name}.db"
        text = desc_text(
            filename=filename,
            name=name,
            version=version,
            depends=list(depends),
        ).encode()
        # 每次重写整份 db：成员集在测试里是累积的，读旧的再加新的。
        existing: list[tuple[str, bytes]] = []
        if db.exists():
            with tarfile.open(db) as tar:
                for member in tar:
                    handle = tar.extractfile(member)
                    if member.isfile() and handle is not None:
                        existing.append((member.name, handle.read()))
        with tarfile.open(db, "w:xz") as tar:
            for member_name, content in existing:
                self._add_member(tar, member_name, content)
            self._add_member(tar, f"{name}-{version}/desc", text)

    @staticmethod
    def _add_member(tar: tarfile.TarFile, name: str, content: bytes) -> None:
        info = tarfile.TarInfo(name)
        info.size = len(content)
        tar.addfile(info, io.BytesIO(content))

    def run_toolchain(self, *extra: str, packages: str = "gcc") -> tuple[int, str, str]:
        """跑脚本主入口，返回 (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        argv = [
            "--repo", "core-3.8",
            "--packages", packages,
            "--into", str(self.prefix),
            "--mirror", self.mirror.as_uri(),
            *extra,
        ]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = toolchain.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_end_to_end(self) -> None:
        self.write_package(
            "core-3.8", "glibc", "2.41-1", {"lib/libc.so.6": "stub"}
        )
        self.write_package(
            "core-3.8",
            "gcc",
            "15.1.1-1",
            {
                "bin/gcc": "stub",
                "lib/gcc/x86_64-pc-linux-gnu/15.1.1/cc1": "stub",
                "lib/gcc/x86_64-pc-linux-gnu/15.1.1/libgcc.a": "stub",
            },
            depends=("glibc",),
        )
        code, out, _ = self.run_toolchain()
        self.assertEqual(code, 0)
        self.assertTrue((self.prefix / "bin/gcc").is_file())
        self.assertTrue(
            (self.prefix / "lib/gcc/x86_64-pc-linux-gnu/15.1.1/cc1").is_file()
        )
        # 依赖被递归取回；包的元数据文件没有跟着进前缀。
        self.assertTrue((self.prefix / "lib/libc.so.6").is_file())
        self.assertFalse((self.prefix / ".PKGINFO").exists())
        self.assertIn("TOOLCHAIN_GCC_VERSION=15.1.1", out)
        self.assertIn(f"export PATH={self.prefix}/bin", out)

    def test_dry_run_downloads_no_package(self) -> None:
        self.add_desc(
            "core-3.8", "gcc", "15.1.1-1", "gcc-15.1.1-1-x86_64.pkg.tar.gz"
        )
        code, out, _ = self.run_toolchain("--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("# would download gcc 15.1.1-1 from", out)
        self.assertFalse(self.prefix.exists())

    def test_missing_dependency_exits_2(self) -> None:
        self.add_desc(
            "core-3.8",
            "gcc",
            "15.1.1-1",
            "gcc-15.1.1-1-x86_64.pkg.tar.gz",
            depends=("glibc",),
        )
        code, out, err = self.run_toolchain()
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("glibc", err)


if __name__ == "__main__":
    unittest.main()
