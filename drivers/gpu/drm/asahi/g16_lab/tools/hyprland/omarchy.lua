-- Load the installed user configuration and theme for the supervised session.
dofile(os.getenv("HOME") .. "/.config/hypr/hyprland.lua")
-- Keep the initial desktop gate within the qualified compositor feature set.
hl.config({ animations = { enabled = false } })
hl.bind("SUPER + SHIFT + Escape", hl.dsp.exec_cmd("touch /run/user/" .. os.getenv("M4_HYPR_UID") .. "/m4-hyprland-stop"))
