// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "detour.h"

#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/logging/file_log.h"

namespace pdht {

namespace log = cameraunlock::logging;
namespace hooks = cameraunlock::hooks;

bool InstallDetour(const char* tag, void* target, void* detour, void** originalOut) {
    hooks::HookManager& hm = hooks::HookManager::Instance();

    // Both hooks initialise the same manager, so the second call reports
    // "already initialized" rather than Ok. IsInitialized() is what says whether
    // the manager is usable.
    const hooks::HookStatus init = hm.Initialize();
    if (init != hooks::HookStatus::Ok && !hm.IsInitialized()) {
        log::Line("[%s] MinHook init failed: %s", tag, hooks::HookStatusToString(init));
        return false;
    }

    const hooks::HookStatus created = hm.CreateHook(target, detour, originalOut);
    if (created != hooks::HookStatus::Ok) {
        log::Line("[%s] CreateHook failed: %s", tag, hooks::HookStatusToString(created));
        return false;
    }

    const hooks::HookStatus enabled = hm.EnableHook(target);
    if (enabled != hooks::HookStatus::Ok) {
        log::Line("[%s] EnableHook failed: %s", tag, hooks::HookStatusToString(enabled));
        // CreateHook succeeded, so a hook object and a live trampoline exist
        // even though nothing is patched. Leaving them behind makes the caller's
        // "nothing was installed" reading a lie and turns any retry into
        // ErrorAlreadyCreated. ScopedHook::Create in the same library unwinds
        // the same way.
        hm.RemoveHook(target);
        *originalOut = nullptr;
        return false;
    }
    return true;
}

}  // namespace pdht
