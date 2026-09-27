// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "config.h"

#include "legacy_config/legacy_config.h"

namespace pdht {

void Config::Load(const std::string& iniPath) {
    legacy::Config read;
    legacy::Load(iniPath, read);

    port = read.port;
    enableOnStartup = read.enableOnStartup;
    yawSensitivity = read.yawSensitivity;
    pitchSensitivity = read.pitchSensitivity;
    rollSensitivity = read.rollSensitivity;
    invertYaw = read.invertYaw;
    invertPitch = read.invertPitch;
    invertRoll = read.invertRoll;
    worldSpaceYaw = read.worldSpaceYaw;
    localSmoothing = read.localSmoothing;
    remoteSmoothing = read.remoteSmoothing;
    positionEnabled = read.positionEnabled;
    positionSensitivityX = read.positionSensitivityX;
    positionSensitivityY = read.positionSensitivityY;
    positionSensitivityZ = read.positionSensitivityZ;
    limitX = read.limitX;
    limitY = read.limitY;
    limitYDown = read.limitYDown;
    limitZ = read.limitZ;
    limitZBack = read.limitZBack;
    fovOffsetDegrees = read.fovOffsetDegrees;
    keyToggle = read.keyToggle;
    keyCycleMode = read.keyCycleMode;
    keyYawMode = read.keyYawMode;
}

}  // namespace pdht
