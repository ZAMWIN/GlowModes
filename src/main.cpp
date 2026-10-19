// Per-Gamemode Glow
//
// Geometry Dash applies one global glow colour + one global "glow enabled" flag to every
// player gamemode. This mod keeps the game's glow sprites exactly as they are and only
// overrides their per-sprite parameters (tint, opacity, blend function) while a given
// gamemode is being drawn, using values from a JSON config.
//
// Injection points (verified against the GD 2.2091 bindings, and confirmed live in the
// log: "Enabled PlayerObject::update hook at <base>+0x4e6fc0"):
//   - PlayerObject::update(float)      win 0x4e6fc0   -> applied every frame (self healing)
//   - PlayerObject::updatePlayerArt()  win 0x4f6bb0   -> applied immediately on art/mode change
//   - CCKeyboardDispatcher::dispatchKeyboardMSG        -> config hotkeys (read-only, always
//                                                        forwards to the original)
//
// PlayerObject::updateGlowColor() and PlayerObject::updatePlayerGlow() are NOT hookable on
// Windows: the bindings expose no win address for either of them, so we deliberately do not
// touch them. For the same reason the mod does not call enableCustomGlowColor() /
// disableCustomGlowColor() either: both are inlined on Windows, but they are plain writes to
// PlayerObject::m_hasCustomGlowColor and PlayerObject::m_glowColor, the two fields the game's
// own glow update reads -- so the mod writes those fields directly.
//
// <Geode/Geode.hpp> is deliberately not included: it also pulls in Geode's UI headers, and
// Geode 5.11.2's ScrollLayer/Scrollbar override `scrollWheel` with 2.2081's `void` return
// type, while the 2.2091 cocos2d declarations in sdk-compat have `bool` (the game changed
// it). Those are the loader's UI classes, which this mod never touches, so include the same
// umbrellas Geode.hpp includes minus UI.hpp.
#include <Geode/Bindings.hpp>
#include <Geode/Loader.hpp>
#include <Geode/Utils.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/modify/Modify.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <matjson.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace pgg {

// ---------------------------------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------------------------------

enum class Mode : std::size_t {
    Cube = 0,
    Ship,
    Ball,
    Ufo,
    Wave,
    Robot,
    Spider,
    Swing,
    Count,
};

constexpr std::size_t kModeCount = static_cast<std::size_t>(Mode::Count);

constexpr std::string_view kModeKeys[kModeCount] = {
    "cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing",
};

// ---------------------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------------------

enum class Hotkey : std::size_t {
    Reload = 0,
    PresetNext,
    PresetPrev,
    HueDown,
    HueUp,
    BrightnessDown,
    BrightnessUp,
    RgbToggle,
    RgbSlower,
    RgbFaster,
    Count,
};

constexpr std::size_t kHotkeyCount = static_cast<std::size_t>(Hotkey::Count);

constexpr std::string_view kHotkeyKeys[kHotkeyCount] = {
    "reload", "preset-next", "preset-prev",
    "hue-down", "hue-up", "brightness-down", "brightness-up", "rgb-toggle",
    "rgb-slower", "rgb-faster",
};

// Defaults avoid everything GD itself binds (Esc, Space, arrows, Ctrl/Shift combos, the
// F1/F2/F3/F4 debug & F11 fullscreen keys); a profile can override every one of them.
// F12 can double as Steam's screenshot key: the key still reaches the game, so the toggle
// works, but anyone who does not want the screenshots can move it in the config.
constexpr cocos2d::enumKeyCodes kDefaultHotkeys[kHotkeyCount] = {
    KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_PageUp, KEY_PageDown,
};

constexpr float kHueStep         = 15.0f;  // degrees per hue key press
constexpr float kBrightnessStep  = 0.1f;   // opacity multiplier per brightness key press

/// Wheel speeds the rgb-slower / rgb-faster hotkeys walk through, in turns per second. Round
/// values on purpose: whatever the log prints can be pasted straight into the config.
constexpr float kRgbSpeedLadder[] = {
    0.005f, 0.0075f, 0.01f, 0.015f, 0.02f, 0.025f, 0.03f, 0.04f, 0.05f, 0.06f,
    0.08f, 0.1f, 0.13f, 0.16f, 0.2f, 0.25f, 0.3f, 0.4f, 0.5f,
};
constexpr std::size_t kRgbSpeedLadderSize =
    sizeof(kRgbSpeedLadder) / sizeof(kRgbSpeedLadder[0]);
constexpr float kRgbMaxSpeed = 5.0f;   // full turns around the colour wheel per second
/// Seconds a colour change is eased over (see Manager::fadeColor).
constexpr float kDefaultFadeSeconds = 0.4f;
constexpr auto  kAutoReloadDelay = std::chrono::milliseconds(500);
constexpr auto  kDebugLogDelay   = std::chrono::milliseconds(1000);
/// A second key-down for the same physical press is ignored within this window (see
/// Manager::onKeyEvent); a longer gap is treated as a fresh press, so a key-up that never
/// arrived cannot disable a hotkey for the rest of the session.
constexpr auto  kHotkeyHeldTimeout = std::chrono::milliseconds(600);

// ---------------------------------------------------------------------------------------
// Profile / preset
// ---------------------------------------------------------------------------------------

struct Profile {
    bool          enabled    = false;
    ccColor3B     color      = { 255, 255, 255 };
    float         brightness = 1.0f;   // multiplies the glow sprite's opacity (0..2)
    bool          additive   = false;  // false = normal alpha, true = additive blend
    float         pulse      = 0.0f;   // seconds per pulse cycle; <= 0 disables pulsing
};

/// The "rgb" block: a rainbow the mod produces itself, overriding every mode while it is on.
/// It lives outside the per-mode profiles on purpose -- it has to work on a fresh config,
/// where every mode is still "enabled": false, and the toggle hotkey has to do something.
struct RgbSettings {
    bool  enabled    = false;
    /// Full turns around the colour wheel per second. 0.02 is one slow, even rainbow in 50 s
    /// (faster reads as flicker); 0 keeps the colour still.
    float speed      = 0.02f;
    float saturation = 1.0f;   // 0 = white, 1 = fully saturated rainbow
    float brightness = 1.0f;   // multiplies the glow sprite's opacity (0..2)
    /// Normal blending by default, not additive: additive *adds* the colour to whatever is
    /// behind it, so over a saturated level background the wheel collapses into two hues
    /// (red over a blue background reads violet, green reads cyan -- "turquoise or purple").
    /// Normal blending puts the colour on top instead, so the whole wheel is visible; anyone
    /// who prefers the additive look can set "blending": "additive" in the rgb block.
    bool  additive   = false;
};

/// One mode's smoothing state: the colour currently on the sprites and the time it was last
/// eased, so a target colour is approached over "fade" seconds instead of jumping.
struct FadeState {
    ccColor3B color { 255, 255, 255 };
    float     time = 0.0f;
    bool      valid = false;
};

/// A named set of per-mode colours that can be switched at runtime with a hotkey. Only
/// colours are overridden: brightness/blending/pulse always come from the mode profile.
struct Preset {
    std::string name;
    std::array<std::optional<ccColor3B>, kModeCount> colors {};
};

// ---------------------------------------------------------------------------------------
// Small JSON helpers (matjson returns Result, so everything is optional-tolerant)
// ---------------------------------------------------------------------------------------

std::optional<std::string> jsonString(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asString()) return res.unwrap();
    return std::nullopt;
}

std::optional<bool> jsonBool(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asBool()) return res.unwrap();
    return std::nullopt;
}

std::optional<double> jsonNumber(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asDouble()) return res.unwrap();
    return std::nullopt;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Accepts "#RRGGBB", "RRGGBB", "#RGB" and "RGB".
std::optional<ccColor3B> parseColor(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);

    for (char c : text) {
        if (hexDigit(c) < 0) return std::nullopt;
    }

    // "#RRGGBB": each channel is two hex digits.
    if (text.size() == 6) {
        return ccColor3B {
            static_cast<GLubyte>(hexDigit(text[0]) * 16 + hexDigit(text[1])),
            static_cast<GLubyte>(hexDigit(text[2]) * 16 + hexDigit(text[3])),
            static_cast<GLubyte>(hexDigit(text[4]) * 16 + hexDigit(text[5])),
        };
    }
    // "#RGB": each digit is doubled (0xF -> 0xFF), the usual CSS shorthand.
    if (text.size() == 3) {
        return ccColor3B {
            static_cast<GLubyte>(hexDigit(text[0]) * 17),
            static_cast<GLubyte>(hexDigit(text[1]) * 17),
            static_cast<GLubyte>(hexDigit(text[2]) * 17),
        };
    }
    return std::nullopt;
}

std::string colorToString(ccColor3B color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

/// Accepts booleans as well as the strings "normal", "additive", "add", "blend", "blending".
std::optional<bool> parseBlending(matjson::Value const& obj) {
    if (!obj.contains("blending")) return std::nullopt;

    if (auto asBool = obj["blending"].asBool()) return asBool.unwrap();

    if (auto asStr = obj["blending"].asString()) {
        auto value = asStr.unwrap();
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto norm = lower(value);
        if (norm == "additive" || norm == "add" || norm == "blend" || norm == "blending") {
            return true;
        }
        if (norm == "normal" || norm == "alpha" || norm == "none") {
            return false;
        }
    }
    return std::nullopt;
}

/// Decimal or "0x" hex integer without throwing (used by parseKey).
std::optional<int> parseInt(std::string_view text);

/// Key names accepted in "hotkeys": F1..F24, a..z, 0..9, the names in kNamedKeys, or a raw
/// key code as a decimal / 0x hex number (which is what GD's own key list uses).
std::optional<cocos2d::enumKeyCodes> parseKey(std::string_view text) {
    std::string key;
    key.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (key.empty() || key == "none" || key == "off" || key == "disabled") {
        return KEY_None;
    }

    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return static_cast<cocos2d::enumKeyCodes>(KEY_A + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<cocos2d::enumKeyCodes>(KEY_Zero + (c - '0'));
    }

    if (key.size() >= 2 && key[0] == 'f') {
        if (auto number = parseInt(key.substr(1)); number && *number >= 1 && *number <= 24) {
            return static_cast<cocos2d::enumKeyCodes>(KEY_F1 + (*number - 1));
        }
    }

    static constexpr std::pair<std::string_view, cocos2d::enumKeyCodes> kNamedKeys[] = {
        { "space", KEY_Space }, { "enter", KEY_Enter }, { "return", KEY_Enter },
        { "tab", KEY_Tab }, { "escape", KEY_Escape }, { "esc", KEY_Escape },
        { "backspace", KEY_Backspace }, { "shift", KEY_Shift }, { "ctrl", KEY_Control },
        { "control", KEY_Control }, { "alt", KEY_Alt }, { "capslock", KEY_CapsLock },
        { "pause", KEY_Pause }, { "insert", KEY_Insert }, { "delete", KEY_Delete },
        { "del", KEY_Delete }, { "home", KEY_Home }, { "end", KEY_End },
        { "pageup", KEY_PageUp }, { "pagedown", KEY_PageDown },
        { "up", KEY_Up }, { "down", KEY_Down }, { "left", KEY_Left }, { "right", KEY_Right },
        { "numlock", KEY_Numlock }, { "scrolllock", KEY_ScrollLock },
    };
    for (auto const& [name, code] : kNamedKeys) {
        if (key == name) return code;
    }

    if (auto number = parseInt(key)) {
        if (*number > 0 && *number < 0x1000) {
            return static_cast<cocos2d::enumKeyCodes>(*number);
        }
    }
    return std::nullopt;
}

std::optional<int> parseInt(std::string_view text) {
    if (text.empty()) return std::nullopt;

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }

    int value = 0;
    for (char c : text) {
        int digit = (base == 16) ? hexDigit(c) : (c >= '0' && c <= '9' ? c - '0' : -1);
        if (digit < 0 || digit >= base) return std::nullopt;
        value = value * base + digit;
        if (value > 0x100000) return std::nullopt;
    }
    return value;
}

// ---------------------------------------------------------------------------------------
// Runtime colour tweaking (hue / brightness hotkeys)
// ---------------------------------------------------------------------------------------

struct Hsv { float h; float s; float v; };

Hsv rgbToHsv(ccColor3B color) {
    float r = color.r / 255.0f, g = color.g / 255.0f, b = color.b / 255.0f;
    float maxValue = std::max({ r, g, b });
    float minValue = std::min({ r, g, b });
    float delta = maxValue - minValue;

    Hsv hsv { 0.0f, maxValue > 0.0f ? delta / maxValue : 0.0f, maxValue };
    if (delta > 0.0f) {
        if (maxValue == r)      hsv.h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        else if (maxValue == g) hsv.h = 60.0f * ((b - r) / delta + 2.0f);
        else                    hsv.h = 60.0f * ((r - g) / delta + 4.0f);
    }
    if (hsv.h < 0.0f) hsv.h += 360.0f;
    return hsv;
}

ccColor3B hsvToRgb(Hsv hsv) {
    float c = hsv.v * hsv.s;
    float h = std::fmod(hsv.h, 360.0f) / 60.0f;
    if (h < 0.0f) h += 6.0f;
    float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float m = hsv.v - c;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (h < 1.0f) { r = c; g = x; }
    else if (h < 2.0f) { r = x; g = c; }
    else if (h < 3.0f) { g = c; b = x; }
    else if (h < 4.0f) { g = x; b = c; }
    else if (h < 5.0f) { r = x; b = c; }
    else               { r = c; b = x; }

    auto channel = [&](float value) {
        return static_cast<GLubyte>(std::lround(std::clamp(value + m, 0.0f, 1.0f) * 255.0f));
    };
    return ccColor3B { channel(r), channel(g), channel(b) };
}

// ---------------------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------------------

/// Seconds since the first call, from a monotonic clock. Deliberately not the game's timers:
/// the RGB phase and the colour fade must keep running across respawns and level restarts,
/// and must not be affected by pauses or by the game's own time scaling.
float monotonicSeconds() {
    static auto const epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - epoch).count();
}

ccColor3B lerpColor(ccColor3B from, ccColor3B to, float t) {
    auto mix = [t](GLubyte a, GLubyte b) {
        return static_cast<GLubyte>(std::lround(a + (b - a) * t));
    };
    return ccColor3B { mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b) };
}

class Manager {
public:
    static Manager& get() {
        static Manager instance;
        return instance;
    }

    /// Reads (and, on first run, creates) <config dir>/per_gamemode_glow.json
    void loadConfig() {
        // Mod::get()->getConfigDir() already creates the per-mod folder
        // (Mod::Impl::getConfigDir(create = true)). The explicit call is kept as
        // belt and braces: neither writeStringSafe nor readJson create parent
        // folders themselves, and a fresh install should never end up with the
        // write failing and the mod silently doing nothing.
        auto configDir = Mod::get()->getConfigDir();
        (void) utils::file::createDirectoryAll(configDir);

        m_configPath = configDir / "per_gamemode_glow.json";

        if (!fs::exists(m_configPath)) {
            writeDefaultConfig();
        }

        // Everything is reset first so a reload really is "the file is the truth":
        // profiles, presets and the runtime colour tweaks all go back to defaults.
        resetToDefaults();

        auto json = utils::file::readJson(m_configPath);
        if (!json) {
            log::error("Per-Gamemode Glow: cannot read {}: {}",
                utils::string::pathToString(m_configPath), json.unwrapErr());
            return;
        }

        auto const& root = json.unwrap();
        if (!root.isObject()) {
            log::error("Per-Gamemode Glow: {} is not a JSON object",
                utils::string::pathToString(m_configPath));
            return;
        }

        if (auto v = jsonBool(root, "debug")) m_debug = *v;
        if (auto v = jsonNumber(root, "fade")) {
            m_fadeSeconds = std::clamp(static_cast<float>(*v), 0.0f, 3.0f);
        }

        std::size_t found = 0;
        for (std::size_t i = 0; i < kModeCount; ++i) {
            auto key = std::string(kModeKeys[i]);
            if (!root.contains(key)) continue;

            auto const& entry = root[key];
            if (!entry.isObject()) continue;

            auto& profile = m_profiles[i];
            profile = Profile {};

            if (auto v = jsonBool(entry, "enabled"))     profile.enabled = *v;
            if (auto v = jsonNumber(entry, "brightness")) profile.brightness = static_cast<float>(*v);
            if (auto v = jsonNumber(entry, "pulse"))      profile.pulse = static_cast<float>(*v);
            if (auto v = parseBlending(entry))            profile.additive = *v;
            if (auto v = jsonString(entry, "color")) {
                if (auto color = parseColor(*v)) {
                    profile.color = *color;
                } else {
                    log::warn("Per-Gamemode Glow: '{}' has an invalid colour '{}' (expected #RRGGBB)",
                        key, *v);
                }
            }

            profile.brightness = std::clamp(profile.brightness, 0.0f, 2.0f);
            profile.pulse      = std::max(profile.pulse, 0.0f);
            ++found;
        }

        loadHotkeys(root);
        loadPresets(root);
        loadRgb(root);

        log::info("Per-Gamemode Glow: loaded {} mode profile(s), {} preset(s), RGB {} from {}",
            found, m_presets.size(), m_rgbOn ? "on" : "off",
            utils::string::pathToString(m_configPath));

        // Geometry Dash draws the robot's and the spider's glow itself, from the icon kit's glow
        // colour, and ignores the per-player custom glow those two modes would need -- verified
        // with the debug dump (MODLOG section 9). Say so once instead of letting an enabled
        // profile silently do nothing.
        for (auto index : { std::size_t(Mode::Robot), std::size_t(Mode::Spider) }) {
            if (!m_profiles[index].enabled) continue;
            log::warn("Per-Gamemode Glow: '{}' is enabled, but Geometry Dash draws this mode's glow "
                      "itself from the icon kit's glow colour and ignores the mod's colour",
                kModeKeys[index]);
        }

        // Remembered file state, so the watcher below can tell "changed on disk" from
        // "the file we just read".
        std::error_code ec;
        m_configWriteTime = fs::last_write_time(m_configPath, ec);
    }

    /// Applies the profile for the player's current gamemode to its glow sprites.
    /// Called every frame and on art changes, so it always wins over the game's own writes.
    void apply(PlayerObject* player, float elapsed, bool& customGlowOwned) {
        if (!player) return;

        // Hot reload: re-read the config when the file changes on disk, so colours can be
        // adjusted in any editor without restarting the game. Checked at most twice a
        // second, and only ever does anything if the timestamp actually moved.
        pollConfigFile();

        auto mode = modeOf(player);
        m_lastMode = mode;           // the hotkeys tweak "the mode the player is in"

        auto const& profile = m_profiles[static_cast<std::size_t>(mode)];

        // Every way this mode can mean "leave it vanilla", in one place, so that turning a
        // profile off really does hand the glow colour back to the game. RGB mode is an
        // override of every mode at once, so while it is on a disabled profile still glows.
        auto vanilla = !Mod::get()->getSettingValue<bool>("enabled")
            || (!m_rgbOn && !profile.enabled);
        if (!vanilla && Mod::get()->getSettingValue<bool>("respect-global-toggle")) {
            auto* gameManager = GameManager::get();
            vanilla = gameManager && !gameManager->getPlayerGlow();
        }
        if (vanilla) {
            releaseCustomGlow(player, customGlowOwned);
            return;
        }

        auto const index = static_cast<std::size_t>(mode);

        // RGB mode: the hue walks around the colour wheel as the player exists. Derived from
        // the time the player has been alive rather than accumulated per frame, so the two
        // apply() call sites (update and updatePlayerArt) always agree on the colour.
        ccColor3B color;
        float brightness;
        bool additive;
        if (m_rgbOn) {
            float turns = 0.0f;
            if (m_rgb.speed > 0.0f) {
                // Wall-clock based, not the player's own timer: a respawn or a level restart
                // used to send the wheel back to red, which read as an abrupt jump.
                turns = std::fmod(monotonicSeconds() * m_rgb.speed, 1.0f);
            }
            auto hue = turns * 360.0f + static_cast<float>(m_hueShift[index]);
            color = hsvToRgb(Hsv { hue, m_rgb.saturation, 1.0f });
            brightness = m_rgb.brightness * m_brightnessMul[index];
            additive = m_rgb.additive;
        } else {
            color = effectiveColor(index);
            brightness = profile.brightness * m_brightnessMul[index];
            if (profile.pulse > 0.0f) {
                constexpr float kTau = 6.28318530718f;
                brightness *= 0.5f + 0.5f * std::sin(kTau * elapsed / profile.pulse);
            }
            additive = profile.additive;
        }

        // Never snap to a new colour in a single frame: turning the RGB mode on or off, or
        // editing a colour in the config, otherwise jumps the glow from one colour to another.
        color = fadeColor(index, color);

        brightness = std::clamp(brightness, 0.0f, 1.0f);
        auto const alpha = static_cast<GLubyte>(std::lround(brightness * 255.0f));

        ccBlendFunc const blend = additive
            ? ccBlendFunc { GL_SRC_ALPHA, GL_ONE }
            : ccBlendFunc { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };

        logDebug(player, mode, index, color, alpha, additive);

        // The game's own per-player custom glow colour: this is the colour the game itself
        // tints glow with, and it is what makes the robot and the spider work at all -- both
        // hide m_iconGlow and m_vehicleGlow (debug log shows vis=0 for each) and draw their
        // glow as part of the limb animation, so tinting sprites never reached them.
        //
        // PlayerObject::enableCustomGlowColor() / disableCustomGlowColor() are "Out of line"
        // on Windows (inlined, no callable symbol), but they are no-ops beyond these two
        // field writes, which is exactly what the game's own glow update reads.
        player->m_glowColor = color;
        player->m_hasCustomGlowColor = true;
        customGlowOwned = true;

        for (auto* sprite : { player->m_iconGlow, player->m_vehicleGlow }) {
            if (!sprite) continue;
            sprite->setColor(color);
            sprite->setOpacity(alpha);
            sprite->setBlendFunc(blend);
        }

        applyPartGlow(player, mode, color, alpha, blend);
    }

    /// Handles one keyboard event. Never consumes the key: the caller always forwards it.
    ///
    /// One physical press must fire exactly once. Both Geode and the game itself can route the
    /// same key-down through dispatchKeyboardMSG, which fired every hotkey twice -- harmless for
    /// "add 15 degrees", but a toggle (the RGB mode) immediately undid itself and looked dead.
    void onKeyEvent(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (!m_hotkeysEnabled || key == KEY_None) return;

        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            if (m_hotkeys[i] != key) continue;

            if (!isKeyDown) {
                if (m_debug && m_hotkeyHeld[i]) {
                    log::info("Per-Gamemode Glow: [debug] '{}' key up", kHotkeyKeys[i]);
                }
                m_hotkeyHeld[i] = false;
                return;
            }

            if (m_debug) {
                log::info("Per-Gamemode Glow: [debug] '{}' key down (repeat={})",
                    kHotkeyKeys[i], isKeyRepeat ? 1 : 0);
            }

            // The OS auto-repeat of a key that is already down is not a new press.
            if (isKeyRepeat) return;

            auto now = std::chrono::steady_clock::now();
            if (m_hotkeyHeld[i] && now - m_hotkeyDownTime[i] < kHotkeyHeldTimeout) {
                if (m_debug) {
                    log::info("Per-Gamemode Glow: [debug] '{}' duplicate key-down ignored",
                        kHotkeyKeys[i]);
                }
                return;
            }

            m_hotkeyHeld[i] = true;
            m_hotkeyDownTime[i] = now;
            fireHotkey(i);
            return;
        }
    }

    void fireHotkey(std::size_t i) {
        switch (static_cast<Hotkey>(i)) {
            case Hotkey::Reload: {
                log::info("Per-Gamemode Glow: '{}' pressed - reloading {}",
                    kHotkeyKeys[i], utils::string::pathToString(m_configPath));
                loadConfig();
                break;
            }
            case Hotkey::PresetNext: cyclePreset(+1); break;
            case Hotkey::PresetPrev: cyclePreset(-1); break;
            case Hotkey::HueDown:    shiftHue(-kHueStep); break;
            case Hotkey::HueUp:      shiftHue(+kHueStep); break;
            case Hotkey::BrightnessDown: shiftBrightness(-kBrightnessStep); break;
            case Hotkey::BrightnessUp:   shiftBrightness(+kBrightnessStep); break;
            case Hotkey::RgbToggle:      toggleRgb(); break;
            case Hotkey::RgbSlower:      adjustRgbSpeed(-1); break;
            case Hotkey::RgbFaster:      adjustRgbSpeed(+1); break;
            case Hotkey::Count: break;
        }
    }

private:
    Manager() = default;

    static Mode modeOf(PlayerObject* player) {
        if (player->m_isShip)   return Mode::Ship;
        if (player->m_isBird)   return Mode::Ufo;
        if (player->m_isBall)   return Mode::Ball;
        if (player->m_isDart)   return Mode::Wave;
        if (player->m_isRobot)  return Mode::Robot;
        if (player->m_isSpider) return Mode::Spider;
        if (player->m_isSwing)  return Mode::Swing;
        return Mode::Cube;
    }

    /// The robot and the spider do not use m_iconGlow / m_vehicleGlow at all: the game hides both
    /// (debug log: "iconGlow was #0040FF a=76 vis=0 | vehicleGlow was #0040FF a=76 vis=0") and
    /// draws the outline the player sees with its own glow pass, which follows the icon kit's
    /// glow colour and ignores the per-player custom glow this mod sets. Their sprite
    /// (GJSpiderSprite derives from GJRobotSprite, so both share the fields) still has the two
    /// glow sprites below, so they are tinted anyway -- it costs nothing and would make the mode
    /// work on a build whose robot/spider glow does read the custom glow.
    static GJRobotSprite* partSpriteOf(PlayerObject* player, Mode mode) {
        if (mode == Mode::Robot) return player->m_robotSprite;
        if (mode == Mode::Spider) return player->m_spiderSprite;
        return nullptr;
    }

    /// GD names every glow frame "<icon>_<id>_glow_<frame>.png", so a glow part of a
    /// robot/spider is one whose frame name says so.
    static bool isGlowPart(cocos2d::CCNode* node) {
        auto* part = typeinfo_cast<CCSpritePart*>(node);
        return part && part->m_spriteFrameName.find("glow") != std::string::npos;
    }

    static void tintSprite(cocos2d::CCSprite* sprite, ccColor3B color, GLubyte alpha,
                           ccBlendFunc blend) {
        if (!sprite) return;
        sprite->setColor(color);
        sprite->setOpacity(alpha);
        sprite->setBlendFunc(blend);
    }

    /// Hands the glow colour back to the game, but only if we are the ones who took it over:
    /// a level's own custom glow colour must not be cleared by a mode this mod leaves alone.
    static void releaseCustomGlow(PlayerObject* player, bool& owned) {
        if (!owned) return;
        player->m_hasCustomGlowColor = false;
        owned = false;
    }

    static void applyPartGlow(PlayerObject* player, Mode mode, ccColor3B color,
                              GLubyte alpha, ccBlendFunc blend) {
        auto* part = partSpriteOf(player, mode);
        if (!part) return;

        tintSprite(part->m_glowSprite, color, alpha, blend);

        // The spider's eye pupil is m_extraSprite, and tinting it is the one thing that makes
        // this mod's colour visible on that icon at all (GD draws the spider's outline itself,
        // see MODLOG section 9). This started as a side effect of a failed fix for the outline
        // and was kept on request: a glowing eye at least shows the mode is doing something.
        // Spider only, on purpose: a robot's m_extraSprite is a different piece of the icon (the
        // game gates it with m_hasExtra) and recolouring icon art is not what this mod does.
        if (mode == Mode::Spider) {
            tintSprite(part->m_extraSprite, color, alpha, blend);
        }
        auto tintGlowParts = [color, alpha, blend](cocos2d::CCArray* array) {
            if (!array) return;
            for (auto* object : CCArrayExt<cocos2d::CCObject*>(array)) {
                auto* node = typeinfo_cast<cocos2d::CCNode*>(object);
                if (isGlowPart(node)) tintSprite(typeinfo_cast<cocos2d::CCSprite*>(node), color, alpha, blend);
            }
        };
        tintGlowParts(part->m_unkArray);
        tintGlowParts(part->m_secondArray);
        if (part->m_paSprite) tintGlowParts(part->m_paSprite->m_spriteParts);
    }

    /// "<frame>:<colour> a=<alpha> vis=<visible>" for one node. Frame names come only from
    /// CCSpritePart's own field: CCSprite::displayFrame() / CCSpriteFrame::getFrameName()
    /// dispatch through a vtable that does not match the vendored cocos2d headers, and the
    /// resulting std::string crashed the game with std::length_error.
    static std::string describeNode(cocos2d::CCNode* node) {
        if (!node) return "null";

        auto* sprite = typeinfo_cast<cocos2d::CCSprite*>(node);
        if (!sprite) {
            char other[32];
            std::snprintf(other, sizeof(other), "node tag=%d", node->getTag());
            return std::string(other);
        }

        std::string name = "-";
        if (auto* part = typeinfo_cast<CCSpritePart*>(sprite)) name = part->m_spriteFrameName;

        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s:%s a=%d vis=%d", name.c_str(),
            colorToString(sprite->getColor()).c_str(), sprite->getOpacity(),
            sprite->isVisible() ? 1 : 0);
        return std::string(buffer);
    }

    /// " <prefix>0=<describeNode> <prefix>1=..." for up to `limit` children of a node. The child
    /// list is how a sprite that is not one of the robot's own fields can still be found.
    static std::string describeChildren(cocos2d::CCNode* node, std::string_view prefix, int limit) {
        std::string out;
        if (!node) return out;

        auto* children = node->getChildren();
        if (!children) return out;

        int index = 0;
        for (auto* child : CCArrayExt<cocos2d::CCNode*>(children)) {
            if (index >= limit) { break; }
            char label[32];
            std::snprintf(label, sizeof(label), "%.8s%d", prefix.data(), index++);
            out += ' ';
            out += label;
            out += '=';
            out += describeNode(child);
        }
        return out;
    }

    /// Debug-only: what a robot/spider sprite is made of at the moment we look at it. This exists
    /// because GD does not draw either of those two modes' own glow sprites: the dump below is
    /// what proved that the outline the player sees is drawn by the game itself -- every visible
    /// sprite in the tree carries the icon's own colours, never the glow colour, and both glow
    /// sprites end up holding our colour without being drawn from it (MODLOG section 9).
    /// Everything here sits in a try/catch on purpose: a debug dump must never take the game
    /// down (an earlier version called CCSprite::displayFrame() and crashed with length_error).
    static std::string describeRobot(GJRobotSprite* robot) {
        if (!robot) return "-";

        try {
            std::string out;
            auto append = [&out](std::string_view tag, cocos2d::CCNode* node) {
                out += ' ';
                out += tag;
                out += '=';
                out += describeNode(node);
            };
            auto appendArray = [&append](std::string_view tag, cocos2d::CCArray* array) {
                if (!array) return;
                int index = 0;
                for (auto* object : CCArrayExt<cocos2d::CCObject*>(array)) {
                    if (index >= 8) { break; }
                    char label[24];
                    std::snprintf(label, sizeof(label), "%.8s%d", tag.data(), index++);
                    append(label, typeinfo_cast<cocos2d::CCNode*>(object));
                }
            };

            char own[80];
            std::snprintf(own, sizeof(own), " ownColor=%s/%s extra=%d",
                colorToString(robot->m_color).c_str(), colorToString(robot->m_secondColor).c_str(),
                robot->m_hasExtra ? 1 : 0);
            out += own;

            append("glow", robot->m_glowSprite);
            append("extra", robot->m_extraSprite);
            append("base", robot->m_sprite);
            append("fbf", robot->m_fbfSprite);
            append("head", robot->m_headSprite);
            append("foot", robot->m_footSprite);
            appendArray("unk", robot->m_unkArray);
            appendArray("second", robot->m_secondArray);
            appendArray("part", robot->m_paSprite ? robot->m_paSprite->m_spriteParts : nullptr);
            out += describeChildren(robot, "child", 4);

            return out;
        } catch (std::exception const& e) {
            return std::string("dump failed: ") + e.what();
        } catch (...) {
            return "dump failed";
        }
    }

    void resetToDefaults() {
        m_profiles = {};
        m_rgb = RgbSettings {};
        m_rgbOn = false;
        m_fade = {};
        m_fadeSeconds = kDefaultFadeSeconds;
        m_presets.clear();
        m_activePreset = -1;
        m_debug = false;
        m_hotkeysEnabled = true;
        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            m_hotkeys[i] = kDefaultHotkeys[i];
        }
        m_hueShift = {};
        for (auto& value : m_brightnessMul) value = 1.0f;
    }

    void loadHotkeys(matjson::Value const& root) {
        if (!root.contains("hotkeys")) return;
        auto const& hotkeys = root["hotkeys"];
        if (!hotkeys.isObject()) {
            log::warn("Per-Gamemode Glow: 'hotkeys' is not an object, keeping the defaults");
            return;
        }

        if (auto v = jsonBool(hotkeys, "enabled")) m_hotkeysEnabled = *v;

        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            auto name = std::string(kHotkeyKeys[i]);
            if (!hotkeys.contains(name)) continue;

            auto value = jsonString(hotkeys, name);
            if (!value) {
                log::warn("Per-Gamemode Glow: hotkey '{}' must be a string, keeping the default", name);
                continue;
            }
            if (auto key = parseKey(*value)) {
                m_hotkeys[i] = *key;
            } else {
                log::warn("Per-Gamemode Glow: hotkey '{}' has an unknown key '{}' (keeping the default)",
                    name, *value);
            }
        }
    }

    void loadPresets(matjson::Value const& root) {
        auto activeName = jsonString(root, "preset");

        if (root.contains("presets")) {
            auto const& presets = root["presets"];
            if (!presets.isArray()) {
                log::warn("Per-Gamemode Glow: 'presets' must be an array of "
                          "{{ \"name\": ..., \"colors\": {{ mode: \"#RRGGBB\" }} }}");
            } else {
                for (auto const& entry : presets) {
                    if (!entry.isObject()) continue;

                    Preset preset;
                    preset.name = jsonString(entry, "name").value_or("preset");

                    if (auto const& colors = entry.contains("colors") ? entry["colors"] : entry;
                        colors.isObject()) {
                        for (std::size_t i = 0; i < kModeCount; ++i) {
                            auto key = std::string(kModeKeys[i]);
                            if (!colors.contains(key)) continue;

                            if (auto value = jsonString(colors, key)) {
                                if (auto color = parseColor(*value)) {
                                    preset.colors[i] = *color;
                                } else {
                                    log::warn("Per-Gamemode Glow: preset '{}' has an invalid colour '{}' for '{}'",
                                        preset.name, *value, key);
                                }
                            }
                        }
                    }
                    m_presets.push_back(std::move(preset));
                }
            }
        }

        // "preset": "name" selects one right away (and survives a restart); "" or a missing
        // field means "use the per-mode colours from this file".
        if (activeName && !activeName->empty()) {
            m_activePreset = findPreset(*activeName);
            if (m_activePreset < 0) {
                log::warn("Per-Gamemode Glow: 'preset' names an unknown preset '{}' (using the per-mode colours)",
                    *activeName);
            }
        }
    }

    int findPreset(std::string_view name) const {
        for (std::size_t i = 0; i < m_presets.size(); ++i) {
            if (m_presets[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    /// Eases the glow towards a target colour with an exponential approach, so no colour change
    /// is ever a single-frame snap. "fade" in the config is the time constant in seconds; 0
    /// applies the colour immediately. Both call sites in a frame share the state, so the second
    /// one (with no time passed) simply keeps the colour the first one faded to.
    ccColor3B fadeColor(std::size_t index, ccColor3B target) {
        auto now = monotonicSeconds();
        auto& state = m_fade[index];

        if (m_fadeSeconds <= 0.0f) {
            state = FadeState { target, now, true };
            return target;
        }

        if (state.valid) {
            float dt = std::max(now - state.time, 0.0f);
            if (dt > 0.0f) {
                float t = std::clamp(1.0f - std::exp(-dt / m_fadeSeconds), 0.0f, 1.0f);
                state.color = lerpColor(state.color, target, t);
            }
        } else {
            state.color = target;
        }
        state.time = now;
        state.valid = true;
        return state.color;
    }

    ccColor3B effectiveColor(std::size_t index) const {
        auto color = m_profiles[index].color;

        if (m_activePreset >= 0 && m_activePreset < static_cast<int>(m_presets.size())) {
            if (auto const& presetColor = m_presets[m_activePreset].colors[index]) {
                color = *presetColor;
            }
        }
        if (m_hueShift[index] != 0) {
            auto hsv = rgbToHsv(color);
            hsv.h += static_cast<float>(m_hueShift[index]);
            color = hsvToRgb(hsv);
        }
        return color;
    }

    void cyclePreset(int step) {
        if (m_presets.empty()) {
            log::info("Per-Gamemode Glow: no presets defined in the config");
            return;
        }

        // -1 means "no preset" (per-mode colours), so cycling starts from the first one.
        auto count = static_cast<int>(m_presets.size());
        m_activePreset = m_activePreset < 0
            ? (step > 0 ? 0 : count - 1)
            : (m_activePreset + step + count) % count;

        auto const& preset = m_presets[m_activePreset];
        log::info("Per-Gamemode Glow: preset {}/{} -> '{}'",
            m_activePreset + 1, count, preset.name);
    }

    void shiftHue(float amount) {
        if (m_lastMode == Mode::Count) {
            log::info("Per-Gamemode Glow: hue keys work while playing (no gamemode seen yet)");
            return;
        }
        auto index = static_cast<std::size_t>(m_lastMode);

        auto shift = m_hueShift[index] + static_cast<int>(amount);
        shift = ((shift % 360) + 360) % 360;
        if (shift > 180) shift -= 360;   // keep it in (-180, 180]
        m_hueShift[index] = shift;

        log::info("Per-Gamemode Glow: '{}' hue {:+d} deg -> {}",
            kModeKeys[index], m_hueShift[index], colorToString(effectiveColor(index)));
    }

    /// Reads the "rgb" block. Absent or malformed values keep their defaults, so a config
    /// written before this block existed behaves exactly as it did.
    void loadRgb(matjson::Value const& root) {
        m_rgb = RgbSettings {};
        m_rgbOn = false;

        if (!root.contains("rgb")) return;

        auto const& node = root["rgb"];
        if (!node.isObject()) {
            log::warn("Per-Gamemode Glow: 'rgb' must be an object like "
                      "{{ \"enabled\": false, \"speed\": 0.25 }} (keeping the defaults)");
            return;
        }

        if (auto v = jsonBool(node, "enabled"))      m_rgb.enabled = *v;
        if (auto v = jsonNumber(node, "speed"))      m_rgb.speed = std::clamp(static_cast<float>(*v), 0.0f, kRgbMaxSpeed);
        if (auto v = jsonNumber(node, "saturation")) m_rgb.saturation = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
        if (auto v = jsonNumber(node, "brightness")) m_rgb.brightness = std::clamp(static_cast<float>(*v), 0.0f, 2.0f);
        if (auto v = parseBlending(node))            m_rgb.additive = *v;

        // The hotkey flips m_rgbOn at runtime only; a reload (and F5) puts it back to whatever
        // the file says, exactly like the hue and brightness tweaks.
        m_rgbOn = m_rgb.enabled;
    }

    void toggleRgb() {
        m_rgbOn = !m_rgbOn;
        // Runtime only: the file is the truth, so a reload (or F5) puts it back.
        log::info("Per-Gamemode Glow: RGB mode {} (speed {:.2f} turns/s, {} blending, "
                  "overrides every mode)",
            m_rgbOn ? "on" : "off", m_rgb.speed, m_rgb.additive ? "additive" : "normal");
    }

    /// The RGB wheel speed is the one value that really has to be tuned by eye, so it gets a
    /// pair of hotkeys that walk a ladder of round values. Runtime only, like the hue and
    /// brightness tweaks; the log line prints the value ready to paste into the config.
    void adjustRgbSpeed(int step) {
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < kRgbSpeedLadderSize; ++i) {
            if (std::abs(kRgbSpeedLadder[i] - m_rgb.speed)
                < std::abs(kRgbSpeedLadder[nearest] - m_rgb.speed)) {
                nearest = i;
            }
        }

        auto next = static_cast<int>(nearest) + step;
        next = std::clamp(next, 0, static_cast<int>(kRgbSpeedLadderSize) - 1);
        m_rgb.speed = kRgbSpeedLadder[next];

        auto secondsPerTurn = m_rgb.speed > 0.0f ? 1.0f / m_rgb.speed : 0.0f;
        log::info("Per-Gamemode Glow: RGB speed {:.4g} turns/s = one full colour wheel in "
                  "{:.0f} s - put \"speed\": {:.4g} in the \"rgb\" block to keep it",
            m_rgb.speed, secondsPerTurn, m_rgb.speed);
    }

    void shiftBrightness(float amount) {
        if (m_lastMode == Mode::Count) {
            log::info("Per-Gamemode Glow: brightness keys work while playing (no gamemode seen yet)");
            return;
        }
        auto index = static_cast<std::size_t>(m_lastMode);

        m_brightnessMul[index] = std::clamp(m_brightnessMul[index] + amount, 0.0f, 2.0f);

        log::info("Per-Gamemode Glow: '{}' brightness x{:.2f} (file value {:.2f}) -> effective {:.2f}",
            kModeKeys[index], m_brightnessMul[index], m_profiles[index].brightness,
            std::clamp(m_profiles[index].brightness * m_brightnessMul[index], 0.0f, 1.0f));
    }

    void pollConfigFile() {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastReloadCheck < kAutoReloadDelay) return;
        m_lastReloadCheck = now;

        std::error_code ec;
        auto writeTime = fs::last_write_time(m_configPath, ec);
        if (ec || writeTime == m_configWriteTime) return;

        log::info("Per-Gamemode Glow: {} changed on disk - reloading",
            utils::string::pathToString(m_configPath));
        loadConfig();
    }

    /// With "debug": true, logs what is applied and what the game had put in the sprite
    /// before, which is how the robot/spider glow question can be answered from the log.
    void logDebug(PlayerObject* player, Mode mode, std::size_t index, ccColor3B color,
                  GLubyte alpha, bool additive) {
        if (!m_debug) return;

        auto now = std::chrono::steady_clock::now();
        if (m_lastDebugMode == mode && now - m_lastDebugLog < kDebugLogDelay) return;
        m_lastDebugMode = mode;
        m_lastDebugLog = now;

        // The game writes the glow colour itself, so logging the sprites' colour right
        // before we overwrite them shows what the game had decided for this mode (and, on
        // the next line, whether anything wrote over us in between). Both glow sprites are
        // logged: in the vehicle modes (ship/ufo/wave/robot/spider/swing) the game hides
        // m_iconGlow and the visible glow is m_vehicleGlow, and vice versa for the cube.
        auto describe = [](cocos2d::CCSprite* sprite) {
            if (!sprite) return std::string("-");
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s a=%d vis=%d",
                colorToString(sprite->getColor()).c_str(), sprite->getOpacity(),
                sprite->isVisible() ? 1 : 0);
            return std::string(buffer);
        };

        auto const iconWas = player ? describe(player->m_iconGlow) : std::string("n/a");
        auto const vehicleWas = player ? describe(player->m_vehicleGlow) : std::string("n/a");
        auto const partWas = player ? describeRobot(partSpriteOf(player, mode)) : std::string("n/a");
        auto const customWas = player && player->m_hasCustomGlowColor
            ? colorToString(player->m_glowColor) : std::string("off");

        log::info("Per-Gamemode Glow: [debug] {} color={} alpha={} blend={} preset={} rgb={} "
                  "hue={:+d} brightness*{:.2f} | customGlow was {} | iconGlow was {} | "
                  "vehicleGlow was {} | robot:{}",
            kModeKeys[index], colorToString(color), alpha, additive ? "additive" : "normal",
            m_activePreset < 0 ? "none" : m_presets[m_activePreset].name,
            m_rgbOn ? "on" : "off",
            m_hueShift[index], m_brightnessMul[index], customWas, iconWas, vehicleWas, partWas);
    }

    void writeDefaultConfig() const {
        auto root = matjson::Value::object();

        root.set("debug", false);
        root.set("fade", 0.4);
        root.set("preset", std::string(""));

        auto hotkeys = matjson::Value::object();
        hotkeys.set("enabled", true);
        hotkeys.set("reload", std::string("F5"));
        hotkeys.set("preset-next", std::string("F6"));
        hotkeys.set("preset-prev", std::string("F7"));
        hotkeys.set("hue-down", std::string("F8"));
        hotkeys.set("hue-up", std::string("F9"));
        hotkeys.set("brightness-down", std::string("F10"));
        hotkeys.set("brightness-up", std::string("F11"));
        hotkeys.set("rgb-toggle", std::string("F12"));
        hotkeys.set("rgb-slower", std::string("PageUp"));
        hotkeys.set("rgb-faster", std::string("PageDown"));
        root.set("hotkeys", std::move(hotkeys));

        // Empty by default; the README shows what a preset looks like.
        root.set("presets", matjson::Value::array());

        auto rgb = matjson::Value::object();
        rgb.set("enabled", false);
        rgb.set("speed", 0.02);
        rgb.set("saturation", 1.0);
        rgb.set("brightness", 1.0);
        rgb.set("blending", std::string("normal"));
        root.set("rgb", std::move(rgb));

        for (std::size_t i = 0; i < kModeCount; ++i) {
            auto entry = matjson::Value::object();
            entry.set("enabled", false);
            entry.set("color", std::string("#FFFFFF"));
            entry.set("brightness", 1.0);
            entry.set("blending", std::string("normal"));
            entry.set("pulse", 0.0);
            root.set(std::string(kModeKeys[i]), std::move(entry));
        }

        auto written = utils::file::writeStringSafe(m_configPath, root.dump(4));
        if (written) {
            log::info("Per-Gamemode Glow: wrote default config to {}",
                utils::string::pathToString(m_configPath));
        } else {
            log::error("Per-Gamemode Glow: could not write default config: {}", written.unwrapErr());
        }
    }

    std::array<Profile, kModeCount> m_profiles {};
    std::vector<Preset> m_presets;
    int m_activePreset = -1;

    std::array<cocos2d::enumKeyCodes, kHotkeyCount> m_hotkeys = [] {
        std::array<cocos2d::enumKeyCodes, kHotkeyCount> keys {};
        for (std::size_t i = 0; i < kHotkeyCount; ++i) keys[i] = kDefaultHotkeys[i];
        return keys;
    }();
    bool m_hotkeysEnabled = true;
    /// Which hotkey keys are currently held down, and since when: used to drop a duplicate
    /// key-down for one press and to recover if a key-up never arrives.
    std::array<bool, kHotkeyCount> m_hotkeyHeld {};
    std::array<std::chrono::steady_clock::time_point, kHotkeyCount> m_hotkeyDownTime {};

    std::array<int, kModeCount>   m_hueShift {};
    std::array<float, kModeCount> m_brightnessMul = [] {
        std::array<float, kModeCount> values {};
        for (auto& value : values) value = 1.0f;
        return values;
    }();

    Mode m_lastMode = Mode::Count;
    bool m_debug = false;

    RgbSettings m_rgb;
    /// Runtime state of the RGB mode: starts at whatever the config says and is flipped by the
    /// rgb-toggle hotkey (never written back to the file).
    bool m_rgbOn = false;

    /// Colour smoothing state (one per mode) and its time constant in seconds.
    std::array<FadeState, kModeCount> m_fade {};
    float m_fadeSeconds = kDefaultFadeSeconds;

    fs::path m_configPath;
    fs::file_time_type m_configWriteTime {};
    std::chrono::steady_clock::time_point m_lastReloadCheck {};
    std::chrono::steady_clock::time_point m_lastDebugLog {};
    Mode m_lastDebugMode = Mode::Count;
};

} // namespace pgg

// ---------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------

class $modify(PGGPlayerObject, PlayerObject) {
    struct Fields {
        float elapsed = 0.0f;
        // Did we set this player's custom glow colour? Kept per PlayerObject, so a second
        // player in a dual level (and every other PlayerObject) has its own state, and so a
        // profile turning off only clears the glow this mod itself took over.
        bool customGlowOwned = false;
    };

    void update(float dt) {
        PlayerObject::update(dt);

        m_fields->elapsed += dt;
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);
    }

    void updatePlayerArt() {
        // Our colour goes in *before* the original: the game's own glow update runs inside it
        // and reads m_glowColor / m_hasCustomGlowColor, and that is the one place the robot
        // and spider limb glow gets its colour from. Re-applied after it too, so the sprites
        // are left with our values whatever the original did to them.
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);

        PlayerObject::updatePlayerArt();

        // Re-applied immediately so a gamemode change never shows one frame of stale glow.
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);
    }
};

/// Read-only keyboard hook for the config hotkeys. Key events are only observed; the
/// original implementation is always called, with the original result returned, so input
/// (including text fields) keeps working exactly as before.
class $modify(PGGKeyboardDispatcher, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        pgg::Manager::get().onKeyEvent(key, isKeyDown, isKeyRepeat);
        return cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

$on_mod(Loaded) {
    pgg::Manager::get().loadConfig();
}
