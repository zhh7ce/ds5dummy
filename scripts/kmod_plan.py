#!/usr/bin/env python3
"""发布侧的两件判断：本轮该为哪些内核构建，以及一份产物是否可用。

`kmod.yml` 只负责"按计划执行"，所有需要判断的地方都在这里，于是判断可以离线跑用例
（A-01、A-02）。四件事：

1. **发现与计划**（`discover`）：读各 `jupiter-*` 仓库的 `.db`，挑出全部内核头文件
   包（P-04），结合 `seen.json`（已知的头文件包 → 内核 release + 补丁指纹）与 Release
   现有资产决定本轮构建哪些；定时轮次跳过已有同名资产的（P-26），补丁指纹与某一份
   产物记录不同的只重建那一个内核（P-25），手动轮次重建 `seen.json` 里的全部已发布
   内核。低于支持下限（`MIN_KERNEL_VERSION`）的内核不进矩阵。每个频道发布在自己的
   固定 Release 里
   （tag 就是频道名），同一个头文件包在几个频道出现就各建一次：设备端按频道依次
   查找，命中的那份来自该频道自己的工具链。
2. **头文件树自证**（`tree-release`）：读 `include/config/kernel.release` 与 `Makefile`
   头部的版本字段互相核对，给出本轮的期望 release（P-14）。
3. **产物判定**（`verify`）：大小、ELF64、`vermagic` 的 release 段与期望逐字相等、
   `srcversion` 可读（P-23）。
4. **取头文件包**（`fetch-header`）：按矩阵行里记的文件名从频道仓库取回并解出
   `lib/modules/<release>/build` 树；取之前先在 `.db` 里复核版本与文件名，防止矩阵
   与镜像在一轮之内错开。

判断全是纯函数，只在对外的四个子命令里碰文件与网络；`scripts/tests/` 用固定夹具跑
它们，不需要内核、工具链或特权。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from steamos_repo import (
    DEFAULT_MIRROR,
    Package,
    SteamosError,
    extract_package,
    fetch,
    load_repo,
    package_url,
)

#: 轮询的 SteamOS 频道仓库（P-01）。`jupiter-rel` 只有 5.x/6.0 的旧包，不在此列。
DISCOVERY_CHANNELS = (
    "jupiter-3.8",
    "jupiter-3.8.1x",
    "jupiter-3.9",
    "jupiter-3.10",
    "jupiter-staging",
    "jupiter-main",
)

#: 头文件包名的形状：`linux-neptune[-<NN>][-<变体>]-headers`（P-04）。变体名不参与
#: 判断，凡是 Linux Neptune 的 `-headers` 都收。
_HEADER_NAME_RE = re.compile(r"^linux-neptune(-[A-Za-z0-9._-]+)*-headers$")

#: 支持的最低内核版本 `(major, minor)`。5.x（SteamOS 3.0/3.1 时代的 5.13）已停止
#: 支持：它的头文件包带着 Valve 用当年那代 gcc 编的预编译 gcc 插件，与频道仓库现在
#: 的 gcc 加载不上（插件 ABI 跨大版本不兼容），而这些镜像也不会再刷新它。
MIN_KERNEL_VERSION = (6, 0)

#: 产物大小上限（字节，P-23 的"合理上限"）。实测未压缩模块约 1 MiB，8 MiB 留足余量
#: 又能在误传整棵内核树时立刻失败。
MAX_MODULE_BYTES = 8 * 1024 * 1024

#: ELF 魔数与 ELFCLASS64 的取值；模块必须能按 64 位 ELF 解析。
_ELF_MAGIC = b"\x7fELF"
_ELF_CLASS64 = 2

#: `Makefile` 头部版本字段的行形状，如 `PATCHLEVEL = 18`。
_MAKEFILE_VERSION_RE = re.compile(
    r"^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)\s*=\s*(\S*)", re.MULTILINE
)

#: `seen.json` 的结构版本；格式一变就拒绝旧文件，宁可重下一轮也不误读。
_SEEN_FORMAT = 1

#: 构建记录行（P-27）缺少任何一项就不写：说明里的每一行都要能追到源码与工具链。
_NOTE_REQUIRED = (
    "release", "channel", "name", "version", "patch_fingerprint", "gcc", "built_at",
)

#: 记录行里显示的补丁指纹长度（十六进制字符）。64 位全串在说明里太长，前 12 位
#: 足以在同一轮的多行里区分，完整值留在产物目录的 meta.json 里。
_NOTE_FINGERPRINT_CHARS = 12


@dataclass(frozen=True)
class BuildTask:
    """矩阵里的一行：为某个头文件包构建一份模块。

    Attributes:
        channel: 头文件包所在频道仓库（`jupiter-3.8`），同时决定工具链仓库。
        core_repo: 该频道的 core 仓库（`core-3.8`），gcc/binutils/make 从这里取。
        extra_repo: 该频道的 extra 仓库（`extra-3.8`），pahole 从这里取。
        name: 头文件包名。
        version: 头文件包版本（`6.18.50.valve2-1`），只用于下载与记录。
        filename: 头文件包文件名。
        release: 已知的内核 release；未知时为空串，由构建阶段解出后回填。
        reason: `new`（首次构建）或 `rebuild`（补丁指纹变化后重建）。
    """

    channel: str
    core_repo: str
    extra_repo: str
    name: str
    version: str
    filename: str
    release: str
    reason: str

    def as_matrix_row(self) -> dict[str, str]:
        """转成 GitHub Actions 矩阵的一行；值一律是字符串，空 release 用空串。"""
        return {
            "channel": self.channel,
            "core_repo": self.core_repo,
            "extra_repo": self.extra_repo,
            "name": self.name,
            "version": self.version,
            "filename": self.filename,
            "release": self.release,
            "reason": self.reason,
        }


def toolchain_repos(channel: str) -> tuple[str, str]:
    """频道仓库 → 它的工具链仓库 `(core, extra)`（P-11）。

    频道名与工具链仓库的后缀一致（`jupiter-3.8` → `core-3.8`），所以不需要维护
    一张表；六个频道全部由这一条推导。

    Args:
        channel: 频道仓库名，必须以 `jupiter-` 开头。

    Returns:
        `(core-<后缀>, extra-<后缀>)`。

    Raises:
        SteamosError: 名字不是 `jupiter-*`，推导不出工具链仓库。
    """
    prefix = "jupiter-"
    if not channel.startswith(prefix) or len(channel) == len(prefix):
        raise SteamosError(f"{channel!r} 不是 channel 仓库，无法推导工具链仓库")
    suffix = channel[len(prefix):]
    return f"core-{suffix}", f"extra-{suffix}"


def is_header_package(name: str) -> bool:
    """判断包名是否是内核头文件包（P-04 的四种形状都算）。"""
    return bool(_HEADER_NAME_RE.match(name))


def kernel_version(version: str) -> tuple[int, int] | None:
    """头文件包版本 → `(major, minor)`。

    Args:
        version: 包版本，如 `6.18.50.valve2-1`。

    Returns:
        前两段解析成整数；形状认不出（不以数字开头、段数不足）时返回 None，由调用方
        决定是保留还是丢弃。
    """
    parts = version.split(".")
    if len(parts) < 2:
        return None
    try:
        return int(parts[0]), int(parts[1])
    except ValueError:
        return None


def is_supported_kernel(version: str) -> bool:
    """版本是否不低于 `MIN_KERNEL_VERSION`。

    认不出形状的版本按"支持"处理：宁可让它在构建阶段失败并被人看见，也不静默丢掉
    一个可能是新格式的内核包。
    """
    parsed = kernel_version(version)
    return parsed is None or parsed >= MIN_KERNEL_VERSION


def asset_name(release: str) -> str:
    """内核 release → Release 资产名（P-13、P-26）：`<release>.ko`，不带版本号。"""
    return f"{release}.ko"


def seen_key(channel: str, name: str) -> str:
    """`seen.json` 的键：频道 + 包名。

    同一个包名可以同时出现在多个频道（发布后后续频道的仓库里也有它），每个频道
    各自发布在自己的 Release 里，所以按频道分开记录：两份产物的工具链不同，
    版本也可能不同。
    """
    return f"{channel}/{name}"


def empty_seen() -> dict[str, object]:
    """一份空的 `seen.json` 结构；补丁指纹记在每份产物自己的条目里。"""
    return {"version": _SEEN_FORMAT, "packages": {}}


def load_seen(path: Path) -> dict[str, object]:
    """读 `seen.json`；文件不存在视为空，结构不对则失败。

    Args:
        path: `seen.json` 路径。

    Returns:
        `{"version": 1, "packages": {key: entry}}`；条目里的 `patch_fingerprint`
        缺失（旧缓存）按"没记录过"处理，那一份会在下一轮重建一次。

    Raises:
        SteamosError: JSON 损坏、版本不符、`packages` 不是对象。
    """
    if not path.exists():
        return empty_seen()
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SteamosError(f"{path}: 读不了 seen.json（{error}）") from error
    if document.get("version") != _SEEN_FORMAT:
        raise SteamosError(
            f"{path}: seen.json 版本是 {document.get('version')!r}，"
            f"本脚本只认 {_SEEN_FORMAT}"
        )
    if not isinstance(document.get("packages"), dict):
        raise SteamosError(f"{path}: seen.json 的 packages 不是对象")
    return document


def apply_results(
    seen: dict[str, object],
    results: list[dict[str, str]],
    fingerprint: str,
) -> dict[str, object]:
    """把本轮成功的结果并进 `seen.json`，每份产物记下编它用的补丁指纹。

    只有成功的结果会被并入：失败的条目保持原样（旧版本、旧指纹，或者根本不存在），
    于是下一轮的 `plan()` 会把它当成"还没有本指纹的产物"再建一次。指纹记在条目里
    而不是整份文件里，就是为了让成功与失败各走各的——整份文件只有一个指纹时，一轮
    部分失败也会把指纹推进，失败的那几个内核就再也不会被重试。

    Args:
        seen: `load_seen()` 给出的结构。
        results: 构建 job 写出的 meta.json 列表，每项含
            `channel`/`name`/`version`/`filename`/`release`。
        fingerprint: 本轮 `kmod/` 的内容指纹；必填，它要写进每一条新记录。

    Returns:
        新的结构（不原地修改，便于测试对账）。

    Raises:
        SteamosError: 指纹为空，或结果缺字段（只能记录真正构建成功的）。
    """
    if not fingerprint:
        raise SteamosError("record 需要一个非空的补丁指纹才能写 seen.json")
    packages = dict(seen["packages"])  # type: ignore[arg-type]
    for result in results:
        missing = [
            key
            for key in ("channel", "name", "version", "filename", "release")
            if not result.get(key)
        ]
        if missing:
            raise SteamosError(f"构建结果缺少字段 {missing}: {result!r}")
        key = seen_key(result["channel"], result["name"])
        packages[key] = {
            "channel": result["channel"],
            "name": result["name"],
            "version": result["version"],
            "filename": result["filename"],
            "release": result["release"],
            "patch_fingerprint": fingerprint,
        }
    return {"version": _SEEN_FORMAT, "packages": packages}


def load_metas(directory: Path) -> list[dict[str, str]]:
    """读构建 job 留下的 `meta.json`（每个产物目录一份）。

    Args:
        directory: 产物根目录；按 `meta.json` 递归查找，找不到返回空列表。

    Returns:
        解析出的元数据字典，按路径排序。

    Raises:
        SteamosError: 某个文件不是合法 JSON 对象。
    """
    metas: list[dict[str, str]] = []
    for path in sorted(directory.glob("**/meta.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise SteamosError(f"{path}: 读不了（{error}）") from error
        if not isinstance(document, dict):
            raise SteamosError(f"{path}: 不是 JSON 对象")
        metas.append(document)
    return metas


def note_line(meta: dict[str, str]) -> str:
    """构建记录行（P-27）：内核 release、频道、头文件包版本、补丁指纹、工具链、时间。

    Args:
        meta: 构建 job 写出的元数据；必需字段见 `_NOTE_REQUIRED`。

    Returns:
        以 `- ` 开头的一行 markdown；补丁指纹只取前 `_NOTE_FINGERPRINT_CHARS` 位。
        重建轮次在时间前加"重建"，对应 P-25 的"说明里记录变化与原因"。

    Raises:
        SteamosError: 缺必需字段。
    """
    missing = [key for key in _NOTE_REQUIRED if not meta.get(key)]
    if missing:
        raise SteamosError(f"构建记录缺少字段 {missing}: {meta!r}")
    parts = [
        f"- `{meta['release']}`",
        meta["channel"],
        f"{meta['name']} {meta['version']}",
        f"patch {meta['patch_fingerprint'][:_NOTE_FINGERPRINT_CHARS]}",
        f"gcc {meta['gcc']}",
    ]
    if meta.get("pahole"):
        parts.append(f"pahole {meta['pahole']}")
    if meta.get("reason") == "rebuild":
        parts.append("重建")
    parts.append(meta["built_at"])
    return " · ".join(parts)


def select_header_packages(
    packages_by_channel: dict[str, list[Package]],
) -> dict[str, list[Package]]:
    """每频道只留头文件包，丢掉低于支持下限的内核，按包名排序。

    下限见 `MIN_KERNEL_VERSION`：5.x 的头文件包带着预编译 gcc 插件，与频道仓库现在
    的 gcc 加载不上，重试多少次都不会成功，所以不进矩阵。

    Args:
        packages_by_channel: 频道 → 该仓库 `.db` 里的全部包。

    Returns:
        同一形状，但只含受支持的头文件包；空频道不出现在结果里。
    """
    selected: dict[str, list[Package]] = {}
    for channel, packages in packages_by_channel.items():
        headers = sorted(
            (
                package for package in packages
                if is_header_package(package.name)
                and is_supported_kernel(package.version)
            ),
            key=lambda package: package.name,
        )
        if headers:
            selected[channel] = headers
    return selected


def plan(
    packages_by_channel: dict[str, list[Package]],
    seen: dict[str, object],
    assets: dict[str, set[str]],
    rebuild_all: bool,
    fingerprint: str,
) -> list[BuildTask]:
    """推导本轮要构建的矩阵。

    每个频道有自己的固定 Release（tag 就是频道名），所以同一个头文件包出现在几个
    频道里就构建几次：每个频道的 Release 都要能独立回答"这个内核有没有模块"，设备端
    按频道依次查找时，命中的那份就是用本频道工具链编出来的。

    定时轮次（`rebuild_all` 为假）需要构建的情形有三种，判断依据都是 `seen.json` 里
    那份产物自己的记录：

    - 没记录过，或头文件包版本与记录不同（包被重打过，release 可能已经不同）；
    - 记录里的 release 为空串（上次没走到解出内核树那一步）；
    - 记录里的补丁指纹与本轮 `kmod/` 的指纹不同——只重建这一个内核（P-25）。
      上一轮失败的内核留在记录里的是旧指纹（或没有记录），因此每轮都会自动重试，
      直到成功为止。

    记录对得上、且本频道 Release 里已有同名资产的，跳过（P-26）。

    重建轮次（`rebuild_all` 为真）：`seen.json` 里每个已发布内核都要重建并覆盖，
    另加当前 `.db` 里从未记录过或版本变了的包（可能是新内核）。

    低于 `MIN_KERNEL_VERSION` 的内核两轮都不构建。

    Args:
        packages_by_channel: 频道 → `.db` 全部包（未过滤）。
        seen: `load_seen()` 的结果。
        assets: 频道 → 该频道 Release 现有的资产名集合；缺的频道按空集算。
        rebuild_all: 是否本轮手动全量重建。
        fingerprint: 本轮 `kmod/` 的内容指纹；空串（没算出来）不构成重建理由。

    Returns:
        按 `(频道, 包名, 版本, release)` 排序的任务列表；可能为空。

    Raises:
        SteamosError: `seen.json` 条目缺字段。
    """
    headers = select_header_packages(packages_by_channel)
    known: dict[str, dict[str, str]] = seen["packages"]  # type: ignore[assignment]
    tasks: dict[tuple[str, str, str, str], BuildTask] = {}

    def add(channel: str, name: str, version: str, filename: str,
            release: str, reason: str) -> None:
        core, extra = toolchain_repos(channel)
        task = BuildTask(
            channel=channel,
            core_repo=core,
            extra_repo=extra,
            name=name,
            version=version,
            filename=filename,
            release=release,
            reason=reason,
        )
        tasks[(channel, name, version, release)] = task

    if rebuild_all:
        # 先排 `seen.json`：已发布过、但当前 `.db` 里可能已经没有的内核也要重建。
        for key in sorted(known):
            entry = known[key]
            for field in ("channel", "name", "version", "filename"):
                if not entry.get(field):
                    raise SteamosError(f"seen.json 条目 {key} 缺少 {field}")
            if not is_supported_kernel(entry["version"]):
                continue  # 低于下限的内核不再重建，留着旧产物即可
            release = entry.get("release", "")
            add(
                entry["channel"], entry["name"], entry["version"], entry["filename"],
                release, "rebuild" if release else "new",
            )

    for channel in sorted(headers):
        published = assets.get(channel, set())
        for package in headers[channel]:
            key = seen_key(channel, package.name)
            entry = known.get(key)
            # 只有版本一致时，记录里的 release 与指纹才能代表这份头文件包。
            same_version = (
                entry is not None and entry.get("version") == package.version
            )
            release = entry.get("release", "") if same_version else ""
            if rebuild_all:
                if same_version:
                    continue  # 上面已经排过同一个内核
                add(channel, package.name, package.version, package.filename, "", "new")
                continue
            if not release:
                add(channel, package.name, package.version, package.filename, "", "new")
                continue
            # 指纹对不上（含旧记录没有指纹）只重建这一份，不影响其它内核。
            stale = bool(fingerprint) and (
                entry.get("patch_fingerprint", "") != fingerprint
            )
            if stale:
                add(channel, package.name, package.version, package.filename,
                    release, "rebuild")
                continue
            if asset_name(release) in published:
                continue  # 定时轮次：本频道已有产物的内核不重复构建
            add(channel, package.name, package.version, package.filename, release, "new")

    return [tasks[key] for key in sorted(tasks)]


def matrix(tasks: list[BuildTask]) -> dict[str, list[dict[str, str]]]:
    """任务列表 → GitHub Actions 的 `matrix` 结构。"""
    return {"include": [task.as_matrix_row() for task in tasks]}


def parse_modinfo(text: str) -> dict[str, str]:
    """把 `modinfo` 输出拆成 键 → 值。

    同名的键取第一次出现的值（`parm:` 会重复，但判断只用到唯一的 `vermagic` 与
    `srcversion`）。

    Args:
        text: `modinfo <file>` 的完整输出。

    Returns:
        每个键对应一个已去掉首尾空白的值。
    """
    fields: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition(":")
        if not separator:
            continue  # 没有冒号的行不是字段
        fields.setdefault(key.strip(), value.strip())
    return fields


def check_module(module: Path, release: str, modinfo_text: str) -> None:
    """判定一份 `.ko` 能否作为 `release` 的产物发布（P-23）。

    判定顺序与日志顺序一致：先便宜的文件检查，再解析 `modinfo`，最后比 vermagic
    ——这个 release 段是设备端唯一的查找键，必须逐字相等（P-13）。

    Args:
        module: 已构建的 `.ko`。
        release: 期望的内核 release。
        modinfo_text: `modinfo <module>` 的输出。

    Raises:
        SteamosError: 文件不存在、为空、超限、不是 ELF64，或 vermagic/`srcversion`
            不合要求；消息即失败原因。
    """
    if not module.is_file():
        raise SteamosError(f"{module}: 不存在")
    size = module.stat().st_size
    if size == 0:
        raise SteamosError(f"{module}: 是空文件")
    if size > MAX_MODULE_BYTES:
        raise SteamosError(
            f"{module}: {size} 字节，超过上限 {MAX_MODULE_BYTES}（P-23）"
        )

    with open(module, "rb") as handle:
        header = handle.read(5)
    if header[:4] != _ELF_MAGIC or len(header) < 5 or header[4] != _ELF_CLASS64:
        raise SteamosError(f"{module}: 不是 ELF64 文件（P-23）")

    fields = parse_modinfo(modinfo_text)
    vermagic = fields.get("vermagic", "")
    if not vermagic:
        raise SteamosError(f"{module}: modinfo 里没有 vermagic（P-23）")
    # vermagic 形如 `<release> SMP preempt mod_unload`，release 是第一个词。
    actual = vermagic.split()[0]
    if actual != release:
        raise SteamosError(
            f"{module}: vermagic 是 {actual!r}，期望 {release!r}；"
            f"这份产物不能用在这个内核上（P-23）"
        )
    if not fields.get("srcversion"):
        raise SteamosError(f"{module}: modinfo 里读不出 srcversion（P-23）")


def kernel_release_from_file(text: str) -> str:
    """从 `include/config/kernel.release` 的内容取内核 release。

    Args:
        text: 文件全文；构建出的内核头文件树里这一行就是完整 release。

    Returns:
        去掉首尾空白的 release。

    Raises:
        SteamosError: 文件为空。
    """
    release = text.strip()
    if not release:
        raise SteamosError("include/config/kernel.release 是空的")
    return release


def check_kernel_tree(release: str, makefile_text: str) -> None:
    """核对头文件树的 `Makefile` 与它自己报告的 release（P-14）。

    只比能比的部分：`VERSION.PATCHLEVEL.SUBLEVEL` 必须逐段相等，`EXTRAVERSION`
    必须是 release 里紧随其后的那段。release 尾部的 `-neptune-<NN>-g<hash>` 来自
    localversion，不在 `Makefile` 头部，所以不作要求。

    Args:
        release: 从 `include/config/kernel.release` 读到的 release。
        makefile_text: 头文件树根 `Makefile` 的全文。

    Raises:
        SteamosError: 缺版本字段、数值段不符，或 `EXTRAVERSION` 不是 release 的
            紧随前缀——都说明这棵树与它声称的内核不是同一份。
    """
    values = dict(_MAKEFILE_VERSION_RE.findall(makefile_text))
    missing = [key for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL") if not values.get(key)]
    if missing:
        raise SteamosError(f"头文件树的 Makefile 缺少版本字段 {missing}")

    base = f"{values['VERSION']}.{values['PATCHLEVEL']}.{values['SUBLEVEL']}"
    if not release.startswith(base):
        raise SteamosError(
            f"头文件树的 Makefile 报告 {base}，kernel.release 是 {release!r}（P-14）"
        )

    extra = values.get("EXTRAVERSION", "").strip()
    if extra:
        # Makefile 里 EXTRAVERSION 通常自带前导 '-'；没有时补上再比，否则
        # `-valve2` 会被拿去和 `valve2-1-...` 比而误判。
        wanted = extra if extra.startswith("-") else f"-{extra}"
        rest = release[len(base):]
        if not rest.startswith(wanted):
            raise SteamosError(
                f"头文件树的 EXTRAVERSION 是 {extra!r}，"
                f"kernel.release 的对应段是 {rest!r}（P-14）"
            )


def read_assets(directory: Path) -> dict[str, set[str]]:
    """读每频道的资产清单：目录里的 `<频道>.txt`，每行一个资产名。

    每个频道一个固定 Release，资产清单因此也按频道分开。目录不存在表示还没有任何
    Release（第一次运行），清单文件缺失表示该频道的 Release 还没建——两者都按
    "这个频道没有资产"处理。

    Args:
        directory: 放 `<频道>.txt` 的目录。

    Returns:
        频道 → 资产名集合；没有清单文件的频道不出现在结果里。
    """
    if not directory.is_dir():
        return {}
    assets: dict[str, set[str]] = {}
    for path in sorted(directory.glob("*.txt")):
        assets[path.stem] = {
            line.strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        }
    return assets


def fetch_header(
    channel: str,
    name: str,
    version: str,
    filename: str,
    mirror: str,
    into: Path,
    extract: Path | None,
) -> Path:
    """取回矩阵行对应的头文件包，并可选地解出内核树。

    先重读该频道的 `.db` 复核 `version`/`filename`：矩阵是 discover 阶段算出来的，
    镜像在这期间可能已经换过包，照着旧文件名直接下载会 404 或取到另一份内核。
    复核不上就失败，让这一项本轮不发布、下一轮重新发现。

    Args:
        channel: 头文件包所在频道仓库（`jupiter-3.8`）。
        name: 头文件包名。
        version: 矩阵里记的包版本；空串表示不校验。
        filename: 矩阵里记的包文件名；空串表示不校验。
        mirror: 镜像根。
        into: 包文件落盘目录，必须已存在。
        extract: 解压目标前缀；None 表示只下载不解压。

    Returns:
        取回的包文件路径。

    Raises:
        SteamosError: 包里没有这个名字、版本或文件名与矩阵不一致，或下载/解压失败。
    """
    with tempfile.TemporaryDirectory(prefix="kmod-fetch-") as tmp:
        packages = load_repo(mirror, channel, Path(tmp))
    package = next((item for item in packages if item.name == name), None)
    if package is None:
        raise SteamosError(f"{channel} 的 .db 里没有 {name}")
    if version and package.version != version:
        raise SteamosError(
            f"{channel} 的 {name} 现在是 {package.version}，矩阵里是 {version}；"
            f"镜像已换包，等下一轮重新发现"
        )
    if filename and package.filename != filename:
        raise SteamosError(
            f"{channel} 的 {name} 文件名是 {package.filename}，矩阵里是 {filename}"
        )

    archive = into / package.filename
    fetch(
        package_url(mirror, channel, package.filename),
        archive,
        f"{package.name}-{package.version}",
    )
    if extract is not None:
        extract_package(archive, extract)
    return archive


def _cmd_fetch_header(args: argparse.Namespace) -> int:
    """`fetch-header` 子命令：按矩阵行取包，可选解出。"""
    archive = fetch_header(
        args.channel, args.name, args.version, args.filename, args.mirror,
        Path(args.into), Path(args.extract) if args.extract else None,
    )
    print(f"kmod_plan: 取回 {archive}")
    return 0


def _cmd_discover(args: argparse.Namespace) -> int:
    """`discover` 子命令：取 db、读 seen/assets、写矩阵。"""
    seen = load_seen(Path(args.seen))
    assets = read_assets(Path(args.assets_dir))
    packages_by_channel: dict[str, list[Package]] = {}
    with tempfile.TemporaryDirectory(prefix="kmod-discover-") as tmp:
        workdir = Path(tmp)
        for channel in args.channel:
            packages_by_channel[channel] = load_repo(args.mirror, channel, workdir)
    skipped = sum(
        1
        for packages in packages_by_channel.values()
        for package in packages
        if is_header_package(package.name)
        and not is_supported_kernel(package.version)
    )
    if skipped:
        major, minor = MIN_KERNEL_VERSION
        print(
            f"kmod_plan: 跳过 {skipped} 个头文件包（低于支持下限 {major}.{minor}）",
            file=sys.stderr,
        )
    tasks = plan(
        packages_by_channel, seen, assets, args.rebuild_all, args.patch_fingerprint,
    )
    document = matrix(tasks)
    Path(args.output).write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    rebuilds = sum(1 for task in tasks if task.reason == "rebuild")
    print(
        f"kmod_plan: {len(tasks)} 个任务（{len(tasks) - rebuilds} 新建 / "
        f"{rebuilds} 重建），矩阵写入 {args.output}",
        file=sys.stderr,
    )
    return 0


def _cmd_channels(args: argparse.Namespace) -> int:
    """`channels` 子命令：逐行打印轮询的频道仓库名。

    频道名同时是它那个固定 Release 的 tag，工作流靠这份清单给每个频道取资产列表，
    不必在 YAML 里再抄一遍频道的名字。
    """
    for channel in DISCOVERY_CHANNELS:
        print(channel)
    return 0


def _cmd_record(args: argparse.Namespace) -> int:
    """`record` 子命令：把本轮成功的产物并进 seen.json，每份记下补丁指纹。"""
    seen = load_seen(Path(args.seen))
    results = load_metas(Path(args.metas))
    updated = apply_results(seen, results, args.patch_fingerprint)
    output = Path(args.output)
    output.write_text(
        json.dumps(updated, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"kmod_plan: 记录 {len(results)} 份产物到 {output}", file=sys.stderr)
    return 0


def _cmd_noteline(args: argparse.Namespace) -> int:
    """`noteline` 子命令：把一份 meta.json 变成 Release 说明里的一行。"""
    try:
        document = json.loads(Path(args.meta).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SteamosError(f"{args.meta}: 读不了（{error}）") from error
    if not isinstance(document, dict):
        raise SteamosError(f"{args.meta}: 不是 JSON 对象")
    print(note_line(document))
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    """`verify` 子命令：判定模块能否发布。"""
    check_module(
        Path(args.module),
        args.release,
        Path(args.modinfo).read_text(encoding="utf-8"),
    )
    print(f"kmod_plan: {args.module} 可用于 {args.release}")
    return 0


def _cmd_tree_release(args: argparse.Namespace) -> int:
    """`tree-release` 子命令：输出头文件树自证的 release。"""
    release = kernel_release_from_file(
        Path(args.kernel_release).read_text(encoding="utf-8")
    )
    check_kernel_tree(
        release, Path(args.makefile).read_text(encoding="utf-8")
    )
    print(release)
    return 0


def build_parser() -> argparse.ArgumentParser:
    """三个子命令的参数；每个子命令只收自己用得到的选项。"""
    parser = argparse.ArgumentParser(description="SteamOS 内核模块的发现与产物判定")
    subparsers = parser.add_subparsers(dest="command", required=True)

    discover = subparsers.add_parser("discover", help="推导本轮构建矩阵")
    discover.add_argument("--seen", required=True, help="seen.json 路径（可不存在）")
    discover.add_argument(
        "--assets-dir", required=True,
        help="放 `<频道>.txt` 资产清单的目录（可不存在，每个频道的 Release 各有清单）",
    )
    discover.add_argument("--output", required=True, help="矩阵 JSON 的输出路径")
    discover.add_argument(
        "--channel", action="append", default=None, metavar="REPO",
        help="要轮询的频道仓库，可重复（默认全部 DISCOVERY_CHANNELS）",
    )
    discover.add_argument("--mirror", default=DEFAULT_MIRROR, help="镜像根")
    discover.add_argument(
        "--rebuild-all", action="store_true",
        help="重建 seen.json 里的全部已发布内核（手动全量重建）",
    )
    discover.add_argument(
        "--patch-fingerprint", default="",
        help="kmod/ 的内容指纹；与某份产物记录的不同即重建那一个内核（P-25）",
    )
    discover.set_defaults(handler=_cmd_discover, normalize_channels=True)

    verify = subparsers.add_parser("verify", help="判定一份 .ko 能否发布")
    verify.add_argument("--module", required=True, help="构建出的 .ko")
    verify.add_argument("--release", required=True, help="期望的内核 release")
    verify.add_argument("--modinfo", required=True, help="modinfo 输出的文件")
    verify.set_defaults(handler=_cmd_verify, normalize_channels=False)

    tree = subparsers.add_parser("tree-release", help="核对头文件树并输出 release")
    tree.add_argument("--makefile", required=True, help="头文件树根部的 Makefile")
    tree.add_argument(
        "--kernel-release", required=True,
        help="头文件树的 include/config/kernel.release",
    )
    tree.set_defaults(handler=_cmd_tree_release, normalize_channels=False)

    fetch_header_parser = subparsers.add_parser(
        "fetch-header", help="按矩阵行取回头文件包（可选解出内核树）",
    )
    fetch_header_parser.add_argument("--channel", required=True, help="频道仓库名")
    fetch_header_parser.add_argument("--name", required=True, help="头文件包名")
    fetch_header_parser.add_argument(
        "--version", default="", help="矩阵里记的包版本（空串不校验）",
    )
    fetch_header_parser.add_argument(
        "--filename", default="", help="矩阵里记的包文件名（空串不校验）",
    )
    fetch_header_parser.add_argument(
        "--into", required=True, help="包文件落盘目录",
    )
    fetch_header_parser.add_argument(
        "--extract", default=None,
        help="解压目标前缀（只取 usr/ 并剥层，得到 lib/modules/<release>/build）",
    )
    fetch_header_parser.add_argument(
        "--mirror", default=DEFAULT_MIRROR, help="镜像根",
    )
    fetch_header_parser.set_defaults(
        handler=_cmd_fetch_header, normalize_channels=False,
    )

    channels = subparsers.add_parser(
        "channels", help="逐行打印轮询的频道仓库名（频道名即其 Release 的 tag）",
    )
    channels.set_defaults(handler=_cmd_channels, normalize_channels=False)

    record = subparsers.add_parser("record", help="把成功的产物并进 seen.json")
    record.add_argument("--seen", required=True, help="上一轮的 seen.json（可不存在）")
    record.add_argument("--metas", required=True, help="产物根目录（递归找 meta.json）")
    record.add_argument(
        "--patch-fingerprint", required=True,
        help="本轮构建用的补丁指纹，记进每一份产物",
    )
    record.add_argument("--output", required=True, help="写出的 seen.json 路径")
    record.set_defaults(handler=_cmd_record, normalize_channels=False)

    noteline = subparsers.add_parser("noteline", help="生成 Release 说明里的一行")
    noteline.add_argument("--meta", required=True, help="一份 meta.json")
    noteline.set_defaults(handler=_cmd_noteline, normalize_channels=False)
    return parser


def main(argv: list[str] | None = None) -> int:
    """子命令分派；判断失败统一退出码 1，脚本自身用错给 2（argparse）。"""
    args = build_parser().parse_args(argv)
    if getattr(args, "normalize_channels", False) and not args.channel:
        args.channel = list(DISCOVERY_CHANNELS)
    try:
        return args.handler(args)
    except SteamosError as error:
        print(f"kmod_plan: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
