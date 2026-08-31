// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cstdint>
#include <string>

#include "cameraunlock/data/position_settings.h"
#include "cameraunlock/math/smoothing_utils.h"

namespace pdht {

// Mod configuration, loaded from PacificDriveHeadTracking.ini next to the
// game exe. Defaults match the CameraUnlock doctrine (all sensitivities 1.0,
// local smoothing 0.0, remote smoothing 0.15).
struct Config {
    // [Network]
    uint16_t port = 4242;

    // [Tracking]
    bool enableOnStartup = true;
    float yawSensitivity = 1.0f;
    float pitchSensitivity = 1.0f;
    float rollSensitivity = 1.0f;
    bool invertYaw = false;
    bool invertPitch = false;
    bool invertRoll = false;
    // Yaw about world up (horizon-locked) rather than the camera's own up.
    bool worldSpaceYaw = true;
    // Smoothing is chosen per connection: local for a tracker on this machine
    // (loopback), remote for a device on the network. Both cover rotation and
    // position.
    float localSmoothing = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
    float remoteSmoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);

    // [Position]
    bool positionEnabled = true;
    float positionSensitivityX = 1.0f;
    float positionSensitivityY = 1.0f;
    float positionSensitivityZ = 1.0f;
    float limitX = cameraunlock::PositionSettings{}.limit_x;
    // The vertical clamp is [-limitYDown, +limitY]. Two fields, because a
    // tighter crouch range than standing range is a real thing to want; when
    // LimitYDown is absent it mirrors LimitY, which is the symmetric case.
    // Leaving limit_y_down at the core struct's own default instead meant a user
    // who set LimitY=0.05 still got 0.20 m of downward travel, with nothing in
    // the log or the docs saying the key was only half-effective.
    float limitY = cameraunlock::PositionSettings{}.limit_y;
    float limitYDown = cameraunlock::PositionSettings{}.limit_y_down;
    float limitZ = cameraunlock::PositionSettings{}.limit_z;
    float limitZBack = cameraunlock::PositionSettings{}.limit_z_back;

    // [Camera]
    // Degrees added to the game's own horizontal field of view. Pacific Drive
    // has no FOV setting of its own, so this is the only way to change it.
    // Additive rather than absolute because the game varies its FOV by context -
    // wider in the car than on foot, wider again with speed - and pinning one
    // number would flatten all of that. 0 renders exactly what the game asked
    // for. The log reports the measured FOV every heartbeat, which is where a
    // player who wants a specific number reads off the offset to ask for.
    float fovOffsetDegrees = 0.0f;

    // [Controls] - nav-cluster keys; chord alternatives are hardwired.
    int keyToggle = 0x23;          // End
    int keyCycleMode = 0x21;       // Page Up
    int keyYawMode = 0x22;         // Page Down

    // Loads from the given INI path. Missing file -> all defaults.
    void Load(const std::string& iniPath);
};

}  // namespace pdht
