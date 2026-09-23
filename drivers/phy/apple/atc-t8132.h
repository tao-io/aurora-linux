/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_ATCPHY_T8132_H
#define APPLE_ATCPHY_T8132_H
struct apple_t8132_atc;
struct apple_t8132_atc *apple_t8132_atc_create(struct platform_device *pdev,
					    struct mutex *lock, struct phy **dp);
void __iomem *apple_t8132_atc_regs(struct apple_t8132_atc *atc);
int apple_t8132_atc_configure(struct apple_t8132_atc *atc, unsigned int mode, bool flipped);
int apple_t8132_atc_usb_get(struct apple_t8132_atc *atc);
int apple_t8132_atc_usb_put(struct apple_t8132_atc *atc);
int apple_t8132_atc_suspend(struct apple_t8132_atc *atc);
#endif
