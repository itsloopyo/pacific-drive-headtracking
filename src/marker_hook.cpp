// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "marker_hook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cameraunlock/logging/file_log.h"
#include "detour.h"

namespace pdht {

namespace log = cameraunlock::logging;

std::atomic<MarkerHook*> MarkerHook::s_instance{nullptr};

namespace {
// MSVC x64 passes an 8-byte POD aggregate in the integer register, which is what
// the engine's own callers do for FVector2D.
using SetPositionFn = void (*)(void*, MarkerHook::PositionArg, bool);
}  // namespace

bool MarkerHook::Install(std::uintptr_t moduleBase, const ue4::BuildProfile* profile) {
    if (!profile || !profile->setPositionInViewportRva) {
        log::Line("[MarkerHook] no SetPositionInViewport RVA for this build - "
                  "world-anchored markers will stay in the clean view's screen space.");
        return false;
    }
    void* target = reinterpret_cast<void*>(moduleBase + profile->setPositionInViewportRva);
    void* original = nullptr;
    if (!InstallDetour("MarkerHook", target, reinterpret_cast<void*>(&Detour), &original)) {
        return false;
    }
    m_target = target;
    m_original = original;
    // After m_original, and with release ordering: the prologue is already
    // patched by the time InstallDetour returns, so the detour can run before
    // the next statement here does.
    s_instance.store(this, std::memory_order_release);
    log::Line("[MarkerHook] hooked UUserWidget::SetPositionInViewport @ rva 0x%zX.",
              profile->setPositionInViewportRva);
    return true;
}

void MarkerHook::Detour(void* self, PositionArg position, bool removeDpiScale) {
    // Acquire, pairing with the release store in Install(). Non-null from the
    // first call onwards: InstallDetour patches the prologue, so the window
    // where the detour is reachable and s_instance is not yet published is real,
    // and there is no trampoline to chain to inside it. Swallowing the call
    // there would drop a marker's position for one frame, which is the least bad
    // of the three options (the other two being a null deref and a call through
    // a pointer that is not yet published).
    MarkerHook* hook = s_instance.load(std::memory_order_acquire);
    if (!hook) return;
    hook->m_calls.fetch_add(1, std::memory_order_relaxed);

    // The flag says which space the caller is in: with it set the setter divides
    // by the DPI scale itself, so the argument is in pixels; without it the caller
    // is already in Slate units. Reprojection works in pixels, so convert both
    // ways around it.
    const float scale = removeDpiScale ? 1.0f : hook->m_dpiScale;
    float x = 0.0f;
    float y = 0.0f;
    if (ue4::ReprojectScreenPoint(hook->m_view, position.X * scale, position.Y * scale, x, y)) {
        position.X = x / scale;
        position.Y = y / scale;
        hook->m_moved.fetch_add(1, std::memory_order_relaxed);
    }

    reinterpret_cast<SetPositionFn>(hook->m_original)(self, position, removeDpiScale);
}

}  // namespace pdht
