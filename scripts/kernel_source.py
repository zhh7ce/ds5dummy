#!/usr/bin/env python3
"""取回某个 SteamOS 内核对应的上游 hid-playstation 驱动源码，并打上 DS5Dummy 补丁。

云端构建的内核模块不再携带一份完整的驱动源码：每个内核都从 linux-stable 的
GitHub 镜像（`gregkh/linux`）取回**该内核基数版本**的
`drivers/hid/hid-playstation.c` 与 `drivers/hid/hid-ids.h`，
再打上 main 的 `kmod/ds5dummy.patch`。这样内核内部 API（分配器、锁、HID
接口的改名）永远由上游保证匹配，补丁只压在与版本无关的几段上。

版本换算：SteamOS 头文件包的版本形如 `7.2.7.valve2-2`、`6.16.12.valve4.drmexec3-1`、
`6.18.53-1`，取前三段数字当上游版本；补丁段为 0 时稳定树里没有 `vX.Y.0` 这个标签
（发布标签是 `vX.Y`），其余情况是 `vX.Y.Z`。

补丁应用策略是 `patch -p1 --fuzz=0`：上游一旦改动补丁附近的行，这一格构建直接失败，
而不是带着偏移硬贴出一个可疑的 `.ko`。每次调用都重新下载并覆盖，所以构建重试直接
再调一遍即可（不会遇到"补丁已应用"）。

用法：

    python3 scripts/kernel_source.py --version 7.2.7.valve2-2 \\
        --patch kmod/ds5dummy.patch --into kmod

只解析不下载用 `--dry-run`（打印标签与地址）。失败统一退出码 2。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from steamos_repo import SteamosError, fetch

#: 稳定树的裸文件入口。用 GregKH 的 linux-stable GitHub 镜像而不是 kernel.org
#: 的 cgit `plain`：后者对非浏览器客户端返回 Anubis 反爬页（HTTP 200 的 HTML），
#: 自动化取不到源码；镜像带每个稳定点版本的标签，内容与上游逐字节相同。
DEFAULT_BASE_URL = "https://raw.githubusercontent.com/gregkh/linux"

#: 取回的两份上游文件：(仓库内路径, 本地文件名)。本地用扁平名是因为 kbuild 在
#: 模块目录里找 `hid-playstation.c`，而 `#include "hid-ids.h"` 也是同目录优先——
#: 内核源码树里的相邻关系要在模块目录里复原。
UPSTREAM_FILES = (
    ("drivers/hid/hid-playstation.c", "hid-playstation.c"),
    ("drivers/hid/hid-ids.h", "hid-ids.h"),
)

#: 补丁允许的上下文偏移量。0 = 只接受逐行吻合的上下文，上游漂移即失败。
PATCH_FUZZ = 0

#: 包版本开头的 `major.minor.patch`；后面的 `.valve*`、`.drmexec*`、`-pkgrel`
#: 都是 Valve 自己的后缀，与上游标签无关。
_VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)")


class KernelSourceError(RuntimeError):
    """版本换算或补丁应用失败；调用方据此以退出码 2 结束。"""


def upstream_tag(package_version: str) -> str:
    """把头文件包版本换成 kernel.org 稳定树的标签。

    Args:
        package_version: 头文件包版本，如 `7.2.7.valve2-2`；允许带 epoch
            （`1:6.18.53-1`）。

    Returns:
        上游标签，如 `v7.2.7`；补丁段为 0 时是 `v6.5` 这样的两段式。

    Raises:
        KernelSourceError: 版本不以三段数字开头。
    """
    without_epoch = package_version.split(":", 1)[-1]
    match = _VERSION_RE.match(without_epoch)
    if match is None:
        raise KernelSourceError(
            f"无法从 {package_version!r} 取出上游版本（期望形如 7.2.7.valve2-2）"
        )
    major, minor, patch = match.groups()
    # 稳定树只给发布版打两段标签；`v6.5.0` / `v5.13.0` 这样的标签不存在。
    if patch == "0":
        return f"v{major}.{minor}"
    return f"v{major}.{minor}.{patch}"


def source_url(relative_path: str, tag: str, base_url: str = DEFAULT_BASE_URL) -> str:
    """拼出某个文件在指定稳定标签下的裸文件地址。

    Args:
        relative_path: 内核树内路径，如 `drivers/hid/hid-playstation.c`。
        tag: 上游标签，如 `v7.2.7`。
        base_url: 稳定树镜像的根（`<root>/<tag>/<path>` 形状）。

    Returns:
        可直接交给 `steamos_repo.fetch` 的 URL。
    """
    return f"{base_url}/{tag}/{relative_path}"


def download(tag: str, into: Path, base_url: str = DEFAULT_BASE_URL) -> None:
    """把该标签下的两份驱动源文件取回 `into`，同名覆盖。

    覆盖是有意的：构建重试与"先失败再重来"都靠它回到干净的源码，调用方不需要
    自己清理上一次的半成品。

    Args:
        tag: 上游标签，如 `v7.2.7`。
        into: 目标目录（模块目录，Makefile 与补丁文件都在这里）。
        base_url: 稳定树的 plain 根；测试用它指向 `file://` 假镜像。

    Raises:
        SteamosError: 任一份文件取不到（网络错误或标签不存在时的 404）。
    """
    into.mkdir(parents=True, exist_ok=True)
    for relative_path, local_name in UPSTREAM_FILES:
        dest = into / local_name
        url = source_url(relative_path, tag, base_url)
        print(f"kernel_source: {tag}: {relative_path} -> {dest}", file=sys.stderr, flush=True)
        fetch(url, dest, f"{tag}:{local_name}")


def apply_patch(into: Path, patch_file: Path) -> None:
    """在 `into` 里用 `patch -p1 --fuzz=0` 打上补丁。

    `--forward` 不能省：`--batch` 下遇到"已打过"的补丁，patch 会自作主张按反向
    应用（把补丁悄悄撤掉，退出码仍是 0）。`--no-backup-if-mismatch` 不留
    `.orig`/`.rej`：失败时这一格构建就要停，残留文件只会让下一次尝试的目录更难读。

    Args:
        into: 源码与补丁共处的目录（`-p1` 剥掉补丁里的 `a/`、`b/` 前缀）。
        patch_file: 补丁文件。

    Raises:
        KernelSourceError: 补丁打不上（上游上下文已变）或 `patch` 不可用。
    """
    command = [
        "patch",
        "-p1",
        f"--fuzz={PATCH_FUZZ}",
        "--forward",
        "--batch",
        "--no-backup-if-mismatch",
        "-i",
        str(patch_file),
    ]
    try:
        result = subprocess.run(
            command, cwd=into, capture_output=True, text=True, check=False
        )
    except OSError as error:
        raise KernelSourceError(f"无法执行 patch：{error}") from error
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise KernelSourceError(
            f"补丁打不上（上游在补丁附近改了代码？）：{detail}"
        )
    for line in result.stdout.splitlines():
        if line.strip():
            print(f"kernel_source: {line}", file=sys.stderr, flush=True)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """命令行解析；参数即契约。"""
    parser = argparse.ArgumentParser(
        description="取回内核基数版本的上游 hid-playstation 源码并打 DS5Dummy 补丁",
    )
    parser.add_argument(
        "--version", required=True, metavar="VERSION",
        help="SteamOS 头文件包版本（如 7.2.7.valve2-2），据此推出上游标签",
    )
    parser.add_argument(
        "--patch", required=True, metavar="FILE",
        help="补丁文件（main 的 kmod/ds5dummy.patch）",
    )
    parser.add_argument(
        "--into", required=True, metavar="DIR",
        help="模块目录：取回的源码写在这里，补丁也在这里应用",
    )
    parser.add_argument(
        "--base-url", default=DEFAULT_BASE_URL, metavar="URL",
        help=f"稳定树 plain 根（默认 {DEFAULT_BASE_URL}）",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="只打印标签与地址，不下载、不打补丁",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """换算标签 → 取回源码 → 打补丁；失败统一转成退出码 2。"""
    args = parse_args(argv)
    try:
        tag = upstream_tag(args.version)
        if args.dry_run:
            print(f"# {args.version} -> {tag}")
            for relative_path, local_name in UPSTREAM_FILES:
                print(f"# would download {source_url(relative_path, tag, args.base_url)}"
                      f" -> {Path(args.into) / local_name}")
            return 0

        patch_file = Path(args.patch).resolve()
        if not patch_file.is_file():
            raise KernelSourceError(f"补丁文件不存在：{patch_file}")
        into = Path(args.into).resolve()
        download(tag, into, args.base_url)
        apply_patch(into, patch_file)
    except (KernelSourceError, SteamosError) as error:
        print(f"kernel_source: {error}", file=sys.stderr)
        return 2
    print(f"kernel_source: {args.version} -> {tag}，补丁已应用", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
