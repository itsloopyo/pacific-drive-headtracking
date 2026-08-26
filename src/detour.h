// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

namespace pdht {

// Installs a MinHook detour on @p target: brings the shared HookManager up,
// creates the hook and enables it, logging exactly which of those three steps
// failed under @p tag.
//
// False means nothing was installed - including the intermediate case where the
// hook was created but could not be enabled, which is unwound before returning
// so the state matches what the log says. That is the only distinction the
// callers need: both run dormant rather than retrying, because a hook that could
// not be placed on this build will not place on the next call either.
//
// There is no matching remove. Detours are installed once and never taken off -
// see CameraHook for why that cannot be made safe here.
bool InstallDetour(const char* tag, void* target, void* detour, void** originalOut);

}  // namespace pdht
