#!/usr/bin/env python3
"""scripts/steamos_repo.py 的单测：desc 解析、索引查找、地址拼接与解包。

除解包用例外全部是纯函数用例，不联网、不落盘。跑法：
python3 scripts/tests/test_steamos_repo.py
"""

from __future__ import annotations

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


def desc_text(**fields: object) -> str:
    """按 `%KEY%` 段拼一份 desc 文本；值可以是字符串或字符串列表。"""
    parts: list[str] = []
    for key, value in fields.items():
        parts.append(f"%{key.upper()}%")
        values = value if isinstance(value, (list, tuple)) else [value]
        parts.extend(str(item) for item in values)
        parts.append("")
    return "\n".join(parts)


def package(name: str, **kwargs: object) -> repo.Package:
    """造一条测试用包记录；未给的字段取可预测的默认值。"""
    return repo.Package(
        repo=str(kwargs.get("repo", "core-3.8")),
        name=name,
        version=str(kwargs.get("version", "1-1")),
        filename=str(kwargs.get("filename", f"{name}-1-1-x86_64.pkg.tar.zst")),
        depends=tuple(kwargs.get("depends", ())),  # type: ignore[arg-type]
        provides=tuple(kwargs.get("provides", ())),  # type: ignore[arg-type]
    )


class DepNameTest(unittest.TestCase):
    """依赖条目归一化：版本约束与说明文字都不该进包名。"""

    def test_plain_name(self) -> None:
        self.assertEqual(repo.dep_name("glibc"), "glibc")

    def test_version_constraints(self) -> None:
        self.assertEqual(repo.dep_name("binutils>=2.38"), "binutils")
        self.assertEqual(repo.dep_name("gcc<16"), "gcc")
        self.assertEqual(repo.dep_name("make=4.4"), "make")

    def test_soname_provide(self) -> None:
        self.assertEqual(repo.dep_name("libfoo.so=1-64"), "libfoo.so")

    def test_description_suffix(self) -> None:
        self.assertEqual(repo.dep_name("python: 可选解释器"), "python")

    def test_rejects_empty(self) -> None:
        with self.assertRaises(repo.SteamosError):
            repo.dep_name("   ")


class ParseDescTest(unittest.TestCase):
    """desc 分段：段内多行是多个值，空行只是分隔符。"""

    def test_sections_and_values(self) -> None:
        text = desc_text(
            name="gcc",
            version="15.1.1-1",
            filename="gcc-15.1.1-1-x86_64.pkg.tar.zst",
            depends=["binutils", "glibc"],
        )
        fields = repo.parse_desc(text)
        self.assertEqual(fields["NAME"], ["gcc"])
        self.assertEqual(fields["DEPENDS"], ["binutils", "glibc"])

    def test_content_before_first_key_is_corrupt(self) -> None:
        with self.assertRaises(repo.SteamosError):
            repo.parse_desc("gcc\n%NAME%\ngcc\n")


class PackageFromDescTest(unittest.TestCase):
    """desc → Package：必填字段缺失要失败，依赖/提供要归一化去重。"""

    def test_full_record(self) -> None:
        record = repo.package_from_desc(
            "core-3.8",
            desc_text(
                name="gcc",
                version="15.1.1-1",
                filename="gcc-15.1.1-1-x86_64.pkg.tar.zst",
                depends=["glibc>=2.38", "binutils", "glibc"],
                provides=["cc", "gcc-libs=15.1.1"],
            ),
        )
        self.assertEqual(record.repo, "core-3.8")
        self.assertEqual(record.depends, ("glibc", "binutils"))
        self.assertEqual(record.provides, ("cc", "gcc-libs"))

    def test_missing_field_is_fatal(self) -> None:
        with self.assertRaises(repo.SteamosError) as caught:
            repo.package_from_desc("core-3.8", desc_text(name="gcc"))
        self.assertIn("VERSION", str(caught.exception))


class RepoIndexTest(unittest.TestCase):
    """索引查找：名字优先，provides 兜底，先加入的仓库赢。"""

    def test_name_and_provide(self) -> None:
        index = repo.RepoIndex()
        index.add(package("glibc", provides=("libc.so",)))
        self.assertEqual(index.find("glibc").name, "glibc")
        self.assertEqual(index.find("libc.so").name, "glibc")
        self.assertIsNone(index.find("nosuch"))

    def test_first_repository_wins(self) -> None:
        index = repo.RepoIndex()
        index.add(package("gcc", repo="core-3.8"))
        index.add(package("gcc", repo="extra-3.8", version="99-1"))
        self.assertEqual(index.find("gcc").version, "1-1")

    def test_packages_are_sorted_by_name_once(self) -> None:
        index = repo.RepoIndex()
        index.add(package("pahole", repo="extra-3.8", provides=("dwarves",)))
        index.add(package("gcc"))
        index.add(package("dwarves", repo="extra-3.8"))
        self.assertEqual([p.name for p in index.packages()], ["dwarves", "gcc", "pahole"])


class UrlTest(unittest.TestCase):
    """地址拼接：镜像根的结尾斜杠不影响结果。"""

    def test_urls_tolerate_trailing_slash(self) -> None:
        for mirror in ("https://m.example", "https://m.example/"):
            self.assertEqual(
                repo.repo_db_url(mirror, "core-3.8"),
                "https://m.example/core-3.8/os/x86_64/core-3.8.db",
            )
            self.assertEqual(
                repo.package_url(mirror, "extra-3.8", "pahole-1.30-1-x86_64.pkg.tar.zst"),
                "https://m.example/extra-3.8/os/x86_64/pahole-1.30-1-x86_64.pkg.tar.zst",
            )


class ExtractPackageTest(unittest.TestCase):
    """解包：只取 `usr/`、剥掉这一层，包内元数据不落地。"""

    def setUp(self) -> None:
        if shutil.which("bsdtar") is None:
            self.skipTest("需要 bsdtar")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_extracts_usr_without_the_top_level(self) -> None:
        archive = self.root / "pkg.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for path in (".PKGINFO", "usr/bin/tool", "usr/lib/libx.so.1", "etc/conf"):
                payload = b"stub"
                info = tarfile.TarInfo(path)
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        prefix = self.root / "prefix"
        prefix.mkdir()
        repo.extract_package(archive, prefix)
        self.assertTrue((prefix / "bin/tool").is_file())
        self.assertTrue((prefix / "lib/libx.so.1").is_file())
        self.assertFalse((prefix / "etc").exists())
        self.assertFalse((prefix / ".PKGINFO").exists())

    def test_package_without_usr_is_skipped(self) -> None:
        # 依赖链里真有这种包（ca-certificates 只装 etc/ 与 usr/share 之外的东西），
        # 它们对前缀没有贡献，但不是错误。
        archive = self.root / "certs.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for path in (".PKGINFO", "etc/ssl/certs/ca.pem"):
                payload = b"stub"
                info = tarfile.TarInfo(path)
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        prefix = self.root / "prefix"
        prefix.mkdir()
        repo.extract_package(archive, prefix)
        self.assertEqual(list(prefix.iterdir()), [])


class FetchTest(unittest.TestCase):
    """下载重试：网络类失败按次数重试，HTTP 错误不重试。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.saved = (repo.FETCH_ATTEMPTS, repo.FETCH_BACKOFF_S)
        repo.FETCH_ATTEMPTS, repo.FETCH_BACKOFF_S = 2, 0.0
        self.addCleanup(self.restore)

    def restore(self) -> None:
        repo.FETCH_ATTEMPTS, repo.FETCH_BACKOFF_S = self.saved

    def test_retries_then_raises(self) -> None:
        dest = self.root / "out"
        with self.assertRaises(repo.SteamosError) as caught:
            repo.fetch((self.root / "missing").as_uri(), dest, "x")
        self.assertIn("取不到", str(caught.exception))
        self.assertFalse(dest.exists())  # 半截文件不能留下

    def test_http_error_does_not_retry(self) -> None:
        attempts = []

        def refuse(*args, **kwargs):
            attempts.append(1)
            raise repo.urllib.error.HTTPError("u", 404, "Not Found", {}, None)

        saved = repo.urllib.request.urlopen
        repo.urllib.request.urlopen = refuse
        self.addCleanup(setattr, repo.urllib.request, "urlopen", saved)
        with self.assertRaises(repo.SteamosError) as caught:
            repo.fetch("https://m.example/x", self.root / "out", "x")
        self.assertIn("HTTP 404", str(caught.exception))
        self.assertEqual(len(attempts), 1)


if __name__ == "__main__":
    unittest.main()
