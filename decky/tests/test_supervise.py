#!/usr/bin/env python3
"""崩溃退避与本后端对转发程序的起停（§4.8、L-05）。"""

from __future__ import annotations

import asyncio
import json
import signal
import unittest

import support
from ds5dummy import supervise


class LoadedModulesTest(unittest.TestCase):
    """`/proc/modules` 的解析是健康检查与接管判定的共同依据。"""

    def test_reads_first_column(self) -> None:
        fake = support.FakeSystem()
        fake.modules = {"hid_playstation", "dummy_hcd"}
        self.assertEqual(
            supervise.loaded_modules(fake), {"hid_playstation", "dummy_hcd"}
        )
        self.assertTrue(supervise.module_loaded(fake, "dummy_hcd"))
        self.assertFalse(supervise.module_loaded(fake, "nope"))

    def test_missing_proc_counts_as_empty(self) -> None:
        class Broken(support.FakeSystem):
            def read(self, path: str) -> str:
                raise OSError(path)

        self.assertEqual(supervise.loaded_modules(Broken()), set())


class BackoffTest(unittest.TestCase):
    """退避阶梯 2/4/8/16/30，用完第 5 档仍站不住就放弃。"""

    def test_ladder(self) -> None:
        backoff = supervise.Backoff()
        self.assertEqual(
            [backoff.record() for _ in range(5)], [2.0, 4.0, 8.0, 16.0, 30.0]
        )
        self.assertIsNone(backoff.record())
        self.assertEqual(backoff.failures, 6)

    def test_reset(self) -> None:
        backoff = supervise.Backoff()
        backoff.record()
        backoff.reset()
        self.assertEqual(backoff.failures, 0)
        self.assertEqual(backoff.record(), 2.0)


class DaemonTest(unittest.IsolatedAsyncioTestCase):
    """起停与 pid 认领：Decky 重载后端之后不能把还在跑的程序弄丢。"""

    async def asyncSetUp(self) -> None:
        self.fake = support.FakeSystem()
        self.layout = support.Layout("/plugin", "/settings", "/data")
        self.daemon = supervise.Daemon(
            self.fake,
            self._sleep,
            self.layout,
            support.EventLog(),
            lambda: self.fake.release,
        )

    async def _sleep(self, seconds: float) -> None:
        self.fake.clock += seconds
        await asyncio.sleep(0)

    async def test_start_writes_pidfile(self) -> None:
        pid = self.daemon.start()
        self.assertTrue(self.fake.alive(pid))
        record = json.loads(self.fake.files[self.layout.pid_file])
        self.assertEqual(record["pid"], pid)
        self.assertEqual(record["release"], self.fake.release)
        self.assertEqual(self.fake.spawns[0][0], (self.layout.daemon_bin,))

    async def test_start_fixes_exec_bit(self) -> None:
        """起进程前必须补执行位：Decky 解 ZIP 不恢复 mode，二进制落在 0644。"""
        self.daemon.start()
        self.assertEqual(self.fake.executable_calls, [self.layout.daemon_bin])

    async def test_stop_terminates(self) -> None:
        pid = self.daemon.start()
        await self.daemon.stop()
        self.assertFalse(self.fake.alive(pid))
        self.assertFalse(self.fake.exists(self.layout.pid_file))

    async def test_stop_is_idempotent(self) -> None:
        await self.daemon.stop()  # 没有 pid 文件、什么都没有
        pid = self.daemon.start()
        self.fake.processes[pid] = 0  # 已经自己退出了
        await self.daemon.stop()

    async def test_stuck_process_raises(self) -> None:
        pid = self.daemon.start()
        self.fake.stubborn.add(pid)
        with self.assertRaises(supervise.StopFailed):
            await self.daemon.stop()
        self.assertTrue(self.fake.alive(pid))

    async def test_adopt_live_process(self) -> None:
        pid = self.daemon.start()
        other = supervise.Daemon(
            self.fake,
            self._sleep,
            self.layout,
            support.EventLog(),
            lambda: self.fake.release,
        )
        self.fake.links[f"/proc/{pid}/exe"] = self.layout.daemon_bin
        self.assertEqual(other.adopt(), pid)
        self.assertEqual(other.status().running, True)

    async def test_adopt_rejects_pid_reuse(self) -> None:
        pid = self.daemon.start()
        other = supervise.Daemon(
            self.fake,
            self._sleep,
            self.layout,
            support.EventLog(),
            lambda: self.fake.release,
        )
        self.fake.links[f"/proc/{pid}/exe"] = "/usr/bin/something-else"
        self.assertIsNone(other.adopt())
        self.assertFalse(self.fake.exists(self.layout.pid_file))

    async def test_status_reports_exit_once(self) -> None:
        pid = self.daemon.start()
        self.fake.processes[pid] = -int(signal.SIGKILL)
        status = self.daemon.status()
        self.assertFalse(status.running)
        self.assertEqual(status.exit_code, -int(signal.SIGKILL))
        # 第二次观察不再有退出码可报（认领进程的语义），但仍然是"没在跑"。
        again = self.daemon.status()
        self.assertFalse(again.running)

    async def test_reap_kills_strays_but_keeps_ours(self) -> None:
        """历次启停留下的同类进程要清掉，正在用的那个不能动。"""
        pid = self.daemon.start()
        self.fake.links[f"/proc/{pid}/exe"] = self.layout.daemon_bin

        stray = 9101
        self.fake.processes[stray] = None
        self.fake.links[f"/proc/{stray}/exe"] = self.layout.daemon_bin

        else_where = 9102
        self.fake.processes[else_where] = None
        self.fake.links[f"/proc/{else_where}/exe"] = "/usr/lib/ds5dummy/bin/ds5dummyd"

        await self.daemon.reap_strays()

        self.assertTrue(self.fake.alive(pid))
        self.assertFalse(self.fake.alive(stray))
        self.assertTrue(self.fake.alive(else_where))


if __name__ == "__main__":
    unittest.main()
