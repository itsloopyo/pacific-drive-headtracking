// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <atomic>
#include <cstdint>

#include "marker_hook.h"
#include "ue4.h"

namespace pdht {

// Moves the aim-anchored parts of the HUD to where the game is actually aiming.
//
// Aim is decoupled - the interaction trace runs from the camera manager's clean
// POV, which the head never touches - so once the head turns, the object about
// to be interacted with is no longer under the centre of the screen. Two widgets
// need the same correction, and both are moved with
// UWidget::SetRenderTranslation, which writes RenderTransform.Translation and
// pushes it to the Slate widget without disturbing any layout:
//
//   - Widget_Crosshair_C, anchored dead centre (slot anchors 0.5,0.5).
//   - UMG_Highlight_Screen_C, a full-screen stretch container (anchors 0,0 to
//     1,1) holding the interaction highlights and contextual prompts. The game
//     positions its children by projecting world points through the CLEAN view -
//     it never calls ULocalPlayer::GetProjectionData for them, confirmed by the
//     hook's call count being identical with and without markers on screen - so
//     they sit where the clean view would have put them. A render transform on
//     the container corrects every child at once, because a transform of the
//     view maps to one transform of the whole projected layer.
// Objective markers are NOT corrected here. Their position is written
// Slate-side by SetPositionInViewport, not into any UMG field, so shifting their
// widget tree does nothing - see MarkerHook, which offsets the setter instead.
// This class still feeds MarkerHook the pixel offset and DPI scale, since it is
// where both are already computed.
//
// A FOV offset is corrected on the same widgets, and needs a scale as well as a
// translation. Changing the FOV is exactly a uniform scale of the frame about
// its centre, so the layer whose children were projected at the game's own FOV
// has to shrink or grow about the centre by the same ratio - a translation
// cannot do that, and at a +20 degree offset an element halfway to the screen
// edge would sit hundreds of pixels out. UWidget::RenderTransform holds Scale
// right after Translation and RenderTransformPivot defaults to the widget's
// centre, which for a full-screen layer is the centre of the screen, so writing
// the scale and then calling SetRenderTranslation (whose UpdateRenderTransform()
// pushes the whole transform) is the correction.
//
// The translation is exact for the view centre and a first-order approximation
// away from it: head roll additionally rotates the layer, which needs
// SetRenderTransform rather than SetRenderTranslation and is not done yet. The
// scale is exact for the FOV change on its own.
//
// Resolve() runs on the mod's worker thread (the object scan walks every
// UObject, and the HUD is rebuilt across level loads). Apply() and Recentre()
// run on the game thread, from inside the view hook.
class Reticle {
public:
    // Binds the two engine calls from the build profile. False when the running
    // build has no RVAs pinned for them; compensation then stays off.
    bool Bind(std::uintptr_t moduleBase, const ue4::BuildProfile* profile);

    // Keeps the cached widget pointers current. Cheap in the steady state: it
    // re-checks the pointers it already has and only falls back to a full
    // UObject scan when one has gone (a level load rebuilds the HUD). Safe to
    // call before the HUD exists - it just finds nothing and backs off.
    void Resolve();

    // Offsets the crosshair to @p offset, in pixels from the centre of the view,
    // and scales the full-screen layer by the frame's FOV change. An invalid
    // offset (aim behind the tracked view) recentres instead of leaving the
    // crosshair stuck at the edge.
    void Apply(const ue4::AimScreenOffset& offset, const ue4::ViewProjection& view);

    // Puts the crosshair back at the centre and the layer back to unit scale.
    // Called when tracking stops and on unload, so a disabled mod never leaves
    // either parked somewhere the game did not put it.
    void Recentre();

    // Set once at bind time; the reticle publishes each frame's pixel offset and
    // DPI scale to it, because the marker hook needs both spellings and the
    // reticle is where the scale is already being fetched.
    void SetMarkerHook(MarkerHook* hook) { m_markerHook = hook; }

    bool IsBound() const { return m_setRenderTranslation != nullptr; }
    int WidgetCount() const { return m_widgetCount.load(std::memory_order_relaxed); }
    unsigned long long MoveCount() const { return m_moves.load(std::memory_order_relaxed); }

private:
    // @p layerScale is the uniform screen-space scale a FOV change amounts to,
    // applied about the widget's centre pivot and ONLY to full-screen layers.
    // The crosshair is deliberately excluded: it is a piece of UI art at the aim
    // point, and scaling it would resize the artwork rather than move it - its
    // position already carries the FOV, because the projection it comes from is
    // the one being rendered.
    void Write(float x, float y, float layerScale);
    // Resolves the tracked UClasses by name, once. Retried until the crosshair
    // class appears, because before the HUD content is loaded there is nothing
    // to find.
    void ResolveTrackedClasses();
    // True when every cached widget still belongs to the crosshair class, i.e.
    // nothing has been destroyed under us and a scan would find the same set.
    bool CachedWidgetsStillLive() const;

    // Has to cover EVERY marker, not a sample of them: the game keeps a large
    // pool of objective markers alive (134 counted in one ordinary session) and
    // the one actually on screen is not necessarily among the first found. A cap
    // of 24 wrote twenty invisible markers and missed the visible one, which
    // looked exactly like the correction not working at all.
    static constexpr int kMaxWidgets = 256;
    static constexpr int kMaxClasses = 4;

    // Low bit of a slot: the widget is a full-screen layer, i.e. one whose
    // CHILDREN are the projected HUD rather than a single element. Only those
    // take the FOV scale. UObjects are at least 8-byte aligned so the bit is
    // free, and packing it into the SAME atomic as the pointer is what stops the
    // game thread pairing a freshly rescanned pointer with the previous
    // occupant's flag - which, when a layer slot is replaced by a crosshair,
    // would stamp the FOV scale onto the crosshair and resize the artwork.
    static constexpr std::uintptr_t kLayerFlag = 1;

    MarkerHook* m_markerHook = nullptr;
    void* m_setRenderTranslation = nullptr;
    void* m_getViewportScale = nullptr;
    // Written by the scan on the worker thread, read by the write on the game
    // thread. Pointer in the upper bits, kLayerFlag in bit 0.
    std::atomic<std::uintptr_t> m_widgets[kMaxWidgets] = {};
    std::atomic<int> m_widgetCount{0};
    // The UClass each widget belonged to when it was found, so liveness is one
    // pointer read per widget rather than a name comparison.
    std::uintptr_t m_widgetClasses[kMaxWidgets] = {};
    // The tracked UClasses, resolved by name once. Scanning by class pointer
    // instead of class name is what keeps a rescan affordable: objective markers
    // are spawned and destroyed as objectives come and go, so the cached set
    // goes stale often and the scan is not a rare event.
    std::uintptr_t m_trackedClasses[kMaxClasses] = {};
    bool m_classesResolved = false;
    // Full scans are rate-limited on their own clock, and back off hard while
    // nothing is found - at the main menu the widgets do not exist at all and
    // rescanning every second there was a visible hitch.
    unsigned long long m_nextScanMs = 0;
    std::atomic<unsigned long long> m_moves{0};
    // Last translation actually written, in Slate units, so an unchanged pose
    // does not invalidate the widget every call.
    float m_lastX = 0.0f;
    float m_lastY = 0.0f;
    float m_lastScale = 1.0f;
    // Atomic because a rescan clears it from the worker thread to force the new
    // widgets to be written; everything else about it is game-thread only.
    std::atomic<bool> m_written{false};
    bool m_scaleWarned = false;
};

}  // namespace pdht
