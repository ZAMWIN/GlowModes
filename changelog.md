# 1.0.0
- Independent glow for every player gamemode (cube, ship, ball, UFO, wave, robot, spider, swing): colour, brightness, blend mode and optional pulse.
- RGB mode: a smooth rainbow across all gamemodes at once, toggled in game (default F12), with its own speed, saturation, brightness and blending. Speed and colour transitions are eased and tuned with hotkeys.
- JSON config with live reload — edits apply within ~half a second, no restart needed.
- Hotkeys: reload config, next/previous preset, hue / brightness of the current gamemode, RGB toggle and RGB speed, and easy rebinding of all of them in the config.
- Named colour presets for switching palettes.
- Optional debug logging for troubleshooting per-mode glow (including a full sprite dump for robot/spider).
- Known limitation: `robot` and `spider` cannot take effect on GD 2.2091 -- the game draws those two modes' glow itself from the icon kit's glow colour (documented in the README, and the mod warns in the log if those profiles are enabled). The spider's eye does take the mod's colour, by request.
