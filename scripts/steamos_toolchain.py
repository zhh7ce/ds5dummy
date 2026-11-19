#!/usr/bin/env python3
"""按内核频道解析并解出 SteamOS 工具链，供云端构建内核模块使用。

SteamOS 的每个内核频道（`jupiter-3.8`、`jupiter-3.9` …）对应一组 Arch 仓库
（`core-<频道>`、`extra-<频道>`）。驱动模块必须用**该频道自己的** gcc 编译：跨频道
能编过，但产物与 Valve 自己编的内核之间会多出一层编译器差异，而 P-11 要求的是同
频道工具链。所以这里不安装系统包、也不回退到构建机自带的 gcc——缺一个包就整体
失败，让流水线看见而不是悄悄换编译器。

本脚本做三件事：

1. 读每个仓库的 `.db`，按 `%DEPENDS%` 递归展开出完整的包集合（解析与下载在
   `steamos_repo.py`，与发现内核头文件包的那条脚本共用）；
2. 逐个解压，`bsdtar` 只取 `usr/` 并剥掉这一层，全部合并进同一个前缀，得到
   `<prefix>/bin/gcc`、`<prefix>/lib/gcc/<triple>/<ver>/`；
3. 在前缀里定位 gcc 的版本目录，向 stdout 打印一组**可 `eval` 的 shell 赋值**
   （`PATH`、`LD_LIBRARY_PATH`、`CC`、`HOSTCC` 与两个用于 Release 说明的版本号）。

`--exclude` 用来跳过**基础运行时**：解出的 gcc/binutils/pahole 都动态链接，需要
`<prefix>/lib` 在 `LD_LIBRARY_PATH` 上才能找到同频道的那份库（`make` 甚至直接
链接 `libguile-3.0.so.1`，所以 guile 不能跳）；但如果把该频道的 `glibc` 也解进来，
前缀里那份旧 libc 会盖住构建机自己的，构建机上新编的普通工具（shell、python）反而
起不来——旧库喂给新的二进制是唯一不允许的方向。所以 CI 传 `--exclude glibc`：
libc 用构建机的（总是更新，向下兼容）。

输出形式是 shell 语句而不是 `KEY=VALUE` 文件：`PATH` 要保留调用方原有的值，而
`$GITHUB_ENV` 写进去的文本不会被展开。用法：

    eval "$(python3 scripts/steamos_toolchain.py --into "$PREFIX" \\
        --packages gcc,binutils,make,pahole --repo core-3.8 --repo extra-3.8 \\
        --exclude glibc)"

只解析不下载用 `--dry-run`：打印将要取的包与地址，不建前缀。所有失败都以退出码 2
结束并把原因写到 stderr；脚本不静默跳过任何包（`--exclude` 的命中会逐条说明）。
"""

from __future__ import annotations

import argparse
import shlex
import sys
import tempfile
from collections import deque
from pathlib import Path

from steamos_repo import (
    DEFAULT_MIRROR,
    Package,
    RepoIndex,
    SteamosError,
    extract_package,
    fetch,
    load_repo,
    package_url,
)


def load_index(mirror: str, repos: list[str], workdir: Path) -> RepoIndex:
    """下载并解析全部仓库的 `.db`，合成一个查找索引。

    Args:
        mirror: 镜像根。
        repos: 仓库名列表，顺序即同名包的优先级。
        workdir: 下载 `.db` 用的临时目录。

    Returns:
        合并后的索引。

    Raises:
        SteamosError: 任一仓库取不到或无法解析。
    """
    index = RepoIndex()
    for repo in repos:
        packages = load_repo(mirror, repo, workdir)
        for package in packages:
            index.add(package)
        print(
            f"steamos_toolchain: {repo}: {len(packages)} 个包",
            file=sys.stderr,
            flush=True,
        )
    return index


def resolve_packages(
    index: RepoIndex, roots: list[str], exclude: frozenset[str] = frozenset()
) -> list[Package]:
    """按 `%DEPENDS%` 递归展开根包，返回去重后的下载顺序。

    根包优先、依赖按名字排序展开（宽度优先），所以同一组输入每次给出同一个顺序
    （下载日志可对账）。同一个包经名字与 provides 两条路径被请求时只保留一份。

    Args:
        index: 仓库索引。
        roots: 根包名（gcc、pahole …）。
        exclude: 不下载的包名（见模块说明的 `--exclude`）。命中时连同它的依赖子树
            一起跳过：被排除的是基础运行时，它的依赖（tzdata、filesystem）同样由
            构建机提供，解出来只会制造第二份。

    Returns:
        需要解压的包，根在前、依赖在后。

    Raises:
        SteamosError: 某个名字在索引里不存在，消息里带完整依赖链与补救方向。
    """
    ordered: list[Package] = []
    requested: set[str] = set()
    taken: set[str] = set()
    # 队列项带一条依赖链，缺失时报出"谁要它"，否则运维只能翻 db 找。
    queue: deque[tuple[str, tuple[str, ...]]] = deque(
        (name, (name,)) for name in sorted(roots)
    )
    while queue:
        name, chain = queue.popleft()
        if name in requested:
            continue
        requested.add(name)
        if name in exclude:
            print(
                f"steamos_toolchain: 跳过 {name}（--exclude，由构建机提供）",
                file=sys.stderr,
                flush=True,
            )
            continue
        package = index.find(name)
        if package is None:
            required_by = " -> ".join(chain[:-1]) or "命令行"
            raise SteamosError(
                f"仓库索引里没有 {name!r}（由 {required_by} 要求）；"
                f"把提供它的仓库加进 --repo，或缺包时先修镜像"
            )
        # provides 解析可能让两个请求命中同一个包：按真实包名去重。
        if package.name in taken:
            continue
        taken.add(package.name)
        ordered.append(package)
        for dependency in sorted(package.depends):
            queue.append((dependency, (*chain, dependency)))
    return ordered


def find_gcc(prefix: Path) -> tuple[str, str]:
    """在前缀里定位 gcc 的内部目录，返回 `(triple, version)`。

    gcc 靠 `lib/gcc/<triple>/<version>/` 找到自己的 `cc1`；`-B` 指的就是它的上一
    级。多份 gcc 合进同一个前缀会让这条路径变得不唯一，此时宁可失败。

    Args:
        prefix: 工具链前缀。

    Returns:
        `triple`（如 `x86_64-pc-linux-gnu`）与 `version`（如 `15.1.1`）。

    Raises:
        SteamosError: 找不到 `cc1`，或找到不止一个版本。
    """
    candidates = sorted(prefix.glob("lib/gcc/*/*/cc1"))
    if not candidates:
        raise SteamosError(f"{prefix} 里没有 gcc（找不到 lib/gcc/*/*/cc1）")
    if len(candidates) > 1:
        found = ", ".join(str(path.relative_to(prefix)) for path in candidates)
        raise SteamosError(f"{prefix} 里有多份 gcc，无法确定用哪一份: {found}")
    cc1 = candidates[0]
    return cc1.parent.parent.name, cc1.parent.name


def short_version(version: str) -> str:
    """把包版本压成可读版本：`1:1.30-1` → `1.30`，`15.1.1-1` → `15.1.1`。

    Release 说明里要的是人能对照的编译器版本，epoch 与 pkgrel 对读者没有意义。
    """
    without_epoch = version.split(":", 1)[-1] if ":" in version else version
    return without_epoch.rsplit("-", 1)[0]


def env_assignments(
    prefix: Path,
    triple: str,
    gcc_version: str,
    pahole_version: str | None,
) -> list[str]:
    """生成供 `eval` 的 shell 赋值行。

    `-B` 必须写进 `CC`：解出来的 gcc 与系统 gcc 并存时，只有显式给出内部目录它才
    用自己那份 `cc1`/`libgcc`。kbuild 会把 `CC` 按词展开，所以这里带参数是合法的。

    `LD_LIBRARY_PATH` 指向前缀 `lib/`：解出的 gcc、binutils（`libsframe.so`）与
    pahole（`libelf.so`）都是动态链接的，且不带 RPATH。前缀里的 libc 已被
    `--exclude` 挡掉，所以这条路径不会盖住构建机的 libc。

    Args:
        prefix: 工具链前缀（`main()` 给出的已 `resolve()` 的绝对路径）。
        triple: gcc 的目标三元组目录名。
        gcc_version: gcc 版本目录名。
        pahole_version: pahole 的可读版本；没有请求 pahole 时为 None，此时不输出
            该行，调用方引用它就会在 eval 之后暴露成空串。

    Returns:
        shell 赋值行列表，按输出顺序排列。`PATH` 与 `LD_LIBRARY_PATH` 用
        `${VAR:+:$VAR}` 追加原有值，原值为空时也不会留下结尾冒号。
    """
    gcc_dir = prefix / "lib" / "gcc" / triple / gcc_version
    cc = f"{prefix / 'bin' / 'gcc'} -B {gcc_dir}"
    lines = [
        f"export PATH={shlex.quote(str(prefix / 'bin'))}${{PATH:+:$PATH}}",
        f"export LD_LIBRARY_PATH={shlex.quote(str(prefix / 'lib'))}"
        "${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}",
        f"export CC={shlex.quote(cc)}",
        f"export HOSTCC={shlex.quote(cc)}",
        f"export TOOLCHAIN_GCC_VERSION={shlex.quote(gcc_version)}",
    ]
    if pahole_version is not None:
        lines.append(f"export TOOLCHAIN_PAHOLE_VERSION={shlex.quote(pahole_version)}")
    return lines


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """命令行解析；参数即契约，校验只做"有没有给"。"""
    parser = argparse.ArgumentParser(
        description="解析 SteamOS 频道工具链并解到前缀，打印可 eval 的环境赋值",
    )
    parser.add_argument(
        "--repo", action="append", required=True, metavar="REPO",
        help="仓库名，可重复；顺序即同名包的优先级（如 core-3.8）",
    )
    parser.add_argument(
        "--packages", required=True, metavar="A,B",
        help="逗号分隔的根包名（如 gcc,binutils,make,pahole）",
    )
    parser.add_argument(
        "--into", required=True, metavar="DIR", help="解压目标前缀",
    )
    parser.add_argument(
        "--mirror", default=DEFAULT_MIRROR, metavar="URL",
        help=f"镜像根（默认 {DEFAULT_MIRROR}）",
    )
    parser.add_argument(
        "--exclude", default="", metavar="A,B",
        help="逗号分隔的不下载包名，连同它们的依赖子树一起跳过（CI 传 glibc）",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="只解析并打印包清单，不下载包、不建前缀",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """解析 → 展开 → 下载解压 → 打印环境；失败统一转成退出码 2。"""
    args = parse_args(argv)
    prefix = Path(args.into).resolve()
    roots = [name.strip() for name in args.packages.split(",") if name.strip()]
    if not roots:
        print("steamos_toolchain: --packages 里没有包名", file=sys.stderr)
        return 2
    exclude = frozenset(
        name.strip() for name in args.exclude.split(",") if name.strip()
    )

    try:
        with tempfile.TemporaryDirectory(prefix="steamos-toolchain-") as tmp:
            workdir = Path(tmp)
            index = load_index(args.mirror, args.repo, workdir)
            packages = resolve_packages(index, roots, exclude)

            if args.dry_run:
                for package in packages:
                    print(
                        f"# would download {package.name} {package.version} "
                        f"from {package_url(args.mirror, package.repo, package.filename)}"
                    )
                return 0

            prefix.mkdir(parents=True, exist_ok=True)
            for package in packages:
                archive = workdir / package.filename
                fetch(
                    package_url(args.mirror, package.repo, package.filename),
                    archive,
                    f"{package.name}-{package.version}",
                )
                extract_package(archive, prefix)
                print(
                    f"steamos_toolchain: 解出 {package.name} {package.version}",
                    file=sys.stderr,
                    flush=True,
                )
    except SteamosError as error:
        print(f"steamos_toolchain: {error}", file=sys.stderr)
        return 2

    # gcc 的版本以解出来的内部目录为准，而不是 db 里的包版本：真正被调用的 cc1
    # 在哪个目录，`-B` 就得指到哪。
    triple, gcc_version = find_gcc(prefix)
    pahole = next((p for p in packages if p.name == "pahole"), None)
    for line in env_assignments(
        prefix, triple, gcc_version,
        short_version(pahole.version) if pahole is not None else None,
    ):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
