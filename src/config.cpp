// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "config.h"

#include <cctype>
#include <cstdlib>

#include "cameraunlock/config/ini_reader.h"
#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/math/finite_utils.h"

namespace pdht {

// The old value is deliberately NOT migrated into the new keys. The single
// smoothing value carried a hidden 0.15 floor, so the number in an existing
// config does not mean what it used to: copying it across would hand a local
// user smoothing they never chose under the new semantics, and copying it into
// only one of the two keys would be a guess about which connection they were on.
static void WarnRetiredSmoothingKey(const cameraunlock::IniReader& reader,
                                    const char* section, const char* key) {
    if (reader.ReadString(section, key, "").empty()) return;
    cameraunlock::logging::Line(
        "Config key [%s] %s has been retired and is IGNORED. Smoothing is now two "
        "keys: LocalSmoothing (default 0, applies to a tracker on this machine) and "
        "RemoteSmoothing (default 0.15, applies to a tracker on the network). The "
        "old value is not migrated because the semantics changed - it carried a "
        "hidden 0.15 floor that no longer exists. Set the two new keys.",
        section, key);
}

// The raw text of a key, with any trailing comment and surrounding whitespace
// removed. Empty means the key is absent or holds nothing.
//
// GetPrivateProfileStringA does NOT strip inline comments - everything after '='
// is the value - so every reader below has to do it. Doing it once here is also
// what lets the bool and key readers accept `Enabled=0 ; disabled for now`,
// which the core's ReadBool silently discards because it compares the whole
// string against its whitelist.
static std::string ReadRaw(const cameraunlock::IniReader& ini, const char* section,
                           const char* key) {
    std::string text = ini.ReadString(section, key, "");
    const std::size_t comment = text.find_first_of(";#");
    if (comment != std::string::npos) text.erase(comment);
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// The INI is the one place a user hands this mod raw numbers, so every value is
// range-checked here at the boundary rather than anywhere downstream. Three
// distinct hazards, all reachable from a single typo:
//
//   - strtod accepts "nan" and "inf", and overflows a literal like 1e400 to
//     +inf. A non-finite sensitivity multiplies straight through the pose into
//     RotateAboutAxis, and InjectHeadPose then writes a NaN basis and a NaN view
//     origin into the renderer every frame. Its orthonormality guard only checks
//     the CLEAN basis it read back from the engine, so nothing downstream
//     catches that and the log says nothing at all.
//   - A negative position limit inverts the clamp bounds in PositionProcessor
//     (Clamp(v, -limit, limit) with limit < 0 returns the lower bound for every
//     input), pinning the lean at a fixed offset instead of freeing it.
//   - strtod parses a PREFIX. `LocalSmoothing=0,15` - a European decimal comma,
//     and IniReader deliberately pins the C locale so this is the expected user
//     error - yields 0.0, which is inside the valid range and so passed every
//     check silently. Likewise `YawSensitivity=one point five` yields the
//     default, which equals the default, so a "corrected to" test never fired.
//     Requiring the whole token to parse is what catches both.
//
// math::SanitizeFinite is the core's helper for the first two: non-finite falls
// back to the default, then the value is clamped. Anything that had to be
// corrected says so, because a silently ignored setting is what sends a user
// looking for a mod fault.
static float ReadFloatChecked(const cameraunlock::IniReader& ini, const char* section,
                              const char* key, float defaultValue, float lo, float hi) {
    const std::string raw = ReadRaw(ini, section, key);
    if (raw.empty()) return defaultValue;

    char* end = nullptr;
    const double parsed = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str() || *end != '\0') {
        cameraunlock::logging::Line(
            "Config key [%s] %s=%s is not a number, so the default %g is used instead. "
            "Use a dot for the decimal point.",
            section, key, raw.c_str(), static_cast<double>(defaultValue));
        return defaultValue;
    }

    const float asked = static_cast<float>(parsed);
    const float value = cameraunlock::math::SanitizeFinite(asked, defaultValue, lo, hi);
    if (value != asked) {
        cameraunlock::logging::Line(
            "Config key [%s] %s=%g is outside %g..%g and has been corrected to %g.",
            section, key, static_cast<double>(asked), static_cast<double>(lo),
            static_cast<double>(hi), static_cast<double>(value));
    }
    return value;
}

// The core's ReadBool compares the WHOLE value against its whitelist and returns
// the default on anything else, with no diagnostic - so `Enabled=off ; disabled
// for now` leaves positional tracking fully ON and nothing says so. This reads
// the trimmed text and reports what it could not use.
static bool ReadBoolChecked(const cameraunlock::IniReader& ini, const char* section,
                            const char* key, bool defaultValue) {
    std::string raw = ReadRaw(ini, section, key);
    if (raw.empty()) return defaultValue;
    for (char& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;

    cameraunlock::logging::Line(
        "Config key [%s] %s=%s is not a yes/no value (use 1 or 0), so the default %d "
        "is used instead.",
        section, key, raw.c_str(), defaultValue ? 1 : 0);
    return defaultValue;
}

// GetPrivateProfileIntA yields 0 - not the default - on a present-but-
// unparseable value, so an empty, quoted or garbage Port line all read as port
// 0, the receiver binds an ephemeral port, and no tracker packet can ever arrive
// while every startup line still reads as healthy. Parsing the raw text here
// makes the difference between "not a number" and "out of range" reportable.
static int ReadIntChecked(const cameraunlock::IniReader& ini, const char* section,
                          const char* key, int defaultValue, int lo, int hi,
                          const char* unit) {
    const std::string raw = ReadRaw(ini, section, key);
    if (raw.empty()) return defaultValue;

    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        cameraunlock::logging::Line(
            "Config key [%s] %s=%s is not a whole number, so the default %d is used "
            "instead.",
            section, key, raw.c_str(), defaultValue);
        return defaultValue;
    }
    if (parsed < lo || parsed > hi) {
        cameraunlock::logging::Line(
            "Config key [%s] %s=%ld is not a usable %s (%d..%d), so the default %d is "
            "used instead.",
            section, key, parsed, unit, lo, hi, defaultValue);
        return defaultValue;
    }
    return static_cast<int>(parsed);
}

// Documented ranges, from the CameraUnlock configuration defaults.
static constexpr float kMinRotationSensitivity = 0.1f;
static constexpr float kMaxRotationSensitivity = 3.0f;
static constexpr float kMinPositionSensitivity = 0.0f;
static constexpr float kMaxPositionSensitivity = 5.0f;
static constexpr float kMinPositionLimit = 0.01f;
static constexpr float kMaxPositionLimit = 0.5f;
static constexpr float kMinSmoothing = 0.0f;
static constexpr float kMaxSmoothing = 1.0f;
// Past this the projection stops being usable rather than just wide, so the FOV
// range is narrower than taste alone would ask for.
static constexpr float kMinFovOffset = -40.0f;
static constexpr float kMaxFovOffset = 60.0f;
// Below 1024 needs privilege on some configurations and collides with well-known
// services; 0 is the "pick any free port" wildcard, which is never what a tracker
// aimed at a fixed port wants.
static constexpr int kMinPort = 1024;
static constexpr int kMaxPort = 65535;
// Windows virtual key codes. 0x00 is "no key" and is also the hotkey poller's
// unset sentinel; 0xFF is reserved.
static constexpr int kMinVirtualKey = 0x01;
static constexpr int kMaxVirtualKey = 0xFE;

// The core's ReadHex runs strtol over the raw text and takes whatever prefix
// parses, which accepts values that are not key codes at all: `KeyToggle=End`
// reads 'E', stops at 'n', and yields 0x0E - an unassigned VK that silently
// never fires. Requiring the whole token to be hex catches that.
//
// What it CANNOT catch is `KeyCycleMode=F1` from a user who means the F1 key:
// that is valid hex (0xF1, VK_OEM_FJ_LOYA) and in range. The hotkey line the mod
// logs at startup names the key it actually bound, which is where that shows up.
static int ReadKeyChecked(const cameraunlock::IniReader& ini, const char* key,
                          int defaultValue) {
    const std::string raw = ReadRaw(ini, "Controls", key);
    if (raw.empty()) return defaultValue;

    const bool prefixed = raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X');
    const char* digits = prefixed ? raw.c_str() + 2 : raw.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(digits, &end, 16);
    if (end == digits || *end != '\0') {
        cameraunlock::logging::Line(
            "Config key [Controls] %s=%s is not a hex virtual key code, so the default "
            "0x%X is kept. Codes look like 0x23 (End) or 0x70 (F1).",
            key, raw.c_str(), defaultValue);
        return defaultValue;
    }
    if (parsed < kMinVirtualKey || parsed > kMaxVirtualKey) {
        cameraunlock::logging::Line(
            "Config key [Controls] %s=0x%lX is not a Windows virtual key code "
            "(0x%02X..0x%02X), so the default 0x%X is kept.",
            key, parsed, kMinVirtualKey, kMaxVirtualKey, defaultValue);
        return defaultValue;
    }
    return static_cast<int>(parsed);
}

void Config::Load(const std::string& iniPath) {
    cameraunlock::IniReader ini;
    if (!ini.Open(iniPath)) {
        // Say so. Running on defaults because the file is genuinely absent is
        // correct; running on defaults because the path could not be opened is a
        // fault, and the two produced an identical "Config: port=4242 ..." line.
        // That made every "my config changes do nothing" report undiagnosable
        // from the log, which is the only artifact users send.
        cameraunlock::logging::Line(
            "Config file %s could not be opened - running on defaults. If you did edit "
            "it, check it is next to the game exe and readable.",
            iniPath.c_str());
        return;
    }
    cameraunlock::logging::Line("Config loaded from %s", iniPath.c_str());

    port = static_cast<uint16_t>(
        ReadIntChecked(ini, "Network", "Port", port, kMinPort, kMaxPort, "UDP port"));

    enableOnStartup = ReadBoolChecked(ini, "Tracking", "EnableOnStartup", enableOnStartup);
    yawSensitivity = ReadFloatChecked(ini, "Tracking", "YawSensitivity", yawSensitivity,
                                      kMinRotationSensitivity, kMaxRotationSensitivity);
    pitchSensitivity = ReadFloatChecked(ini, "Tracking", "PitchSensitivity", pitchSensitivity,
                                        kMinRotationSensitivity, kMaxRotationSensitivity);
    rollSensitivity = ReadFloatChecked(ini, "Tracking", "RollSensitivity", rollSensitivity,
                                       kMinRotationSensitivity, kMaxRotationSensitivity);
    invertYaw = ReadBoolChecked(ini, "Tracking", "InvertYaw", invertYaw);
    invertPitch = ReadBoolChecked(ini, "Tracking", "InvertPitch", invertPitch);
    invertRoll = ReadBoolChecked(ini, "Tracking", "InvertRoll", invertRoll);
    worldSpaceYaw = ReadBoolChecked(ini, "Tracking", "WorldSpaceYaw", worldSpaceYaw);
    localSmoothing = ReadFloatChecked(ini, "Tracking", "LocalSmoothing", localSmoothing,
                                      kMinSmoothing, kMaxSmoothing);
    remoteSmoothing = ReadFloatChecked(ini, "Tracking", "RemoteSmoothing", remoteSmoothing,
                                       kMinSmoothing, kMaxSmoothing);
    WarnRetiredSmoothingKey(ini, "Tracking", "Smoothing");
    WarnRetiredSmoothingKey(ini, "Position", "Smoothing");

    positionEnabled = ReadBoolChecked(ini, "Position", "Enabled", positionEnabled);
    positionSensitivityX =
        ReadFloatChecked(ini, "Position", "SensitivityX", positionSensitivityX,
                         kMinPositionSensitivity, kMaxPositionSensitivity);
    positionSensitivityY =
        ReadFloatChecked(ini, "Position", "SensitivityY", positionSensitivityY,
                         kMinPositionSensitivity, kMaxPositionSensitivity);
    positionSensitivityZ =
        ReadFloatChecked(ini, "Position", "SensitivityZ", positionSensitivityZ,
                         kMinPositionSensitivity, kMaxPositionSensitivity);
    limitX = ReadFloatChecked(ini, "Position", "LimitX", limitX, kMinPositionLimit,
                              kMaxPositionLimit);
    limitY = ReadFloatChecked(ini, "Position", "LimitY", limitY, kMinPositionLimit,
                              kMaxPositionLimit);
    // Defaults to whatever LimitY ended up as, so the common case stays symmetric
    // without the user having to write the same number twice.
    limitYDown = limitY;
    limitYDown = ReadFloatChecked(ini, "Position", "LimitYDown", limitYDown,
                                  kMinPositionLimit, kMaxPositionLimit);
    limitZ = ReadFloatChecked(ini, "Position", "LimitZ", limitZ, kMinPositionLimit,
                              kMaxPositionLimit);
    limitZBack = ReadFloatChecked(ini, "Position", "LimitZBack", limitZBack,
                                  kMinPositionLimit, kMaxPositionLimit);

    fovOffsetDegrees = ReadFloatChecked(ini, "Camera", "FovOffset", fovOffsetDegrees,
                                        kMinFovOffset, kMaxFovOffset);

    keyToggle = ReadKeyChecked(ini, "KeyToggle", keyToggle);
    keyCycleMode = ReadKeyChecked(ini, "KeyCycleMode", keyCycleMode);
    keyYawMode = ReadKeyChecked(ini, "KeyYawMode", keyYawMode);
}

}  // namespace pdht
