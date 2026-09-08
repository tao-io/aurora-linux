#!/bin/sh
# Supervised M4 desktop using the installed user's Omarchy configuration.
set -eu
exec env \
    M4_HYPR_NATIVE_KMS=1 M4_HYPR_PHYSICAL=1 M4_HYPR_WIDTH=2560 M4_HYPR_HEIGHT=1664 \
    __EGL_VENDOR_LIBRARY_FILENAMES=/opt/m4mesa/share/glvnd/egl_vendor.d/50_mesa.json \
    LIBGL_DRIVERS_PATH=/opt/m4mesa/lib/dri GBM_BACKENDS_PATH=/opt/m4mesa/lib/gbm \
    MESA_GLES_VERSION_OVERRIDE=3.0 MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
    ASAHI_M4_LAB=1 ASAHI_MESA_DEBUG=nocompress LIBGL_ALWAYS_SOFTWARE=0 \
    VK_DRIVER_FILES=/opt/m4mesa/share/vulkan/icd.d/asahi_icd.armv8-a.json \
    LD_LIBRARY_PATH=/opt/m4mesa/lib PYTHONPATH=/root \
    python3 /root/g16-submit-test/run-render.py \
    --render-power-controller --module /root/g16-submit-test/g16_lab.ko --render-drm-submit \
    --drm-client-mode composite --drm-submit-test /root/hyprland-validation/session.py \
    --desktop-user "${1:-e}" --interactive-seconds "${2:-3600}" --results /root/g16-render-results
