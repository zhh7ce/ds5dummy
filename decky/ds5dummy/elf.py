#!/usr/bin/env python3
"""ELF64 的 `.modinfo` 解析：设备侧判定「这份 .ko 属于哪个内核」的全部依据（M-03、M-04）。

内核模块的 `.modinfo` 节是一串 `key=value\\0` 文本，`vermagic` 的第一个词就是内核
release。读出它不需要 kmod，也不需要 root：按 ELF64 头找到节头表，再从节名字符串表
里定位 `.modinfo` 即可。这里只认 ELF64 小端——x86_64 的内核模块都是这个形状，别的
形状直接判为「不是可用的模块」，不做兼容。

与发布侧 `scripts/kmod_plan.py` 的判定同源但不同实现：那边调 `modinfo` 读文本，这边
不能依赖外部工具（Deck 上没有 kmod 是常态），所以自己解字节。两边认的键一致：
`vermagic` 的 release 段逐字相等（P-13）。
"""

from __future__ import annotations

import struct
from pathlib import Path

# 与发布侧 `scripts/kmod_plan.py` 的 `MAX_MODULE_BYTES` 同一个数（P-23）：两端对
# 「多大的 .ko 还算正常」必须一致，否则发布出去的东西设备端会拒收。实测产物约 0.9 MiB。
MAX_MODULE_BYTES = 8 * 1024 * 1024

_ELF_MAGIC = b"\x7fELF"
_ELFCLASS64 = 2
_ELFDATA2LSB = 1
# ELF64 头里 e_shoff 的偏移；其后紧跟着 e_shentsize/e_shnum/e_shstrndx。
_E_SHOFF_OFFSET = 0x28
_E_SHENT_TAIL_OFFSET = 0x3A
_EHDR_SIZE = 64
_SECTION_HEADER_SIZE = 64

# 节头表用 0xffff 表示「真实索引在 0 号节的 sh_link 里」。内核模块不会那么多样节，
# 遇到就按不可解析处理，不去追这个间接层。
_SHN_XINDEX = 0xFFFF

MODINFO_SECTION = ".modinfo"
VERMAGIC_KEY = "vermagic"


class ElfError(ValueError):
    """文件不是可解析的 ELF64，或其中找不到可读的 `.modinfo`。"""


def module_release(vermagic: str) -> str:
    """从 `vermagic` 取内核 release（P-13 的唯一查找键）。

    Args:
        vermagic: `.modinfo` 里的原始值，形如
            `6.18.50-valve2-1-neptune-618-gc7289a96b14d SMP preempt mod_unload`。

    Returns:
        第一个空白分隔的词；输入全空白时返回空串，交给调用方判失败。
    """
    parts = vermagic.split()
    return parts[0] if parts else ""


def read_modinfo(blob: bytes) -> dict[str, str]:
    """解析一份 ELF64 模块镜像里的 `.modinfo`。

    Args:
        blob: 整个文件的字节；调用方负责先按 `MAX_MODULE_BYTES` 限长，这里不重复读盘。

    Returns:
        键到值的映射，值已按 UTF-8 解码（坏字节替换成 U+FFFD 而不是抛错——一个键读
        不准不该让整份文件失去判断价值，`vermagic` 是 ASCII）。同名键后出现的覆盖先
        出现的，与内核按存储顺序取值的语义一致。

    Raises:
        ElfError: 太短、不是 ELF64 小端、节头表越界、没有 `.modinfo`，或节里出现
            不成 `key=value` 的条目（说明文件已损坏）。
    """
    if len(blob) < _EHDR_SIZE:
        raise ElfError(f"文件只有 {len(blob)} 字节，放不下一个 ELF64 头")
    if blob[:4] != _ELF_MAGIC:
        raise ElfError("不是 ELF 文件（魔数不符）")
    if blob[4] != _ELFCLASS64:
        raise ElfError(f"不是 ELF64（EI_CLASS={blob[4]}）")
    if blob[5] != _ELFDATA2LSB:
        raise ElfError(f"不是小端 ELF（EI_DATA={blob[5]}）")

    shoff = struct.unpack_from("<Q", blob, _E_SHOFF_OFFSET)[0]
    shentsize, shnum, shstrndx = struct.unpack_from(
        "<HHH", blob, _E_SHENT_TAIL_OFFSET
    )
    if shentsize != _SECTION_HEADER_SIZE:
        raise ElfError(f"节头尺寸是 {shentsize}，不是 64")
    if shnum == 0 or shstrndx == _SHN_XINDEX or shstrndx >= shnum:
        raise ElfError(f"节头表不可用（shnum={shnum}, shstrndx={shstrndx}）")
    if shoff + shnum * shentsize > len(blob):
        raise ElfError("节头表越过文件末尾")

    sections = [
        _read_section(blob, shoff + index * shentsize) for index in range(shnum)
    ]
    # shstrtab 自己的名字就在 shstrtab 里；它按惯例在 0 号节，但只信 shstrndx。
    names = _section_bytes(blob, sections[shstrndx])

    modinfo: _Section | None = None
    for section in sections:
        if _section_name(names, section.name) == MODINFO_SECTION:
            modinfo = section
            break
    if modinfo is None:
        raise ElfError(f"没有 {MODINFO_SECTION} 节")

    fields: dict[str, str] = {}
    for entry in _section_bytes(blob, modinfo).split(b"\0"):
        if not entry:
            continue  # 每个条目自带结尾 NUL，split 会多出空串
        key, separator, value = entry.partition(b"=")
        if not separator:
            raise ElfError(f"{MODINFO_SECTION} 里有不成 key=value 的条目 {entry!r}")
        fields[key.decode("ascii", "replace")] = value.decode("utf-8", "replace")
    return fields


def read_modinfo_file(
    path: str | Path, max_bytes: int = MAX_MODULE_BYTES
) -> dict[str, str]:
    """读盘并解析 `.modinfo`；超过 `max_bytes` 不读，直接判错。

    Args:
        path: `.ko` 路径。
        max_bytes: 允许读取的最大字节数；默认与发布侧上限一致。

    Returns:
        与 `read_modinfo` 相同。

    Raises:
        ElfError: 文件不存在、为空或超限，或内容不可解析。
    """
    target = Path(path)
    try:
        size = target.stat().st_size
    except OSError as error:
        raise ElfError(f"{target}: 读不到（{error}）") from error
    if size == 0:
        raise ElfError(f"{target}: 是空文件")
    if size > max_bytes:
        raise ElfError(f"{target}: {size} 字节，超过上限 {max_bytes}")
    try:
        return read_modinfo(target.read_bytes())
    except OSError as error:
        raise ElfError(f"{target}: 读不到（{error}）") from error


class _Section:
    """节头表里的一行：只保留名字偏移与内容区间。"""

    __slots__ = ("name", "offset", "size")

    def __init__(self, name: int, offset: int, size: int) -> None:
        """收下三个本模块要用的字段。

        Args:
            name: 节名在 shstrtab 里的字节偏移。
            offset: 节内容在文件里的偏移。
            size: 节内容长度。
        """
        self.name = name
        self.offset = offset
        self.size = size


def _read_section(blob: bytes, header_offset: int) -> _Section:
    """解析一个 64 字节的节头，丢掉本模块用不到的字段。

    字段顺序按 ELF64 的 `Elf64_Shdr`：`sh_name`、`sh_type`、`sh_flags`、`sh_addr`、
    `sh_offset`、`sh_size` 各占 4/4/8/8/8/8 字节。

    Args:
        blob: 文件全文；调用前已确认节头表在界内。
        header_offset: 该节头在文件里的起始偏移。

    Returns:
        只含名字偏移、内容偏移与长度的节描述。
    """
    name, _sh_type, _flags, _addr, offset, size = struct.unpack_from(
        "<IIQQQQ", blob, header_offset
    )
    return _Section(name, offset, size)


def _section_bytes(blob: bytes, section: _Section) -> bytes:
    """取节内容的切片，越界即报错。

    Args:
        blob: 文件全文。
        section: 目标节。

    Returns:
        `blob[offset:offset+size]`。

    Raises:
        ElfError: 区间越过文件末尾（节头表被改坏或文件被截断）。
    """
    end = section.offset + section.size
    if end > len(blob):
        raise ElfError(f"节内容 [{section.offset}, {end}) 越过文件末尾 {len(blob)}")
    return blob[section.offset:end]


def _section_name(names: bytes, offset: int) -> str:
    """从节名字符串表取一个名字。

    Args:
        names: shstrtab 全文。
        offset: 名字的字节偏移。

    Returns:
        到第一个 NUL 为止的 ASCII 名字；越界返回空串（这样一个坏节名只会让匹配失败，
        不会掩盖其他节的可用性）。
    """
    if offset >= len(names):
        return ""
    end = names.find(b"\0", offset)
    return names[offset:end if end >= 0 else len(names)].decode("ascii", "replace")
