/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_ATCPHY_T8132_SESSION_H
#define APPLE_ATCPHY_T8132_SESSION_H

#include "atc-t8132-power.h"
#include "atc-t8132-common.h"

/* Per-PHY common/AUX lifetime, independent of USB2 and of DCP lifetime.
 * Caller serializes all operations and holds MMIO, PM and module references
 * before start. A touched or failed session must retain those references.
 * Stop is legal only after the consumer has stopped its main link and crossbar;
 * it does not establish DCP DMA quiescence or retire any display buffer.
 */
struct apple_t8132_atc_session {
	void __iomem *regs;
	struct apple_t8132_tuning tuning[ARRAY_SIZE(apple_t8132_tuning_specs)];
	u32 saved[ARRAY_SIZE(apple_t8132_tuning_specs)];
	bool touched;
	bool active;
	bool failed;
	bool unflipped; /* Zero preserves the original pair0 diagnostic route. */
};

static const u32 apple_t8132_atc_idle_offsets[] = {
	8, 0x60, 0x64, 0x804, 0xa00, 0x16000, 0x16400,
};

static int apple_t8132_atc_session_idle(struct apple_t8132_atc_session *s)
{
	int ret = apple_t8132_atc_power_idle(s->regs);

	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(apple_t8132_atc_idle_offsets); i++)
		if (readl(s->regs + apple_t8132_atc_idle_offsets[i]))
			return -EBUSY;
	return 0;
}

static int apple_t8132_atc_session_start(struct apple_t8132_atc_session *s,
				       bool route_pin_d)
{
	int ret;

	if (s->failed)
		return -EIO;
	if (s->touched)
		return -EBUSY;
	/* No register operation may consume an unvalidated calibration tuple. */
	for (unsigned int i = 0; i < ARRAY_SIZE(s->tuning); i++) {
		const struct apple_t8132_tuning *t = &s->tuning[i];

		if (t->offset != apple_t8132_tuning_specs[i].offset ||
		    t->mask != apple_t8132_tuning_specs[i].mask || t->value & ~t->mask)
			return -EINVAL;
	}
	ret = apple_t8132_atc_session_idle(s);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->saved); i++)
		s->saved[i] = readl(s->regs + s->tuning[i].offset);
	s->touched = true;
	ret = apple_t8132_atc_power_up_reset(s->regs);
	if (ret)
		goto fail;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->tuning); i++) {
		const struct apple_t8132_tuning *t = &s->tuning[i];

		apple_t8132_atc_component_update(s->regs, t->offset, t->mask, t->value);
		if ((readl(s->regs + t->offset) & t->mask) != t->value) {
			ret = -EIO;
			goto fail;
		}
	}
	if (route_pin_d) {
		ret = apple_t8132_atc_route_pin_d(s->regs, !s->unflipped);
		if (ret)
			goto fail;
	}
	udelay(10);
	apple_t8132_atc_component_update(s->regs, 0x20000, 0, 0x10);
	udelay(100);
	ret = apple_t8132_atc_common_start(s->regs);
	if (ret)
		goto fail;
	apple_t8132_atc_aux_start(s->regs);
	if ((readl(s->regs + ATC_AUX_CTRL) & 0x30f) != 0x10f ||
	    readl(s->regs + ATC_AUX_CFG) & 1) {
		ret = -EIO;
		goto fail;
	}
	s->active = true;
	return 0;
fail:
	s->failed = true;
	return ret;
}

static int apple_t8132_atc_session_stop(struct apple_t8132_atc_session *s)
{
	int ret;

	if (s->failed)
		return -EIO;
	if (!s->touched)
		return 0;
	/* A ready main-link PLL still has a separate live owner. */
	if (readl(s->regs + 0x7034) & 8)
		return -EBUSY;
	apple_t8132_atc_aux_stop(s->regs);
	apple_t8132_atc_common_stop(s->regs);
	ret = apple_t8132_atc_power_down(s->regs);
	if (ret)
		goto fail;
	ret = apple_t8132_atc_session_idle(s);
	if (ret) {
		ret = -EIO;
		goto fail;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(s->saved); i++) {
		if (readl(s->regs + s->tuning[i].offset) != s->saved[i]) {
			ret = -EIO;
			goto fail;
		}
	}
	s->active = false;
	s->touched = false;
	return 0;
fail:
	s->failed = true;
	return ret;
}

#endif
