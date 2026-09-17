/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic owner of the maintained T8132 crossbar sequencer. */
#include "apple-display-crossbar-t8132.h"
static struct apple_dpxbar_t8132 session_xbar;
static bool session_dpxbar_owned;
#define session_dpxbar session_xbar.regs
#define session_dpxbar_selected session_xbar.selected
static int dpxbar_check_idle(void) { return apple_dpxbar_t8132_idle(&session_xbar); }
static int dpxbar_up(void) { return apple_dpxbar_t8132_up(&session_xbar); }
static int dpxbar_down(void) { return apple_dpxbar_t8132_down(&session_xbar); }
