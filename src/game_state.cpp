// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "game_state.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/unreal/ue_runtime.h"
#include "ue4.h"

namespace pdht {

namespace log = cameraunlock::logging;
namespace un = cameraunlock::unreal;

namespace {

constexpr unsigned long long kRescanMissingMs = 4000;

std::uintptr_t FindWorldSettings() { return ue4::FindLiveInstance("WorldSettings"); }

// True when the cached actor is still the same live object it was found as.
// IsObjectAlive says the object array still holds this pointer at this index and
// has not flagged it PendingKill; the class compare says it is still the same
// KIND of object. Both are needed: UE recycles object memory and array indices,
// so a fresh object can inherit the address and the index of the one just
// destroyed and pass the liveness test on its own. The offset cached alongside
// was resolved against the old class, and applying it to the new object reads a
// raw qword from an unrelated struct.
bool CachedActorStillValid(const CachedActorProperty& cached) {
    if (!cached.actor || !cached.cls) return false;
    if (!ue4::IsObjectAlive(cached.actor)) return false;
    return ue4::ClassOf(cached.actor) == cached.cls;
}

// Brings @p cached up to date: keeps the actor it already has while that is the
// same live object, rescans the moment it is not, and resolves the property
// offset once per actor. False means the signal cannot be read this tick -
// either nothing usable was found, or the class has no such property.
bool RefreshCachedProperty(CachedActorProperty& cached, std::uintptr_t (*findActor)(),
                           const char* propertyName) {
    if (!CachedActorStillValid(cached)) {
        const unsigned long long nowMs = GetTickCount64();
        // Only a scan that cannot produce a usable actor backs off. A cached
        // actor that has just died is replaced at once, because a level load is
        // exactly when the gate must not report "not in gameplay" for several
        // seconds.
        const bool hadOne = cached.actor != 0;
        if (!hadOne && nowMs < cached.nextScanMs) return false;
        cached.actor = findActor();
        cached.cls = cached.actor ? ue4::ClassOf(cached.actor) : 0;
        cached.offset = 0;
        cached.offsetResolved = false;
        if (!cached.actor || !cached.cls) {
            // Arm the backoff even when a scan DID return something unusable.
            // Keying it on "found nothing" alone meant a scan that kept handing
            // back the same unusable actor re-walked every UObject header in the
            // process, once per tick, for as long as it lasted - which is the
            // once-per-second hitch this whole caching pattern exists to avoid.
            cached.actor = 0;
            cached.nextScanMs = nowMs + kRescanMissingMs;
            return false;
        }
    }
    if (cached.offsetResolved) return true;
    if (ue4::FindPropertyOffset(cached.cls, propertyName, cached.offset)) {
        cached.offsetResolved = true;
        return true;
    }
    // The walk that just failed is up to 32 SuperStruct hops of up to 8192 field
    // name resolves, each one an FName decode and a std::string. Back off rather
    // than repeating it every tick for the rest of the session.
    cached.nextScanMs = GetTickCount64() + kRescanMissingMs;
    return false;
}

}  // namespace

bool GameState::Initialize() {
    // UWidget is a native class and exists from engine startup, so this resolves
    // long before any level is loaded - which is where the gate is needed most.
    const std::uintptr_t widgetClass = ue4::FindClassByName("Widget");
    if (!widgetClass) {
        // Counted and logged like the property failure below. Left silent, a
        // permanent failure to resolve the class produced no log output at all
        // for the whole session while FindClassByName - a full object-array walk
        // that resolves an FName per object - re-ran every three seconds. The
        // same class of invisible failure has shipped here once already.
        if (++m_initAttempts == kInitAttemptsBeforeWarning) {
            log::Line("[GameState] the UWidget class is still unresolvable after several "
                      "attempts - the menu/cutscene gate stays off and tracking runs "
                      "everywhere.");
        }
        return false;
    }

    std::size_t renderTransform = 0;
    if (!ue4::FindPropertyOffset(widgetClass, "RenderTransform", renderTransform)) {
        // Expected on the first attempts: the class exists before its properties
        // are populated. Only complain once it has clearly stopped being a
        // startup race, otherwise every launch logs a failure it then recovers
        // from a few seconds later.
        if (++m_initAttempts == kInitAttemptsBeforeWarning) {
            log::Line("[GameState] UWidget properties still unreadable after several "
                      "attempts - the menu/cutscene gate stays off and tracking runs "
                      "everywhere.");
        }
        return false;
    }
    if (renderTransform != ue4::kRenderTransformOffset) {
        log::Line("[GameState] UWidget::RenderTransform reflects to +0x%zX but the "
                  "disassembly says +0x%zX, so the FProperty layout has moved. The "
                  "menu/cutscene gate stays off rather than trusting property offsets.",
                  renderTransform, ue4::kRenderTransformOffset);
        return false;
    }

    m_active = true;
    log::Line("[GameState] gate active (FProperty layout confirmed against "
              "UWidget::RenderTransform at +0x%zX).", renderTransform);
    return true;
}

Signal GameState::HasPossessedPawn() {
    if (!RefreshCachedProperty(m_controller, &ue4::FindPlayerController, "Pawn")) {
        return Signal::Unavailable;
    }

    std::uintptr_t pawn = 0;
    if (!un::SafeReadPtr(m_controller.actor + m_controller.offset, pawn)) {
        return Signal::Unavailable;
    }

    if (pawn != m_lastPawn) {
        m_lastPawn = pawn;
        // Non-null, and pointer-SHAPED. If this offset were ever applied to the
        // wrong object, a float or a flags word sitting there would read as
        // "possessed" and latch tracking on over the menu, which is the exact
        // regression the pawn check replaced a widget check to fix.
        //
        // Deliberately a shape test and not a class-name test: the pawn is a
        // Blueprint class whose name this code does not know, and a fragment
        // match that missed would suppress tracking for a whole session. A gate
        // that wrongly suppresses is far worse than one that wrongly allows, and
        // the controller's own identity (class + liveness, see
        // CachedActorStillValid) is what actually establishes that this offset
        // points at the Pawn member.
        m_lastPawnIsPawn = pawn != 0 && un::LooksLikePointer(pawn);
    }
    const bool possessed = m_lastPawnIsPawn;

    // Report what the check actually saw when the answer changes. "No possessed
    // pawn" while the HUD is clearly up and the camera is live is not something
    // to guess about a second time.
    if (static_cast<int>(possessed) != m_lastPossessed) {
        m_lastPossessed = static_cast<int>(possessed);
        log::Line("[GameState] controller 0x%llX (%s) pawnOffset=+0x%zX pawn=0x%llX%s",
                  static_cast<unsigned long long>(m_controller.actor),
                  un::ClassName(m_controller.actor).c_str(), m_controller.offset,
                  static_cast<unsigned long long>(pawn),
                  pawn ? (ue4::IsObjectAlive(pawn) ? " (alive)" : " (NOT alive)") : "");
    }
    return possessed ? Signal::True : Signal::False;
}

Signal GameState::IsPaused() {
    if (!RefreshCachedProperty(m_worldSettings, &FindWorldSettings, "Pauser")) {
        return Signal::Unavailable;
    }

    std::uintptr_t pauser = 0;
    if (!un::SafeReadPtr(m_worldSettings.actor + m_worldSettings.offset, pauser)) {
        return Signal::Unavailable;
    }
    if (!pauser) return Signal::False;
    // A wrong offset would hand back some other member that happens to be
    // non-null and report the game paused forever. Pauser is an APlayerState, so
    // require that of whatever is there; the name resolve only runs when the
    // pointer is set, which is rare.
    return un::ContainsCI(un::ClassName(pauser), "PlayerState") ? Signal::True
                                                                : Signal::False;
}

Signal GameState::IsCutscene(std::uintptr_t cameraManager) {
    if (!cameraManager) return Signal::Unavailable;
    // ViewTarget's offset belongs to the class it was resolved against. A
    // different manager instance can be a different subclass, so re-resolve
    // rather than reusing a number from another class - and drop the memoised
    // verdict with it, since it described a different manager's view target.
    if (cameraManager != m_viewTargetOwner) {
        m_viewTargetOwner = cameraManager;
        m_viewTargetOffsetResolved = false;
        m_lastViewTarget = 0;
        m_lastViewTargetClass = 0;
        m_viewTargetIsCamera = false;
    }
    if (!m_viewTargetOffsetResolved) {
        if (!ue4::FindPropertyOffset(ue4::ClassOf(cameraManager), "ViewTarget",
                                     m_viewTargetOffset)) {
            return Signal::Unavailable;
        }
        m_viewTargetOffsetResolved = true;
    }

    // FTViewTarget::Target is the struct's first member.
    std::uintptr_t target = 0;
    if (!un::SafeReadPtr(cameraManager + m_viewTargetOffset, target)) {
        return Signal::Unavailable;
    }
    const std::uintptr_t targetClass = target ? ue4::ClassOf(target) : 0;
    // Both the address AND the class, so a recycled address is re-examined.
    if (target == m_lastViewTarget && targetClass == m_lastViewTargetClass) {
        return m_viewTargetIsCamera ? Signal::True : Signal::False;
    }

    m_lastViewTarget = target;
    m_lastViewTargetClass = targetClass;
    const std::string targetName = target ? un::ClassName(target) : std::string();
    // "CineCamera", not "Camera". Matching "Camera" killed tracking for a whole
    // session: Pacific Drive's ordinary gameplay view target is a plain
    // CameraActor (the in-car camera), while a LevelSequence cutscene uses a
    // CineCameraActor. Observed classes: CineCameraActor during a cutscene,
    // CameraActor and PDP_MainCharacter_C during gameplay.
    m_viewTargetIsCamera = un::ContainsCI(targetName, "CineCamera")
                           || un::ContainsCI(targetName, "Cinematic");
    // Logged on change only, and it is the whole diagnostic for this check: if a
    // cutscene is still tracked, this line names the class to key off instead.
    log::Line("[GameState] view target -> %s%s",
              targetName.empty() ? "(none)" : targetName.c_str(),
              m_viewTargetIsCamera ? "  (treated as a cutscene)" : "");
    return m_viewTargetIsCamera ? Signal::True : Signal::False;
}

void GameState::Update(std::uintptr_t cameraManager) {
    if (!m_active) return;

    const Signal pawn = HasPossessedPawn();
    const Signal paused = pawn == Signal::True ? IsPaused() : Signal::False;
    const Signal cutscene =
        pawn == Signal::True && paused == Signal::False ? IsCutscene(cameraManager)
                                                        : Signal::False;

    // Fails OPEN, every signal alike. A signal the mod cannot read says nothing
    // about whether the player is playing, and a gate that wrongly suppresses is
    // far worse than one that wrongly allows: the user sees no head tracking
    // anywhere and the log names a menu or a loading screen as the cause.
    if (pawn == Signal::Unavailable && !m_unavailableLogged) {
        m_unavailableLogged = true;
        log::Line("[GameState] the possessed-pawn signal cannot be read on this build "
                  "(no local PlayerController found, or no reachable Pawn property). "
                  "The gate leaves tracking ON rather than reporting a menu that is not "
                  "there. Report this log.");
    }

    const char* reason = nullptr;
    if (pawn == Signal::False) {
        reason = "tracking suppressed: no possessed pawn (menu, loading or not in a level)";
    } else if (paused == Signal::True) {
        reason = "tracking suppressed: game paused";
    } else if (cutscene == Signal::True) {
        reason = "tracking suppressed: cutscene (view target is a cine camera)";
    }

    const bool playing = reason == nullptr;
    if (playing != m_inGameplay.exchange(playing, std::memory_order_relaxed)) {
        // With the reason on the transition line, a report of "tracking stopped"
        // is diagnosable from the log alone. Without it the only clue was
        // whichever heartbeat happened to land next, up to 30 seconds later.
        log::Line("[GameState] %s", playing ? "gameplay - tracking on"
                                            : reason);
    }
    m_reason.store(playing ? "in gameplay" : reason, std::memory_order_relaxed);
}

}  // namespace pdht
