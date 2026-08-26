// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <atomic>

#include "game_state.h"
#include "marker_hook.h"
#include "reticle.h"
#include "ue4.h"

namespace pdht {

class Mod;

// Owns the UE4 camera modification.
//
// Decoupled by construction: the hook sits on ULocalPlayer::GetProjectionData,
// which builds the renderer's view transform and is reached only from the view
// and screen-projection paths. The gameplay simulation takes its view point
// from APlayerCameraManager's cached POV instead, which this mod only ever
// reads - so interaction traces, AI vision and physics see the clean camera
// while the player sees the head-tracked one.
//
// Install() starts a worker that runs the tracking pipeline and publishes the
// latest pose; the game-thread detour applies it. Without a pinned
// getProjectionDataRva for the running build no hook is installed and the mod
// runs as UDP receiver + hotkeys only.
//
// Install is one-way. Taking a detour back off cannot be made safe here: MinHook
// rewinds a thread parked on the patched prologue but not one already executing
// the detour body, so a game thread mid-frame would resume in code that is being
// unmapped. The module is pinned instead, which makes "never unloaded" a
// property of the binary rather than an assumption. See DllMain.
class CameraHook {
public:
    explicit CameraHook(Mod& mod) : m_mod(mod) {}

    bool Install();

private:
    // The head pose the worker hands to the game-thread detour. Rotation is in
    // degrees and position in centimetres along the clean view axes: the
    // tracker-to-engine conversion happens on the worker so the detour stays a
    // straight apply.
    //
    // Relaxed ordering throughout. Each axis is independently meaningful, the
    // worker republishes every 4ms, and the cost of a torn read is one frame
    // built from two adjacent samples.
    struct PublishedPose {
        std::atomic<float> pitch{0.0f};
        std::atomic<float> yaw{0.0f};
        std::atomic<float> roll{0.0f};
        std::atomic<float> offsetRight{0.0f};
        std::atomic<float> offsetUp{0.0f};
        std::atomic<float> offsetForward{0.0f};
        // False whenever the detour must leave the view alone: no tracker data,
        // or the gate has blended fully out.
        std::atomic<bool> valid{false};

        void StoreRotation(float newPitch, float newYaw, float newRoll);
        void StoreOffset(float right, float up, float forward);
        // Fills the rotation and offset of @p out, leaving its mode fields alone.
        void LoadInto(ue4::HeadPose& out) const;
    };

    // ProjectionMatrix [0][0] and [1][1] as they were finally rendered, plus
    // [0][0] as the engine built it - the horizontal pair is what the offset is
    // read off, and a second vertical number says nothing the aspect ratio does
    // not. Read on every hook call rather than only while tracking, so the log
    // reports the FOV whether or not a tracker is connected; game and effective
    // differ only when a FOV offset is configured.
    struct RenderedProjection {
        std::atomic<float> gameXX{0.0f};
        std::atomic<float> effectiveXX{0.0f};
        std::atomic<float> effectiveYY{0.0f};

        // What the game asked for. Also seeds the effective pair, which is what
        // gets rendered whenever no FOV offset is applied.
        void StoreAsBuilt(float xx, float yy);
        void StoreRendered(float xx, float yy);
    };

    void Worker();
    // One pass of the worker loop, split so each step reads as what it does.
    void PollReflection();
    void RefreshEngineObjects();
    void AdvanceGateBlend(float dt);
    // Converts this tick's tracker sample into the engine's conventions and
    // publishes it. True when a sample actually arrived.
    bool PublishPose(float dt);
    void LogHeartbeat();

    // Keeps m_cameraManager current for the heartbeat's clean-POV line. Steady
    // state is one pointer read; the full object scans only run when the cached
    // pointer has died, because walking every UObject header on a timer is a
    // visible hitch.
    void ResolveCameraManager();

    bool InstallProjectionHook();
    // ULocalPlayer::GetProjectionData(FViewport*, EStereoscopicPass,
    // FSceneViewProjectionData&). rcx/rdx/r8d/r9, returns false when the view
    // could not be built (no viewport, no player controller, zero-size view).
    static bool GetProjectionDataDetour(void* localPlayer, void* viewport, int stereoPass,
                                        void* projectionData);

    Mod& m_mod;
    Reticle m_reticle;
    MarkerHook m_markerHook;
    GameState m_gameState;
    bool m_reflectionReady = false;
    bool m_gateReady = false;
    unsigned long long m_nextGateInitMs = 0;
    // Eases 0..1 with the gate so entering a menu settles the view back to the
    // game's own camera instead of snapping to it.
    float m_gateBlend = 0.0f;
    const ue4::BuildProfile* m_profile = nullptr;
    std::uintptr_t m_moduleBase = 0;
    std::uintptr_t m_moduleEnd = 0;
    std::uintptr_t m_cameraManager = 0;
    std::uintptr_t m_cameraManagerClass = 0;
    unsigned long long m_nextCameraScanMs = 0;
    std::size_t m_povRotationOffset = 0;

    PublishedPose m_pose;
    RenderedProjection m_projection;

    // Set once at Install() from config and read on the game thread from then
    // on. The FOV offset is deliberately NOT gated on tracking or on the
    // menu/cutscene blend: it is a rendering preference, not part of the head
    // pose, and blending it would pop the view every time a menu opened.
    float m_fovOffsetDegrees = 0.0f;
    // Set when the view transform at the hooked struct's offsets turns out not
    // to be one. Injection stops for the rest of the session rather than
    // writing rotated axes over whatever is really there.
    std::atomic<bool> m_layoutRejected{false};
    std::atomic<unsigned long long> m_detourFires{0};
    std::atomic<unsigned long long> m_detourInjects{0};
    void* m_projectionHookOrig = nullptr;
    void* m_projectionHookTarget = nullptr;
    // Release-stored once the hook's own state is built and acquire-loaded in
    // the detour, so a game thread that reaches the detour the instant the
    // prologue is patched cannot observe a half-constructed CameraHook.
    static std::atomic<CameraHook*> s_self;
};

}  // namespace pdht
