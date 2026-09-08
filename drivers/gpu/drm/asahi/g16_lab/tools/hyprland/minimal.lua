-- Isolated M4 rendering validation; no production user configuration changes.
hl.monitor({ output = "HEADLESS-1", mode = "256x256@10", position = "0x0", scale = 1 })
hl.config({
  misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
           force_default_wallpaper = 0 },
  general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
  decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
  animations = { enabled = false },
  debug = { disable_logs = false, enable_stdout_logs = true },
})
