#!/usr/bin/env python3
"""SteamOS Arch 镜像的只读访问层：仓库数据库解析、包索引与文件下载。

镜像的机器可读入口只有两样东西：每个仓库的 `<repo>.db`（压缩 tar，每包一份
`desc`）与同目录下的包文件。这一层把它们变成 `Package` 记录与本地文件，不认识
工具链、不认识内核，也不做任何取舍——谁该装、谁该跳过由调用方决定。

调用方有两个：`steamos_toolchain.py`（按频道解出 gcc/pahole）与
`kmod_plan.py`（发现内核头文件包，并按构建矩阵取回头文件包）。所有失败都抛
`SteamosError`，其中带原始 URL 或仓库名，便于在 CI 日志里定位是哪一步的网络或
解析出了问题。
"""

from __future__ import annotations

import http.client
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

#: SteamOS 的 Arch 镜像根。仓库地址形如 `<mirror>/<repo>/os/<arch>/<repo>.db`。
DEFAULT_MIRROR = "https://steamdeck-packages.steamos.cloud/archlinux-mirror"

#: SteamOS 只有 x86_64 一种架构。写成常量而不是参数：地址里的架构段是可推导的，
#: 做成参数只会多出一条填错就 404 的路径。
ARCH = "x86_64"

#: 单个 HTTP 请求的读超时（秒）。gcc 包约 58 MiB，CI 网络下留足余量。
HTTP_TIMEOUT_S = 60

#: 下载过程中的进度打印粒度（字节）。16 MiB 一级：CI 日志里看得出在动，又不至于
#: 每个读块刷一行。
PROGRESS_STEP_BYTES = 16 * 1024 * 1024

#: 读块大小（字节）。1 MiB 是系统调用次数与内存占用的折中。
READ_CHUNK_BYTES = 1024 * 1024

#: 单次下载的尝试次数与线性退避（秒）。镜像与代理都会偶发断流（TLS EOF、
#: 读超时），一次就放弃会让整轮白跑；HTTP 4xx 不重试，那是地址或权限错了。
FETCH_ATTEMPTS = 3
FETCH_BACKOFF_S = 2.0

#: `desc` 里的字段标记行，例如 `%DEPENDS%`（整个文件是若干 `%KEY%` 段）。
_DESC_KEY_RE = re.compile(r"^%([A-Z0-9_]+)%$")

#: 依赖/提供项的包名部分：`binutils>=2.38` → `binutils`，`libfoo.so=1-64` →
#: `libfoo.so`，`pkg: 说明` → `pkg`。
_DEP_NAME_RE = re.compile(r"^[^\s<>=:]+")


class SteamosError(RuntimeError):
    """镜像取回、`.db` 解析或包下载失败；调用方据此以退出码 2 结束。"""


@dataclass(frozen=True)
class Package:
    """`.db` 里的一条约包记录，只保留解析与下载需要的字段。

    Attributes:
        repo: 这条记录来自哪个仓库（`core-3.8`、`jupiter-3.8` 等），用于拼下载地址。
        name: 包名，如 `gcc`、`linux-neptune-618-headers`。
        version: 完整包版本，如 `15.1.1-1`；可能带 epoch（`1:1.30-1`）。
        filename: 包文件名，与 `version` 不一致的情况由 `.db` 自身保证正确。
        depends: 已归一化为包名的硬依赖（去掉版本约束与说明文字）。
        provides: 已归一化为名字的 provides 项，用于虚拟包解析。
    """

    repo: str
    name: str
    version: str
    filename: str
    depends: tuple[str, ...]
    provides: tuple[str, ...]


def dep_name(spec: str) -> str:
    """取依赖/提供项里的包名，丢弃版本约束与说明文字。

    Args:
        spec: `desc` 里的原始条目，如 `glibc`、`binutils>=2.38`、`libfoo.so=1-64`、
            `python: 可选解释器`。

    Returns:
        包名（或 provide 名），保证非空。

    Raises:
        SteamosError: 条目里没有可识别的名字（例如只有一个版本号）。
    """
    match = _DEP_NAME_RE.match(spec.strip())
    if match is None:
        raise SteamosError(f"依赖条目里没有包名: {spec!r}")
    return match.group(0)


def parse_desc(text: str) -> dict[str, list[str]]:
    """把一份 `.db` 的 `desc` 文本拆成 `%KEY%` → 值列表。

    `desc` 的形状是若干段，每段以 `%KEY%` 单独一行开头，之后每行一个值，段间用
    空行分隔。同一段的值不合并（`%DEPENDS%` 的多行就是多个依赖），所以值是列表。

    Args:
        text: `desc` 文件全文（UTF-8）。

    Returns:
        键为去掉百分号的大写字段名，值为按文件顺序的值列表；空段值为空列表。

    Raises:
        SteamosError: 首段标记之前出现内容（说明这不是一份 desc）。
    """
    fields: dict[str, list[str]] = {}
    current: list[str] | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue  # 段间空行只是分隔符，不是值
        match = _DESC_KEY_RE.match(line)
        if match is not None:
            current = fields.setdefault(match.group(1), [])
            continue
        if current is None:
            # 第一段标记之前出现内容说明这不是一份 desc；按损坏处理而不是丢行。
            raise SteamosError(f"desc 在首段标记之前出现内容: {line!r}")
        current.append(line)
    return fields


def package_from_desc(repo: str, text: str) -> Package:
    """把一份 `desc` 解成 Package。

    Args:
        repo: 这份 `desc` 所属仓库名。
        text: `desc` 全文。

    Returns:
        解析出的包记录。

    Raises:
        SteamosError: 缺 `%NAME%`/`%VERSION%`/`%FILENAME%`，或依赖项无法解析。
    """
    fields = parse_desc(text)
    missing = [key for key in ("NAME", "VERSION", "FILENAME") if not fields.get(key)]
    if missing:
        raise SteamosError(f"{repo} 的 desc 缺少字段 {missing}")

    def names(key: str) -> tuple[str, ...]:
        # 同一名字可能出现多次（provides 里常见），去重但保持文件顺序。
        return tuple(dict.fromkeys(dep_name(v) for v in fields.get(key, [])))

    return Package(
        repo=repo,
        name=fields["NAME"][0],
        version=fields["VERSION"][0],
        filename=fields["FILENAME"][0],
        depends=names("DEPENDS"),
        provides=names("PROVIDES"),
    )


class RepoIndex:
    """多个仓库的包记录合在一起按名字、再按 provides 查找。

    同名包以**先加入**的仓库为准：调用方按命令行给出的仓库顺序加载，于是
    `--repo core-3.8 --repo extra-3.8` 时 core 优先，行为可预测。
    """

    def __init__(self) -> None:
        self._by_name: dict[str, Package] = {}
        self._by_provide: dict[str, Package] = {}

    def add(self, package: Package) -> None:
        """登记一条包记录；名字与 provide 都是先到先得。"""
        self._by_name.setdefault(package.name, package)
        for provide in package.provides:
            self._by_provide.setdefault(provide, package)

    def find(self, name: str) -> Package | None:
        """按包名查，查不到再按 provides 查；都没有返回 None。"""
        return self._by_name.get(name) or self._by_provide.get(name)

    def packages(self) -> list[Package]:
        """按包名排序返回全部记录（不包括只通过 provides 命中的重复项）。"""
        return [self._by_name[name] for name in sorted(self._by_name)]


def repo_db_url(mirror: str, repo: str) -> str:
    """仓库数据库地址：`<mirror>/<repo>/os/<arch>/<repo>.db`。

    Args:
        mirror: 镜像根，末尾斜杠可有可无。
        repo: 仓库名，如 `core-3.8`。

    Returns:
        可直接取回 `.db` 的 URL。
    """
    return f"{mirror.rstrip('/')}/{repo}/os/{ARCH}/{repo}.db"


def package_url(mirror: str, repo: str, filename: str) -> str:
    """包文件地址：与 `.db` 同目录，文件名来自 `%FILENAME%`。"""
    return f"{mirror.rstrip('/')}/{repo}/os/{ARCH}/{filename}"


def _report_progress(label: str, done: int, total: int, started: float) -> None:
    """向 stderr 打一行下载进度；`total` 未知（无 Content-Length）时只报已下载量。"""
    elapsed = time.monotonic() - started
    done_mib = done / (1024 * 1024)
    if total > 0:
        total_mib = total / (1024 * 1024)
        message = f"{label}: {done_mib:.1f}/{total_mib:.1f} MiB ({elapsed:.1f}s)"
    else:
        message = f"{label}: {done_mib:.1f} MiB ({elapsed:.1f}s)"
    print(f"steamos: {message}", file=sys.stderr, flush=True)


def fetch(url: str, dest: Path, label: str) -> None:
    """流式下载 `url` 到 `dest`，每 `PROGRESS_STEP_BYTES` 打一行进度。

    网络类失败（连接断开、TLS 中断、读超时、写盘失败）按 `FETCH_ATTEMPTS` 重试，
    退避 `FETCH_BACKOFF_S × 尝试次数`；每次都从头写，失败时先删掉半截文件，所以
    调用方看到的 `dest` 要么完整要么不存在。HTTP 4xx 不重试。

    Args:
        url: http(s) 或 file URL。
        dest: 目标文件路径；父目录必须已存在。
        label: 进度行里的名字（包名或仓库名）。

    Raises:
        SteamosError: 网络不可达、HTTP 错误（含 404）或写盘失败，附带 URL。
    """
    for attempt in range(1, FETCH_ATTEMPTS + 1):
        try:
            _fetch_once(url, dest, label)
            return
        except _TransientFetchError as error:
            dest.unlink(missing_ok=True)
            if attempt == FETCH_ATTEMPTS:
                raise SteamosError(str(error)) from error
            delay = FETCH_BACKOFF_S * attempt
            print(
                f"steamos: {label}: 第 {attempt} 次取回失败，{delay:.0f}s 后重试"
                f"（{error}）",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(delay)


class _TransientFetchError(RuntimeError):
    """一次下载尝试遇到可重试的网络故障；`fetch` 据此决定要不要再来一次。"""


def _fetch_once(url: str, dest: Path, label: str) -> None:
    """下载一次；HTTP 错误直接抛 `SteamosError`，网络故障抛 `_TransientFetchError`。"""
    try:
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT_S) as response:
            total = int(response.headers.get("Content-Length") or 0)
            done = 0
            reported = 0
            started = time.monotonic()
            with open(dest, "wb") as sink:
                while True:
                    chunk = response.read(READ_CHUNK_BYTES)
                    if not chunk:
                        break
                    sink.write(chunk)
                    done += len(chunk)
                    if done - reported >= PROGRESS_STEP_BYTES:
                        reported = done
                        _report_progress(label, done, total, started)
            _report_progress(label, done, total, started)
    except urllib.error.HTTPError as error:
        raise SteamosError(f"{url}: HTTP {error.code}") from error
    except (urllib.error.URLError, http.client.HTTPException, OSError) as error:
        raise _TransientFetchError(f"{url}: 取不到（{error}）") from error


def extract_package(archive: Path, prefix: Path) -> None:
    """把一个包解进前缀：只取 `usr/`，并剥掉这一层。

    只取 `usr` 是有意的：Arch 包的内容都在 `usr` 下，`etc/`、`.PKGINFO` 之类一律
    不进前缀。剥层之后包内路径与前缀相对布局一致（`<prefix>/bin/gcc`、
    `<prefix>/lib/modules/<release>/build`），调用方不必再拼包内的那段路径。
    依赖链里混着只装 `etc/` 的包（`ca-certificates`），它们对前缀没有贡献，跳过。

    Args:
        archive: 包文件（`.pkg.tar.*`，按 tar 读，压缩格式由 bsdtar 识别）。
        prefix: 目标前缀，必须已存在。

    Raises:
        SteamosError: 找不到 `bsdtar`，或解压返回非零（如包损坏）。
    """
    bsdtar = shutil.which("bsdtar")
    if bsdtar is None:
        raise SteamosError("需要 bsdtar（libarchive-tools）来解包")
    # 先读成员表：`bsdtar -xf ... usr` 在包里没有 usr 时以非零退出，而"没有 usr"
    # 是正常情况（etc/ 下的包），不能和损坏混在一起。
    listing = subprocess.run(
        [bsdtar, "-tf", str(archive)],
        capture_output=True,
        text=True,
    )
    if listing.returncode != 0:
        raise SteamosError(
            f"{archive.name} 无法读取（退出码 {listing.returncode}）: "
            f"{listing.stderr.strip()}"
        )
    members = listing.stdout.splitlines()
    if not any(name == "usr" or name.startswith("usr/") for name in members):
        return
    result = subprocess.run(
        [bsdtar, "-xf", str(archive), "-C", str(prefix),
         "--strip-components", "1", "usr"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SteamosError(
            f"{archive.name} 解压失败（退出码 {result.returncode}）: "
            f"{result.stderr.strip()}"
        )


def read_db_packages(repo: str, archive: Path) -> list[Package]:
    """读一份 `.db`（压缩 tar）里的全部包描述。

    Args:
        repo: 仓库名，写进每条 Package。
        archive: 已落盘的 `.db` 文件。

    Returns:
        包记录列表；顺序与压缩包内成员顺序一致。

    Raises:
        SteamosError: 不是可读的 tar、解码失败，或一个包描述都没有。
    """
    packages: list[Package] = []
    try:
        with tarfile.open(archive) as tar:
            for member in tar:
                if not member.isfile() or not member.name.endswith("/desc"):
                    continue  # 同名目录里还会有别的元数据文件，只认 desc
                handle = tar.extractfile(member)
                if handle is None:
                    continue  # 只可能是目录，已经被 isfile 挡掉；防御性跳过
                packages.append(package_from_desc(repo, handle.read().decode("utf-8")))
    except (tarfile.TarError, UnicodeDecodeError) as error:
        raise SteamosError(f"{repo} 的 .db 无法解析（{error}）") from error
    if not packages:
        raise SteamosError(f"{repo} 的 .db 里没有包描述")
    return packages


def load_repo(mirror: str, repo: str, workdir: Path) -> list[Package]:
    """下载并解析单个仓库的 `.db`。

    Args:
        mirror: 镜像根。
        repo: 仓库名。
        workdir: 下载 `.db` 用的临时目录。

    Returns:
        该仓库的全部包记录。

    Raises:
        SteamosError: 取不到或无法解析。
    """
    db_path = workdir / f"{repo}.db"
    fetch(repo_db_url(mirror, repo), db_path, f"{repo}.db")
    return read_db_packages(repo, db_path)
