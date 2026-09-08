#!/bin/sh
# Private, root-only M4 bring-up session. Requires a fresh boot.
set -eu
exec env \
    M4_HYPR_PHYSICAL=1 M4_HYPR_WIDTH=2560 M4_HYPR_HEIGHT=1600 \
    __EGL_VENDOR_LIBRARY_FILENAMES=/root/m4mesa/share/glvnd/egl_vendor.d/50_mesa.json \
    LIBGL_DRIVERS_PATH=/root/m4mesa/lib/dri \
    MESA_GLES_VERSION_OVERRIDE=3.0 MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
    ASAHI_M4_LAB=1 ASAHI_MESA_DEBUG=nocompress \
    VK_DRIVER_FILES=/root/m4mesa/share/vulkan/icd.d/asahi_icd.armv8-a.json \
    LD_LIBRARY_PATH=/root/m4mesa/lib PYTHONPATH=/root \
    python3 /root/g16-submit-test/run-render.py \
    --module /root/g16-submit-test/g16_lab.ko --render-drm-submit \
    --drm-client-mode composite --drm-submit-test /root/hyprland-validation/session.py \
    --interactive-seconds "${1:-3600}" --results /root/g16-render-results
