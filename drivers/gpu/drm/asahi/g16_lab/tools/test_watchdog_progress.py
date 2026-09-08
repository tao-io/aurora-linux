#!/usr/bin/env python3
"""CPU-only checks: never open a watchdog or GPU."""
import unittest
from unittest.mock import Mock
from experiment_watchdog import ExperimentWatchdog, KEEPALIVE


class WatchdogProgressTest(unittest.TestCase):
    def test_only_verified_gpu_and_compositor_progress_feeds(self):
        valid = dict(passed=True, shader_submitted=True, vulkan=True, frames=8,
                     device_id=0x8132, pixels_verified=524288, compositor_responsive=True)
        guard = ExperimentWatchdog()
        guard.integer = Mock(return_value=0)
        guard.keepalive_after_gpu_check(valid)
        guard.integer.assert_called_once_with(KEEPALIVE)
        for field in valid:
            for replacement in (None, False, 'true'):
                with self.subTest(field=field, replacement=replacement):
                    guard.integer.reset_mock()
                    bad = {**valid, field: replacement}
                    with self.assertRaises(RuntimeError):
                        guard.keepalive_after_gpu_check(bad)
                    guard.integer.assert_not_called()


if __name__ == '__main__':
    unittest.main()
