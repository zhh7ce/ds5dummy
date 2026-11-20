#!/usr/bin/env python3
"""状态机：解析、接管、退出码分流、等待重试、崩溃退避、内核变化（A-03）。"""

from __future__ import annotations

import unittest

import support
from ds5dummy import resolve
from ds5dummy import takeover
from ds5dummy.elf import VERMAGIC_KEY
from ds5dummy.system import DownloadHttpError


def url(release: str) -> str:
    """某个频道 Release 上该内核的产物地址（远端命中第一个频道就够）。"""
    return resolve.module_url(release, resolve.CHANNEL_TAGS[0])


def block_all_channels(fake: support.FakeSystem, release: str) -> None:
    """让每个频道都回 404；任何频道漏填都会变成网络错误。"""
    for tag in resolve.CHANNEL_TAGS:
        fake.downloads[resolve.module_url(release, tag)] = DownloadHttpError(404)


class StateTestBase(unittest.IsolatedAsyncioTestCase):
    """每个用例一套干净的假世界与状态机。"""

    async def asyncSetUp(self) -> None:
        self.h = support.Harness()
        self.fake = self.h.fake
        self.ctl = self.h.controller

    async def enable_from_cache(self) -> dict[str, object]:
        """把一份缓存模块装好并启用，返回 RUNNING 快照。"""
        self.h.install_cached_module()
        snapshot = await self.ctl.set_enabled(True)
        self.assertEqual(snapshot["state"], "running")
        return snapshot


class ResolveBranchTest(StateTestBase):
    """M-01/I-03 的三条解析分支。"""

    async def test_cache_hit(self) -> None:
        snapshot = await self.enable_from_cache()
        self.assertEqual(snapshot["module"]["source"], "cache")
        self.assertEqual(snapshot["module"]["owner"], "custom")

    async def test_remote_download(self) -> None:
        self.h.push_remote_module()
        snapshot = await self.ctl.set_enabled(True)
        self.assertEqual(snapshot["state"], "running")
        self.assertEqual(snapshot["module"]["source"], "remote")
        self.assertTrue(self.fake.exists(self.h.cache_file))

    async def test_no_artifact(self) -> None:
        block_all_channels(self.fake, self.fake.release)

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "unavailable")
        self.assertEqual(snapshot["reason"], "no_artifact")
        self.assertIn(self.fake.release, snapshot["reason_text"])
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))

    async def test_bad_remote_artifact(self) -> None:
        self.fake.downloads[url(self.fake.release)] = support.make_elf(
            {VERMAGIC_KEY: "6.1.0-other SMP preempt mod_unload"}
        )

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["reason"], "bad_artifact")
        self.assertFalse(self.fake.exists(self.h.cache_file))

    async def test_network_error(self) -> None:
        snapshot = await self.ctl.set_enabled(True)
        self.assertEqual(snapshot["reason"], "network")

    async def test_recheck_does_not_touch_kernel(self) -> None:
        block_all_channels(self.fake, self.fake.release)

        snapshot = await self.ctl.recheck()

        self.assertEqual(snapshot["state"], "unavailable")
        self.assertEqual(self.fake.commands, [])


class ExitCodeTest(StateTestBase):
    """窗口内退出码分流：0 关、1 等、2/≥120 起不来、其它按崩溃退避。"""

    async def test_zero_turns_off(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 0

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "disabled")
        self.assertFalse(snapshot["enabled"])
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))

    async def test_one_waits_without_counting_crash(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 1

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "waiting")
        self.assertEqual(snapshot["reason"], "not_ready")
        self.assertEqual(self.ctl._backoff.failures, 0)
        self.assertEqual(snapshot["daemon"]["restarts"], 0)
        # 等待期间不动内核：接管已被回滚。
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))

    async def test_waiting_retries_after_ten_seconds(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 1
        await self.ctl.set_enabled(True)

        self.fake.clock = self.ctl._retry_at
        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "waiting")
        self.assertGreaterEqual(snapshot["waited_s"], 10.0)
        self.assertEqual(self.ctl._backoff.failures, 0)

    async def test_bad_snapshot_does_not_retry(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 2

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "failed")
        self.assertEqual(snapshot["reason"], "daemon_start")
        self.assertIsNone(self.ctl._retry_kind)

    async def test_exec_failure_does_not_retry(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 125

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["reason"], "daemon_start")
        self.assertIsNone(self.ctl._retry_kind)

    async def test_crash_backoff_and_give_up(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 3

        snapshot = await self.ctl.set_enabled(True)
        self.assertEqual(snapshot["reason"], "daemon_start")
        self.assertEqual(self.ctl._retry_kind, "crash")
        self.assertEqual(self.ctl._retry_at, 2.0)

        for _ in range(5):
            self.fake.clock = self.ctl._retry_at
            await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "failed")
        self.assertEqual(snapshot["reason"], "daemon_crash")
        self.assertIsNone(self.ctl._retry_kind)
        self.assertEqual(snapshot["daemon"]["restarts"], 6)

    async def test_manual_reenable_resets_backoff(self) -> None:
        self.h.install_cached_module()
        self.fake.spawn_exit = 3
        await self.ctl.set_enabled(True)
        self.assertEqual(self.ctl._backoff.failures, 1)

        self.fake.spawn_exit = None
        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "running")
        self.assertEqual(self.ctl._backoff.failures, 0)


class RunningTest(StateTestBase):
    """运行中的健康检查、外部改动纠正与程序退出。"""

    async def test_daemon_death_exit_one_goes_waiting(self) -> None:
        await self.enable_from_cache()
        pid = self.ctl._daemon.pid
        self.fake.processes[pid] = 1

        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "waiting")
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)

    async def test_daemon_death_exit_zero_turns_off(self) -> None:
        await self.enable_from_cache()
        pid = self.ctl._daemon.pid
        self.fake.processes[pid] = 0

        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "disabled")
        self.assertFalse(snapshot["enabled"])
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertIn(takeover.IN_TREE_MODULE, self.fake.modules)

    async def test_daemon_crash_schedules_retry(self) -> None:
        await self.enable_from_cache()
        pid = self.ctl._daemon.pid
        self.fake.processes[pid] = 3

        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "failed")
        self.assertEqual(snapshot["reason"], "daemon_exit")
        self.assertEqual(self.ctl._retry_kind, "crash")

    async def test_module_removed_externally_is_reloaded(self) -> None:
        await self.enable_from_cache()
        self.fake.modules.discard(takeover.CUSTOM_MODULE)

        await self.ctl._health_tick()

        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertEqual(self.ctl.snapshot()["state"], "running")

    async def test_blacklist_removed_externally_is_rewritten(self) -> None:
        await self.enable_from_cache()
        self.fake.remove(takeover.BLACKLIST_PATH)

        await self.ctl._health_tick()

        self.assertTrue(self.fake.exists(takeover.BLACKLIST_PATH))

    async def test_dummy_hcd_removed_externally_is_reloaded(self) -> None:
        await self.enable_from_cache()
        self.fake.modules.discard("dummy_hcd")
        self.fake.directories.discard(takeover.DUMMY_HCD_DIR)
        self.fake.files.pop(takeover.DUMMY_HCD_NUM, None)

        await self.ctl._health_tick()

        self.assertTrue(self.fake.exists(takeover.DUMMY_HCD_DIR))
        self.assertEqual(self.fake.files[takeover.DUMMY_HCD_NUM].strip(), "3")

    async def test_second_enable_is_health_check_only(self) -> None:
        await self.enable_from_cache()
        spawns = len(self.fake.spawns)

        snapshot = await self.ctl.set_enabled(True)

        self.assertEqual(snapshot["state"], "running")
        self.assertEqual(len(self.fake.spawns), spawns)

    async def test_redownload_refused_while_running(self) -> None:
        await self.enable_from_cache()

        snapshot = await self.ctl.redownload()

        self.assertEqual(snapshot["state"], "running")
        self.assertIn("先停用", snapshot["detail"])

    async def test_restart_daemon_keeps_module(self) -> None:
        await self.enable_from_cache()
        pid = self.ctl._daemon.pid

        snapshot = await self.ctl.restart_daemon()

        self.assertEqual(snapshot["state"], "running")
        self.assertNotEqual(self.ctl._daemon.pid, pid)
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)

    async def test_new_backend_adopts_running_daemon(self) -> None:
        await self.enable_from_cache()
        spawns = len(self.fake.spawns)
        replacement = support.Controller(
            self.fake,
            self.h.layout,
            sleep=self.h.sleep,
            version="0.1.0",
        )

        snapshot = await replacement.set_enabled(True)

        self.assertEqual(snapshot["state"], "running")
        self.assertEqual(len(self.fake.spawns), spawns)


class DisableTest(StateTestBase):
    """停用顺序与幂等（D-01～D-09）。"""

    async def test_disable_restores_everything(self) -> None:
        await self.enable_from_cache()

        snapshot = await self.ctl.set_enabled(False)

        self.assertEqual(snapshot["state"], "disabled")
        self.assertFalse(snapshot["enabled"])
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertFalse(self.fake.exists(takeover.BLACKLIST_PATH))
        self.assertIn(takeover.IN_TREE_MODULE, self.fake.modules)
        self.assertEqual(snapshot["module"]["owner"], "in_tree")
        # D-09：缓存不删。
        self.assertTrue(self.fake.exists(self.h.cache_file))

    async def test_disable_when_never_enabled(self) -> None:
        snapshot = await self.ctl.set_enabled(False)
        self.assertEqual(snapshot["state"], "disabled")

    async def test_stuck_daemon_keeps_switch_on(self) -> None:
        await self.enable_from_cache()
        pid = self.ctl._daemon.pid
        self.fake.stubborn.add(pid)

        snapshot = await self.ctl.set_enabled(False)

        self.assertEqual(snapshot["state"], "failed")
        self.assertEqual(snapshot["reason"], "daemon_stuck")
        self.assertTrue(snapshot["enabled"])
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)
        self.assertTrue(self.fake.exists(takeover.BLACKLIST_PATH))


class KernelChangeTest(StateTestBase):
    """SteamOS 更新内核后的第一个动作（L-03、L-04）。"""

    async def test_rebuilds_for_new_release(self) -> None:
        await self.enable_from_cache()
        old_pid = self.ctl._daemon.pid

        new_release = "6.18.51-valve2-1-neptune-700-gdeadbeef"
        self.fake.release = new_release
        self.fake.downloads[url(new_release)] = support.make_elf(
            {VERMAGIC_KEY: new_release + " SMP preempt mod_unload"}
        )

        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["uname"], new_release)
        self.assertEqual(snapshot["state"], "running")
        self.assertEqual(snapshot["module"]["release"], new_release)
        self.assertNotEqual(self.ctl._daemon.pid, old_pid)
        self.assertIn(takeover.CUSTOM_MODULE, self.fake.modules)

    async def test_new_kernel_without_artifact(self) -> None:
        await self.enable_from_cache()
        new_release = "6.18.51-valve2-1-neptune-700-gdeadbeef"
        self.fake.release = new_release
        block_all_channels(self.fake, new_release)

        await self.ctl._health_tick()

        snapshot = self.ctl.snapshot()
        self.assertEqual(snapshot["state"], "unavailable")
        self.assertEqual(snapshot["reason"], "no_artifact")
        self.assertNotIn(takeover.CUSTOM_MODULE, self.fake.modules)


class UiSupportTest(StateTestBase):
    """面板要用到的只读字段与日志（U-01、U-07、U-08）。"""

    async def test_snapshot_fields(self) -> None:
        snapshot = await self.enable_from_cache()
        self.assertEqual(snapshot["uname"], self.fake.release)
        self.assertEqual(snapshot["version"], "0.1.0")
        self.assertEqual(snapshot["daemon"]["version"], "0.1.0")
        self.assertTrue(snapshot["daemon"]["running"])
        self.assertIsNotNone(snapshot["daemon"]["pid"])
        self.assertFalse(snapshot["busy"])

    async def test_logs(self) -> None:
        self.fake.files[self.h.layout.daemon_log] = "line1\nline2\n"

        self.h.log.info("一条事件")

        logs = await self.ctl.get_logs()

        self.assertEqual(logs["daemon"], ["line1", "line2"])
        self.assertEqual(len(logs["events"]), 1)

    async def test_clean_cache(self) -> None:
        self.fake.mkdirs("/data/kmod/other-release")

        await self.ctl.clean_cache()

        self.assertFalse(self.fake.exists("/data/kmod/other-release"))


if __name__ == "__main__":
    unittest.main()
