#!/usr/bin/env python3
"""把仓库这一份源码打成 Decky 插件 ZIP（自编前端 + 自编后端，官方形状）。

输入不再是任何预先打好的包：本脚本自己调 CMake 编出便携后端（数据面程序、配置
校验器、随包 `.so`），自己调 npm 编出前端，然后按 decky-loader 官方 CLI
（`SteamDeckHomebrew/cli` 的 `plugin build`）那套白名单组装 ZIP。两边的文件来源
完全不同，产物形状却一致，所以将来交商店不需要重排。

ZIP 里允许出现什么，是官方 CLI 的硬规则（`src/cli/plugin/build.rs::zip_plugin`）：

```
<plugin.json 的 name>/
  plugin.json          loader 读 name/author/flags/api_version
  package.json         loader 从它取版本号；`type: module` 决定按 ESM 加载 dist/
  main.py              入口文件名是 loader 写死的
  README.md  LICENSE   官方白名单要求根上有这两份（缺了官方 CLI 直接 panic）
  *.py                 根上其它 Python 文件会被自动带上
  dist/index.js        npm run build 的产物，唯一前端入口
  py_modules/ds5dummy/ loader 只把 <插件>/py_modules 加进 sys.path
  bin/ds5dummy/        整棵便携树原样嵌在这里（官方 CLI 递归收录 bin/）
```

**`bin/ds5dummy/` 里是完整的一棵树**，与 tarball 解压后逐字节相同：`bin/` 与
`lib/` 保持兄弟关系（RUNPATH 是 `$ORIGIN/../lib`），程序按自身位置推树根，所以
同一个后端在 tarball 与插件里行为一致。这层目录名固定（不带版本、不带架构），
升级换版本不会让路径变。

打包前会校验"便携"这件事本身：RUNPATH 必须指向树内 `lib/`、`ldd` 不能有
`not found`、三个二进制都必须在。不合格直接失败，不打出一个装上去起不来的 ZIP。

依赖三个外部命令：`cmake`（编后端）、`npm`（编前端）、`readelf`/`ldd`（校验）。
缺哪个直接报错，不做静默降级。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DECKY_DIR = REPO_ROOT / "decky"

# 插件目录里那棵便携树的固定目录名（`bin/ds5dummy/`）。不带版本号与架构：插件
# 目录里只有一个后端，版本看 plugin.json / package.json。
TREE_DIR_NAME = "ds5dummy"

# 官方 CLI 用来放原生后端产物的目录名。
PAYLOAD_PARENT = "bin"

# 后端必须有的三个二进制：缺一个就说明构建没跑全，不能打成"能用"的包。
REQUIRED_BINARIES = ("ds5dummyd", "buttons_check", "ds5dummy-config")

# RUNPATH 必须包含这一项：树里 `bin/` 与 `lib/` 是兄弟（见 CMakeLists 的
# ds5dummy_make_portable）。
REQUIRED_RUNPATH = "$ORIGIN/../lib"

# 版本号只允许这些字符：它会进文件名、ZIP 内文本与 Decky 的目录名，出现空格或
# 斜杠会破坏其中至少一处。前导 `v` 在解析阶段就去掉。
_VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]*$")

# 以前那个"吃 pacman 包"的版本默认输出这个文件名，保持不变，省得手册与脚本失效。
_DEFAULT_OUTPUT = REPO_ROOT / "build" / "decky" / "ds5dummy-decky.zip"


def _run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    """跑一个外部命令；失败时把命令与返回码一起抛出。

    Args:
        command: argv，第一项是可执行文件名。
        cwd: 工作目录。
        env: 完整环境；None 表示继承当前进程。

    Raises:
        SystemExit: 命令不存在或返回非零。
    """
    try:
        subprocess.run(command, cwd=cwd, check=True, env=env)
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


def read_plugin_meta(decky_dir: Path) -> dict:
    """读 `plugin.json`；ZIP 的顶层目录名用它，缺了就没法装。

    Args:
        decky_dir: 插件源码目录（`decky/`）。

    Returns:
        解析后的 JSON 对象。

    Raises:
        SystemExit: 文件缺失、不是 JSON，或缺 `name`。
    """
    path = decky_dir / "plugin.json"
    try:
        meta = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SystemExit(f"读不到 {path}：{error}") from error
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path} 不是合法 JSON：{error}") from error
    if not isinstance(meta, dict) or not isinstance(meta.get("name"), str):
        raise SystemExit(f"{path} 缺少 name 字段（ZIP 顶层目录名用它）")
    return meta


def build_frontend(decky_dir: Path) -> Path:
    """在 `decky/` 里装依赖并构建前端。

    `npm install` 按提交在仓库里的 `package-lock.json` 解析版本；不用 `npm ci`
    是因为它会先把 `node_modules` 整个删掉，在只读/受限环境里会直接失败，而
    我们要的只是"版本按 lock 走"这件事。

    Args:
        decky_dir: 插件源码目录。

    Returns:
        `dist/index.js` 的路径。

    Raises:
        SystemExit: npm 失败或构建产物缺失。
    """
    _run(["npm", "install", "--no-audit", "--no-fund"], decky_dir)
    _run(["npm", "run", "build"], decky_dir)
    bundle = decky_dir / "dist" / "index.js"
    if not bundle.is_file():
        raise SystemExit("npm run build 没有产出 dist/index.js")
    return bundle


def build_backend(build_dir: Path, backend: str) -> None:
    """配置并编译便携后端（`DS5DUMMY_PORTABLE` + `DS5DUMMY_LAYOUT=tree`）。

    两个开关都不能省：没有 PORTABLE 就没有随包 yaml-cpp 与 RUNPATH，换台机器起
    不来；没有 tree 布局程序会去读 `/etc/ds5dummy`，插件目录里的树就白带了。

    Args:
        build_dir: CMake 构建目录（会被复用，增量编译更快）。
        backend: `hidraw` 或 `l2cap`，对应 `BT_BACKEND`。

    Raises:
        SystemExit: 配置或编译失败。
    """
    _run(
        [
            "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DBT_BACKEND={backend.upper()}",
            "-DDS5DUMMY_PORTABLE=ON",
            "-DDS5DUMMY_LAYOUT=tree",
            "-DDS5DUMMY_STRICT=ON",
            "-DDS5DUMMY_BUILD_TOOLS=OFF",
            "-DDS5DUMMY_BUILD_TESTS=OFF",
        ],
        REPO_ROOT,
    )
    _run(
        [
            "cmake", "--build", str(build_dir),
            "--target", "ds5dummyd", "buttons_check", "ds5dummy_idle", "ds5dummy_buttons",
            "-j", str(os.cpu_count() or 1),
        ],
        REPO_ROOT,
    )


def install_tree(build_dir: Path, destination: Path) -> Path:
    """把编好的东西按树铺到 `destination`（与 `dist` 目标同一份 install 清单）。

    走 `cmake --install` 而不是"打 tarball 再解开"：后者多一道压缩解压，还得依赖
    tar；这里直接把 staging 当 DESTDIR，得到的树与 tarball 里那份一致。

    Args:
        build_dir: CMake 构建目录。
        destination: 树的根（`<staging>/tree`）。

    Returns:
        `destination`，便于链式调用。

    Raises:
        SystemExit: 安装失败或树里缺二进制。
    """
    destination.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["DESTDIR"] = str(destination)
    _run(
        ["cmake", "--install", str(build_dir), "--prefix", "/", "--strip"],
        REPO_ROOT,
        env=env,
    )
    for name in REQUIRED_BINARIES:
        binary = destination / "bin" / name
        if not binary.is_file():
            raise SystemExit(f"树里缺 bin/{name}：install 清单与构建目标不一致")
    return destination


def read_runpath(binary: Path) -> list[str]:
    """读 ELF 的 RUNPATH（`readelf -d`）。

    Args:
        binary: 待检查的可执行文件。

    Returns:
        RUNPATH 里的目录列表；没有该项时为空。

    Raises:
        SystemExit: `readelf` 不存在或读失败。
    """
    try:
        output = subprocess.run(
            ["readelf", "-d", str(binary)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except FileNotFoundError as error:
        raise SystemExit(f"找不到 readelf（binutils）：{error}") from error
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"readelf 读不了 {binary}：{error}") from error

    match = re.search(r"\(RUNPATH\).*\[([^\]]*)\]", output)
    return match.group(1).split(":") if match else []


def check_tree(tree: Path) -> None:
    """校验这棵树真的能"搬走就跑"。

    三条判据：RUNPATH 指向树内 `lib/`、`ldd` 没有 `not found`、随包的 yaml-cpp
    在 `lib/` 里。任何一条不过都直接失败——打出一个装上去起不来的 ZIP 比不打包
    更糟，用户只会看到"插件装了但没反应"。

    Args:
        tree: `install_tree` 铺出来的树根。

    Raises:
        SystemExit: 任何一条判据不过。
    """
    binary = tree / "bin" / "ds5dummyd"
    runpath = read_runpath(binary)
    if REQUIRED_RUNPATH not in runpath:
        raise SystemExit(
            f"{binary} 的 RUNPATH 是 {runpath or '（没有）'}，"
            f"必须是含 {REQUIRED_RUNPATH} 的便携构建"
        )
    if not any((tree / "lib").glob("libyaml-cpp.so*")):
        raise SystemExit(f"{tree}/lib 里没有随包的 libyaml-cpp，换台机器会起不来")

    missing = []
    for name in REQUIRED_BINARIES:
        path = tree / "bin" / name
        try:
            output = subprocess.run(
                ["ldd", str(path)], capture_output=True, text=True, check=False
            ).stdout
        except FileNotFoundError as error:
            raise SystemExit(f"找不到 ldd：{error}") from error
        for line in output.splitlines():
            if "not found" in line:
                missing.append(f"{name}: {line.strip()}")
    if missing:
        raise SystemExit("这些依赖在构建机上就找不到，装上一定起不来：\n  " + "\n  ".join(missing))


def copy_python_package(destination: Path, decky_dir: Path) -> None:
    """把本地 Python 包放进 `py_modules/`（loader 只把这一层加进 `sys.path`）。

    Args:
        destination: 插件根目录（staging 里的 `<plugin.json 的 name>/`）。
        decky_dir: 插件源码目录。
    """
    modules = destination / "py_modules"
    modules.mkdir(parents=True)
    shutil.copytree(
        decky_dir / "ds5dummy",
        modules / "ds5dummy",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )


def write_package_json(destination: Path, version: str, meta: dict) -> None:
    """写 `package.json`：loader 从这里取版本号，`type: module` 决定 ESM 加载。

    Args:
        destination: 插件根目录。
        version: 已规范化的版本号。
        meta: `plugin.json` 的内容，作者与描述沿用它的 publish 段。
    """
    publish = meta.get("publish") if isinstance(meta.get("publish"), dict) else {}
    payload = {
        "name": TREE_DIR_NAME,
        "version": version,
        "description": publish.get("description", "DS5Dummy Decky 插件"),
        "author": meta.get("author", ""),
        # loader 按这个字段决定用 ESM 加载 dist/index.js；缺了它会退回旧的
        # IIFE eval 方式，我们的 bundle 是 ESM，装上也加载不起来。
        "type": "module",
    }
    (destination / "package.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def stage_plugin(
    staging: Path, tree: Path, version: str, decky_dir: Path | None = None
) -> Path:
    """按官方白名单搭出 ZIP 里那份插件目录。

    Args:
        staging: 空的临时目录。
        tree: `install_tree` 铺出来的便携树。
        version: 已规范化的版本号。
        decky_dir: 插件源码目录；None 表示用模块级 `DECKY_DIR`（测试会替换它）。

    Returns:
        staging 里的插件根目录（名字取自 `plugin.json` 的 name）。

    Raises:
        SystemExit: 源文件缺失（根上那几份缺一不可，官方 CLI 同样会失败）。
    """
    source = decky_dir or DECKY_DIR
    meta = read_plugin_meta(source)
    plugin = staging / meta["name"]
    plugin.mkdir()

    # 根上必需的四份 + LICENSE：官方 CLI 对这五份是硬要求（`fs::read` 直接
    # unwrap），我们也在这里卡住。
    for name, origin in (
        ("plugin.json", source / "plugin.json"),
        ("main.py", source / "main.py"),
        ("README.md", source / "README.md"),
        ("LICENSE", REPO_ROOT / "LICENSE"),
    ):
        if not origin.is_file():
            raise SystemExit(f"缺少 {origin}（ZIP 根上必须带 {name}）")
        shutil.copy2(origin, plugin / name)

    write_package_json(plugin, version, meta)
    # 面板显示的版本号：与 package.json 同源，写在同一层方便 main.py 读。
    (plugin / "VERSION").write_text(version + "\n", encoding="utf-8")

    copy_python_package(plugin, source)

    # 前端 bundle：loader 只认 `dist/index.js`，整目录复制（sourcemap 一起带上，
    # 现场排查面板问题时才有得看）。
    dist = source / "dist"
    if not (dist / "index.js").is_file():
        raise SystemExit(f"缺少前端产物：{dist / 'index.js'}（先 npm run build）")
    shutil.copytree(dist, plugin / "dist", copy_function=shutil.copy2)

    # 整棵便携树原样嵌进 bin/：官方 CLI 对 bin/ 递归收录，树内形状不变，所以
    # 程序推树根、RUNPATH、插件目录查找全都只有一套规则。
    payload = plugin / PAYLOAD_PARENT / TREE_DIR_NAME
    payload.parent.mkdir(parents=True)
    shutil.copytree(tree, payload, symlinks=True, copy_function=shutil.copy2)

    return plugin


def write_zip(plugin: Path, output: Path) -> None:
    """把插件目录压成 ZIP；条目时间戳固定、权限位取自文件本身。

    时间戳固定成 1980-01-01 是为了同样的输入产出逐字节相同的 ZIP（重复打包不会
    因为构建时刻不同而处处变脏）。官方 CLI 不这么做，但那是它的事，不是约束。

    Args:
        plugin: staging 里的插件根目录。
        output: 输出 ZIP 路径；父目录自动创建。
    """
    output.parent.mkdir(parents=True, exist_ok=True)
    top = plugin.name
    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as bundle:
        for path in sorted(plugin.rglob("*")):
            if path.is_dir():
                continue
            relative = path.relative_to(plugin).as_posix()
            info = zipfile.ZipInfo(f"{top}/{relative}", date_time=(1980, 1, 1, 0, 0, 0))
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
        description="从源码编出后端与前端，打成 Decky 插件 ZIP"
    )
    parser.add_argument("--version", required=True, help="release tag（v 前缀可有可无）")
    parser.add_argument(
        "--out",
        type=Path,
        default=_DEFAULT_OUTPUT,
        help=f"输出 ZIP 路径（缺省 {_DEFAULT_OUTPUT.relative_to(REPO_ROOT)}）",
    )
    parser.add_argument(
        "--backend",
        choices=("hidraw", "l2cap"),
        default="hidraw",
        help="蓝牙后端（缺省 hidraw，SteamOS 用的就是它）",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="复用已有的 decky/dist/ 与后端构建目录，不跑 npm 与 cmake 编译",
    )
    args = parser.parse_args(argv)

    version = normalize_version(args.version)

    if not args.skip_build:
        build_frontend(DECKY_DIR)

    # 后端构建目录复用（增量编译）；staging 每次都是新的临时目录。
    build_dir = REPO_ROOT / "build" / "decky-cmake"
    scratch_root = REPO_ROOT / "build" / "decky"
    scratch_root.mkdir(parents=True, exist_ok=True)
    # 用 mkdtemp + 尽力清理而不是 TemporaryDirectory：ZIP 已经写出去之后，清理失败
    # 不该让整条命令变成失败（受限环境里删目录可能被拦）。
    scratch = Path(tempfile.mkdtemp(dir=scratch_root, prefix="pack-"))
    try:
        tree = scratch / "tree"
        if args.skip_build:
            if not (build_dir / "CMakeCache.txt").is_file():
                raise SystemExit(f"--skip-build 要求 {build_dir} 里已有配置好的构建")
        else:
            build_backend(build_dir, args.backend)
        install_tree(build_dir, tree)
        check_tree(tree)
        plugin = stage_plugin(scratch, tree, version)
        write_zip(plugin, args.out)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    count = sum(1 for path in zipfile.ZipFile(args.out).infolist() if not path.is_dir())
    size = args.out.stat().st_size
    print(f"{args.out}：{count} 个文件，{size / 1024 / 1024:.1f} MiB（版本 {version}）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
