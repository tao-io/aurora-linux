/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * DPTX remote-port lifecycle for the J713 external DCP (endpoint 0x2a,
 * service "dispext0:dcpdptx-port-epic:0"): connect / display request /
 * HPD on / bounded observation / HPD off / display release / disconnect,
 * plus the firmware callbacks (AUX, partner, display hints, link training).
 *
 * Message layouts come from the local J713 RemotePortProxy connectTo /
 * setPowerState / displayRelease / hotPlugDetectChangeOccurred extracts
 * (DISPLAYPORT-POC.md "Native AFKEP V2 command reference"); protocol
 * antecedents are Sven Peter / Asahi dptxep.c and dptxep.h (callback names,
 * target field bits: core 3:0, ATC 7:4, die 11:8, connected bit 15).
 * Display hints layout is qualified by captured J713 HPD-on/off callbacks.
 * Predecessors: tools/m4-dcpext-probe/dcpext_dptx_trial.h,
 * dcpext_dptx_link.h and dcpext_display_hints.h (qualified in
 * afk-monitor-cleanup-trial-20260914-2, afk-link-training-20260914-1,
 * iboot-image-20260914-5).
 *
 * Facts kept: native displayRelease and HPD use service group 8 (older
 * Linux used group 0); request/release carry 16 data bytes, connect /
 * disconnect / HPD 32; service status sits at raw 112 for connect
 * (data 32) and 96 otherwise; a route connect obliges a matching
 * acknowledged disconnect before DART release (transport quiescence does
 * not clear it); a display request obliges a release; HPD-on obliges
 * HPD-off; the PHY policy is up to HBR2, two lanes, SSC off, level-zero
 * drive qualified; unimplemented callbacks are answered with
 * kIOReturnUnsupported (0xe00002c7), never fabricated success; a failed PHY
 * operation is latched (phy_fault) and never acknowledged as success.
 * Callback ordering that must stay: 5 (willChangeLinkConfiguration) takes
 * the crossbar connection down without erasing the cached rate, 9 changes
 * the rate after that, 6 (didChangeLinkConfiguration) brings the crossbar
 * back only for a nonzero rate.
 *
 * Layering and locking as in m4_dcpext_edid.h: own interface/sequence,
 * tags are phase + 1, ops->write onto a record-only AFK channel,
 * caller-serialised. The PHY hooks are the ATC/crossbar side that remains
 * in tools/m4-dcpext-probe (dcpext_session_atc.h, dcpext_atc_*.h,
 * dcpext_dpxbar.h) until drivers/phy/apple/atc.c gains the T8132 DP path.
 */
#ifndef M4_DCPEXT_DPTX_H
#define M4_DCPEXT_DPTX_H
#include "m4_dcpext_afk.h"

#define M4_DCPEXT_DPTX_SERVICE "dispext0:dcpdptx-port-epic:0"
#define M4_DCPEXT_DPTX_TARGET_ATC1 0x8010	/* connected | ATC1 | core0 | die0 */
#define M4_DCPEXT_DPTX_RATE_HBR 0xa
#define M4_DCPEXT_DPTX_RATE_HBR2 0x14
#define M4_DCPEXT_DPTX_HPD_OBSERVE_MS 600

/* Link-training state, PHY-written values only. */
struct m4_dcpext_dptx_link {
	u32 rate, lanes;
	bool fault;
	u32 drive[6]; /* Two successfully written {kind, voltage, preemphasis}. */
};

/* Real PHY writes; retain ownership on failure, stop the PHY before teardown. */
struct m4_dcpext_dptx_phy_ops {
	int (*set_rate)(void *cookie, struct m4_dcpext_dptx_link *l, u32 rate);
	int (*begin_config)(void *cookie, struct m4_dcpext_dptx_link *l);
	int (*set_drive)(void *cookie, struct m4_dcpext_dptx_link *l, const u32 drive[6]);
	int (*validate_config)(void *cookie, struct m4_dcpext_dptx_link *l);
};

/* Native handleSetDisplayHints: 108-byte IOAVDisplayHints at service 64,
 * status written at 208. DCP's parsed monitor data, not a raw EDID.
 */
struct m4_dcpext_monitor {
	bool seen;
	char name[17];
	u32 width, height;
};

/* Firmware's deviceNotResponding/BusyTimeout/NotStarted notifications.
 * Acknowledging receipt never proves link training or clears ownership.
 * Keep diagnostics separately from lifecycle command completion status.
 */
struct m4_dcpext_dptx_errors {
	u32 count[3];
	u32 last_command;
};
int m4_dcpext_dptx_error_notification(struct m4_dcpext_dptx_errors *errors,
				    u32 command, u32 size);

enum m4_dcpext_dptx_phase {
	M4_DCPEXT_DPTX_CONNECT, M4_DCPEXT_DPTX_REQUEST, M4_DCPEXT_DPTX_RELEASE,
	M4_DCPEXT_DPTX_DISCONNECT, M4_DCPEXT_DPTX_DONE,
	M4_DCPEXT_DPTX_HPD_ON, M4_DCPEXT_DPTX_HPD_WAIT, M4_DCPEXT_DPTX_HPD_OFF,
};

struct m4_dcpext_dptx_ops {
	int (*write)(void *cookie, const u8 *payload, u32 bytes);
	u64 (*now_ms)(void *cookie);
	int (*aux)(void *cookie, bool enable);	/* callbacks 0 / 1 */
	int (*check_partner)(void *cookie);	/* callbacks 10 and 7 */
	bool (*hpd)(void *cookie);		/* physical HPD before HPD-on */
	const struct m4_dcpext_dptx_phy_ops *phy;
};

struct m4_dcpext_dptx {
	enum m4_dcpext_dptx_phase phase;
	u16 interface;			/* 0 until the port publication matched */
	u8 sequence;
	/* Policy (set before use). */
	bool aux_callbacks;		/* service AUX/partner/hints callbacks */
	bool release_before_hpd_off; /* Explicit m1n1 ordering experiment. */
	bool hpd_routing;		/* request -> HPD on/wait/off -> release */
	bool training;			/* dispatch link-training callbacks */
	bool capabilities;		/* experimental capability replies */
	/* Side-service gating inputs (iBoot client on 0x23). */
	bool iboot_gate, iboot_ready, iboot_finished;
	/* Progress. */
	bool sent, replied;
	int result;			/* -ENODATA until a reply; never success by default */
	u64 observe_until;
	unsigned int callbacks;
	/* Obligations, cleared only by the matching acknowledged reply. */
	bool route_dirty, display_dirty, hpd_dirty, phy_fault;
	struct m4_dcpext_dptx_link link;
	struct m4_dcpext_monitor monitor;
	struct m4_dcpext_dptx_errors errors;
};

/* Standalone link-training callback dispatcher (Unsupported for anything
 * outside the qualified policy; touches neither reply nor hardware then).
 */
int m4_dcpext_dptx_link_callback(struct m4_dcpext_dptx_link *l,
				 const struct m4_dcpext_dptx_phy_ops *ops, void *cookie,
				 u32 command, const u8 *data, u32 size, u8 *reply);
int m4_dcpext_dptx_display_hints(struct m4_dcpext_monitor *m, const u8 *data, u32 size);
/* Lifecycle. */
int m4_dcpext_dptx_send(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops, void *cookie);
int m4_dcpext_dptx_callback(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			    void *cookie, const u8 *p, u32 bytes);
/* Command replies and firmware commands on the selected interface. */
int m4_dcpext_dptx_receive(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			   void *cookie, const u8 *p, u32 bytes);
int m4_dcpext_dptx_progress(struct m4_dcpext_dptx *d, const struct m4_dcpext_dptx_ops *ops,
			    void *cookie);
#endif
