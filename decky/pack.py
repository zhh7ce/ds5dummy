#!/usr/bin/env python3
"""把应用 release 的预编译包打成 Decky 插件 ZIP（C-02、C-03、T-02）。

输入是 `pkg/ds5dummy_bin` 打出来的 pacman 包（`ds5dummy_bin.pkg.tar.zst`），输出是可
直接装进 Decky 的 `ds5dummy_decky.zip`。两者是同一份产物的两种形状：ZIP 里的
`bin/`、`lib/`、`plugins/` 就是那个包 `/usr/lib/ds5dummy/` 下的文件，逐字节相同，
不重新编译数据面（G-02、C-03）。

ZIP 内的布局由 decky-loader 的真实读取路径决定，不是随意摆的：

```
ds5dummy/
  plugin.json            loader 读 name/author/flags/api_version
  package.json           loader 从它取版本号；`type: module` 决定按 ESM 加载 dist/
  main.py                入口文件名是 loader 写死的（不读 plugin.json 的键）
  py_modules/ds5dummy/   本地 Python 包：loader 只把 <插件>/py_modules 加进 sys.path
  dist/index.js          npm run build 产出的前端 bundle
  bin/ lib/ plugins/     便携分发；RPATH 是 $ORIGIN/../lib，三者必须保持兄弟
  share/doc/ds5dummy/    随包文档
  README.md              插件安装/使用说明（T-03）
  VERSION                面板显示的版本号，与 package.json 同源
```

压缩包里仍然写真实的 Unix 权限位（`unzip` 之类按 `external_attr` 解包时用得上），但
别指望安装方一定会恢复它：Decky 用 `zipfile.extractall`，而 CPython 的解包不恢复
mode，`bin/ds5dummyd` 会落在 0644。所以执行位真正靠的是运行期那一次补齐——转发程序
启动前 `Daemon.start()` 走 `System.make_executable()` 把执行位加上。

依赖两个外部命令：`bsdtar`（读 zstd 压缩的 pacman 包；Python 3.12 的 `tarfile` 不认
zstd）与 `npm`（构建前端）。缺哪个就直接报错，不做静默降级。
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DECKY_DIR = REPO_ROOT / "decky"

# ZIP 顶层目录名，同时也是 `homebrew/plugins/<目录>` 的目录名；必须与 plugin.json 的
# 目录约定一致，否则 Decky 里会出现两个同名插件或找不到它。
PLUGIN_DIR_NAME = "ds5dummy"

# pacman 包里便携分发的落点（`pkg/ds5dummy_bin` 的 package() 铺出来的位置）。
PAYLOAD_PREFIX = Path("usr/lib/ds5dummy")

# 设备侧真正会用到的子目录：数据面二进制、它随包的共享库、随包插件。
PAYLOAD_PARTS = ("bin", "lib", "plugins")

# 版本号只允许这些字符：它会进文件名、ZIP 内文本与 Decky 的目录名，出现空格或斜杠
# 会破坏其中至少一处。前导 `v` 在解析阶段就去掉。
_VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]*$")

_BIN_NAME = "ds5dummyd"


def _run(command: list[str], cwd: Path) -> None:
    """跑一个外部命令；失败时把命令与返回码一起抛出。

    Args:
        command: argv，第一项是可执行文件名。
        cwd: 工作目录。

    Raises:
        SystemExit: 命令不存在或返回非零。
    """
    try:
        subprocess.run(command, cwd=cwd, check=True)
    except FileNotFoundError as error:
        raise SystemExit(f"找不到命令 {command[0]}：{error}") from error
    except subprocess.CalledProcessError as error:
        raise SystemExit(
            f"命令失败（返回码 {error.returncode}）：{' '.join(command)}"
        ) from error


def normalize_version(raw: str) -> str:
    """把 release 的 tag 变成版本号（去掉前导 `v`）。

    Args:
        raw: 形如 `v0.2.0` 的 tag，也接受已经是版本号的 `0.2.0`。

    Returns:
        合法的版本号字符串。

    Raises:
        SystemExit: 去掉前缀后为空或含非法字符。
    """
    version = raw[1:] if raw.startswith("v") else raw
    if not _VERSION_RE.match(version):
        raise SystemExit(f"版本号 {raw!r} 不能作为 ZIP 里的版本：{_VERSION_RE.pattern}")
    return version


def extract_package(package: Path, destination: Path) -> Path:
    """用 `bsdtar` 把 pacman 包解开。

    Args:
        package: `.pkg.tar.zst` 文件。
        destination: 解包目标目录（已存在）。

    Returns:
        解包根目录，等于 `destination`。

    Raises:
        SystemExit: 包不存在或解包失败。
    """
    if not package.is_file():
        raise SystemExit(f"找不到输入包：{package}")
    # `bsdtar` 以 `destination` 为工作目录跑，相对路径会落到解包目录里去，所以这里
    # 先按调用方的当前目录解析成绝对路径。
    _run(["bsdtar", "-xf", str(package.resolve()), "-C", str(destination)], destination)
    return destination


def collect_payload(extracted: Path) -> Path:
    """校验便携分发树并把三样东西挑出来。

    Args:
        extracted: 解包根目录。

    Returns:
        `usr/lib/ds5dummy` 在解包树里的路径。

    Raises:
        SystemExit: 缺目录、缺二进制、二进制没有执行位，或某个目录是空的。
    """
    payload = extracted / PAYLOAD_PREFIX
    for part in PAYLOAD_PARTS:
        if not (payload / part).is_dir():
            raise SystemExit(f"输入包里没有 {PAYLOAD_PREFIX / part}，不是预期的便携构建")
    binary = payload / "bin" / _BIN_NAME
    if not binary.is_file():
        raise SystemExit(f"输入包里没有 {PAYLOAD_PREFIX / 'bin' / _BIN_NAME}")
    if not binary.stat().st_mode & 0o111:
        raise SystemExit(f"{binary} 没有执行位，装到 Deck 上也起不来")
    for part in ("lib", "plugins"):
        if not any((payload / part).iterdir()):
            raise SystemExit(f"输入包里的 {PAYLOAD_PREFIX / part} 是空的")
    return payload


def build_frontend() -> Path:
    """在 `decky/` 里装依赖并构建前端。

    `npm ci` 用提交在仓库里的 `package-lock.json`，保证 CI 与本机装出同一棵依赖树。

    Returns:
        `dist/index.js` 的路径。

    Raises:
        SystemExit: npm 失败或构建产物缺失。
    """
    _run(["npm", "ci", "--no-audit", "--no-fund"], DECKY_DIR)
    _run(["npm", "run", "build"], DECKY_DIR)
    bundle = DECKY_DIR / "dist" / "index.js"
    if not bundle.is_file():
        raise SystemExit("npm run build 没有产出 dist/index.js")
    return bundle


def copy_python_package(destination: Path) -> None:
    """把本地 Python 包放进 `py_modules/`（loader 只把这一层加进 `sys.path`）。

    Args:
        destination: 插件根目录（staging 里的 `ds5dummy/`）。
    """
    modules = destination / "py_modules"
    modules.mkdir(parents=True)
    shutil.copytree(
        DECKY_DIR / "ds5dummy",
        modules / "ds5dummy",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )


def stage_plugin(staging: Path, payload: Path, version: str) -> Path:
    """搭出 ZIP 里那份插件目录。

    Args:
        staging: 空的临时目录。
        payload: `collect_payload` 返回的便携分发目录。
        version: 已规范化的版本号。

    Returns:
        staging 里的插件根目录。

    Raises:
        SystemExit: 源文件缺失。
    """
    plugin = staging / PLUGIN_DIR_NAME
    plugin.mkdir()

    # 入口与元数据。plugin.json 原样复制：版本号由 package.json 承载（loader 从那里
    # 取），往 plugin.json 里塞它不认识的键只会多一处得记住同步的副本。
    for name in ("plugin.json", "main.py", "README.md"):
        source = DECKY_DIR / name
        if not source.is_file():
            raise SystemExit(f"缺少插件的 {name}：{source}")
        shutil.copy2(source, plugin / name)

    package_json = {
        "name": PLUGIN_DIR_NAME,
        "version": version,
        "description": "在 Steam Deck 上以虚拟 USB 身份转发蓝牙 DualSense。",
        "author": "zhh7ce",
        # loader 按这个字段决定用 ESM 加载 dist/index.js；缺了它会退回复制旧的
        # IIFE eval 方式，我们的 bundle 是 ESM，装上也加载不起来。
        "type": "module",
    }
    (plugin / "package.json").write_text(
        json.dumps(package_json, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (plugin / "VERSION").write_text(version + "\n", encoding="utf-8")

    copy_python_package(plugin)

    # 前端 bundle：loader 只认 `dist/index.js`，整目录复制（sourcemap 一起带上，
    # 现场排查面板问题时才有得看）。
    dist = DECKY_DIR / "dist"
    if not (dist / "index.js").is_file():
        raise SystemExit(f"缺少前端产物：{dist / 'index.js'}（先 npm run build）")
    shutil.copytree(dist, plugin / "dist", copy_function=shutil.copy2)

    for part in PAYLOAD_PARTS:
        shutil.copytree(payload / part, plugin / part, copy_function=shutil.copy2)

    docs = payload / "share" / "doc" / "ds5dummy"
    if docs.is_dir():
        shutil.copytree(docs, plugin / "share" / "doc" / "ds5dummy")

    return plugin


def write_zip(plugin: Path, output: Path) -> None:
    """把插件目录压成 ZIP；条目时间戳固定、权限位取自文件本身。

    `zipfile.extractall` 在 Decky 侧解包时会按 `external_attr` 恢复权限，所以这里
    必须写真实的 mode；时间戳固定成 1980-01-01 是为了同样的输入产出逐字节相同的
    ZIP（重复打包不会因为构建时刻不同而处处变脏）。

    Args:
        plugin: staging 里的插件根目录。
        output: 输出 ZIP 路径；父目录自动创建。
    """
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as bundle:
        for path in sorted(plugin.rglob("*")):
            if path.is_dir():
                continue
            relative = path.relative_to(plugin).as_posix()
            info = zipfile.ZipInfo(
                f"{PLUGIN_DIR_NAME}/{relative}", date_time=(1980, 1, 1, 0, 0, 0)
            )
            info.external_attr = (path.stat().st_mode & 0o7777) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix，权限位才会被解包方采信
            bundle.writestr(info, path.read_bytes())


def main(argv: list[str]) -> int:
    """命令行入口。

    Args:
        argv: 不含程序名的参数列表。

    Returns:
        进程退出码：0 成功，1 失败（错误信息已打到 stderr）。
    """
    parser = argparse.ArgumentParser(
        description="把 ds5dummy_bin 的 pacman 包打成 Decky 插件 ZIP"
    )
    parser.add_argument("--pkg", required=True, type=Path, help="ds5dummy_bin.pkg.tar.zst")
    parser.add_argument("--version", required=True, help="release tag（v 前缀可有可无）")
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "build" / "decky" / "ds5dummy_decky.zip",
        help="输出 ZIP 路径（缺省 build/decky/ds5dummy_decky.zip）",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="复用已有的 decky/dist/，不跑 npm（本机调试用）",
    )
    args = parser.parse_args(argv)

    version = normalize_version(args.version)
    bundle = DECKY_DIR / "dist" / "index.js"
    if args.skip_build:
        if not bundle.is_file():
            raise SystemExit("--skip-build 要求 decky/dist/index.js 已存在")
    else:
        build_frontend()

    # 所有临时文件都放在 build/ 下（仓库内的临时区），不落系统临时目录。
    scratch_root = REPO_ROOT / "build" / "decky"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch_root, prefix="pack-") as scratch:
        extracted = Path(scratch) / "package"
        extracted.mkdir()
        extract_package(args.pkg, extracted)
        payload = collect_payload(extracted)
        plugin = stage_plugin(Path(scratch), payload, version)
        write_zip(plugin, args.out)

    count = sum(1 for path in zipfile.ZipFile(args.out).infolist() if not path.is_dir())
    size = args.out.stat().st_size
    print(f"{args.out}：{count} 个文件，{size / 1024 / 1024:.1f} MiB（版本 {version}）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
