// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <string>

#include "cameraunlock/config/config_concepts.g.h"
#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/defaults_file.h"
#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/data/position_settings.h"
#include "cameraunlock/math/smoothing_utils.h"
#include "cameraunlock/tracking/tracking_mode.h"

namespace pdht {

// The settings CameraUnlock.ini holds, at their defaults.
struct Config {
    int udpPort = 4242;
    bool enableOnStartup = true;
    // Yaw about world up (horizon-locked) rather than the camera's own up.
    bool worldSpaceYaw = true;

    // The tracking mode at startup, the pair the mode hotkey saves.
    bool rotationEnabled = true;
    bool positionEnabled = true;

    // Smoothing is chosen per connection: local for a tracker on this machine
    // (loopback), remote for a device on the network. Both cover rotation and
    // position.
    float localSmoothing = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
    float remoteSmoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);

    // The vertical clamp is [-limitYDown, +limitY].
    float limitX = cameraunlock::PositionSettings{}.limit_x;
    float limitY = cameraunlock::PositionSettings{}.limit_y;
    float limitYDown = cameraunlock::PositionSettings{}.limit_y_down;
    float limitZ = cameraunlock::PositionSettings{}.limit_z;
    float limitZBack = cameraunlock::PositionSettings{}.limit_z_back;

    std::string toggleKey =
        cameraunlock::config::schema::ConceptTraits<cameraunlock::config::schema::Concept::ToggleKey>::kCanonicalDefault;
    std::string cycleTrackingModeKey =
        cameraunlock::config::schema::ConceptTraits<cameraunlock::config::schema::Concept::CycleTrackingModeKey>::kCanonicalDefault;
    std::string yawModeKey =
        cameraunlock::config::schema::ConceptTraits<cameraunlock::config::schema::Concept::YawModeKey>::kCanonicalDefault;

    // Degrees added to the game's own horizontal field of view. Pacific Drive
    // has no FOV setting of its own, so this is the only way to change it.
    // Additive rather than absolute because the game varies its FOV by context -
    // wider in the car than on foot, wider again with speed - and pinning one
    // number would flatten all of that. 0 renders exactly what the game asked
    // for. The log reports the measured FOV every heartbeat, which is where a
    // player who wants a specific number reads off the offset to ask for.
    float fovOffsetDegrees = 0.0f;
};

}  // namespace pdht

// CameraUnlock.ini, beside the game exe, in cameraunlock-core's canonical config
// format. One ConfigOwner reads and writes it; nothing else in the mod touches
// it. PacificDriveHeadTracking.ini, the file every earlier build read, is
// imported once while CameraUnlock.ini is absent and is never written.
namespace pdht::config {

cameraunlock::config::ConfigTable<Config> Table();

cameraunlock::config::RenderHeader Header();

// PacificDriveHeadTracking.ini through the frozen reader in src/legacy_config/,
// mapped into Config.
cameraunlock::config::LegacyImport<Config> Import();

// The owner's options for CameraUnlock.ini in `exeDir`, a full path, with
// PacificDriveHeadTracking.ini beside it as the legacy file and Defaults.ini
// where `defaults` says.
cameraunlock::config::ConfigOwnerOptions<Config> OwnerOptions(const std::wstring& exeDir,
                                                              cameraunlock::config::DefaultsFile defaults);

// Reads, imports or creates CameraUnlock.ini in `exeDir`, logs what the owner
// reports, and returns the settings the session runs on. Call once, from the
// bootstrap thread, with the log open. `defaults` is DefaultsFile::PerUser() in
// the mod.
Config Load(const std::wstring& exeDir, cameraunlock::config::DefaultsFile defaults);

// The tracking mode the settings start in. The table never gives both rows
// false.
cameraunlock::TrackingMode StartupTrackingMode(const Config& config);

// Saves the value a hotkey has just applied. The session keeps it whether or
// not the save succeeds; a failed save is logged. Called on the hotkey thread.
void SaveWorldSpaceYaw(bool worldSpaceYaw);
void SaveTrackingMode(cameraunlock::TrackingMode mode);

}  // namespace pdht::config
