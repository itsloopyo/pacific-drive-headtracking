// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pdht {

// An actor found by object scan, plus the byte offset of the one property read
// off it and when the next scan is allowed. Both of the gate's actor signals
// have this shape: re-check the pointer already held, rescan the moment it dies
// (a level load replaces it, and reporting "not in gameplay" through a
// self-imposed backoff would suppress tracking for no reason), and back off only
// when a scan cannot produce a usable actor.
struct CachedActorProperty {
    std::uintptr_t actor = 0;
    // The UClass the actor had when it was found. IsObjectAlive alone is not
    // identity: UE recycles both UObject memory and object-array indices, so a
    // new object can land on the same address at the same index, pass the
    // "slot still holds this pointer, not PendingKill" test, and inherit an
    // offset that was resolved against a completely different class.
    std::uintptr_t cls = 0;
    std::size_t offset = 0;
    // Explicit, because 0 is a legitimate property offset and using it as the
    // "not resolved yet" sentinel re-ran the whole SuperStruct walk - up to 32
    // hops of up to 8192 FField name resolves, each one a std::string - on every
    // tick for the rest of the session whenever a lookup failed.
    bool offsetResolved = false;
    unsigned long long nextScanMs = 0;
};

// Unavailable signals retain their own diagnostic instead of reporting a menu.
enum class Signal { True, False, Unavailable };

// Decides whether the player is actually playing, so head tracking is not
// running over the main menu, the pause menu, or a cutscene.
//
// Three signals, all read through reflection rather than pinned offsets:
//
//   - The local PlayerController has a possessed Pawn and is not the main menu
//     controller, which also possesses a pawn.
//   - AWorldSettings::Pauser is the engine's own pause flag - non-null means
//     paused, whatever is drawn on top.
//   - The camera manager's ViewTarget.Target is a CINE camera. A LevelSequence
//     cutscene points the view target at a CineCameraActor; ordinary gameplay
//     points it at a pawn (PDP_MainCharacter_C) or at a plain CameraActor for the
//     in-car view. Testing for "Camera" rather than "CineCamera" therefore reads
//     normal driving as a cutscene and suppresses tracking for the whole
//     session.
//
// Every one of those is an ACTOR, which is the point: an actor is flagged
// PendingKill the moment Destroy() runs, so a cached pointer can be told apart
// from a live one immediately. Widget state was tried first and does not have
// that property - quitting a save back to the menu left the cached HUD widget
// passing every check available, and tracking stayed on over the menu.
//
// Update() runs on the mod's worker thread. InGameplay() is read from the game
// thread and is a plain atomic load.
class GameState {
public:
    // Checks the FProperty layout by looking up UWidget::RenderTransform, whose
    // offset is known independently from the disassembly of
    // SetRenderTranslation. A mismatch means property offsets cannot be trusted,
    // so tracking remains suppressed until the layout is validated.
    bool Initialize();

    // Re-reads the state. Cheap in the steady state: the cached actors are
    // re-checked, not rescanned. @p cameraManager may be 0 before one has been
    // found, in which case the cutscene check is skipped.
    void Update(std::uintptr_t cameraManager);

    bool InGameplay() const { return m_inGameplay.load(std::memory_order_relaxed); }

    // Whether the gate has validated the property layout.
    bool IsActive() const { return m_active; }

    // Last reason tracking is suppressed, for the heartbeat. Never null.
    const char* Reason() const { return m_reason.load(std::memory_order_relaxed); }

private:
    Signal HasPossessedPawn();
    Signal IsPaused();
    Signal IsCutscene(std::uintptr_t cameraManager);

    bool m_active = false;
    // Initialize() is retried while the engine finishes starting up, so the
    // first few failures are normal and must not be logged.
    int m_initAttempts = 0;
    static constexpr int kInitAttemptsBeforeWarning = 5;

    // The local PlayerController and its Pawn property.
    CachedActorProperty m_controller;
    // Tri-state, so the first answer is reported whichever way it goes: -1 =
    // nothing read yet, 0 = no pawn, 1 = possessed.
    int m_lastPossessed = -1;
    // The pawn pointer last seen, and whether its class checked out. Validating
    // the class costs an FName resolve and a std::string, so it is done when the
    // pointer CHANGES rather than every tick - the same reason IsPaused only
    // resolves Pauser's class when the pointer is set.
    std::uintptr_t m_lastPawn = 0;
    bool m_lastPawnIsPawn = false;

    // AWorldSettings and its Pauser property. A level actor, so it is found by
    // class fragment rather than as a transient runtime instance.
    CachedActorProperty m_worldSettings;

    // FTViewTarget within APlayerCameraManager; Target is its first member.
    std::size_t m_viewTargetOffset = 0;
    bool m_viewTargetOffsetResolved = false;
    // Which camera manager m_viewTargetOffset was resolved against, so a
    // different manager subclass re-resolves rather than reusing an offset from
    // another class.
    std::uintptr_t m_viewTargetOwner = 0;
    std::uintptr_t m_lastViewTarget = 0;
    // The view target's class as well as its address. Memoising the verdict on
    // the address alone let a destroyed CineCameraActor's memory be recycled
    // into the gameplay view target and keep answering "cutscene" - suppressing
    // tracking for the rest of the session, with the one diagnostic line that
    // would have named it never printing, because the address never changed.
    std::uintptr_t m_lastViewTargetClass = 0;
    bool m_viewTargetIsCamera = false;

    // One-shot, so a gate signal that cannot be read says so once instead of
    // either staying silent or repeating every tick.
    bool m_unavailableLogged = false;

    bool m_mainMenu = false;
    std::atomic<bool> m_inGameplay{false};
    std::atomic<const char*> m_reason{"starting up"};
};

}  // namespace pdht
