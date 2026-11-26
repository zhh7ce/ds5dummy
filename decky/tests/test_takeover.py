#!/usr/bin/env python3
"""接管顺序、回滚与停用（E-01～E-06、D-01～D-07）。"""

from __future__ import annotations

import asyncio
import os
import unittest

import support
from ds5dummy import supervise, takeover

DEVICE = "/sys/bus/hid/devices/0003:054C:0CE6.0001"


class TakeoverTest(unittest.IsolatedAsyncioTestCase):
    """`Takeover.run/stop` 在假内核上的行为。"""

    async def asyncSetUp(self) -> None:
        self.fake = support.FakeSystem()
        self.log = support.EventLog()
        self.daemon = supervise.Daemon(
            self.fake,
            self._sleep,
            support.Layout("/plugin", "/settings", "/data"),
            self.log,
            lambda: self.fake.release,
        )
        self.agent = takeover.Takeover(
            self.fake, self.daemon, self._sleep, self.log, udc_num=3
        )
        self.module_path = "/data/kmod/%s/%s.ko" % (self.fake.release, self.fake.release)

    async def _sleep(self, seconds: float) -> None:
        self.fake.clock += seconds
        await asyncio.sleep(0)

    def bind_in_tree_device(self) -> None:
        """造一个绑在 in-tree 驱动上的蓝牙手柄。"""
        self.fake.modules.add(takeover.IN_TREE_MODULE)
        self.fake.directories.add(takeover.HID_DRIVER)
        self.fake.directories.add(DEVICE)
        # 真 sysfs 里 driver 链接指向驱动目录本身（../../bus/hid/drivers/hid-playstation）。
        self.fake.links[DEVICE + "/driver"] = takeover.HID_DRIVER

    def verbose(self) -> list[tuple[str, ...]]:
        """去掉 systemctl 之外的命令，按发生顺序返回。"""
        return [args for args in self.fake.commands if args[0] != "systemctl"]

    async def test_run_sequence(self) -> None:
        self.bind_in_tree_device()

        await self.agent.run(self.module_path)

        self.assertEqual(
            self.verbose(),
            [
                ("modprobe", takeover.GADGET_MODULE),
                ("modprobe", "dummy_hcd", "is_high_speed=0", "num=3"),
                ("rmmod", takeover.IN_TREE_MODULE),
                ("modprobe", "hid"),
                ("modprobe", "ff-memless"),
                ("modprobe", "led-class-multicolor"),
                ("insmod", self.module_path),
            ],
        )
        self.assertEqual(
            self.fake.files[takeover.HID_DRIVER + "/unbind"], DEVICE.rsplit("/", 1)[-1] + "\n"
        )
        self.assertIn("blacklist hid_playstation", self.fake.files[takeover.BLACKLIST_PATH])
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)

    async def test_gadget_module_loaded_before_dummy_hcd(self) -> None:
        """libcomposite 必须早于建站，缺它时 usb_gadget/ 目录根本不存在。"""
        await self.agent.run(self.module_path)

        commands = self.verbose()
        self.assertLess(
            commands.index(("modprobe", takeover.GADGET_MODULE)),
            commands.index(("modprobe", "dummy_hcd", "is_high_speed=0", "num=3")),
        )
        self.assertIn(takeover.GADGET_MODULE, self.fake.modules)

    async def test_gadget_module_already_loaded_is_skipped(self) -> None:
        """已加载时不再 modprobe，但 insmod 依赖仍照常补。"""
        self.fake.modules.add(takeover.GADGET_MODULE)
        self.fake.directories.add("/sys/kernel/config/usb_gadget")

        await self.agent.run(self.module_path)

        self.assertNotIn(("modprobe", takeover.GADGET_MODULE), self.verbose())
        self.assertIn(("modprobe", "ff-memless"), self.verbose())
        self.assertIn(("modprobe", "led-class-multicolor"), self.verbose())

    async def test_gadget_module_failure_blocks_takeover(self) -> None:
        """modprobe 失败时接管停在 gadget_module，不往下碰 dummy_hcd。"""
        self.fake.fail["modprobe"] = 1

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.run(self.module_path)

        self.assertEqual(caught.exception.reason, "gadget_module")
        self.assertNotIn(("insmod", self.module_path), self.verbose())

    async def test_dummy_hcd_reload_when_num_differs(self) -> None:
        self.fake.modules.add("dummy_hcd")
        self.fake.directories.add(takeover.DUMMY_HCD_DIR)
        self.fake.files[takeover.DUMMY_HCD_NUM] = "1\n"
        self.fake.files["/sys/class/udc/dummy_udc.0/UDC"] = "\n"

        await self.agent.run(self.module_path)

        self.assertIn(("rmmod", "dummy_hcd"), self.verbose())
        self.assertTrue(
            any(args[:2] == ("modprobe", "dummy_hcd") and "num=3" in args for args in self.verbose())
        )
        self.assertEqual(self.fake.files[takeover.DUMMY_HCD_NUM], "3\n")

    async def test_busy_udc_blocks_reload(self) -> None:
        self.fake.modules.add("dummy_hcd")
        self.fake.directories.add(takeover.DUMMY_HCD_DIR)
        self.fake.files[takeover.DUMMY_HCD_NUM] = "1\n"
        self.fake.files["/sys/class/udc/dummy_udc.0/UDC"] = "ds5dummy-deadbeef\n"

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.run(self.module_path)

        self.assertEqual(caught.exception.reason, "udc_busy")
        self.assertNotIn(("rmmod", "dummy_hcd"), self.verbose())

    async def test_existing_system_service_blocks(self) -> None:
        self.fake.systemctl_active = True

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.run(self.module_path)

        self.assertEqual(caught.exception.reason, "system_service")
        self.assertEqual(self.verbose(), [])

    async def test_dkms_package_blocks(self) -> None:
        self.fake.files[takeover.DKMS_CONF_PATH] = "blacklist hid_playstation\n"

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.run(self.module_path)

        self.assertEqual(caught.exception.reason, "dkms_package")

    async def test_failure_rolls_back(self) -> None:
        self.bind_in_tree_device()
        self.fake.fail["insmod"] = 1

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.run(self.module_path)

        self.assertEqual(caught.exception.reason, "insmod")
        self.assertEqual(caught.exception.rollback_errors, [])
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertIn(takeover.IN_TREE_MODULE, self.fake.modules)
        self.assertTrue(self.fake.exists(takeover.HID_DRIVER))

    async def test_second_run_is_idempotent(self) -> None:
        await self.agent.run(self.module_path)
        again = takeover.Takeover(
            self.fake, self.daemon, self._sleep, self.log, udc_num=3
        )

        await again.run(self.module_path)

        self.assertEqual(
            [args for args in self.verbose() if args[0] == "insmod"],
            [("insmod", self.module_path)],
        )

    async def test_stop_sequence(self) -> None:
        await self.agent.run(self.module_path)
        pid = self.daemon.start()

        await self.agent.stop()

        self.assertFalse(self.fake.alive(pid))
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))
        self.assertIn(takeover.IN_TREE_MODULE, self.fake.modules)
        self.assertEqual(self.verbose()[-1], ("modprobe", takeover.IN_TREE_MODULE))

    async def test_stop_is_idempotent(self) -> None:
        await self.agent.stop()  # 什么都没有：程序没跑、模块没载、黑名单不在
        self.assertIn(takeover.IN_TREE_MODULE, self.fake.modules)

    async def test_stop_skips_in_tree_restore(self) -> None:
        await self.agent.stop(restore_in_tree=False)
        self.assertNotIn(
            ("modprobe", takeover.IN_TREE_MODULE), self.fake.commands
        )

    async def test_stuck_daemon_does_not_touch_modules(self) -> None:
        await self.agent.run(self.module_path)
        pid = self.daemon.start()
        self.fake.stubborn.add(pid)

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.stop()

        self.assertEqual(caught.exception.reason, "daemon_stuck")
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertTrue(self.fake.exists(takeover.BLACKLIST_PATH))
        self.assertNotIn(("rmmod", takeover.CUSTOM_MODULE), self.verbose())

    async def test_unload_failure_reason(self) -> None:
        await self.agent.run(self.module_path)
        self.fake.fail["rmmod"] = 1

        with self.assertRaises(takeover.TakeoverFailed) as caught:
            await self.agent.stop()

        self.assertEqual(caught.exception.reason, "unload")


class ConstantsTest(unittest.TestCase):
    """黑名单与互斥检查的路径是规格里的字面量（G-06、G-08）。"""

    def test_blacklist_path(self) -> None:
        self.assertEqual(
            takeover.BLACKLIST_PATH, "/etc/modprobe.d/ds5dummy-decky.conf"
        )

    def test_dkms_conf_path(self) -> None:
        self.assertEqual(
            takeover.DKMS_CONF_PATH,
            "/usr/lib/modprobe.d/ds5dummy-hid-playstation.conf",
        )


if __name__ == "__main__":
    unittest.main()
