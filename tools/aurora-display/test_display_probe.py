# SPDX-License-Identifier: GPL-2.0-only
import copy
import importlib.util
import tomllib
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("display_probe", Path(__file__).with_name("display_probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def connected_fixture():
    return {"thunderbolt": [{"name": "LG UltraFine 5K", "authorized": True}],
            "drm": [{"name": "DP-1", "status": "connected", "enabled": "enabled", "link_status": 0,
                     "edid": {"valid": True, "manufacturer": "GSM", "name": "LG UltraFine"},
                     "modes": [{"width": 5120, "height": 2880, "refresh": 60, "flags": 0}]}],
            "hyprland": [{"name": "DP-1", "width": 5120, "height": 2880, "refreshRate": 59.94,
                          "scale": 2, "disabled": False, "dpmsStatus": True}]}


class ProbeTest(unittest.TestCase):
    def test_real_lg_device_uses_vendor_name_without_lg_in_product_name(self):
        fixture = connected_fixture()
        fixture["thunderbolt"][0].update(name="UltraFine 5K", vendor="LG Electronics")
        self.assertEqual(probe.evaluate(fixture)["checks"][0]["status"], "pass")

    def test_report_root_fields_stay_at_root_after_toml_parse(self):
        fixture = probe.evaluate(connected_fixture())
        fixture["machine"] = {"kernel": "test"}
        fixture["recent_display_logs"] = ["DP teardown"]
        fixture["errors"] = []
        parsed = tomllib.loads(probe.emit(fixture))
        self.assertEqual(parsed, fixture)

    def test_native_mode_accepts_scale_two_but_cannot_claim_panel_or_routing(self):
        report = probe.evaluate(connected_fixture())
        self.assertTrue(report["result"]["software_mode_pass"])
        self.assertFalse(report["result"]["support_verified"])
        self.assertFalse(report["result"]["panel_observed"])
        self.assertEqual(report["checks"][-2]["status"], "unknown")
        self.assertEqual(tomllib.loads(probe.emit(report))["result"], report["result"])

    def test_tunnel_failure_cannot_pass_from_thunderbolt_enumeration_alone(self):
        report = connected_fixture()
        report["drm"][0].update(status="disconnected", enabled="disabled")
        report["drm"][0]["edid"]["valid"] = False
        report["hyprland"] = []
        result = probe.evaluate(report)
        self.assertEqual(result["checks"][0]["status"], "pass")
        self.assertFalse(result["result"]["software_mode_pass"])

    def test_wrong_refresh_disabled_or_wrong_connector_cannot_pass(self):
        original = connected_fixture()
        for patch in [{"refreshRate": 30}, {"width": 2560}, {"name": "DP-2"},
                      {"disabled": True}, {"dpmsStatus": False}]:
            with self.subTest(patch=patch):
                fixture = copy.deepcopy(original)
                fixture["hyprland"][0].update(patch)
                self.assertFalse(probe.evaluate(fixture)["result"]["software_mode_pass"])

    def test_bad_link_invalid_edid_or_two_independent_halves_cannot_pass(self):
        for change in ["bad_link", "bad_edid", "halves", "interlaced"]:
            with self.subTest(change=change):
                fixture = connected_fixture()
                if change == "bad_link": fixture["drm"][0]["link_status"] = 1
                if change == "bad_edid": fixture["drm"][0]["edid"]["valid"] = False
                if change == "halves":
                    fixture["drm"][0]["modes"][0]["width"] = 2560
                    fixture["drm"].append(copy.deepcopy(fixture["drm"][0]))
                if change == "interlaced": fixture["drm"][0]["modes"][0]["flags"] = 16
                self.assertFalse(probe.evaluate(fixture)["result"]["software_mode_pass"])

    def test_edid_checks_checksum_and_advertised_extension_count(self):
        edid = bytearray(128)
        edid[:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
        edid[8:10] = b"\x1e\x6d"
        edid[127] = (-sum(edid)) % 256
        self.assertTrue(probe.edid_info(bytes(edid))["valid"])
        self.assertEqual(probe.edid_info(bytes(edid))["manufacturer"], "GSM")
        edid[10] ^= 1
        self.assertFalse(probe.edid_info(bytes(edid))["valid"])
        edid[126] = 1
        edid[127] = 0
        edid[127] = (-sum(edid)) % 256
        self.assertFalse(probe.edid_info(bytes(edid))["valid"])


if __name__ == "__main__":
    unittest.main()
