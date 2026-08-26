// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "reticle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>

#include "cameraunlock/logging/file_log.h"

namespace pdht {

namespace log = cameraunlock::logging;

namespace {

// Blueprint classes of the aim-anchored HUD. Game content, so these are names
// and not offsets - a content update could rename one, and the log says how many
// were found so a zero is visible.
struct AimAnchoredClass {
    const char* name;
    // True for a container whose CHILDREN are the projected HUD. Those take the
    // FOV scale as well as the translation; a single element at the aim point
    // takes the translation only.
    bool fullScreenLayer;
};

constexpr AimAnchoredClass kAimAnchoredClasses[] = {
    // /Game/UI/PlayerHUD/Widget_Crosshair, centred.
    {"Widget_Crosshair_C", false},
    // Full-screen container for interaction highlights.
    {"UMG_Highlight_Screen_C", true},
    // Objective waypoints are handled by name below, not by class:
    // UMG_GenericObjective_C is also what the expedition planner uses, so moving
    // that class dragged map UI around for no benefit.
};
constexpr int kAimAnchoredClassCount =
    static_cast<int>(sizeof(kAimAnchoredClasses) / sizeof(kAimAnchoredClasses[0]));

// Slate units. Below this the crosshair has not visibly moved and the write is
// only costing a widget invalidation.
constexpr float kMoveEpsilon = 0.25f;
// Same idea for the layer scale, which is a ratio: 0.001 is a tenth of a pixel
// at the edge of a 4K frame.
constexpr float kScaleEpsilon = 0.001f;

// How long to wait before another full UObject scan. Short once the HUD is up
// (a level load has to be noticed reasonably quickly), long while it is not -
// at the main menu the widgets do not exist and a scan there is pure cost.
constexpr unsigned long long kRescanFoundMs = 1500;
constexpr unsigned long long kRescanMissingMs = 5000;

struct FVector2D32 {
    float X;
    float Y;
};

// MSVC x64 passes an 8-byte POD aggregate in the integer register, which is
// what the engine's own callers do, so letting the compiler handle FVector2D
// by value is the ABI-correct form here.
using SetRenderTranslationFn = void (*)(void*, FVector2D32);
using GetViewportScaleFn = float (*)(void*);

}  // namespace

bool Reticle::Bind(std::uintptr_t moduleBase, const ue4::BuildProfile* profile) {
    if (!profile || !profile->setRenderTranslationRva || !profile->getViewportScaleRva) {
        log::Line("[Reticle] no UMG call RVAs for this build - the crosshair stays at "
                  "screen centre while aim stays decoupled.");
        return false;
    }
    m_setRenderTranslation = reinterpret_cast<void*>(moduleBase + profile->setRenderTranslationRva);
    m_getViewportScale = reinterpret_cast<void*>(moduleBase + profile->getViewportScaleRva);
    return true;
}

void Reticle::ResolveTrackedClasses() {
    static_assert(kAimAnchoredClassCount <= Reticle::kMaxClasses,
                  "kMaxClasses must cover every aim-anchored class");
    if (m_classesResolved) return;

    for (int c = 0; c < kAimAnchoredClassCount; ++c) {
        if (!m_trackedClasses[c])
            m_trackedClasses[c] = ue4::FindClassByName(kAimAnchoredClasses[c].name);
    }
    // The crosshair class is loaded with the HUD content, so if even that is
    // missing the UI is not up yet and it is worth trying again.
    m_classesResolved = m_trackedClasses[0] != 0;
    if (!m_classesResolved) return;

    // Say which ones did not resolve. A class that never resolves moves nothing
    // and is otherwise invisible - that shipped once already, as a build where
    // the whole compensation silently did nothing.
    for (int c = 0; c < kAimAnchoredClassCount; ++c) {
        if (!m_trackedClasses[c]) {
            log::Line("[Reticle] class '%s' not found - those elements will stay put "
                      "on screen.", kAimAnchoredClasses[c].name);
        }
    }
}

bool Reticle::CachedWidgetsStillLive() const {
    const int n = m_widgetCount.load(std::memory_order_relaxed);
    if (n == 0) return false;
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t widget =
            m_widgets[i].load(std::memory_order_relaxed) & ~kLayerFlag;
        if (!widget || !m_widgetClasses[i] || ue4::ClassOf(widget) != m_widgetClasses[i])
            return false;
        // ClassOf alone is not liveness: it only proves the memory has not been
        // reused yet, so a destroyed widget keeps passing indefinitely. Write()
        // then stores two floats straight into widget+0x98 and calls
        // SetRenderTranslation on it - and its __try only catches an access
        // violation, not a SUCCESSFUL write into whatever object the engine has
        // since handed that memory to. IsObjectAlive checks the global object
        // array still holds this exact pointer at its own index, which is the
        // check GameState already uses for the same reason.
        if (!ue4::IsObjectAlive(widget)) return false;
    }
    return true;
}

void Reticle::Resolve() {
    if (!IsBound()) return;
    if (CachedWidgetsStillLive()) return;

    const unsigned long long nowMs = GetTickCount64();
    if (nowMs < m_nextScanMs) return;

    ResolveTrackedClasses();

    std::uintptr_t found[kMaxWidgets] = {};
    bool isLayer[kMaxWidgets] = {};
    int n = 0;
    for (int c = 0; c < kAimAnchoredClassCount && n < kMaxWidgets; ++c) {
        if (!m_trackedClasses[c]) continue;
        const int added =
            ue4::FindInstancesOfClass(m_trackedClasses[c], found + n, kMaxWidgets - n);
        for (int i = n; i < n + added; ++i) isLayer[i] = kAimAnchoredClasses[c].fullScreenLayer;
        n += added;
    }
    const int previous = m_widgetCount.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxWidgets; ++i) {
        const std::uintptr_t slot =
            i < n ? (found[i] | (isLayer[i] ? kLayerFlag : std::uintptr_t{0})) : std::uintptr_t{0};
        m_widgets[i].store(slot, std::memory_order_relaxed);
        m_widgetClasses[i] = i < n ? ue4::ClassOf(found[i]) : 0;
    }
    m_widgetCount.store(n, std::memory_order_relaxed);
    // The widgets that just arrived have never been written, and the guard below
    // skips a write whose values match the last one. With a head pose that is
    // moot - the offset changes every frame - but a FOV offset on its own is a
    // constant, so without this a newly spawned layer would keep the game's own
    // scale for as long as the player held still.
    m_written.store(false, std::memory_order_relaxed);
    m_nextScanMs = nowMs + (n > 0 ? kRescanFoundMs : kRescanMissingMs);
    if (n != previous) {
        log::Line("[Reticle] %d live aim-anchored widget(s)%s.", n,
                  n >= kMaxWidgets ? " - AT THE CAP, so some are being missed" : "");
    }
}

void Reticle::Apply(const ue4::AimScreenOffset& offset, const ue4::ViewProjection& view) {
    if (!IsBound()) return;
    // GetViewportScale needs a world context object and the first cached widget
    // is the only one to hand, so a null slot 0 means both "nothing to move" and
    // "no scale to fetch". Taking that branch here rather than in three places
    // further down is what keeps the DPI scale published on every path that can
    // know it: leaving it at its 1.0 default while the real scale is 2.0 makes a
    // marker at Slate centre read as pixel (960,540) on a 3840-wide viewport,
    // i.e. up and to the left of where it is.
    void* context = reinterpret_cast<void*>(
        m_widgets[0].load(std::memory_order_relaxed) & ~kLayerFlag);
    if (!context) return;

    // What the FOV offset does to the frame: every projected point moves to this
    // fraction of its distance from the centre. 1.0 whenever the offset is 0,
    // which is the only case the two elements differ in.
    const float layerScale = view.valid && view.cleanProjXX > 0.0f
                                 ? view.trackedProjXX / view.cleanProjXX
                                 : 1.0f;

    // UMG positions are in Slate units; the projection produced pixels. The
    // engine caches this per frame, so calling it per write is cheap.
    float scale = 1.0f;
    __try {
        scale = reinterpret_cast<GetViewportScaleFn>(m_getViewportScale)(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        scale = 0.0f;
    }
    if (!(scale >= 0.25f && scale <= 4.0f)) {
        if (!m_scaleWarned) {
            m_scaleWarned = true;
            log::Line("[Reticle] viewport scale came back as %.3f, which is not a DPI scale; "
                      "using 1.0. The crosshair will be offset by the wrong amount at "
                      "non-native UI scales.", scale);
        }
        scale = 1.0f;
    }

    // The marker hook only needs the DPI scale from here; the view bases come
    // straight from the view hook, since a marker away from the centre has to be
    // reprojected rather than shifted by the centre's offset.
    if (m_markerHook) m_markerHook->SetDpiScale(scale);

    // Aim behind the tracked view: the crosshair has nowhere to sit, so centre
    // it. The scale still stands - the frame is being rendered at the offset FOV
    // whatever the head is doing, and dropping to unit scale here would pop the
    // highlight layer every time the player looked past 90 degrees.
    if (!offset.valid) {
        Write(0.0f, 0.0f, layerScale);
        return;
    }

    Write(offset.x / scale, offset.y / scale, layerScale);
}

void Reticle::Recentre() {
    if (!IsBound()) return;
    Write(0.0f, 0.0f, 1.0f);
}

void Reticle::Write(float x, float y, float layerScale) {
    const int n = m_widgetCount.load(std::memory_order_relaxed);
    if (n == 0) return;  // nothing to move, and the move counter should say so
    if (m_written.load(std::memory_order_relaxed) && std::fabs(x - m_lastX) < kMoveEpsilon
        && std::fabs(y - m_lastY) < kMoveEpsilon
        && std::fabs(layerScale - m_lastScale) < kScaleEpsilon)
        return;
    const FVector2D32 translation{x, y};
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t slot = m_widgets[i].load(std::memory_order_relaxed);
        const std::uintptr_t address = slot & ~kLayerFlag;
        if (!address) continue;
        // Liveness, on the thread that actually writes. Resolve() checks it too,
        // but that runs on the worker at best once a second and behind its own
        // 1.5s rescan clock, so a widget destroyed by a level load stays in this
        // table for a couple of seconds. Inside that window the two stores below
        // land at recycled+0x98 and SUCCEED - the __try catches an access
        // violation, not a well-formed write into whatever object the engine has
        // since handed that memory to - corrupting eight bytes of a live,
        // unrelated object several times a second with nothing logged.
        if (!ue4::IsObjectAlive(address)) {
            m_widgets[i].store(0, std::memory_order_relaxed);
            continue;
        }
        void* widget = reinterpret_cast<void*>(address);
        __try {
            // Scale first, then the setter: its UpdateRenderTransform() is what
            // pushes the whole FWidgetTransform down to Slate, so one call
            // publishes both. The struct offset is part of the build profile's
            // pinned layout, cross-checked against reflection by GameState.
            if (slot & kLayerFlag) {
                auto* widgetScale =
                    reinterpret_cast<float*>(address + ue4::kRenderTransformScaleOffset);
                widgetScale[0] = layerScale;
                widgetScale[1] = layerScale;
            }
            reinterpret_cast<SetRenderTranslationFn>(m_setRenderTranslation)(widget, translation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // A widget destroyed between the liveness check and here. Drop it;
            // the next Resolve() re-populates from the live object list.
            m_widgets[i].store(0, std::memory_order_relaxed);
        }
    }
    m_lastX = x;
    m_lastY = y;
    m_lastScale = layerScale;
    m_written.store(true, std::memory_order_relaxed);
    m_moves.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace pdht
