-- Load the installed user configuration and theme for the supervised session.
dofile(os.getenv("HOME") .. "/.config/hypr/hyprland.lua")
hl.env("PATH", "/opt/m4-hyprland/bin:/usr/share/omarchy/bin:" .. os.getenv("PATH"))
-- Keep the initial desktop gate within the qualified compositor feature set.
hl.config({
  animations = { enabled = false },
  general = { border_size = 0 },
  debug = { disable_logs = false, enable_stdout_logs = true },
})
-- The retained framebuffer reports synthetic physical dimensions, so auto
-- scaling cannot detect the J713 panel's real density.
hl.monitor({ output = os.getenv("M4_HYPR_OUTPUT") or "Unknown-1",
  mode = (os.getenv("M4_HYPR_WIDTH") or "2560") .. "x" .. (os.getenv("M4_HYPR_HEIGHT") or "1600") .. "@60",
  position = "0x0", scale = 2 })
hl.bind("SUPER + SHIFT + Escape", hl.dsp.exec_cmd("touch /run/user/" .. os.getenv("M4_HYPR_UID") .. "/m4-hyprland-stop"))
