# J613 (MacBook Air 13" M3) kernel: 25G83 (macOS 26.6.2) firmware path

Branch `satchlj/j613-kernel-25g83`, on Eryk's `eryk_m4_gpu` at `7a72ddf3a` (2026-10-05, the branch head when this was
pushed).

## What this branch is

J613 on Eryk's M4 "g16" GPU path, with the GPU and DCP firmware from macOS 26.6.2 (25G83). This is the path that gets
the GPU to its full clock. In order:

1. `051e943fa`..`673571aee` (41): the J613 board on the g16 path, then the native startup, power and render work
   (research steps G17–G52). These were developed on an archive import of `7a72ddf3a` and replayed here with identical
   trees. Upstream files the import lacked (93 binary fixtures) are untouched.
2. 13 HID/haptics commits by Yicong Hui and Chromatischer (aurora-silicon/linux#4), unchanged.
3. `85c22ffa6`..`8468a01f0` (5): the 25G83 display. PMGR retained display domains, the drm/apple v26.6 backend with
   the v14.7 backend alongside, the disabled 25G83 display DT, the CTM on both backends, and a read-only PMP sensor
   readout. After `8468a01f0` the drm/apple directory is byte-identical to the tested G55 build.
4. `face8de62`..`bdfe8ba90` (3): the daily-driver candidate D2. Display coldplug without a manual module load, the
   haptic force-stage commit from the 14.x branch, and the 14.x daily's USB-C recovery ported.

62 commits: 49 ours, 13 by others.

## Build and boot

- `make ARCH=arm64 LLVM=1` (Rust 1.93.1), Eryk's g16 config plus `CONFIG_DRM_ASAHI=m`, `CONFIG_DRM_APPLE=y`, `CONFIG_TYPEC=y`,
  `CONFIG_APPLE_PMP=y`, `CONFIG_HID_APPLE_MTP_HAPTIC=m`. `CONFIG_DRM_APPLE_J613_25G83_ACTIVATE` stays unset from D1 on,
  because the gate is built into appledrm. Up to G55 it was a separate module loaded with
  `display_clock_hz=<this boot's ADT clock>`.
- DTB: `t8122-j613-25g83.dtb` (all display nodes disabled; the gate enables them).
- m1n1: `satchlj/j613-m1n1` (aurora-silicon/m3_m1n1_dirtyroom), including the 25G83 handoff `30e01de` and the
  display-clock witness `d5d2932`. The J613 stage 1 on that branch (`f974c0f`) loads this stage 2 from the ESP with
  no host.
- System firmware must be 26.6.2 (25G83). The GPU firmware and DCP firmware are those that iBoot loads; nothing is
  included.
- Userspace: `satchlj/j613-mesa-25g83` (aurora-silicon/mesa-eryk-m4), native Gallium GL. Its `tools/j613-d2/` has the
  session integration.

## Tested on hardware vs compile-only

| Commits | Status |
|---|---|
| 41 g16 commits | each step was booted on the J613 in its research cycle; the final tree is what G54/G55 ran |
| haptics (13) | G54: the actuator binds and clicks |
| PMGR, drm/apple v26.6 + v14.7, 25G83 DT | G54: first client, native 2560x1664@60 swaps (971 in one session), brightness, native Mesa Hyprland and browser |
| CTM `cc0c4759f` | G55: setter/getter status 0, exact readback incl. a negative coefficient; the owner saw the panel warm at 4000 K |
| PMP sensors `8468a01f0` | G54: module load, read, unload |
| D1 `face8de62`, haptic stages `90c666cae`, USB `bdfe8ba90` | compile-only (D1 and D2 kernels built; not booted). The haptic and USB changes run on the 14.x daily kernel |

Compile: the G55 and D2 builds used these sources. The HID Kconfig/Makefile lines in G55 were worded differently but
are equivalent. The 25G83 DTB and the PMP sensor module were also compiled against the G55 tree on 2026-10-07. Not
tested: suspend, long-term stability, physical luminance, and boots with no USB host attached.

## Key findings

- **Full GPU clock.** On 25G83 the GPU reaches its top state, SGX state 8 / macOS profile 13, 1338 MHz at 925 mV.
  During 1.29 s of compute, every one of 9,088 samples read clkgen `0x290e1c000+0x14 = 0x8180` (low nibble 0). The
  3/4 pulse-skip seen on 14.8.3 is gone.
- What it took: the native startup sequence (one 0x16 then six 0x1b device-control commands, kick 0x84/0x11 after
  initdata ready); PowerPerformance +0x62a8 = 1 (1 ms secondary performance timer; zero meant immediate expiry); Power
  +0xc8 = 11000 and +0xcc = 8000 (centi-degC thermal target and release). Left at zero, those two trigger the thermal
  PI at room temperature and cap the GPU at state 1. Also the firmware idle policy (idle power-off) and a separate
  G15G 25G83 parameter-manager encoding.
- **DCP 26.6 ABI** differs from 14.x and from H17P: PIODMA D201 output 16, swap-info D590 input 1840, D576
  input 4 / output 0. The first client needs D451 (20/28 bytes); without it A000 fails with -EINVAL.
- **CTM selectors:** **A423** setter / **A422** getter on 25G83, against **A421 / A420** on 14.8.3. Same 80-byte
  layout, signed two's-complement Q32.
- The display clock comes from the same boot's ADT (`clock-ids` 345/416). The J613 reads 712000000 Hz. It is passed
  as a witness from m1n1, never as a fixed rate.

## Data from Apple software or this machine

No firmware, Apple binaries or disassembly. Constants and offsets in the commits (object offsets, selector numbers,
ABI sizes) were derived from the 25G83 firmware and from hypervisor traces of macOS.

## Commits (oldest first; author is Satya Benson unless named)

- `051e943fa` J613 on the 25G83 g16 path: board table, DT, PMP report
- `30d068485` J613: MTR/SoCHot thermal config, cacheable hw objects
- `9a9eb57bb` J613: ring the device-control doorbell when the startup NOP stalls; log firmware trace on timeout
- `40c549a2f` PMP: wait for firmware setup and the PTD ready bit before power requests (J613 early GPU vote crash)
- `391cd270a` J613: asahi.g16_trace_mask diagnostic; DRM_ASAHI as module for live-load tests
- `5d6fdf766` J613: A/B the 33 stable missing Main words from macOS 25G83
- `f299efeeb` J613: preserve requested startup trace bits and snapshot pstate handshake
- `aabf4af10` J613: poll pstate completion instead of waiting for an undelivered IRQ
- `a81b7d05b` J613: drop the ineffective Main prefix A/B while retaining pstate polling
- `844be9095` J613: use native device-control kick after initdata ready
- `b48ac6bd6` J613: publish packed control generation and retain ring diagnostics
- `894cc06d6` J613: defer idle power-off during startup control diagnostic
- `f25c78b11` J613: disable initial firmware tracing for startup isolation
- `b0ffcf1ed` J613: test native zero-generation device-control startup
- `4cb2718d5` asahi: test native J613 bootstrap pstate independently of ceiling
- `5be45314e` J613: request bounded RTKit crash diagnostic after preserved startup timeout
- `fdd0322e3` J613: use native secondary performance timer interval
- `25661f87b` J613: stage native empty-channel wake and idle notification
- `d47df3f4d` J613: extend native startup to the owned queue barrier
- `e71a1b1e3` J613: test native context release with packed generation accounting
- `d719e33d7` J613: enable staged compute self-test and observe native clock state
- `c6bc264ab` J613: bounded owned compute load and joint clock timing census
- `5c5a12042` J613: restore native thermal target and release thresholds
- `6ff0d039c` J613: enable firmware idle policy through identified device control
- `5e83a545a` J613: qualify sustained board-top compute within two-second bound
- `02385c6fb` J613: construct native load-following controller inputs from device tree
- `f564d77d5` J613: carry ADT controller tunables in the 25G83 DT and retain native power budget
- `6d590990f` J613: retain the common default for optional performance reset iterations
- `1bafaa56e` J613: match native pre-init thermal maximum and power-PI seed
- `e2bf7eabc` J613: initialize inactive limiter ceilings without pinning lower bounds
- `3c3392d54` J613: guard qualification clock reads against idle and buffer ramp observations
- `7b4a6e544` J613: supply the firmware-verified Globals max-power input
- `78f41e437` J613: remove diagnostic sweeps of partially clocked engine banks
- `0c8429d41` J613: initialize cold SE integral to its firmware reset value
- `0d04650eb` J613: keep ADT inputs out of the captured static board overlay
- `be7a38449` J613: encode the G15G 25G83 parameter manager separately from M4
- `9b767721a` J613: select parameter-manager ABI from admitted board identity
- `d05dd6414` J613: serialize TA microsequence with 25G83 dispatch strides
- `4558bc679` Revert "J613: serialize TA microsequence with 25G83 dispatch strides"
- `af1296a4a` J613: trim only the StartFragment tail from the 25G83 sequence
- `673571aee` J613: end FinalizeFragment at the 25G83 finish opcode
- `d3c3079fc` HID: dockchannel-hid: forward correct report type on SET_REPORT (Yicong Hui)
- `397b86012` HID: spi-hid-apple: enumerate the fourth HID interface (Chromatischer)
- `7620fb2a5` HID: spi-hid-apple: address reports by report id, not interface id (Chromatischer)
- `c0a6cf862` HID: apple-mtp-haptic: add driver for the MTP haptic touchpad actuator (Yicong Hui)
- `92278d0a5` HID: apple-mtp-haptic: bind the actuator on SPI machines (Chromatischer)
- `c1676fdef` HID: magicmouse: add support for controlling MTP touchpad haptics (Yicong Hui)
- `56d7e7920` HID: magicmouse: allow haptics on SPI trackpads (Chromatischer)
- `50670c9e8` HID: magicmouse: make the haptic waveform shape configurable (Chromatischer)
- `6a55ed34a` HID: magicmouse: drive click feedback from the host (Chromatischer)
- `4a92e1b57` HID: magicmouse: add a deep click (Chromatischer)
- `99d4c1eb6` HID: magicmouse: derive the click while the host holds the actuator (Chromatischer)
- `fb5ef2fee` HID: magicmouse: forget the actuator's mode across system sleep (Chromatischer)
- `e9f99788f` HID: apple-mtp-haptic: put the actuator back in device-controlled mode (Chromatischer)
- `85c22ffa6` pmdomain: apple: keep J613's inherited display domains out of auto-PM
- `d705f1f8e` drm: apple: J613 display on 25G83 (macOS 26.6.2) DCP firmware
- `c0d70a6bd` arm64: dts: apple: t8122-j613: 25G83 display topology, disabled
- `cc0c4759f` drm: apple: send the colour matrix (CTM) on the v14.7 and v26.6 backends
- `8468a01f0` hwmon: read-only J613 PMP sensor records (25G83)
- `face8de62` drm: apple: J613 25G83 display comes up by itself (D1)
- `90c666cae` HID: magicmouse: click through each force stage on the way up
- `bdfe8ba90` usb: J613 USB-C role and PHY recovery (D2, from the 14.x daily R3)
