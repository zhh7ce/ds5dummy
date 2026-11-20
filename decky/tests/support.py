#!/usr/bin/env python3
"""测试替身：`FakeSystem` 把整层外部动作换成一个内存模型（A-03）。

模型只有三样东西：一份「路径 → 内容」的字典（`/proc`、`/sys`、`/etc` 与缓存文件
全在里面）、一组已加载模块（`/proc/modules` 由它合成）、一张进程表。命令不真执行，
而是按 argv 更新这些状态——`modprobe dummy_hcd` 会造出 `/sys/module/dummy_hcd`，
`insmod` 会把定制模块记进模块表，如此往返，状态机看到的因果与真机一致。

测试按需注入失败：`fail` 里放某条命令的首个词，`run()` 对它返回非零；`spawn_exit`
让新起的转发程序立刻"退出"为某个码；`stubborn` 里的 pid 不响应信号（D-07）。
"""

from __future__ import annotations

import asyncio
import os
import signal
import struct
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ds5dummy import Controller, Layout  # noqa: E402
from ds5dummy import resolve, supervise, takeover  # noqa: E402
from ds5dummy.elf import VERMAGIC_KEY  # noqa: E402
from ds5dummy.log import EventLog  # noqa: E402
from ds5dummy.system import (  # noqa: E402
    DownloadHttpError,
    DownloadNetworkError,
    Result,
    System,
)

# 一个真实的 SteamOS 内核 release（与发布侧夹具同一个），vermagic 后面三个词按真机。
RELEASE = "6.18.50-valve2-1-neptune-618-gc7289a96b14d"
VERMAGIC = RELEASE + " SMP preempt mod_unload"


def make_elf(modinfo: dict[str, str] | None = None) -> bytes:
    """拼一个最小 ELF64（小端），只含 null、`.shstrtab` 与 `.modinfo` 三个节。

    状态机只读 `.modinfo`，所以别的节可以一律不造；这个形状足以让 `read_modinfo`
    走完整的"节头表 → 节名表 → 节内容"路径，而不是只命中某个捷径。

    Args:
        modinfo: `.modinfo` 的键值；缺省是一份能用的定制模块（名字与真 vermagic）。

    Returns:
        完整文件字节。
    """
    fields = modinfo if modinfo is not None else {
        "name": takeover.CUSTOM_MODULE,
        VERMAGIC_KEY: VERMAGIC,
        "license": "GPL",
    }
    payload = b"".join(
        key.encode() + b"=" + value.encode() + b"\0" for key, value in fields.items()
    )
    names = b"\0.shstrtab\0.modinfo\0"
    shstrtab_name = 1  # ".shstrtab" 在 names 里的字节偏移
    modinfo_name = 11  # ".modinfo" 紧随其后（1 + len(".shstrtab") + 1）

    modinfo_offset = 64
    names_offset = modinfo_offset + len(payload)
    shoff = names_offset + len(names)

    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # EI_CLASS = ELFCLASS64
    header[5] = 1  # EI_DATA = ELFDATA2LSB
    header[6] = 1  # EI_VERSION
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HHH", header, 0x3A, 64, 3, 1)  # shentsize, shnum, shstrndx

    def section(name: int, kind: int, offset: int, size: int) -> bytes:
        # 完整 Elf64_Shdr：后四组（link/info/addralign/entsize）状态机不读，但节头
        # 定长 64 字节，少写就等于文件被截断。
        return struct.pack("<IIQQQQIIQQ", name, kind, 0, 0, offset, size, 0, 0, 0, 0)

    null = b"\0" * 64
    shstrtab = section(shstrtab_name, 3, names_offset, len(names))  # SHT_STRTAB
    modinfo = section(modinfo_name, 1, modinfo_offset, len(payload))  # SHT_PROGBITS
    return bytes(header) + payload + names + null + shstrtab + modinfo


class FakeSystem(System):
    """`System` 的内存实现；所有状态都是本对象上的普通字段。"""

    def __init__(self, release: str = RELEASE) -> None:
        """建一个空世界：没有文件、没有模块、没有进程。

        Args:
            release: `uname -r` 的返回值。
        """
        self.release = release
        self.files: dict[str, str] = {}
        self.blobs: dict[str, bytes] = {}
        self.directories: set[str] = {"/", "/sys/class/udc"}
        self.links: dict[str, str] = {}
        self.modules: set[str] = set()
        self.downloads: dict[str, object] = {}
        self.commands: list[tuple[str, ...]] = []
        self.executable_calls: list[str] = []
        self.fail: dict[str, int] = {}
        self.spawn_exit: int | None = None
        self.stubborn: set[int] = set()
        self.systemctl_active = False
        self.clock = 0.0
        self.wall_epoch = 1_700_000_000.0
        self.processes: dict[int, int | None] = {}
        self.spawns: list[tuple[tuple[str, ...], str, str]] = []
        self._next_pid = 4000

    # -- 命令

    def run(self, argv: Sequence[str], timeout: float = 30.0) -> Result:
        """记录 argv；按 `fail` 注入非零退出，否则把已知命令的副作用落到内存里。"""
        args = tuple(str(item) for item in argv)
        self.commands.append(args)
        if args[:2] == ("systemctl", "is-active"):
            if self.systemctl_active:
                return Result(args, 0, "active\n", "")
            return Result(args, 3, "inactive\n", "")
        code = self.fail.get(args[0], 0)
        if code != 0:
            return Result(args, code, "", f"{args[0]} 失败（注入）")
        self._apply(args)
        return Result(args, 0, "", "")

    def _apply(self, args: tuple[str, ...]) -> None:
        """把成功命令的内核侧副作用写进内存模型。"""
        if args[0] == "modprobe" and len(args) >= 2:
            name = args[1]
            self.modules.add(name)
            if name == "dummy_hcd":
                num = "3"
                for extra in args[2:]:
                    if extra.startswith("num="):
                        num = extra.split("=", 1)[1]
                self.directories.update(
                    {takeover.DUMMY_HCD_DIR, "/sys/module/dummy_hcd/parameters"}
                )
                self.files[takeover.DUMMY_HCD_NUM] = num + "\n"
                self.files["/sys/module/dummy_hcd/parameters/is_high_speed"] = "N\n"
            elif name == takeover.IN_TREE_MODULE:
                self.directories.add(takeover.HID_DRIVER)
        elif args[0] == "rmmod" and len(args) >= 2:
            name = args[1]
            self.modules.discard(name)
            if name == "dummy_hcd":
                self.directories.discard(takeover.DUMMY_HCD_DIR)
                self.files.pop(takeover.DUMMY_HCD_NUM, None)
            elif name == takeover.IN_TREE_MODULE:
                self.directories.discard(takeover.HID_DRIVER)
        elif args[0] == "insmod" and len(args) >= 2:
            self.modules.add(takeover.CUSTOM_MODULE)

    # -- 文件

    def read(self, path: str) -> str:
        """读文本；`/proc/modules` 由模块集合合成。"""
        if path == supervise.PROC_MODULES:
            return "".join(
                f"{name} 16384 0 - Live 0x0\n" for name in sorted(self.modules)
            )
        if path in self.files:
            return self.files[path]
        if path in self.blobs:
            return self.blobs[path].decode("utf-8", "replace")
        raise FileNotFoundError(path)

    def read_bytes(self, path: str) -> bytes:
        """读字节。"""
        if path in self.blobs:
            return self.blobs[path]
        if path in self.files:
            return self.files[path].encode()
        raise FileNotFoundError(path)

    def write(self, path: str, text: str) -> None:
        """整份覆盖写，并补齐它出现的父目录。"""
        self.files[path] = text
        self._add_parents(path)

    def exists(self, path: str) -> bool:
        """文件、目录、链接任一存在即为真。"""
        return (
            path in self.files
            or path in self.blobs
            or path in self.directories
            or path in self.links
        )

    def listdir(self, path: str) -> list[str]:
        """列出直接子项；目录为空返回空列表，目录不存在抛 `FileNotFoundError`。"""
        prefix = path.rstrip("/") + "/"
        children: set[str] = set()
        for key in (
            *self.files,
            *self.blobs,
            *self.directories,
            *self.links,
        ):
            if key.startswith(prefix):
                child = key[len(prefix):].split("/", 1)[0]
                if child:
                    children.add(child)
        if not children and path not in self.directories:
            raise FileNotFoundError(path)
        return sorted(children)

    def readlink(self, path: str) -> str:
        """读链接表；没有就是 `OSError`。"""
        if path in self.links:
            return self.links[path]
        raise FileNotFoundError(path)

    def replace(self, source: str, destination: str) -> None:
        """原子改名（模型里就是搬 key）。"""
        if source in self.blobs:
            self.blobs[destination] = self.blobs.pop(source)
        elif source in self.files:
            self.files[destination] = self.files.pop(source)
        else:
            raise FileNotFoundError(source)
        self._add_parents(destination)

    def remove(self, path: str) -> None:
        """删文件/空目录/链接，不存在算成功。"""
        self.files.pop(path, None)
        self.blobs.pop(path, None)
        self.links.pop(path, None)
        self.directories.discard(path)

    def remove_tree(self, path: str) -> None:
        """递归删目录。"""
        prefix = path.rstrip("/") + "/"
        for table in (self.files, self.blobs, self.links):
            for key in [k for k in table if k == path or k.startswith(prefix)]:
                table.pop(key, None)
        self.directories = {
            d for d in self.directories if d != path and not d.startswith(prefix)
        }

    def mkdirs(self, path: str) -> None:
        """逐级登记目录。"""
        current = ""
        for part in path.split("/"):
            if not part:
                current = "/"
                continue
            current = current.rstrip("/") + "/" + part
            self.directories.add(current)

    def make_executable(self, path: str) -> None:
        """只登记调用；内存模型里没有权限位，测试断言"起进程前补过执行位"即可。"""
        self.executable_calls.append(path)

    def stat_size(self, path: str) -> int:
        """字节数。"""
        if path in self.blobs:
            return len(self.blobs[path])
        if path in self.files:
            return len(self.files[path].encode())
        raise FileNotFoundError(path)

    def stat_mtime(self, path: str) -> float:
        """固定一个墙上时刻；只用于展示，测试不关心具体值。"""
        if self.exists(path):
            return self.wall_epoch
        raise FileNotFoundError(path)

    def uname_release(self) -> str:
        """当前内核 release。"""
        return self.release

    # -- 进程

    def spawn(self, argv: Sequence[str], cwd: str, log_path: str) -> int:
        """登记一个新进程；`spawn_exit` 让它在被观察时已经"退出"。"""
        pid = self._next_pid
        self._next_pid += 1
        self.processes[pid] = self.spawn_exit
        self.spawns.append((tuple(str(item) for item in argv), cwd, log_path))
        self.files[log_path] = ""
        self._add_parents(log_path)
        self.directories.add(cwd)
        return pid

    def wait(self, pid: int) -> int | None:
        """非阻塞回收：进程表里该 pid 已记成退出码时就交出来并删掉。"""
        code = self.processes.get(pid)
        if code is None:
            return None
        del self.processes[pid]
        return code

    def alive(self, pid: int) -> bool:
        """进程在表里且还没有退出码。"""
        return pid in self.processes and self.processes[pid] is None

    def kill(self, pid: int, sig: int = signal.SIGTERM) -> bool:
        """发信号：正常进程按信号退出；`stubborn` 里的进程岿然不动。"""
        if pid not in self.processes:
            return False
        if pid in self.stubborn:
            return True
        self.processes[pid] = -int(sig)
        return True

    # -- 网络与时钟

    def download(self, url: str, destination: str, on_progress) -> None:
        """按 `downloads` 表取内容；值是异常就抛，是字节就落盘。"""
        payload = self.downloads.get(url)
        if isinstance(payload, DownloadHttpError):
            raise payload
        if isinstance(payload, DownloadNetworkError):
            raise payload
        if not isinstance(payload, bytes):
            raise DownloadNetworkError(f"没有为 {url} 准备下载内容")
        self.blobs[destination] = payload
        on_progress(len(payload), len(payload))

    def now(self) -> float:
        """虚拟单调时钟。"""
        return self.clock

    def wall(self) -> float:
        """虚拟墙上时钟。"""
        return self.wall_epoch + self.clock

    def _add_parents(self, path: str) -> None:
        """把路径的各级父目录登记进 `directories`。"""
        self.mkdirs(os.path.dirname(path) or "/")


class Harness:
    """一个装好假世界与状态机的测试台。"""

    def __init__(self) -> None:
        """搭默认布局；`/sys/class/udc` 为空（没有在用的虚拟控制器）。"""
        self.fake = FakeSystem()
        self.layout = Layout("/plugin", "/settings", "/data")
        self.log = EventLog()
        self.controller = Controller(
            self.fake,
            self.layout,
            sleep=self.sleep,
            version="0.1.0",
            log=self.log,
        )

    async def sleep(self, seconds: float) -> None:
        """推进虚拟时钟后让出控制权（`_watch_window` 靠 sleep 前进）。"""
        self.fake.clock += seconds
        await asyncio.sleep(0)

    @property
    def cache_file(self) -> str:
        """当前内核缓存条目的路径。"""
        return os.path.join(
            self.layout.cache_root, self.fake.release, self.fake.release + ".ko"
        )

    def install_cached_module(self, blob: bytes | None = None) -> str:
        """把一份可用模块放进缓存，返回它的路径。"""
        path = self.cache_file
        self.fake.blobs[path] = blob if blob is not None else make_elf()
        self.fake.mkdirs(os.path.dirname(path))
        return path

    def push_remote_module(self, blob: bytes | None = None, release: str | None = None) -> str:
        """为远端准备一份可下载模块，返回它对应的 URL（第一个频道）。"""
        target = release or self.fake.release
        url = resolve.module_url(target, resolve.CHANNEL_TAGS[0])
        self.fake.downloads[url] = blob if blob is not None else make_elf(
            {"name": takeover.CUSTOM_MODULE, VERMAGIC_KEY: target + " SMP preempt mod_unload"}
        )
        return url
