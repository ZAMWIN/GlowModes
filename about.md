# Per-Gamemode Glow

Independent glow settings for every player gamemode in Geometry Dash: give the cube, ship,
ball, UFO, wave, robot, spider and swing their own colour, brightness, blend mode and pulse
instead of one global glow for all of them.

Configured with a JSON file (`config/zam.per-gamemode-glow/per_gamemode_glow.json`), which is
re-read on the fly, plus in-game hotkeys (reload, presets, hue and brightness of the current
gamemode, and an RGB rainbow mode with its own speed control).

An RGB mode overlays a smooth rainbow on every gamemode at once — one key toggles it, two more
tune the speed while playing, and the log prints the exact value to keep.

Fully client-side: nothing is sent to the server and level progress is not affected.

Known limitation: Geometry Dash draws the robot's and the spider's own glow itself, from the icon
kit's glow colour, and ignores a per-player glow colour for those two modes -- so `robot` and
`spider` cannot take effect on this build. The mod says so in its log and documents it in the
README.

Made by zam, with the help of an AI assistant (opencode, DeepSeek V4.1 Flash) — the author set the
goals and checked every step in the game.

License for the mod's code: GPL-3.0 (full text in the repository). Geometry Dash and its assets
belong to RobTop Games.

See `README.md` for the config format, hotkeys, limitations and compatibility notes.
