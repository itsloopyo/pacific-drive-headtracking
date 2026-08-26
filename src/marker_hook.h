// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <atomic>
#include <cstdint>

#include "ue4.h"

namespace pdht {

// Shifts widgets that the game positions directly in the viewport, so
// world-anchored markers land where the player is actually looking.
//
// Objective markers are not children of the HUD: each is its own UserWidget
// added to the viewport, and the game places it with
// UUserWidget::SetPositionInViewport. That writes a Slate-side slot rather than
// any UMG property, which is why every attempt to move them through UMG state
// failed - the position was never in a field this could reach. Hooking the setter
// and offsetting the incoming position is the only place it can be corrected, and
// it fixes every viewport-positioned widget at once.
//
// The detour runs on the game thread, as does SetOffset(), which the view hook
// calls once it has the frame's aim offset.
class MarkerHook {
public:
    // One-way, like the view hook: there is no safe point at which a detour can
    // be taken back off. See CameraHook.
    bool Install(std::uintptr_t moduleBase, const ue4::BuildProfile* profile);

    // The frame's two view bases, and the UMG viewport scale needed to convert
    // the setter's argument to pixels. Not an offset: a screen point has to be
    // REPROJECTED, because under roll the centre does not move and a translation
    // would leave every marker behind while the world turns.
    void SetViewState(const ue4::ViewProjection& view) { m_view = view; }
    void SetDpiScale(float dpiScale) {
        m_dpiScale = dpiScale > 0.01f ? dpiScale : 1.0f;
    }
    void ClearViewState() { m_view.valid = false; }

    bool IsInstalled() const { return m_target != nullptr; }
    unsigned long long CallCount() const { return m_calls.load(std::memory_order_relaxed); }
    unsigned long long MovedCount() const { return m_moved.load(std::memory_order_relaxed); }

    // FVector2D as the setter takes it, by value.
    struct PositionArg {
        float X;
        float Y;
    };

private:
    static void Detour(void* self, PositionArg position, bool removeDpiScale);

    void* m_target = nullptr;
    void* m_original = nullptr;
    // Release-stored after m_original is set, acquire-loaded in the detour.
    static std::atomic<MarkerHook*> s_instance;
    // Written by the view hook and read by the detour, both on the game thread.
    ue4::ViewProjection m_view{};
    float m_dpiScale = 1.0f;
    std::atomic<unsigned long long> m_calls{0};
    std::atomic<unsigned long long> m_moved{0};
};

}  // namespace pdht
