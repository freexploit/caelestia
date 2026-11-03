#!/usr/bin/env python3
"""Journal and retry regressions for the Activity coordinator.

Runs without a session bus: dbus_next is stubbed when it is not installed, and
the coordinator's D-Bus and subprocess edges are replaced per test.
"""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
LIB = REPO / "src/lib/caelestia_activities.py"


def load_module():
    try:
        import dbus_next  # noqa: F401
    except ImportError:
        package = types.ModuleType("dbus_next")

        class Variant:
            def __init__(self, value):
                self.value = value

        class Message:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)

        class MessageType:
            ERROR = "error"
            SIGNAL = "signal"

        package.Variant = Variant
        package.Message = Message
        package.MessageType = MessageType
        aio = types.ModuleType("dbus_next.aio")
        aio.MessageBus = object
        package.aio = aio
        sys.modules["dbus_next"] = package
        sys.modules["dbus_next.aio"] = aio
    spec = importlib.util.spec_from_file_location("caelestia_activities", LIB)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeProcess:
    returncode = None

    async def wait(self):
        return 0

    def terminate(self):
        self.returncode = -15

    def kill(self):
        self.returncode = -9


class CoordinatorTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.module = load_module()
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        os.environ["XDG_STATE_HOME"] = self.temporary.name
        self.coordinator = self.module.Coordinator({
            "baselineActivity": "11111111-1111-4111-8111-111111111111",
            "createActivity": "Caelestia",
            "shellCommand": ["/bin/true"],
            "startupTimeout": 1,
            "trackerRequired": False,
        })
        self.coordinator.target = "22222222-2222-4222-8222-222222222222"
        self.journal_path = self.coordinator.journal_path

    def read_journal(self):
        return json.loads(self.journal_path.read_text())

    async def test_save_sentinel_preserves_then_removes_error(self):
        coordinator = self.coordinator
        coordinator.save("baseline", error="boom")
        self.assertEqual(self.read_journal()["error"], "boom")

        coordinator.save("entering")
        self.assertEqual(self.read_journal()["error"], "boom")
        self.assertEqual(self.read_journal()["state"], "entering")

        coordinator.save(error=None)
        self.assertNotIn("error", self.read_journal())

    async def test_stale_journal_error_is_cleared_on_new_attempt(self):
        self.journal_path.parent.mkdir(parents=True, exist_ok=True)
        self.journal_path.write_text(json.dumps({
            "version": 1, "state": "baseline", "panels": [], "meta": None,
            "trackerOwned": False, "error": "Inline-reply notifications require a reply-capable Caelestia UI",
        }))
        coordinator = self.module.Coordinator({"shellCommand": ["/bin/true"]})
        coordinator.target = "22222222-2222-4222-8222-222222222222"
        self.assertIn("error", coordinator.journal)

        seen = {}

        async def preflight():
            seen.update(self.read_journal())
            return []

        async def current():
            return coordinator.target

        async def ipc(method):
            return {"ready": True, "activity": coordinator.target} if method == "status" else True

        async def shortcuts():
            return None

        async def visibility(*_args):
            return None

        with mock.patch.object(coordinator, "preflight", preflight), \
             mock.patch.object(coordinator, "current", current), \
             mock.patch.object(coordinator, "ipc", ipc), \
             mock.patch.object(coordinator, "check_shortcuts", shortcuts), \
             mock.patch.object(coordinator, "set_visibility", visibility), \
             mock.patch.object(self.module.asyncio, "create_subprocess_exec",
                               mock.AsyncMock(return_value=FakeProcess())):
            await coordinator.enter()

        self.assertEqual(seen["state"], "entering")
        self.assertNotIn("error", seen, "attempt start must clear the stale failure")
        stored = self.read_journal()
        self.assertEqual(stored["state"], "caelestia")
        self.assertNotIn("error", stored)

    async def test_transient_failure_backs_off_and_recovers(self):
        coordinator = self.coordinator
        delays = []
        for _ in range(len(self.module.RETRY_DELAYS) + 2):
            coordinator.record_failure(self.module.EntryFailure("presenter unsupported", transient=True))
            delays.append(coordinator.retry_at - self.module.asyncio.get_running_loop().time())

        self.assertEqual(delays[0], pytest_approx(2))
        self.assertEqual(delays[1], pytest_approx(5))
        self.assertEqual(delays[2], pytest_approx(15))
        self.assertEqual(delays[3], pytest_approx(60))
        self.assertEqual(delays[-1], pytest_approx(60), "backoff must stay bounded")
        self.assertFalse(coordinator.retry_ready())

        coordinator.retry_at = self.module.asyncio.get_running_loop().time() - 1
        self.assertTrue(coordinator.retry_ready(), "a transient failure must become retryable without an Activity switch")

        coordinator.reset_retry()
        self.assertIsNone(coordinator.failure)
        self.assertTrue(coordinator.retry_ready())

    async def test_persistent_failure_stops_retrying_until_wake(self):
        coordinator = self.coordinator
        coordinator.record_failure(RuntimeError("identity does not match"))
        self.assertEqual(coordinator.retry_at, float("inf"))
        self.assertFalse(coordinator.retry_ready())
        coordinator.reset_retry()
        self.assertTrue(coordinator.retry_ready())

    async def test_shell_readiness_failure_is_transient(self):
        coordinator = self.module.Coordinator({
            "shellCommand": ["/bin/true"], "startupTimeout": 0, "trackerRequired": False,
        })
        coordinator.target = "22222222-2222-4222-8222-222222222222"

        async def preflight():
            return []

        async def current():
            return coordinator.target

        async def ipc(_method):
            return {"ready": False, "activity": coordinator.target}

        with mock.patch.object(coordinator, "preflight", preflight), \
             mock.patch.object(coordinator, "current", current), \
             mock.patch.object(coordinator, "ipc", ipc), \
             mock.patch.object(self.module.asyncio, "create_subprocess_exec",
                               mock.AsyncMock(return_value=FakeProcess())):
            with self.assertRaises(self.module.EntryFailure) as context:
                await coordinator.enter()

        self.assertTrue(context.exception.transient,
            "a shell that missed plasmashell startup must be retried without an Activity change")

    async def test_service_wake_during_failure_keeps_retry_alive(self):
        coordinator = self.coordinator
        coordinator.record_failure(RuntimeError("org.kde.plasmashell.evaluateScript: Remote peer disconnected"))
        self.assertFalse(coordinator.retry_ready())

        # plasmashell's NameOwnerChanged arrives while the failure is recorded.
        coordinator.wake_serial += 1
        coordinator.consume_wake_resets()
        self.assertTrue(coordinator.retry_ready(), "a service restart must survive a persistent failure latch")

        # A serial with no new event must not invent a retry.
        coordinator.record_failure(RuntimeError("still broken"))
        coordinator.consume_wake_resets()
        self.assertFalse(coordinator.retry_ready())

    async def test_recover_accepts_version1_journal_with_stale_error(self):
        self.journal_path.parent.mkdir(parents=True, exist_ok=True)
        self.journal_path.write_text(json.dumps({
            "version": 1, "state": "baseline", "panels": [], "meta": None,
            "trackerOwned": False, "error": "old failure",
        }))
        coordinator = self.module.Coordinator({"shellCommand": ["/bin/true"]})

        async def noop():
            return None

        async def command(*_args):
            return ""

        async def panels():
            return []

        class FakeBus:
            def disconnect(self):
                return None

        async def connect():
            coordinator.bus = FakeBus()

        with mock.patch.object(coordinator, "lock", lambda: None), \
             mock.patch.object(coordinator, "connect", connect), \
             mock.patch.object(coordinator, "command", command), \
             mock.patch.object(coordinator, "panels", panels):
            await coordinator.run(recover=True)

        stored = self.read_journal()
        self.assertEqual(stored["version"], 1)
        self.assertEqual(stored["state"], "baseline")
        self.assertNotIn("error", stored)

    async def test_clean_leave_removes_error(self):
        coordinator = self.coordinator
        coordinator.save("caelestia", error="old failure")

        async def command(*_args):
            return ""

        async def panels():
            return []

        with mock.patch.object(coordinator, "command", command), \
             mock.patch.object(coordinator, "panels", panels):
            await coordinator.leave()

        stored = self.read_journal()
        self.assertEqual(stored["state"], "baseline")
        self.assertNotIn("error", stored)


def pytest_approx(value, tolerance=0.5):
    class Approx:
        def __eq__(self, other):
            return abs(other - value) <= tolerance
    return Approx()


if __name__ == "__main__":
    unittest.main(verbosity=2)
