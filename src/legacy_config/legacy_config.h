// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The pre-canonical PacificDriveHeadTracking.ini reader, frozen. It reads a
// file the way the last build before the canonical config format did (the
// rolling dev pre-release, 21a4f1e), so a player's old file is carried over as
// that build read it. Never edit anything in this folder: CMakeLists.txt pins
// every file here by hash.
//
// Frozen from src/config.cpp and src/config.h at b8ac5e2 (Config::Load and the
// helpers it calls, unchanged since 21a4f1e), with three changes: it fills this
// frozen copy of that commit's settings and their defaults instead of the mod's
// Config, it returns whether the file was there to read, and it lives in
// namespace pdht::legacy. It never wrote the file and still does not. The
// defaults are written as the literals the code held then (cameraunlock-core's
// PositionSettings limits and its smoothing defaults), and core's
// math::SanitizeFinite is copied in, so a later change to core cannot move what
// an old file means.
namespace pdht::legacy {

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
    bool worldSpaceYaw = true;
    float localSmoothing = 0.0f;
    float remoteSmoothing = 0.15f;

    // [Position]. A file without a usable LimitYDown takes LimitY for it.
    bool positionEnabled = true;
    float positionSensitivityX = 1.0f;
    float positionSensitivityY = 1.0f;
    float positionSensitivityZ = 1.0f;
    float limitX = 0.30f;
    float limitY = 0.20f;
    float limitYDown = 0.20f;
    float limitZ = 0.40f;
    float limitZBack = 0.10f;

    // [Camera]
    float fovOffsetDegrees = 0.0f;

    // [Controls]. Virtual-key codes; one outside 0x01-0xFE, or text that is not
    // a whole hex number, keeps the default. The Ctrl+Shift+Y/G/H chords were
    // bound in code beside them.
    int keyToggle = 0x23;     // End
    int keyCycleMode = 0x21;  // Page Up
    int keyYawMode = 0x22;    // Page Down
};

// Reads `iniPath` (an ANSI path, as the published build opened it) into
// `out`; keys the file lacks keep their defaults. Returns whether the file was
// there to read (IniReader::Open).
bool Load(const std::string& iniPath, Config& out);

struct Key {
    const char* section;
    const char* key;
};

// Every key Load takes a value from. The retired [Tracking] Smoothing and
// [Position] Smoothing are read only to warn that they are ignored, so they
// are not among them.
std::vector<Key> ReadKeys();

}  // namespace pdht::legacy
