#!/usr/bin/env python3
"""内核侧接管与还原（E-01～E-06、D-01～D-09）。

接管把系统从「in-tree `hid-playstation` 管蓝牙手柄」切到「我们编的模块 + 插件起的
转发程序」；停用把系统还原成没装这个插件的样子。每一步都按「失败时怎么退回去」
设计，rollback 栈在 `run()` 里逐条压入：

1. 前置检查：现有 systemd 安装不能在跑（G-08），DKMS 包不能在场（两个接管方不能
   并存）。
2. `dummy_hcd` 就绪：只跑 full-speed、`num` 等于插件设置（E-02）。`num` 是加载参数，
   值不对只能重载；有站占着 UDC 时不重载，报 `udc_busy`。
3. 让位：把绑在 in-tree `hid-playstation` 上的手柄逐个解绑、卸载该模块、写黑名单
   （E-03）。黑名单是**唯一**写进系统目录的东西（G-06），停用时删除。
4. 加载定制模块（绝对路径，不用 `modprobe` 名字，避免撞上 DKMS 的安装路径），确认它
   真的进了内核（E-04）。

`insmod` 之后再读 `/proc/modules` 确认是必须的：`insmod` 的退出码只表示"内核接受了
这个文件"，模块若在初始化里自己退回，`insmod` 仍然成功。这里等的是"模块在列"，不是
"命令没报错"。
"""

from __future__ import annotations

from typing import Callable, Awaitable

from . import supervise
from .log import EventLog
from .system import System

CUSTOM_MODULE = "hid_playstation_ds5dummy"
IN_TREE_MODULE = "hid_playstation"
# 模块名是下划线，sysfs 驱动目录用的是驱动结构体的名字（连字符）。解绑判定比的是
# 目录基名，不是模块名——拿模块名去比永远匹配不上。
IN_TREE_DRIVER = "hid-playstation"

# 本插件写的黑名单（G-06）。放 `/etc` 而不是 `/usr/lib/modprobe.d`：后者属于发行版
# 与包管理器，写进去会被下一次系统更新当成自己的文件。
BLACKLIST_PATH = "/etc/modprobe.d/ds5dummy-decky.conf"
BLACKLIST_TEXT = (
    "# DS5Dummy Decky 插件：接管期间阻止 in-tree hid_playstation 自动加载。\n"
    "# 停用或卸载插件时由插件删除。\n"
    "blacklist hid_playstation\n"
)

# DKMS 包安装的那份黑名单（`pkg/hid-playstation-ds5dummy`）。它存在就说明系统里
# 已经有一个接管方，两个一起上会抢同一批设备（E-01 的第二条）。
DKMS_CONF_PATH = "/usr/lib/modprobe.d/ds5dummy-hid-playstation.conf"
SYSTEM_UNIT = "ds5dummy.service"

DUMMY_HCD_DIR = "/sys/module/dummy_hcd"
DUMMY_HCD_NUM = "/sys/module/dummy_hcd/parameters/num"
UDC_CLASS = "/sys/class/udc"
# configfs 的 usb_gadget/ 顶层目录由 libcomposite 注册。SteamOS 内核把它编成模块
# （CONFIG_USB_LIBCOMPOSITE=m），没加载时 daemon 建 gadget 会在 mkdir 上拿到 EPERM，
# 每 2 秒重试一次却永远建不成站，表现为「手柄连上了但虚拟手柄出不来」。
GADGET_MODULE = "libcomposite"
HID_DEVICES = "/sys/bus/hid/devices"
HID_DRIVER = "/sys/bus/hid/drivers/hid-playstation"

# 各命令的超时：insmod 要跑模块初始化（含 HID 注册），给足一分钟；其余都是毫秒级
# 动作，超时只可能意味着内核侧卡住了。
INSMOD_TIMEOUT_S = 60.0
COMMAND_TIMEOUT_S = 15.0

# `modprobe hid_playstation` 之后等驱动重新出现在 sysfs；正常是几十毫秒。
# 恢复 in-tree 驱动后，最多等这么久看 `/sys/bus/hid/drivers/hid-playstation` 出现。
# 这个目录在部分内核上永远不会出现（驱动注册的名字不同、或模块根本没探测到设备），
# 所以它只是"给绑定留一点时间"的观察窗口，不能当成必须满足的条件——否则每次停用
# 都会白等满一整段，覆盖安装时的卸载因此要多花好几秒。
IN_TREE_SETTLE_S = 1.0


class TakeoverFailed(Exception):
    """某一步失败；`reason` 是面板分流用的原因码，`detail` 面向人。

    Attributes:
        reason: 原因码（`system_service`、`insmod` 等，见设计 §5）。
        detail: 具体错误（命令 stderr、sysfs 读值等）。
        rollback_errors: 失败后回滚时自身出错的步骤；空列表表示回滚干净。
    """

    def __init__(self, reason: str, detail: str = "") -> None:
        """记录原因码与说明。

        Args:
            reason: 原因码。
            detail: 补充说明，可为空。
        """
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason
        self.detail = detail
        self.rollback_errors: list[str] = []


class Takeover:
    """一次接管/还原的执行器；每次使用新建一个，不跨流程复用回滚栈。"""

    def __init__(
        self,
        system: System,
        daemon: supervise.Daemon,
        sleep: Callable[[float], Awaitable[None]],
        log: EventLog,
        udc_num: int,
    ) -> None:
        """构造执行器；不产生任何副作用。

        Args:
            system: 执行接缝。
            daemon: 程序守护对象，停用序列要用它停程序（D-01）。
            sleep: 异步睡眠函数，等 in-tree 驱动重现时用。
            log: 事件日志；每一步前记一行，G-09。
            udc_num: `dummy_hcd num` 的目标值。
        """
        self._system = system
        self._daemon = daemon
        self._sleep = sleep
        self._log = log
        self._udc_num = udc_num
        self._undo: list[tuple[str, Callable[[], None]]] = []
        self._unloaded_in_tree = False

    async def run(self, module_path: str) -> None:
        """执行接管的全部内核侧步骤（不含启动程序）。

        Args:
            module_path: 要 `insmod` 的 `.ko` 绝对路径。

        Raises:
            TakeoverFailed: 任一步失败；已完成的步骤会被回滚，`rollback_errors`
                非空表示回滚也不干净。
        """
        self._undo.clear()
        self._unloaded_in_tree = False
        try:
            self._precheck()
            self._ensure_gadget_module()
            self._ensure_dummy_hcd()
            self._unbind_in_tree()
            self._write_blacklist()
            self._load_module(module_path)
        except TakeoverFailed as error:
            self._log.error(f"接管失败（{error.reason}）：{error.detail}")
            error.rollback_errors = await self.rollback()
            raise

    async def rollback(self) -> list[str]:
        """按相反顺序撤销已完成的内核侧步骤，最后把 in-tree 驱动请回来。

        `dummy_hcd` 不回滚：它是无害的公共前置，卸载它只会让下一次接管多等一拍。

        Returns:
            回滚中出错的步骤描述；空列表表示全部成功。每一条都已记进日志。
        """
        errors: list[str] = []
        while self._undo:
            label, undo = self._undo.pop()
            try:
                undo()
            except OSError as error:
                errors.append(f"{label}: {error}")
                self._log.error(f"回滚 {label} 失败：{error}")
        if self._unloaded_in_tree:
            try:
                await self._restore_in_tree()
            except TakeoverFailed as error:
                errors.append(f"恢复 in-tree hid-playstation: {error.detail}")
        return errors

    async def stop(self, *, restore_in_tree: bool = True) -> None:
        """停用序列（D-01～D-04）：先停程序，再卸模块、删黑名单、恢复 in-tree。

        Args:
            restore_in_tree: False 用于内核变化后的重来——紧接着就要在新内核上接管，
                把 in-tree 驱动请回来只会让它马上又被赶走。

        Raises:
            TakeoverFailed: 程序没停住（`daemon_stuck`，此时不碰内核模块，D-07），
                或卸载模块失败（`unload`）。
        """
        self._log.info("停用：先停转发程序")
        try:
            await self._daemon.stop()
        except supervise.StopFailed as error:
            raise TakeoverFailed("daemon_stuck", str(error)) from error

        loaded = supervise.loaded_modules(self._system)
        if CUSTOM_MODULE in loaded:
            self._log.info(f"停用：卸载 {CUSTOM_MODULE}")
            result = self._system.run(["rmmod", CUSTOM_MODULE], timeout=COMMAND_TIMEOUT_S)
            if result.code != 0:
                raise TakeoverFailed(
                    "unload",
                    f"rmmod {CUSTOM_MODULE} 失败（退出码 {result.code}）："
                    f"{result.stderr.strip()}",
                )

        self._log.info("停用：删除黑名单")
        self._system.remove(BLACKLIST_PATH)
        if restore_in_tree:
            await self._restore_in_tree()
        self._log.info("停用完成")

    def load_dummy_hcd(self) -> None:
        """按插件设置补回 `dummy_hcd`（L-08 的外部改动纠正路径）。

        与启用流程共用 `_ensure_dummy_hcd()`：模块不在就加载，在但 `num` 不符就重载。

        Raises:
            TakeoverFailed: `udc_busy`（有站时不能重载）或 `dummy_hcd`（加载失败）。
        """
        self._ensure_gadget_module()
        self._ensure_dummy_hcd()

    def _precheck(self) -> None:
        """两条互斥检查（E-01；G-08）。

        Raises:
            TakeoverFailed: `system_service` 或 `dkms_package`。
        """
        result = self._system.run(
            ["systemctl", "is-active", SYSTEM_UNIT], timeout=COMMAND_TIMEOUT_S
        )
        if result.code == 0:
            raise TakeoverFailed(
                "system_service", f"{SYSTEM_UNIT} 正在运行，先停掉它再启用插件"
            )
        if self._system.exists(DKMS_CONF_PATH):
            raise TakeoverFailed(
                "dkms_package",
                f"检测到 {DKMS_CONF_PATH}（DKMS 内核驱动包），两者不能同时接管",
            )

    def _ensure_gadget_module(self) -> None:
        """保证 `libcomposite` 已加载，configfs 的 `usb_gadget/` 才会出现。

        SteamOS 把它编成模块（CONFIG_USB_LIBCOMPOSITE=m），开机不自动加载。缺它时
        daemon 建 gadget 的第一步 mkdir 就撞 EPERM，每 2 秒重试一次却永远建不成站。
        加载一次即可，卸载/回滚都不碰它——它是无害的公共前置，和 dummy_hcd 一样。

        Raises:
            TakeoverFailed: `gadget_module`（modprobe 失败或目录没出现）。
        """
        if supervise.module_loaded(self._system, GADGET_MODULE):
            return
        self._log.info(f"加载 gadget 前置模块 {GADGET_MODULE}")
        result = self._system.run(
            ["modprobe", GADGET_MODULE], timeout=COMMAND_TIMEOUT_S
        )
        if result.code != 0:
            raise TakeoverFailed(
                "gadget_module",
                f"modprobe {GADGET_MODULE} 失败（退出码 {result.code}）："
                f"{result.stderr.strip()}",
            )
        if not self._system.exists("/sys/kernel/config/usb_gadget"):
            raise TakeoverFailed(
                "gadget_module",
                f"modprobe {GADGET_MODULE} 成功，但 /sys/kernel/config/usb_gadget 没有出现",
            )

    def _ensure_dummy_hcd(self) -> None:
        """保证 `dummy_hcd` 已按 `is_high_speed=0 num=<设置>` 加载（E-02）。

        Raises:
            TakeoverFailed: `udc_busy`（有站时不能重载）或 `dummy_hcd`（加载失败）。
        """
        if not self._system.exists(DUMMY_HCD_DIR):
            self._log.info(f"加载 dummy_hcd（num={self._udc_num}）")
            self._modprobe_dummy_hcd()
            return

        current = self._system.read(DUMMY_HCD_NUM).strip()
        if current == str(self._udc_num):
            return
        self._log.info(
            f"dummy_hcd 的 num 是 {current}，设置是 {self._udc_num}；需要重载"
        )
        if self._udc_in_use():
            raise TakeoverFailed(
                "udc_busy",
                f"dummy_hcd 正被占用（num={current}），无法改到 {self._udc_num}；"
                "先停掉正在使用虚拟手柄的程序",
            )
        result = self._system.run(["rmmod", "dummy_hcd"], timeout=COMMAND_TIMEOUT_S)
        if result.code != 0:
            raise TakeoverFailed(
                "dummy_hcd",
                f"rmmod dummy_hcd 失败（退出码 {result.code}）：{result.stderr.strip()}",
            )
        self._modprobe_dummy_hcd()

    def _modprobe_dummy_hcd(self) -> None:
        """按插件设置加载 `dummy_hcd` 并确认它进了内核。

        Raises:
            TakeoverFailed: `dummy_hcd`。
        """
        result = self._system.run(
            ["modprobe", "dummy_hcd", "is_high_speed=0", f"num={self._udc_num}"],
            timeout=COMMAND_TIMEOUT_S,
        )
        if result.code != 0:
            raise TakeoverFailed(
                "dummy_hcd",
                f"modprobe dummy_hcd 失败（退出码 {result.code}）：{result.stderr.strip()}",
            )
        if not self._system.exists(DUMMY_HCD_DIR):
            raise TakeoverFailed("dummy_hcd", "加载命令成功，但 /sys/module/dummy_hcd 不存在")

    def _udc_in_use(self) -> bool:
        """是否有虚拟控制器已经被某个 gadget 绑定。

        Returns:
            任一 `/sys/class/udc/*/UDC` 非空时为 True；目录不存在（没有 UDC）时为
            False——那正好说明可以安全重载。
        """
        try:
            names = self._system.listdir(UDC_CLASS)
        except OSError:
            return False
        for name in names:
            try:
                if self._system.read(f"{UDC_CLASS}/{name}/UDC").strip():
                    return True
            except OSError:
                continue
        return False

    def _unbind_in_tree(self) -> None:
        """把绑在 in-tree `hid-playstation` 上的设备逐个解绑（E-03）。

        Raises:
            TakeoverFailed: `in_tree`，写 unbind 失败时。
        """
        try:
            names = self._system.listdir(HID_DEVICES)
        except OSError:
            return
        for name in sorted(names):
            try:
                target = self._system.readlink(f"{HID_DEVICES}/{name}/driver")
            except OSError:
                continue  # 没有 driver 链接：设备未绑定，不用让位
            if (target.rstrip("/").rsplit("/", 1)[-1]) != IN_TREE_DRIVER:
                continue
            self._log.info(f"解绑 {name}（in-tree {IN_TREE_MODULE}）")
            try:
                # sysfs 的 unbind 属性按设备名匹配，行尾空白由 sysfs 自己剥掉。
                self._system.write(f"{HID_DRIVER}/unbind", name + "\n")
            except OSError as error:
                raise TakeoverFailed("in_tree", f"解绑 {name} 失败：{error}") from error

        if supervise.module_loaded(self._system, IN_TREE_MODULE):
            self._log.info(f"卸载 in-tree {IN_TREE_MODULE}")
            result = self._system.run(
                ["rmmod", IN_TREE_MODULE], timeout=COMMAND_TIMEOUT_S
            )
            if result.code != 0:
                raise TakeoverFailed(
                    "in_tree",
                    f"rmmod {IN_TREE_MODULE} 失败（退出码 {result.code}）："
                    f"{result.stderr.strip()}",
                )
            self._unloaded_in_tree = True

    def _write_blacklist(self) -> None:
        """写黑名单并压入回滚项（E-03、G-06）。

        Raises:
            TakeoverFailed: `in_tree`；根文件系统只读时会带内核的原始错误文本。
        """
        self._log.info(f"写黑名单 {BLACKLIST_PATH}")
        try:
            self._system.mkdirs("/etc/modprobe.d")
            self._system.write(BLACKLIST_PATH, BLACKLIST_TEXT)
        except OSError as error:
            raise TakeoverFailed(
                "in_tree",
                f"写 {BLACKLIST_PATH} 失败：{error}（SteamOS 根文件系统只读时"
                "需要先 steamos-readonly disable）",
            ) from error
        self._undo.append(("删除黑名单", lambda: self._system.remove(BLACKLIST_PATH)))

    def _load_module(self, module_path: str) -> None:
        """加载定制模块并确认它真的进了内核（E-04）。

        已经加载过就跳过——重试与 E-07 的健康检查都会走到这里，`insmod` 对已加载的
        模块只会报 "File exists"。

        Raises:
            TakeoverFailed: `insmod`。
        """
        if supervise.module_loaded(self._system, CUSTOM_MODULE):
            self._log.info(f"{CUSTOM_MODULE} 已在内核里，跳过 insmod")
            return

        # 定制模块的真实依赖（modinfo depends: ff-memless,led-class-multicolor）。
        # insmod 不像 modprobe 会自动拉依赖：in-tree hid_playstation 尚未加载过时，
        # 这两个模块不在内核里，insmod 就会报 "Unknown symbol in module"。
        # 失败不拦，让紧随其后的 insmod 报真正的错。
        for dependency in ("hid", "ff-memless", "led-class-multicolor"):
            self._system.run(["modprobe", dependency], timeout=COMMAND_TIMEOUT_S)
        self._log.info(f"insmod {module_path}")
        result = self._system.run(["insmod", module_path], timeout=INSMOD_TIMEOUT_S)
        if result.code != 0:
            raise TakeoverFailed(
                "insmod",
                f"insmod 失败（退出码 {result.code}）：{result.stderr.strip()}",
            )
        if not supervise.module_loaded(self._system, CUSTOM_MODULE):
            raise TakeoverFailed(
                "insmod", f"insmod 返回 0，但 {CUSTOM_MODULE} 没有出现在 /proc/modules"
            )
        self._undo.append(
            (
                f"卸载 {CUSTOM_MODULE}",
                lambda: self._rmmod_quietly(CUSTOM_MODULE),
            )
        )

    def _rmmod_quietly(self, name: str) -> None:
        """回滚用：卸载模块，已不在时不动。

        Args:
            name: 模块名。

        Raises:
            OSError: 模块在但卸不掉（调用方记成回滚失败）。
        """
        if not supervise.module_loaded(self._system, name):
            return
        result = self._system.run(["rmmod", name], timeout=COMMAND_TIMEOUT_S)
        if result.code != 0:
            raise OSError(
                f"rmmod {name} 失败（退出码 {result.code}）：{result.stderr.strip()}"
            )

    async def _restore_in_tree(self) -> None:
        """把 in-tree 驱动请回来，并等它重新出现在 sysfs（D-04）。

        Raises:
            TakeoverFailed: `in_tree`。
        """
        result = self._system.run(
            ["modprobe", IN_TREE_MODULE], timeout=COMMAND_TIMEOUT_S
        )
        if result.code != 0:
            raise TakeoverFailed(
                "in_tree",
                f"modprobe {IN_TREE_MODULE} 失败（退出码 {result.code}）："
                f"{result.stderr.strip()}",
            )
        deadline = self._system.now() + IN_TREE_SETTLE_S
        while self._system.now() < deadline:
            if self._system.exists(HID_DRIVER):
                return
            await self._sleep(supervise.POLL_S)
        # 驱动目录没出现：模块可能被编进内核（没有各自的目录），不算失败。
        self._log.warn(
            f"{IN_TREE_SETTLE_S:g}s 内没等到 {HID_DRIVER}；若 {IN_TREE_MODULE} "
            "已在内核里（/proc/modules），系统已经处于 in-tree 接管状态"
        )
