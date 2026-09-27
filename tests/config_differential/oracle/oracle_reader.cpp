// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock

// The dev pre-release's reader and startup code (tag dev, commit 21a4f1e; no
// v* release exists).
//
// src/config.cpp and src/config.h beside this file are byte copies of 21a4f1e's,
// compiled here as they shipped, with their namespace renamed by the macro
// below so they can sit in one program beside this build's pdht. Every
// cameraunlock-core source they include holds the same bytes at 21a4f1e's pin
// (7820a8c) and at this repo's (CMakeLists.txt checks both). What is
// transcribed is the startup code that consumed the settings, which cannot be
// compiled into a test because it hooks the game:
//
//   src/mod.cpp         lines 107-123  LoadConfig
//                       lines 134-169  ApplyConfigToSession
//                       lines 183-210  RegisterHotkeys, as data
//   src/camera_hook.cpp line 190       the FOV offset the hook applies

#define pdht pdht_dev
#include "src/config.cpp"
#undef pdht

#include "oracle_reader.h"

namespace pdht_oracle {

namespace {

constexpr int kVkY = 0x59;
constexpr int kVkG = 0x47;
constexpr int kVkH = 0x48;

constexpr unsigned kNav = 0;
constexpr unsigned kChord = 3;

}  // namespace

Published Read(const std::string& ini_path) {
    pdht_dev::Config config;
    config.Load(ini_path);

    Published p;
    p.port = config.port;
    p.tracking_enabled = config.enableOnStartup;
    p.world_space_yaw = config.worldSpaceYaw;

    p.yaw_sens = config.yawSensitivity;
    p.pitch_sens = config.pitchSensitivity;
    p.roll_sens = config.rollSensitivity;
    p.invert_yaw = config.invertYaw;
    p.invert_pitch = config.invertPitch;
    p.invert_roll = config.invertRoll;

    p.pos_sens_x = config.positionSensitivityX;
    p.pos_sens_y = config.positionSensitivityY;
    p.pos_sens_z = config.positionSensitivityZ;
    p.limit_x = config.limitX;
    p.limit_y = config.limitY;
    p.limit_y_down = config.limitYDown;
    p.limit_z = config.limitZ;
    p.limit_z_back = config.limitZBack;

    p.local_smoothing = config.localSmoothing;
    p.remote_smoothing = config.remoteSmoothing;

    p.tracking_mode = config.positionEnabled ? 0 : 1;

    p.fov_offset = config.fovOffsetDegrees;

    p.hotkeys.push_back({kToggle, config.keyToggle, kNav});
    p.hotkeys.push_back({kCycleMode, config.keyCycleMode, kNav});
    p.hotkeys.push_back({kYawMode, config.keyYawMode, kNav});
    p.hotkeys.push_back({kToggle, kVkY, kChord});
    p.hotkeys.push_back({kCycleMode, kVkG, kChord});
    p.hotkeys.push_back({kYawMode, kVkH, kChord});
    return p;
}

}  // namespace pdht_oracle
