#!/usr/bin/env python3
"""外部世界的唯一接缝（A-03）：进程、文件、时钟、网络全部从这里过。

状态机、解析、启停序列都只调用本类的原语，不直接碰 `subprocess`/`open`/`os`；
测试用 `FakeSystem` 换掉整层，于是接管顺序、回滚、退出码分流都能在没有 root、
没有手柄、没有网络的机器上跑。

两类方法要分清：

- **阻塞且可能慢**（`run`、`download`）：由调用方决定放到线程里跑，本类不假装异步。
- **查询/小动作**（`exists`、`read`、`stat_*`）：在事件循环里同步调用，sysfs 与
  `/proc` 的读都是微秒级。

时间有两把钟：`now()` 是单调时钟，只用来算间隔与已等待时长；`wall()` 是墙上时间，
只用来给人看的记录。两者不可混用——单调时钟的绝对值放进日志没有意义。
"""

from __future__ import annotations

import http.client
import os
import shutil
import signal
import ssl
import stat
import subprocess
import time
import urllib.error
import urllib.request
from typing import Callable, NamedTuple, Sequence

DOWNLOAD_TIMEOUT_S = 30.0

# 一次读回来的块大小。1 MiB 的模块在环回/宽带下都是十几块的事，进度也因此够细。
DOWNLOAD_CHUNK_BYTES = 64 * 1024

# `subprocess` 没有「超时」的退出码语义，借 shell 的 124 表达，调用方据此区分
# 「命令跑完返回非零」与「命令卡住」。
TIMEOUT_CODE = 124

# 发行版把整份 CA 包放在这两个路径之一（Arch/SteamOS 用前者，Debian 系用后者）。
# 只在 OpenSSL 自己解析出的 cafile 不存在时才会用到，见 `_download_ssl_context()`。
CA_BUNDLE_CANDIDATES = ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt")


def _download_ssl_context() -> ssl.SSLContext:
    """下载用的 TLS 上下文：显式挑一个存在的 CA 包，不依赖进程内 OpenSSL 的默认目录。

    `ssl.create_default_context()` 只按进程里加载的那份 libssl 的 OPENSSLDIR 找 CA。
    插件跑在 decky-loader 的子进程里，用的是装载器随包的那份 libssl（不是系统的）：
    SteamOS 上实测它的 OPENSSLDIR 是 `/usr/lib/ssl`，那里不存在，于是默认信任链为空，
    访问任何 HTTPS 站点都报 `unable to get local issuer certificate`，而机器的 CA 包
    其实在 `/etc/ssl`。

    先采用 OpenSSL 自己解析出的 cafile（它会读 `SSL_CERT_FILE`），再按
    `CA_BUNDLE_CANDIDATES` 的顺序退到发行版标准路径。

    Returns:
        带验证链的客户端上下文；一个可用 CA 包都找不到时退回默认上下文（保持原有的
        失败方式，不静默关闭证书校验）。
    """
    default = ssl.get_default_verify_paths()
    for cafile in (default.cafile, *CA_BUNDLE_CANDIDATES):
        if cafile and os.path.exists(cafile):
            return ssl.create_default_context(cafile=cafile)
    return ssl.create_default_context()


class Result(NamedTuple):
    """一次命令执行的结果。

    Attributes:
        argv: 原样保存的参数元组，日志里直接打印它就能复现命令。
        code: 退出码；`TIMEOUT_CODE` 表示超时，127 表示可执行文件不存在。
        stdout: 标准输出全文（已解码）。
        stderr: 标准错误全文（已解码）；失败原因通常在这里。
    """

    argv: tuple[str, ...]
    code: int
    stdout: str
    stderr: str


class DownloadHttpError(Exception):
    """远端明确回了 HTTP 错误；`status == 404` 表示「没有这个内核的产物」（M-05）。"""

    def __init__(self, status: int) -> None:
        """只收状态码。

        Args:
            status: HTTP 状态码，原样保留，调用方按它分流。
        """
        super().__init__(f"HTTP {status}")
        self.status = status


class DownloadNetworkError(Exception):
    """连不上/超时/读断了：与「远端说没有」是两件事（M-05）。"""


class System:
    """真实世界的原语集合；子类或测试替身实现全部方法。"""

    def run(
        self, argv: Sequence[str], timeout: float = 30.0
    ) -> Result:
        """同步执行一条命令并收集输出。

        Args:
            argv: 参数字符串序列，不经 shell，因此不存在引号与注入问题。
            timeout: 超时秒数；到期强杀并按 `TIMEOUT_CODE` 返回。

        Returns:
            `Result`；从不抛异常——找不到可执行文件也好、超时也好，都由退出码表达，
            调用方的分支因此只有一处。
        """
        raise NotImplementedError

    def read(self, path: str) -> str:
        """读一个文本文件（sysfs、`/proc/modules`、日志）。

        Args:
            path: 绝对路径。

        Returns:
            全文；坏字节按替换字符解码，不因个别字节让调用方失败。

        Raises:
            OSError: 文件不存在或读不了。
        """
        raise NotImplementedError

    def read_bytes(self, path: str) -> bytes:
        """读任意文件为字节，供 ELF 解析使用。

        Args:
            path: 绝对路径。

        Returns:
            文件全文。

        Raises:
            OSError: 文件不存在或读不了。
        """
        raise NotImplementedError

    def write(self, path: str, text: str) -> None:
        """整份覆盖写一个文本文件（sysfs 属性、黑名单、pid 文件）。

        Args:
            path: 绝对路径；父目录必须已存在。

        Raises:
            OSError: 写不了（权限、只读文件系统、父目录缺失）。
        """
        raise NotImplementedError

    def exists(self, path: str) -> bool:
        """路径是否存在（跟随符号链接，与 `test -e` 同义）。"""
        raise NotImplementedError

    def listdir(self, path: str) -> list[str]:
        """列目录，不排序（调用方按需要的顺序自己排）。

        Raises:
            OSError: 目录不存在或读不了。
        """
        raise NotImplementedError

    def readlink(self, path: str) -> str:
        """读符号链接目标，不解析成绝对路径。

        Raises:
            OSError: 不是符号链接或不存在。
        """
        raise NotImplementedError

    def replace(self, source: str, destination: str) -> None:
        """原子改名（M-06：临时文件校验通过后才进缓存）。

        Raises:
            OSError: 跨设备、目标为目录或权限不足。
        """
        raise NotImplementedError

    def remove(self, path: str) -> None:
        """删文件或空目录；不存在算成功（幂等）。"""
        raise NotImplementedError

    def remove_tree(self, path: str) -> None:
        """递归删目录；不存在算成功。"""
        raise NotImplementedError

    def mkdirs(self, path: str) -> None:
        """建目录（含父级）；已存在算成功。"""
        raise NotImplementedError

    def make_executable(self, path: str) -> None:
        """给文件补上三个执行位，其余权限位不变。

        Decky 安装插件 ZIP 时用 `zipfile.extractall`，而 CPython 的解包不恢复
        `external_attr` 里的 mode——`bin/ds5dummyd` 会落在 0644，`Popen` 直接报
        `EACCES`。启动前补一次，插件从任何安装路径（Decky、`unzip`、手工拷贝）
        进来都跑得起来。

        Args:
            path: 目标文件；必须已存在。

        Raises:
            OSError: 文件不存在或改不了权限。
        """
        raise NotImplementedError

    def stat_size(self, path: str) -> int:
        """文件字节数。

        Raises:
            OSError: 不存在或读不了。
        """
        raise NotImplementedError

    def stat_mtime(self, path: str) -> float:
        """文件的修改时间，墙上时钟秒（给人看的「模块取回时间」）。

        Raises:
            OSError: 不存在或读不了。
        """
        raise NotImplementedError

    def uname_release(self) -> str:
        """当前运行内核的 release（`uname -r`），也是模块查找键。"""
        raise NotImplementedError

    def spawn(self, argv: Sequence[str], cwd: str, log_path: str) -> int:
        """起一个长命子进程，stdout/stderr 覆盖写入 `log_path`，自成会话。

        Args:
            argv: 参数序列。
            cwd: 工作目录（插件目录，C-04）。
            log_path: 日志文件；已存在则截断——每次启动都从头记，避免等待重试时
                无限追加（E-05 的面板要读它的最后一行）。

        Returns:
            子进程 pid。

        Raises:
            OSError: 可执行文件不存在或日志写不了。
        """
        raise NotImplementedError

    def wait(self, pid: int) -> int | None:
        """非阻塞回收子进程。

        Args:
            pid: `spawn` 返回的 pid。

        Returns:
            已退出时返回退出码，被信号杀掉时返回负的信号号；仍在跑、或根本不是本
            进程的子进程（插件后端重启过，子进程已被过继）时返回 None。后一种情况
            要靠 `alive` 判断生死，退出码则无从得知。
        """
        raise NotImplementedError

    def alive(self, pid: int) -> bool:
        """进程是否还在（`kill(pid, 0)`；没有权限也按「在」算）。"""
        raise NotImplementedError

    def kill(self, pid: int, sig: int = signal.SIGTERM) -> bool:
        """发信号。

        Args:
            pid: 目标进程。
            sig: 信号编号，缺省 SIGTERM。

        Returns:
            发出去了为 True，进程已不存在为 False（不算错误，D-05 幂等）。
        """
        raise NotImplementedError

    def pids_with_exe(self, path: str) -> list[int]:
        """扫 `/proc`，找出可执行文件指向 `path` 的进程。

        用来清"历次启停留下的自己"：按 exe 路径精确匹配，不会误伤别人装的同一份
        程序（路径不同），也不需要 root 之外的信息。

        Args:
            path: 期望的可执行文件路径（按 realpath 比较，符号链接也算同一条）。

        Returns:
            匹配的 pid 列表；读不到的进程跳过。
        """
        raise NotImplementedError

    def download(
        self,
        url: str,
        destination: str,
        on_progress: Callable[[int, int], None],
    ) -> None:
        """流式下载到 `destination`。

        Args:
            url: 完整地址。
            destination: 目标文件；调用方传的是 `.part` 临时路径（M-06）。
            on_progress: 每写一块调用一次，参数为 `(已写字节, 总字节)`；远端没给
                `Content-Length` 时总字节为 0。

        Raises:
            DownloadHttpError: 远端回了 4xx/5xx。
            DownloadNetworkError: 连接、超时、读断等其它失败。
        """
        raise NotImplementedError

    def now(self) -> float:
        """单调时钟秒；只用于测量间隔。"""
        raise NotImplementedError

    def wall(self) -> float:
        """墙上时钟秒；只用于写给人看的记录。"""
        raise NotImplementedError


class RealSystem(System):
    """`System` 在真机上的实现。"""

    def run(self, argv: Sequence[str], timeout: float = 30.0) -> Result:
        """执行命令；异常全部转成退出码（见基类契约）。"""
        args = tuple(str(item) for item in argv)
        try:
            completed = subprocess.run(
                args, capture_output=True, text=True, timeout=timeout, check=False
            )
        except FileNotFoundError as error:
            return Result(args, 127, "", str(error))
        except subprocess.TimeoutExpired:
            return Result(args, TIMEOUT_CODE, "", f"超时（{timeout:g}s）")
        return Result(args, completed.returncode, completed.stdout, completed.stderr)

    def read(self, path: str) -> str:
        """按 UTF-8 读文本，坏字节替换。"""
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def read_bytes(self, path: str) -> bytes:
        """读原始字节。"""
        with open(path, "rb") as handle:
            return handle.read()

    def write(self, path: str, text: str) -> None:
        """整份覆盖写。"""
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)

    def exists(self, path: str) -> bool:
        """`os.path.exists`。"""
        return os.path.exists(path)

    def listdir(self, path: str) -> list[str]:
        """`os.listdir`。"""
        return os.listdir(path)

    def readlink(self, path: str) -> str:
        """`os.readlink`。"""
        return os.readlink(path)

    def replace(self, source: str, destination: str) -> None:
        """`os.replace`。"""
        os.replace(source, destination)

    def remove(self, path: str) -> None:
        """删文件或空目录，忽略不存在。"""
        try:
            if os.path.isdir(path):
                os.rmdir(path)
            else:
                os.remove(path)
        except FileNotFoundError:
            pass

    def remove_tree(self, path: str) -> None:
        """递归删目录，忽略不存在。"""
        if os.path.exists(path):
            shutil.rmtree(path)

    def mkdirs(self, path: str) -> None:
        """`os.makedirs(exist_ok=True)`。"""
        os.makedirs(path, exist_ok=True)

    def make_executable(self, path: str) -> None:
        """在原权限位上按位或 `0o111`。"""
        os.chmod(path, stat.S_IMODE(os.stat(path).st_mode) | 0o111)

    def stat_size(self, path: str) -> int:
        """`os.stat().st_size`。"""
        return os.stat(path).st_size

    def stat_mtime(self, path: str) -> float:
        """`os.stat().st_mtime`。"""
        return os.stat(path).st_mtime

    def uname_release(self) -> str:
        """`os.uname().release`。"""
        return os.uname().release

    def spawn(self, argv: Sequence[str], cwd: str, log_path: str) -> int:
        """起程序；日志先截断再交给子进程。"""
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "w", encoding="utf-8") as sink:
            process = subprocess.Popen(
                [str(item) for item in argv],
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=sink,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                close_fds=True,
            )
        return process.pid

    def wait(self, pid: int) -> int | None:
        """`waitpid(WNOHANG)`；不是我们的子进程时返回 None。"""
        try:
            reaped, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            return None
        if reaped == 0:
            return None
        if os.WIFEXITED(status):
            return os.WEXITSTATUS(status)
        if os.WIFSIGNALED(status):
            return -os.WTERMSIG(status)
        return None

    def alive(self, pid: int) -> bool:
        """`kill(pid, 0)` 探活。"""
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def kill(self, pid: int, sig: int = signal.SIGTERM) -> bool:
        """发信号；进程不在返回 False。"""
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            return False
        except PermissionError:
            return False
        return True

    def pids_with_exe(self, path: str) -> list[int]:
        """扫 `/proc`，按 exe 的真实路径匹配。"""
        target = os.path.realpath(path)
        found: list[int] = []
        try:
            entries = os.listdir("/proc")
        except OSError:
            return found
        for entry in entries:
            if not entry.isdigit():
                continue
            try:
                exe = os.readlink(f"/proc/{entry}/exe")
            except OSError:
                # 进程刚好退出，或不是本用户的进程（没有 CAP_SYS_PTRACE）。
                continue
            if os.path.realpath(exe) == target:
                found.append(int(entry))
        return found

    def download(
        self,
        url: str,
        destination: str,
        on_progress: Callable[[int, int], None],
    ) -> None:
        """流式下载；4xx/5xx 与网络故障分开抛。

        证书校验走 `_download_ssl_context()` 选的信任链，不用进程内 OpenSSL 的默认
        目录——原因见那个函数。
        """
        request = urllib.request.Request(
            url, headers={"User-Agent": "ds5dummy-decky"}
        )
        # 整段下载只有两类失败要分开：远端明确回了状态码，和连接层的故障。调用方按
        # 前者里的 404 换下一个频道，对其余状态与后者直接上报（M-05）。
        try:
            with urllib.request.urlopen(
                request, timeout=DOWNLOAD_TIMEOUT_S, context=_download_ssl_context()
            ) as response:
                # 缺 Content-Length 时按 0 记，进度回报随之为「总量未知」。
                total = int(response.headers.get("Content-Length") or 0)
                written = 0
                with open(destination, "wb") as sink:
                    while True:
                        chunk = response.read(DOWNLOAD_CHUNK_BYTES)
                        if not chunk:
                            break
                        sink.write(chunk)
                        written += len(chunk)
                        on_progress(written, total)
        except urllib.error.HTTPError as error:
            raise DownloadHttpError(error.code) from error
        except (urllib.error.URLError, http.client.HTTPException, OSError) as error:
            raise DownloadNetworkError(str(error)) from error

    def now(self) -> float:
        """`time.monotonic()`。"""
        return time.monotonic()

    def wall(self) -> float:
        """`time.time()`。"""
        return time.time()
