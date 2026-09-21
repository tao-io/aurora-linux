# SPDX-License-Identifier: GPL-2.0-only OR MIT
# J713 external DisplayPort (DCPEXT): normal built-in kernel driver.
# Standalone modules remain available for explicitly guarded diagnostics.
# Build diagnostic modules against the prepared
# build/linux-asahi-j713 output directory with a separate module
# output directory, e.g. from the workspace root:
#   make -C build/linux-asahi-j713 ARCH=arm64 LLVM=1 \
#     M="$PWD/linux/linux-asahi/drivers/gpu/drm/apple/m4-dcpext" \
#     MO="$PWD/build/m4-dcpext-modules" modules
# Without MO= this kbuild writes its generated stub Makefile into the module
# source directory (this file) and every later build recurses.
m4_dcpext-y := m4_dcpext_afk.o m4_dcpext_dptx.o m4_dcpext_iboot.o m4_dcpext_edid.o m4_dcpext_modes.o m4_dcpext_connector.o m4_dcpext_rpc.o m4_dcpext_native.o m4_dcpext_kms.o
ifneq ($(KBUILD_EXTMOD),)
obj-m += m4_dcpext.o
obj-m += m4_dcpext_desktop.o
else
obj-$(CONFIG_DRM_APPLE_M4_DCPEXT) += m4_dcpext_desktop.o
endif
m4_dcpext_desktop-y := runtime/m4_dcpext_desktop_main.o runtime/dcpext_session.o runtime/dcpext_native_query.o m4_dcpext_routes.o m4_dcpext_audio_transport.o $(m4_dcpext-y)
ifeq ($(KBUILD_EXTMOD),)
m4_dcpext_desktop-y += runtime/m4_dcpext_secondary_main.o
endif
ccflags-y += -I$(src) -I$(src)/../../../../phy/apple -I$(src)/../../../../mux -I$(src)/../../../../usb/typec/tipd

obj-$(CONFIG_DRM_APPLE_M4_DCPEXT_AUDIO) += m4_dcpext_audio_pcm.o
