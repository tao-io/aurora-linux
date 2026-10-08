# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real split attach and teardown functions with failing I/O."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "drivers/gpu/drm/apple/dcp-fabric.c"

SHIMS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define MUX_IDLE_DISCONNECT -1
#define lockdep_assert_held(x) ((void)0)
#define scoped_guard(kind, lock) for (int once = 1; once; once = 0)
#define READ_ONCE(x) (x)
#define time_before(a, b) ((a) < (b))
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
static unsigned long jiffies;
struct mux_chip;
struct mux_control { struct mux_chip *chip; };
struct mux_chip { struct mux_control mux[3]; int controllers; };
struct dptx_port { bool enabled, tile_hint; unsigned int tiles_h, tiles_v; void *service; };
struct apple_dcp {
    void *dev; bool external; int tb_lock;
    struct dptx_port dptxport[2];
    struct { bool active, xbar_up, clock_ok; struct mux_control *xbar; int mux_state;
             uint64_t generation; int (*set_active)(void *, bool); void *binding; } split;
};
struct apple_dcp_typec_route {
    struct apple_dcp *dcp; bool tunnel; unsigned int tunnel_dpin;
    struct mux_control *dpin[2], *xbar; int mux_index;
};
struct apple_dcp_typec_port {
    struct apple_dcp *split_dcp; struct apple_dcp_typec_route *owner;
    unsigned long tile_hint_deadline;
};
struct dcp_tb_attach_context {
    uint64_t generation; bool active; int (*set_active)(void *, bool); void *binding;
};
static int fail_phase, connect_calls, disconnect_calls, release_calls;
static int dptxport_set_hpd(void *service, bool active) { return 0; }
static int dcp_dptx_disconnect(struct apple_dcp *dcp, int unit) { disconnect_calls++; return 0; }
static void mux_control_deselect(struct mux_control *mux) { release_calls++; }
static int dcp_dpxbar_tunnel_select_source(struct mux_control *mux, int state) {
    if (state < 0) { release_calls++; return 0; }
    return fail_phase == 1 ? -EIO : fail_phase == 4 ? -EOPNOTSUPP : 0;
}
static int dcp_dpxbar_preselect(struct mux_control *mux, int state) {
    return state >= 0 && fail_phase == 2 ? -ENODEV : 0;
}
static int dcp_dptx_connect(struct apple_dcp *dcp, int unit) {
    connect_calls++; return fail_phase == 3 ? -ETIMEDOUT : fail_phase == 5 ? -EOPNOTSUPP : 0;
}
static int dcp_fabric_binding_request(uint64_t current, uint64_t request, bool active, bool same) {
    return current == request && same ? (active ? 1 : 0) : -ESTALE;
}
static int activate(void *binding, bool active) { return 0; }
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
"""

MAIN = r"""
int main(int argc, char **argv) {
    fail_phase = argc > 1 ? argv[1][0] - '0' : 0;
    struct mux_chip chip = { .controllers = 3 };
    for (int i = 0; i < 3; i++) chip.mux[i].chip = &chip;
    struct apple_dcp dcp = { .dptxport = { { .enabled = true, .tile_hint = true,
        .tiles_h = 2, .tiles_v = 1 }, { .enabled = true } } };
    struct apple_dcp_typec_route route = { .dcp = &dcp, .tunnel = true,
        .dpin = { NULL, &chip.mux[2] }, .xbar = &chip.mux[0], .mux_index = 2 };
    struct apple_dcp_typec_port port = { .owner = &route, .tile_hint_deadline = 10 };
    struct dcp_tb_attach_context req = { .generation = 42, .active = true,
        .set_active = activate, .binding = &chip };
    int ret = dcp_tb_split_locked(&port, &req);
    if ((fail_phase >= 1 && fail_phase <= 3) || fail_phase == 5) {
        int expected[] = { 0, -EIO, -ENODEV, -ETIMEDOUT, 0, -EIO };
        CHECK(ret == expected[fail_phase]);
        CHECK(port.split_dcp == NULL);
        CHECK(!dcp.split.active && dcp.split.xbar == NULL);
        CHECK(!dcp.split.xbar_up && !dcp.split.clock_ok);
        CHECK(dcp.split.generation == 0);
        CHECK(dcp.split.binding == NULL && dcp.split.set_active == NULL);
        CHECK(disconnect_calls == 1 && release_calls == 1);
        CHECK(connect_calls == (fail_phase == 3 || fail_phase == 5));
        CHECK(port.owner == &route && route.tunnel);
        fail_phase = 0;
        ret = dcp_tb_split_locked(&port, &req);
    }
    CHECK(ret == 0);
    CHECK(port.split_dcp == &dcp && dcp.split.active);
    CHECK(dcp.split.generation == 42 && dcp.split.binding == &chip);
    CHECK(dcp.split.xbar == &chip.mux[2]);
    dcp_tb_split_teardown_locked(&port);
    CHECK(port.split_dcp == NULL && !dcp.split.active);
    return 0;
}
"""


class SplitFailureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        revision = os.environ.get("LG5K_TEST_SOURCE_REV")
        source = (subprocess.check_output(["git", "show", revision + ":" + SOURCE], cwd=ROOT, text=True)
                  if revision else (ROOT / SOURCE).read_text())
        pieces = []
        for name in ("dcp_tb_split_teardown_locked", "dcp_tb_split_locked"):
            match = re.search(r"^static (?:int|void) " + name + r"\(.*?^\}\n", source, re.M | re.S)
            if not match:
                raise ValueError("Missing real driver function: " + name)
            pieces.append(match.group())
        cls.temp = tempfile.TemporaryDirectory()
        directory = Path(cls.temp.name)
        test = directory / "split.c"
        test.write_text(SHIMS + "\n".join(pieces) + MAIN)
        cls.executable = directory / "split"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", str(test), "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_success_and_unsupported_optional_source_select(self):
        for phase in (0, 4):
            with self.subTest(phase=phase):
                subprocess.run([str(self.executable), str(phase)], check=True)

    def test_each_failed_stage_releases_state_and_allows_retry(self):
        for phase in (1, 2, 3, 5):
            with self.subTest(phase=phase):
                subprocess.run([str(self.executable), str(phase)], check=True)


if __name__ == "__main__":
    unittest.main()
