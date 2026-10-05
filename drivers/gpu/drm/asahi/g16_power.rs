// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! 25G83 performance-state policy. The tables are per board (g16_board): the native
//! constructor's compacted states, not PLL writes. The GPU firmware requests each transition
//! through its PMP/Fender interface; g16_thermal (where the board has it) lowers the ceiling.
//! Load-based DVFS remains TODO; bounded tests retain independent guards.

use kernel::prelude::*;

/// asahi.g16_pstate, clamped to the board's highest state.
pub(crate) fn requested_state() -> Result<u32> {
    let board = crate::g16_board::get()?;
    let state = (*crate::module_parameters::g16_pstate.value()).min(board.max_pstate());
    if state == 0 { return Err(EINVAL); }
    Ok(state)
}

pub(crate) fn frequency_khz() -> Result<u32> {
    Ok(crate::g16_board::get()?.frequencies_mhz[requested_state()? as usize] * 1000)
}
