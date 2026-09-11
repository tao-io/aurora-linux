-- Normal user configuration, including Omarchy effects, bindings and services.
dofile(os.getenv("HOME") .. "/.config/hypr/hyprland.lua")
hl.config({ render = { new_render_scheduling = true } })
-- The development DCP driver lacks trustworthy physical panel dimensions.
hl.monitor({ output = "eDP-1", mode = "preferred", position = "0x0", scale = 2 })
