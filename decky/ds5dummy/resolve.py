#!/usr/bin/env python3
"""模块解析与校验：本机缓存 → GitHub → 无（M-01、M-05、M-06、M-09）。

顺序是固定的，前一步成功就不再走下一步：

1. `<cache_root>/<release>/<release>.ko` 存在且校验通过 → 直接用（缓存命中不联网，
   M-02/M-09）。缓存条目校验不过就整条删掉，继续第 2 步——坏文件留在缓存里只会
   让每次启动都先失败一遍。
2. `https://github.com/<owner>/<repo>/releases/download/<频道>/<release>.ko`，按
   `CHANNEL_TAGS` 的顺序逐个频道试，404 换下一个；先落
   `.tmp/<release>.ko.part`，校验通过才原子改名进缓存（M-06）。
3. 失败分三种，分别对应远端没发布（所有频道都是 404）、网络故障、取回的文件不对
   （M-05）。

发布侧与本模块的契约只有两条：每个频道的固定 Release 里资产名是 `<release>.ko`；
文件里的 `vermagic` release 段与 `uname -r` 逐字相等。没有索引、没有清单文件
（P-29）：有与没有由 HTTP 状态回答，能不能用由模块自己回答。
"""

from __future__ import annotations

import json
import os
from typing import Callable, NamedTuple

from . import elf
from .system import (
    DownloadHttpError,
    DownloadNetworkError,
    System,
)

# 产物所在的固定 Release：每个 SteamOS 频道一个，tag 就是频道名（P-31）。设备不知道
# 自己在哪个频道，只能按顺序试。先试 main 与 staging——新内核最早出现在这两个仓库；
# 老频道排在后面兜底，因为被弃用的内核只留在自己的 `jupiter-3.x` 仓库里。顺序不影响
# 正确性，只决定命中最快的那条路径。owner/repo 写死在这里：插件拿不到自己的仓库地址，
# 换仓库时改这两行。
OWNER = "zhh7ce"
REPO = "ds5dummy"
CHANNEL_TAGS = (
    "jupiter-main",
    "jupiter-staging",
    "jupiter-3.10",
    "jupiter-3.9",
    "jupiter-3.8.1x",
    "jupiter-3.8",
)

SOURCE_CACHE = "cache"
SOURCE_REMOTE = "remote"

REASON_NO_ARTIFACT = "no_artifact"
REASON_NETWORK = "network"
REASON_BAD_ARTIFACT = "bad_artifact"


class Aborted(Exception):
    """用户关掉了开关：解析在下一个检查点停下，不留下临时文件。"""


class Unavailable(Exception):
    """取不到可用模块；`reason` 是给面板分流的三种原因码之一（M-05）。"""

    def __init__(self, reason: str, detail: str = "") -> None:
        """记录原因码与附加说明。

        Args:
            reason: `REASON_NO_ARTIFACT`/`REASON_NETWORK`/`REASON_BAD_ARTIFACT`。
            detail: 具体到能帮助排查的一句话（HTTP 状态、vermagic 实际值等）。
        """
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason
        self.detail = detail


class Resolved(NamedTuple):
    """一次成功的解析结果。

    Attributes:
        path: `.ko` 的绝对路径。
        source: `SOURCE_CACHE` 或 `SOURCE_REMOTE`，面板据此显示「缓存命中 / 已取回」。
    """

    path: str
    source: str


def module_url(release: str, tag: str) -> str:
    """拼出某个频道 Release 里这个内核的下载地址（P-31）。

    Args:
        release: 内核 release（`uname -r`）。
        tag: 频道 Release 的 tag，取自 `CHANNEL_TAGS`。

    Returns:
        `releases/download/<tag>/<release>.ko` 的完整 HTTPS 地址。
    """
    return f"https://github.com/{OWNER}/{REPO}/releases/download/{tag}/{release}.ko"


def cache_path(cache_root: str, release: str) -> str:
    """缓存里某个内核的模块路径（M-02）。

    Args:
        cache_root: 缓存根（`<插件数据目录>/kmod`）。
        release: 内核 release。

    Returns:
        `<cache_root>/<release>/<release>.ko`。
    """
    return os.path.join(cache_root, release, release + ".ko")


def check_module(system: System, path: str, release: str) -> None:
    """校验一份文件能否加载到 `release` 上（M-03）。

    四条判据按代价排序：存在、非空、不超限，然后才整份读进来解析 ELF。任何一条不过
    都抛 `Unavailable(REASON_BAD_ARTIFACT)`——对用户而言就是「这份文件不能用来接管」。

    Args:
        system: 执行接缝。
        path: 待校验的文件。
        release: 期望的内核 release，与模块 `vermagic` 的第一个词逐字比较。

    Raises:
        Unavailable: 原因码 `bad_artifact`，`detail` 说明是哪条判据不过。
    """
    if not system.exists(path):
        raise Unavailable(REASON_BAD_ARTIFACT, f"{path} 不存在")
    try:
        size = system.stat_size(path)
    except OSError as error:
        raise Unavailable(REASON_BAD_ARTIFACT, f"{path} 读不到（{error}）") from error
    if size == 0:
        raise Unavailable(REASON_BAD_ARTIFACT, f"{path} 是空文件")
    if size > elf.MAX_MODULE_BYTES:
        raise Unavailable(
            REASON_BAD_ARTIFACT, f"{path} 有 {size} 字节，超过上限 {elf.MAX_MODULE_BYTES}"
        )

    try:
        fields = elf.read_modinfo(system.read_bytes(path))
    except (elf.ElfError, OSError) as error:
        raise Unavailable(REASON_BAD_ARTIFACT, str(error)) from error
    vermagic = fields.get(elf.VERMAGIC_KEY, "")
    if not vermagic:
        raise Unavailable(REASON_BAD_ARTIFACT, f"{path} 的 .modinfo 里没有 vermagic")
    actual = elf.module_release(vermagic)
    if actual != release:
        raise Unavailable(
            REASON_BAD_ARTIFACT,
            f"模块的 vermagic 属于 {actual!r}，当前内核是 {release!r}",
        )


def resolve(
    system: System,
    cache_root: str,
    release: str,
    *,
    force_remote: bool = False,
    cancel: Callable[[], None] | None = None,
    progress: Callable[[int, int], None] | None = None,
) -> Resolved:
    """按 M-01 的顺序取模块。

    Args:
        system: 执行接缝。
        cache_root: 缓存根。
        release: 当前内核 release。
        force_remote: True 时跳过缓存直查远端，并覆盖缓存条目（U-03、M-09）。
        cancel: 每块下载与每次重试前调用；需要中止时由它抛 `Aborted`。为 None 表示
            不可取消。它可能在下载线程里被调用。
        progress: 下载进度回调，参数为 `(已写字节, 总字节)`，总字节 0 表示未知。

    Returns:
        模块路径与来源。

    Raises:
        Unavailable: 三种取不到的原因之一。
        Aborted: `cancel` 抛出时原样透传。
        OSError: 缓存目录建不出或写不进（磁盘问题，不是"没有模块"）。
    """
    target = cache_path(cache_root, release)

    # 1) 缓存优先。校验不过就整条删掉：半好的缓存条目每次启动都会先失败一遍。
    if not force_remote and system.exists(target):
        try:
            check_module(system, target, release)
        except Unavailable:
            system.remove_tree(os.path.dirname(target))
        else:
            return Resolved(target, SOURCE_CACHE)

    # 2) 远端：按频道顺序试。只有 404 换下一个频道；其它失败说明网络或服务不对，
    # 换频道也不会好，直接上报。临时文件与缓存同层（同一文件系统），改名才是原子的。
    system.mkdirs(os.path.join(cache_root, ".tmp"))
    part = os.path.join(cache_root, ".tmp", release + ".ko.part")
    url = ""
    for tag in CHANNEL_TAGS:
        url = module_url(release, tag)
        try:
            if cancel is not None:
                cancel()
            system.download(url, part, progress or (lambda done, total: None))
        except DownloadHttpError as error:
            system.remove(part)
            if error.status == 404:
                continue
            raise Unavailable(REASON_NETWORK, f"{tag}：下载返回 {error}") from error
        except DownloadNetworkError as error:
            system.remove(part)
            raise Unavailable(REASON_NETWORK, str(error)) from error
        break
    else:
        raise Unavailable(
            REASON_NO_ARTIFACT,
            f"{release} 在所有频道里都还没有产物（{len(CHANNEL_TAGS)} 个 Release "
            f"都返回 404）",
        )

    # 3) 下载完的文件同样要过校验；不过就删掉，不留在缓存里（M-06）。
    try:
        if cancel is not None:
            cancel()
        check_module(system, part, release)
    except (Aborted, Unavailable, OSError):
        system.remove(part)
        raise

    # 4) 进缓存并留一份取回记录，便于面板显示来源与时间。
    destination_dir = os.path.dirname(target)
    system.mkdirs(destination_dir)
    system.replace(part, target)
    meta = {
        "release": release,
        "url": url,
        "size": system.stat_size(target),
        "fetched_at": system.wall(),
    }
    system.write(
        os.path.join(destination_dir, "meta.json"),
        json.dumps(meta, ensure_ascii=False) + "\n",
    )
    return Resolved(target, SOURCE_REMOTE)


def clean_cache(system: System, cache_root: str, keep_release: str) -> list[str]:
    """删掉当前内核以外的缓存条目（M-07、U-06）。

    Args:
        system: 执行接缝。
        cache_root: 缓存根；不存在时什么都不做。
        keep_release: 要保留的内核 release。

    Returns:
        真正删掉的 release 列表（给日志用）。
    """
    if not system.exists(cache_root):
        return []
    removed: list[str] = []
    for name in sorted(system.listdir(cache_root)):
        if name == ".tmp" or name == keep_release:
            continue
        system.remove_tree(os.path.join(cache_root, name))
        removed.append(name)
    return removed
