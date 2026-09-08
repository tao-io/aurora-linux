-- Isolated M4 rendering validation; no production user configuration changes.
hl.monitor({ output = os.getenv("M4_HYPR_OUTPUT") or "HEADLESS-1", mode = (os.getenv("M4_HYPR_WIDTH") or "256") .. "x" .. (os.getenv("M4_HYPR_HEIGHT") or "256") .. (os.getenv("M4_HYPR_PHYSICAL") == "1" and "@60" or "@10"), position = "0x0", scale = 1 })
hl.config({
  misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
           force_default_wallpaper = 0 },
  general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
  decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
  animations = { enabled = false },
  debug = { disable_logs = false, enable_stdout_logs = true },
})
