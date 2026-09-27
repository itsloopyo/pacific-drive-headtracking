// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "config.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "legacy_config/legacy_config.h"

#include "cameraunlock/config/value_codecs.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/logging/file_log.h"

namespace pdht::config {

namespace {

namespace cfg = ::cameraunlock::config;
namespace log = ::cameraunlock::logging;
using cfg::schema::Concept;
using ::cameraunlock::input::FormatKeyBindings;
using ::cameraunlock::input::KeyModifiers;

constexpr const wchar_t* kIniName = L"CameraUnlock.ini";
constexpr const wchar_t* kLegacyIniName = L"PacificDriveHeadTracking.ini";

// data/games.json's display_name for pacific-drive.
constexpr const char* kDisplayName = "Pacific Drive";

// FovOffset's bounds, the ones every earlier build clamped it to. Past them the
// projection stops being usable rather than just wide.
constexpr double kMinFovOffset = -40.0;
constexpr double kMaxFovOffset = 60.0;

constexpr KeyModifiers kChord = KeyModifiers::kCtrl | KeyModifiers::kShift;

// The Ctrl+Shift chords every build before the canonical format bound in code
// beside the three key codes in the file.
constexpr int kVkY = 0x59;
constexpr int kVkG = 0x47;
constexpr int kVkH = 0x48;

std::unique_ptr<cfg::ConfigOwner<Config>> g_owner;

void Save(const char* rows, const std::function<void(Config&)>& change) {
    const cfg::ConfigSaveResult result = g_owner->Save(change);
    if (result.status != cfg::ConfigSaveStatus::Saved) {
        log::Line("Config: %s %s: %s", rows, cfg::ConfigSaveStatusName(result.status), result.reason.c_str());
    }
    for (const std::string& line : result.log) log::Line("Config: %s", line.c_str());
}

// A legacy key code (N1 and N3 applied) followed by the action's chord, which
// was always bound beside it.
std::string KeyList(int code, const char* key, int chordVk, std::vector<cfg::DroppedValue>& dropped) {
    const std::string bound = cfg::LegacyVirtualKeyToBindings(code, "Controls", key, dropped);
    const std::string chord = FormatKeyBindings({{kChord, chordVk}});
    return bound.empty() ? chord : bound + ", " + chord;
}

cfg::ImportResult RunImport(const cfg::LegacyInput& input, Config& out) {
    // Every earlier build opened PacificDriveHeadTracking.ini by its ANSI path,
    // and the frozen reader does the same. Where it finds no file, the published
    // build ran on its defaults.
    legacy::Config read;
    const bool present = legacy::Load(input.ansi_path, read);

    std::vector<cfg::DroppedValue> dropped;
    std::vector<cfg::PoseShapingValue> poseShaping;

    // Every sensitivity and inversion shipped at identity, so nothing folds: the
    // mod applies the pose as the tracker sends it, and a value the player
    // changed is dropped.
    const auto shaping = [&](auto value, auto shipped, const char* section, const char* key) {
        cfg::LegacyPoseShaping(value, shipped, section, key, poseShaping, dropped);
    };
    shaping(read.yawSensitivity, 1.0f, "Tracking", "YawSensitivity");
    shaping(read.pitchSensitivity, 1.0f, "Tracking", "PitchSensitivity");
    shaping(read.rollSensitivity, 1.0f, "Tracking", "RollSensitivity");
    shaping(read.invertYaw, false, "Tracking", "InvertYaw");
    shaping(read.invertPitch, false, "Tracking", "InvertPitch");
    shaping(read.invertRoll, false, "Tracking", "InvertRoll");
    shaping(read.positionSensitivityX, 1.0f, "Position", "SensitivityX");
    shaping(read.positionSensitivityY, 1.0f, "Position", "SensitivityY");
    shaping(read.positionSensitivityZ, 1.0f, "Position", "SensitivityZ");

    // The reader keeps the port inside 1024-65535, and the smoothing pair, the
    // limits and the FOV offset finite and inside ranges the canonical rows
    // hold, so each carries over as it is.
    out.udpPort = read.port;
    out.enableOnStartup = read.enableOnStartup;
    out.worldSpaceYaw = read.worldSpaceYaw;
    out.localSmoothing = read.localSmoothing;
    out.remoteSmoothing = read.remoteSmoothing;
    out.limitX = read.limitX;
    out.limitY = read.limitY;
    out.limitYDown = read.limitYDown;
    out.limitZ = read.limitZ;
    out.limitZBack = read.limitZBack;
    out.fovOffsetDegrees = read.fovOffsetDegrees;

    // [Position] Enabled chose the startup mode and nothing else: the cycle
    // reached every mode either way.
    const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(
        read.positionEnabled ? cameraunlock::TrackingMode::RotationAndPosition
                             : cameraunlock::TrackingMode::RotationOnly);
    out.rotationEnabled = channels.rotation_enabled;
    out.positionEnabled = channels.position_enabled;

    // The reader keeps each code inside 0x01-0xFE. A code on Ctrl, Shift or Alt
    // alone is unbound (N3) and the chord stays.
    out.toggleKey = KeyList(read.keyToggle, "KeyToggle", kVkY, dropped);
    out.cycleTrackingModeKey = KeyList(read.keyCycleMode, "KeyCycleMode", kVkG, dropped);
    out.yawModeKey = KeyList(read.keyYawMode, "KeyYawMode", kVkH, dropped);

    // A setting the player never changed from what the dev build shipped
    // follows Defaults.ini.
    const legacy::Config shipped;
    cfg::LegacyFollowsDefaultsIni follows;
    follows.Setting(Concept::UdpPort, read.port, shipped.port);
    follows.Setting(Concept::EnableOnStartup, read.enableOnStartup, shipped.enableOnStartup);
    follows.Setting(Concept::WorldSpaceYaw, read.worldSpaceYaw, shipped.worldSpaceYaw);
    follows.TrackingMode(read.positionEnabled, shipped.positionEnabled);
    follows.Setting(Concept::LocalSmoothing, read.localSmoothing, shipped.localSmoothing);
    follows.Setting(Concept::RemoteSmoothing, read.remoteSmoothing, shipped.remoteSmoothing);
    follows.Setting(Concept::PositionLimitX, read.limitX, shipped.limitX);
    follows.Setting(Concept::PositionLimitY, read.limitY, shipped.limitY);
    follows.Setting(Concept::PositionLimitYDown, read.limitYDown, shipped.limitYDown);
    follows.Setting(Concept::PositionLimitZ, read.limitZ, shipped.limitZ);
    follows.Setting(Concept::PositionLimitZBack, read.limitZBack, shipped.limitZBack);
    follows.Setting(Concept::ToggleKey, read.keyToggle, shipped.keyToggle);
    follows.Setting(Concept::CycleTrackingModeKey, read.keyCycleMode, shipped.keyCycleMode);
    follows.Setting(Concept::YawModeKey, read.keyYawMode, shipped.keyYawMode);

    return present ? cfg::ImportResult::Imported(std::move(dropped), std::move(poseShaping), follows.Concepts())
                   : cfg::ImportResult::Absent(std::move(dropped), std::move(poseShaping), follows.Concepts());
}

}  // namespace

cfg::ConfigTable<Config> Table() {
    cfg::ConfigTable<Config> table;
    table.Concept<Concept::UdpPort>(&Config::udpPort)
        .Concept<Concept::EnableOnStartup>(&Config::enableOnStartup)
        .Concept<Concept::WorldSpaceYaw>(&Config::worldSpaceYaw)
        .Writable()
        .Concept<Concept::RotationEnabled>(&Config::rotationEnabled)
        .Writable()
        .Concept<Concept::LocalSmoothing>(&Config::localSmoothing)
        .Concept<Concept::RemoteSmoothing>(&Config::remoteSmoothing)
        .Concept<Concept::PositionEnabled>(&Config::positionEnabled)
        .Writable()
        .Concept<Concept::PositionLimitX>(&Config::limitX)
        .Concept<Concept::PositionLimitY>(&Config::limitY)
        .Concept<Concept::PositionLimitYDown>(&Config::limitYDown)
        .Concept<Concept::PositionLimitZ>(&Config::limitZ)
        .Concept<Concept::PositionLimitZBack>(&Config::limitZBack)
        .Concept<Concept::ToggleKey>(&Config::toggleKey)
        .Concept<Concept::CycleTrackingModeKey>(&Config::cycleTrackingModeKey)
        .Concept<Concept::YawModeKey>(&Config::yawModeKey)
        .Local("Camera", "FovOffset", &Config::fovOffsetDegrees, cfg::FloatCodec(),
               "Degrees added to the game's field of view, -40 to 60. 0 renders what the game\n"
               "asks for. The game has no field of view setting of its own, and widens its view\n"
               "in the car and with speed; this adds to that rather than replacing it.\n"
               "HeadTracking.log shows the field of view the game draws at on its fov line.")
        .Range(kMinFovOffset, kMaxFovOffset);
    return table;
}

cfg::RenderHeader Header() {
    cfg::RenderHeader header;
    header.display_name = kDisplayName;
    return header;
}

cfg::LegacyImport<Config> Import() {
    cfg::LegacyImport<Config> import;
    import.run = &RunImport;
    for (const legacy::Key& key : legacy::ReadKeys()) import.keys.push_back({key.section, key.key});
    return import;
}

cfg::ConfigOwnerOptions<Config> OwnerOptions(const std::wstring& exeDir, cfg::DefaultsFile defaults) {
    cfg::ConfigOwnerOptions<Config> options;
    options.path = exeDir + L"\\" + kIniName;
    options.table = Table();
    options.import = Import();
    options.legacy_path = exeDir + L"\\" + kLegacyIniName;
    options.header = Header();
    options.defaults = std::move(defaults);
    return options;
}

Config Load(const std::wstring& exeDir, cfg::DefaultsFile defaults) {
    g_owner = std::make_unique<cfg::ConfigOwner<Config>>(OwnerOptions(exeDir, std::move(defaults)));
    const cfg::ConfigLoadResult<Config> result = g_owner->Load();
    for (const std::string& line : result.log) log::Line("Config: %s", line.c_str());
    if (!result.reason.empty()) log::Line("Config: %s", result.reason.c_str());
    log::Line("Config: %s", cfg::ConfigLoadStatusName(result.status));
    return result.config;
}

cameraunlock::TrackingMode StartupTrackingMode(const Config& config) {
    const auto mode = cameraunlock::DecodeTrackingMode(config.rotationEnabled, config.positionEnabled);
    if (!mode) throw std::logic_error("RotationEnabled and PositionEnabled are both false, which the table never gives");
    return *mode;
}

void SaveWorldSpaceYaw(bool worldSpaceYaw) {
    Save("[General] WorldSpaceYaw", [worldSpaceYaw](Config& c) { c.worldSpaceYaw = worldSpaceYaw; });
}

void SaveTrackingMode(cameraunlock::TrackingMode mode) {
    const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(mode);
    Save("[General] RotationEnabled and [Position] PositionEnabled", [channels](Config& c) {
        c.rotationEnabled = channels.rotation_enabled;
        c.positionEnabled = channels.position_enabled;
    });
}

}  // namespace pdht::config
