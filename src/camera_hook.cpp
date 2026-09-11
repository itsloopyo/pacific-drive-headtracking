// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "camera_hook.h"

#include <chrono>
#include <cmath>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/unreal/ue_runtime.h"
#include "detour.h"
#include "mod.h"

namespace pdht {

namespace log = cameraunlock::logging;
namespace un = cameraunlock::unreal;

namespace {

// UE works in centimetres; the tracker reports metres.
constexpr float kMetresToCentimetres = 100.0f;

// Gate blend rate, as the doctrine's frame-rate-independent exponential: a 0.1s
// time constant, so opening a menu settles the view rather than snapping it.
constexpr float kGateBlendSpeed = 10.0f;
// Below this the gate has blended out far enough that the pose is not worth
// applying, and the detour should hand the view straight back to the game.
constexpr float kGateBlendFloor = 0.001f;

// Publish rate of the worker loop. Well above any refresh rate, so the detour
// always has a fresh sample without the loop being a busy wait.
constexpr int kPublishIntervalMs = 4;
// A dt outside this is a debugger break or a stalled load screen, not a frame;
// blending over it would jump the gate. Substitute a nominal frame instead.
constexpr float kMaxPlausibleFrameSeconds = 0.25f;
constexpr float kNominalFrameSeconds = 1.0f / 60.0f;

// Discovery scans every writable data section twice, decoding an FName for every
// pointer-shaped slot - order 10^6 guarded reads and 10^5 std::string
// constructions per attempt. On a patched build where the layout has moved that
// never succeeds, so a flat one-second retry burns that for the whole session
// and evicts ~20 MB of the game's data out of cache each time. Back off
// geometrically instead: the first few attempts still cover a healthy startup
// race, and a permanent failure settles at one attempt a minute.
constexpr float kReflectionRetryFirstSeconds = 1.0f;
constexpr float kReflectionRetryMaxSeconds = 60.0f;
constexpr float kEngineRefreshSeconds = 1.0f;
// 30s, not 2s: the heartbeat is ~600 bytes per beat and the publish loop runs
// for the whole session, so a 2s beat was 1 MB/hour in every user's log.
constexpr float kHeartbeatSeconds = 30.0f;
// Everything a "no head tracking" report needs settles in the first few
// minutes - hook installed, reflection up, gate opening on the first level,
// port bound, first packet parsed - and 30s is the density that makes those
// beats readable against each other. Past that a beat only proves the detour is
// still firing, and at 30s for a five-hour session that alone is most of the
// log. Ten dense beats then one every five minutes is a ~9x cut with the
// diagnostic window untouched.
constexpr int kDenseHeartbeats = 10;
constexpr float kSettledHeartbeatSeconds = 300.0f;
constexpr float kTrackerSilenceWarningSeconds = 20.0f;

constexpr unsigned long long kCameraScanFoundMs = 2000;
// Back off hard while there is nothing to find: at the main menu the scan would
// otherwise run every second for no result.
constexpr unsigned long long kCameraScanMissingMs = 5000;
constexpr unsigned long long kGateInitRetryMs = 3000;

// Used when the build is unknown but reflection came up anyway. It is the same
// CameraCachePrivate.POV.Rotation offset the known profile pins; on a build that
// moved it the reads simply return implausible angles in the heartbeat, and
// nothing is ever written there.
constexpr std::size_t kFallbackPovRotationOffset = 0x1AFC;

using GetProjectionDataFn = bool (*)(void*, void*, int, void*);

// Says once, and only once, that no tracker data has arrived - and says which of
// the two causes it is, because "the tracker app is not running" and "another
// app is holding the port" send the user to completely different places.
//
// The verdict names the state the receiver was in at the time, and "another app
// is still holding the port" stops being true the moment that app exits. The
// supervisor reclaims the port on its own, so the state flips underneath a
// one-shot warning and the log's only diagnosis is then the stale one - which
// reads as the mod never noticing, and sends the user back to a game they have
// already closed. Re-arming on that transition is what keeps it honest.
class TrackerSilenceWatch {
public:
    using Clock = std::chrono::steady_clock;

    TrackerSilenceWatch(bool retrying, unsigned port, Clock::time_point now)
        : m_port(port), m_wasRetrying(retrying), m_silentSince(now) {}

    void NoteSample() { m_everReceived = true; }

    void Update(bool retrying, Clock::time_point now) {
        if (m_everReceived) return;

        if (retrying != m_wasRetrying) {
            m_wasRetrying = retrying;
            m_warned = false;
            m_silentSince = now;
        }
        if (m_warned) return;

        const float silentSeconds =
            std::chrono::duration_cast<std::chrono::duration<float>>(now - m_silentSince).count();
        if (silentSeconds < kTrackerSilenceWarningSeconds) return;
        m_warned = true;

        if (retrying) {
            // Saying "the port is bound" here would send the user after the
            // tracker app when the fault is a second process on the port.
            log::Line("No tracker data on UDP port %u after %.0f seconds: another app is "
                      "still holding the port. Close it and tracking starts by itself - "
                      "the receiver keeps retrying and nothing else about the mod is "
                      "shut down while it waits.", m_port, kTrackerSilenceWarningSeconds);
        } else {
            log::Line("No tracker data on UDP port %u after %.0f seconds. The port is bound, "
                      "so nothing else is holding it - the tracker app is either not running "
                      "or not pointed at this machine. Head tracking cannot do anything until "
                      "a packet arrives.", m_port, kTrackerSilenceWarningSeconds);
        }
    }

private:
    unsigned m_port;
    bool m_everReceived = false;
    bool m_warned = false;
    bool m_wasRetrying;
    Clock::time_point m_silentSince;
};

}  // namespace

void CameraHook::PublishedPose::StoreRotation(float newPitch, float newYaw, float newRoll) {
    pitch.store(newPitch, std::memory_order_relaxed);
    yaw.store(newYaw, std::memory_order_relaxed);
    roll.store(newRoll, std::memory_order_relaxed);
}

void CameraHook::PublishedPose::StoreOffset(float right, float up, float forward) {
    offsetRight.store(right, std::memory_order_relaxed);
    offsetUp.store(up, std::memory_order_relaxed);
    offsetForward.store(forward, std::memory_order_relaxed);
}

void CameraHook::PublishedPose::LoadInto(ue4::HeadPose& out) const {
    out.pitch = pitch.load(std::memory_order_relaxed);
    out.yaw = yaw.load(std::memory_order_relaxed);
    out.roll = roll.load(std::memory_order_relaxed);
    out.offsetRight = offsetRight.load(std::memory_order_relaxed);
    out.offsetUp = offsetUp.load(std::memory_order_relaxed);
    out.offsetForward = offsetForward.load(std::memory_order_relaxed);
}

void CameraHook::RenderedProjection::StoreAsBuilt(float xx, float yy) {
    gameXX.store(xx, std::memory_order_relaxed);
    effectiveXX.store(xx, std::memory_order_relaxed);
    effectiveYY.store(yy, std::memory_order_relaxed);
}

void CameraHook::RenderedProjection::StoreRendered(float xx, float yy) {
    effectiveXX.store(xx, std::memory_order_relaxed);
    effectiveYY.store(yy, std::memory_order_relaxed);
}

std::atomic<CameraHook*> CameraHook::s_self{nullptr};

bool CameraHook::Install() {
    if (!ue4::GetGameModule(m_moduleBase, m_moduleEnd)) {
        log::Line("[CameraHook] could not resolve game module range");
        return false;
    }

    m_profile = ue4::MatchProfile(reinterpret_cast<void*>(m_moduleBase));
    if (m_profile) {
        log::Line("[CameraHook] matched build profile '%s'", m_profile->name);
    } else {
        log::Line("[CameraHook] unknown build - no view hook. Reflection discovery is "
                  "still attempted (it self-validates) so the log reports what is there.");
    }

    m_povRotationOffset = m_profile ? m_profile->povRotationOffset : 0;
    m_fovOffsetDegrees = m_mod.GetConfig().fovOffsetDegrees;
    if (m_fovOffsetDegrees != 0.0f) {
        log::Line("[CameraHook] FOV offset %+.1f degrees, applied to the rendered "
                  "projection only - the game's own view point keeps the FOV it asked "
                  "for, so nothing the simulation does changes with it.",
                  m_fovOffsetDegrees);
    }

    m_markerHook.Install(m_moduleBase, m_profile);
    m_reticle.SetMarkerHook(&m_markerHook);
    m_reticle.Bind(m_moduleBase, m_profile);
    // Last, and with release ordering: patching the prologue can hand the game
    // thread to the detour on the very next frame, so everything the detour
    // touches has to be built and visible before s_self is.
    s_self.store(this, std::memory_order_release);
    InstallProjectionHook();
    // Detached, not joined. Nothing ever stops this thread - the module is
    // pinned and there is no teardown path - and a std::thread member that is
    // never joined would call std::terminate if the object were ever destroyed.
    std::thread(&CameraHook::Worker, this).detach();
    return true;
}

bool CameraHook::GetProjectionDataDetour(void* localPlayer, void* viewport, int stereoPass,
                                         void* projectionData) {
    // Acquire, pairing with the release store in Install(). Null only in the
    // window between MinHook patching the prologue and Install() publishing -
    // which cannot happen, because the store comes first - and after a failed
    // Install. Report the view as unbuildable rather than faulting in the
    // render path; that is a return the engine already handles (no viewport, no
    // player controller).
    CameraHook* self = s_self.load(std::memory_order_acquire);
    if (!self) return false;
    const bool built = reinterpret_cast<GetProjectionDataFn>(self->m_projectionHookOrig)(
        localPlayer, viewport, stereoPass, projectionData);
    self->m_detourFires.fetch_add(1, std::memory_order_relaxed);

    if (!built) return built;

    // Before anything is decided about tracking: this is the FOV the game asked
    // for this frame, and it is worth reading whether or not a pose is being
    // applied - it is what the log reports and what the offset is relative to.
    float projXX = 0.0f;
    float projYY = 0.0f;
    if (ue4::ReadProjectionScale(reinterpret_cast<std::uintptr_t>(projectionData), projXX,
                                projYY)) {
        self->m_projection.StoreAsBuilt(projXX, projYY);
    }

    const bool rejected = self->m_layoutRejected.load(std::memory_order_relaxed);
    const bool tracking = self->m_pose.valid.load(std::memory_order_relaxed) && !rejected;
    // The FOV offset stands on its own: a player who set one and has not started
    // their tracker still gets it, and the HUD still has to be reprojected for
    // it, because the game placed every marker at the FOV it asked for.
    const bool changingFov = self->m_fovOffsetDegrees != 0.0f && !rejected;
    if (!tracking && !changingFov) {
        // Neither: the crosshair belongs back at the aim point, which with no
        // head pose is the centre of the screen, and the game's own marker
        // projection should be left exactly as it computes it.
        self->m_reticle.Recentre();
        self->m_markerHook.ClearViewState();
        return built;
    }

    ue4::HeadPose pose{};
    if (tracking) self->m_pose.LoadInto(pose);
    pose.worldSpaceYaw = self->m_mod.WorldSpaceYaw();
    pose.fovOffsetDegrees = self->m_fovOffsetDegrees;

    ue4::AimScreenOffset aim{};
    ue4::ViewProjection view{};
    const ue4::InjectStatus status = ue4::InjectHeadPose(
        reinterpret_cast<std::uintptr_t>(projectionData), pose, aim, view);
    if (status == ue4::InjectStatus::Ok) {
        self->m_detourInjects.fetch_add(1, std::memory_order_relaxed);
        self->m_projection.StoreRendered(view.trackedProjXX, view.trackedProjYY);
        // Same thread and same frame as the view it was computed from, and ahead
        // of the Slate pass that paints the widget.
        self->m_reticle.Apply(aim, view);
        // Markers are positioned later in the frame, on this thread, and each has
        // to be reprojected through both bases rather than shifted.
        self->m_markerHook.SetViewState(view);
        return built;
    }

    // Anything other than Ok means the struct is not what the profile says it
    // is. Carrying on writes rotated axes over whatever is really there, every
    // frame, so stop for good and say why.
    if (!self->m_layoutRejected.exchange(true, std::memory_order_relaxed)) {
        const char* reason = status == ue4::InjectStatus::NotOrthonormal
                                 ? "ViewRotationMatrix is not an orthonormal basis"
                                 : (status == ue4::InjectStatus::ReadFailed ? "read faulted"
                                                                            : "write faulted");
        log::Line("[CameraHook] view injection DISABLED: %s. The hooked function is not "
                  "ULocalPlayer::GetProjectionData on this build, or FSceneViewProjectionData "
                  "has moved. Report this log.", reason);
    }
    return built;
}

void CameraHook::ResolveCameraManager() {
    // Liveness as well as the class compare. A destroyed APlayerCameraManager
    // keeps its ClassPrivate indefinitely, so the compare alone passes forever
    // and the cached pointer is never rescanned: GameState::IsCutscene would
    // then read ViewTarget off dead memory, and the heartbeat would report a
    // frozen clean POV, which reads as proof the decoupling works.
    if (m_cameraManager && m_cameraManagerClass
        && ue4::IsObjectAlive(m_cameraManager)
        && ue4::ClassOf(m_cameraManager) == m_cameraManagerClass) {
        return;
    }

    const unsigned long long nowMs = GetTickCount64();
    if (nowMs < m_nextCameraScanMs) return;

    std::uintptr_t pc = 0;
    std::size_t off = 0;
    std::uintptr_t found = ue4::FindActiveCameraManager(pc, off);
    if (!found) found = ue4::FindCameraManager();
    m_nextCameraScanMs = nowMs + (found ? kCameraScanFoundMs : kCameraScanMissingMs);
    if (!found || found == m_cameraManager) return;

    m_cameraManager = found;
    m_cameraManagerClass = ue4::ClassOf(found);
    log::Line("[CameraHook] camera manager 0x%llX (class '%s')",
              static_cast<unsigned long long>(found), un::ClassName(found).c_str());
}

bool CameraHook::InstallProjectionHook() {
    if (!m_profile || !m_profile->getProjectionDataRva) {
        log::Line("[CameraHook] no GetProjectionData RVA pinned - head tracking disabled.");
        return false;
    }
    m_projectionHookTarget =
        reinterpret_cast<void*>(m_moduleBase + m_profile->getProjectionDataRva);
    if (!InstallDetour("CameraHook", m_projectionHookTarget,
                       reinterpret_cast<void*>(&GetProjectionDataDetour),
                       &m_projectionHookOrig)) {
        m_projectionHookTarget = nullptr;
        return false;
    }
    log::Line("[CameraHook] render view hook installed on ULocalPlayer::GetProjectionData "
              "@ rva 0x%zX.", m_profile->getProjectionDataRva);
    return true;
}

void CameraHook::PollReflection() {
    int objCount = 0;
    if (!ue4::InitReflection(m_moduleBase, m_moduleEnd, objCount)) return;

    m_reflectionReady = true;
    log::Line("[CameraHook] reflection ready: %d UObjects.", objCount);
    if (!m_povRotationOffset) m_povRotationOffset = kFallbackPovRotationOffset;
}

void CameraHook::RefreshEngineObjects() {
    ResolveCameraManager();
    m_reticle.Resolve();
    if (!m_gateReady) {
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs >= m_nextGateInitMs) {
            m_nextGateInitMs = nowMs + kGateInitRetryMs;
            m_gateReady = m_gameState.Initialize();
        }
    }
    m_gameState.Update(m_cameraManager);
}

void CameraHook::AdvanceGateBlend(float dt) {
    const bool gateOpen = m_gameState.IsActive() && m_gameState.InGameplay();
    m_gateBlend += ((gateOpen ? 1.0f : 0.0f) - m_gateBlend)
                   * (1.0f - std::exp(-kGateBlendSpeed * dt));
}

bool CameraHook::PublishPose(float dt) {
    // The hotkey thread only raises a flag; the mode change is applied HERE,
    // on the thread that owns the pipeline. HeadTrackingSession::SetMode does
    // more than an atomic store - switching position off also resets the
    // position processor's smoothing and the interpolator - and doing that from
    // the hotkey thread mid-Update() reset the interpolator's sample buffer and
    // last-sample timestamp while this thread was reading them.
    m_mod.ApplyPendingModeChange();

    bool haveTracker = false;
    if (m_mod.TrackingEnabled() && m_mod.Session().Update(dt)) {
        float yaw = 0, pitch = 0, roll = 0;
        m_mod.Session().GetRotation(yaw, pitch, roll);
        // Tracker-to-engine boundary, confirmed in game 2026-08-25: no rotation
        // axis needs negating here.
        //
        // The fleet rule is to negate yaw and roll, but that rule is for hooks
        // that write Euler angles into engine rotation state. This one rotates
        // the view's basis vectors instead, and a basis rotation in UE's
        // left-handed axes already runs opposite to the right-handed intuition -
        // InjectHeadPose negates pitch for exactly that reason. Negating yaw and
        // roll on top of that double-negated them, which is what the first
        // in-game test reported.
        m_pose.StoreRotation(pitch * m_gateBlend, yaw * m_gateBlend, roll * m_gateBlend);

        float x = 0, y = 0, z = 0;
        // Zeroed when the mode cycle has position off, and stored either way:
        // holding the last offset would park the view at whatever lean the player
        // happened to hold when they cycled.
        m_mod.Session().GetPositionOffset(x, y, z);
        // Same mirroring on x, and the processor's negative z is the forward lean
        // while the engine's forward axis is positive.
        m_pose.StoreOffset(-x * kMetresToCentimetres * m_gateBlend,
                           y * kMetresToCentimetres * m_gateBlend,
                           -z * kMetresToCentimetres * m_gateBlend);
        haveTracker = true;
        // Only once a packet has parsed: the session's locality flag starts at
        // false, so reporting it earlier reads as proof a local tracker connected
        // when nothing has arrived.
        m_mod.LogConnectionChange();
    }
    // Injection has to stay live through the blend-out, otherwise the view and
    // the crosshair snap back the instant a menu opens.
    m_pose.valid.store(haveTracker && m_gateBlend > kGateBlendFloor,
                       std::memory_order_relaxed);
    return haveTracker;
}

void CameraHook::LogHeartbeat() {
    ue4::FRotator clean{};
    const bool haveClean = m_cameraManager && m_povRotationOffset
                           && ue4::ReadPovRotation(m_cameraManager, m_povRotationOffset, clean);
    log::Line("[CameraHook][hb] hook=%d inject=%d fires=%llu injects=%llu "
              "head(P=%.1f Y=%.1f R=%.1f off=%.1f,%.1f,%.1fcm) "
              "reticle(bound=%d widgets=%d moves=%llu) "
              "markerHook(%d calls=%llu moved=%llu) gate(%.2f %s) yaw=%s",
              m_projectionHookTarget ? 1 : 0, m_pose.valid.load() ? 1 : 0,
              m_detourFires.load(), m_detourInjects.load(),
              m_pose.pitch.load(), m_pose.yaw.load(), m_pose.roll.load(),
              m_pose.offsetRight.load(), m_pose.offsetUp.load(), m_pose.offsetForward.load(),
              m_reticle.IsBound() ? 1 : 0, m_reticle.WidgetCount(), m_reticle.MoveCount(),
              m_markerHook.IsInstalled() ? 1 : 0, m_markerHook.CallCount(),
              m_markerHook.MovedCount(),
              m_gateBlend, m_gameState.IsActive() ? m_gameState.Reason() : "gate inactive",
              m_mod.WorldSpaceYaw() ? "world" : "local");
    if (haveClean) {
        // The view point the gameplay simulation uses. It must not follow the
        // head - if it does, the decoupling is broken.
        float povFov = 0.0f;
        ue4::ReadPovFov(m_cameraManager, m_povRotationOffset, povFov);
        log::Line("[CameraHook][hb]   gameplay POV P=%.1f Y=%.1f R=%.1f FOV=%.1f",
                  clean.Pitch, clean.Yaw, clean.Roll, povFov);
    }
    // Measured off the projection matrix, so it is what is on screen rather than
    // what any engine field claims. The game FOV is the number to add FovOffset
    // to when a player wants a specific one, and it moves on its own - Pacific
    // Drive widens it in the car and with speed.
    log::Line("[CameraHook][hb]   fov game=%.1f rendered=%.1f vertical=%.1f (offset %+.1f)",
              ue4::FovDegreesFromProjectionScale(m_projection.gameXX.load()),
              ue4::FovDegreesFromProjectionScale(m_projection.effectiveXX.load()),
              ue4::FovDegreesFromProjectionScale(m_projection.effectiveYY.load()),
              m_fovOffsetDegrees);
    // Whether the port is ours yet is the first thing a "no head tracking" report
    // needs and the one thing the startup lines stop being true about: the user
    // closes the other game mid-session and the receiver takes the port over
    // without anything else in this log changing.
    const auto& rx = m_mod.Receiver();
    log::Line("[CameraHook][hb]   udp port=%u bound=%d retrying=%d receiving=%d "
              "rejected=%llu frozen=%llu",
              m_mod.GetConfig().port, rx.IsRunning() ? 1 : 0, rx.IsRetrying() ? 1 : 0,
              rx.IsReceiving() ? 1 : 0,
              static_cast<unsigned long long>(rx.GetRejectedPacketCount()),
              static_cast<unsigned long long>(rx.GetFrozenPacketCount()));
}

void CameraHook::Worker() {
    using namespace std::chrono;
    using clock = steady_clock;

    auto secs = [](clock::time_point a, clock::time_point b) {
        return duration_cast<duration<float>>(a - b).count();
    };
    const auto start = clock::now();
    // Backdated one full period so that step runs on the first iteration. For
    // the heartbeat that matters: it is the only proof the game-thread detour is
    // running, and a user who quits inside 30 seconds would otherwise send a log
    // without it.
    auto backdated = [start](float period) {
        return start - duration_cast<clock::duration>(duration<float>(period));
    };
    auto t0 = start;
    float reflectionRetrySeconds = kReflectionRetryFirstSeconds;
    auto lastReflectionTry = backdated(reflectionRetrySeconds);
    auto lastRefresh = start;
    auto lastBeat = backdated(kHeartbeatSeconds);
    int beats = 0;
    // "No head tracking" with a healthy log and a silent tracker looks identical
    // to a mod fault from the outside, and it cost a debugging round. Say so.
    TrackerSilenceWatch silence(m_mod.Receiver().IsRetrying(), m_mod.GetConfig().port, start);

    for (;;) {
        const auto now = clock::now();
        float dt = secs(now, t0);
        t0 = now;
        if (dt <= 0.0f || dt > kMaxPlausibleFrameSeconds) dt = kNominalFrameSeconds;

        // Reflection is not needed to inject - the hook runs off a pinned RVA.
        // It is what lets the heartbeat read the camera manager's cached POV,
        // which is the evidence that the gameplay view point stays clean.
        if (!m_reflectionReady && secs(now, lastReflectionTry) >= reflectionRetrySeconds) {
            lastReflectionTry = now;
            PollReflection();
            if (!m_reflectionReady) {
                reflectionRetrySeconds =
                    std::fmin(reflectionRetrySeconds * 2.0f, kReflectionRetryMaxSeconds);
            }
        }

        if (m_reflectionReady && secs(now, lastRefresh) >= kEngineRefreshSeconds) {
            lastRefresh = now;
            RefreshEngineObjects();
        }

        AdvanceGateBlend(dt);
        PublishPose(dt);
        // Off the RECEIVER, not off the publish result. PublishPose short-
        // circuits when tracking is toggled off, so keying the watch on it made
        // a user who set EnableOnStartup=0 (or pressed End inside the first 20
        // seconds) get "the tracker app is either not running or not pointed at
        // this machine" while packets were in fact arriving and parsing - the
        // one log line the "no head tracking" triage depends on, naming the
        // wrong cause.
        if (m_mod.Receiver().IsReceiving()) silence.NoteSample();
        silence.Update(m_mod.Receiver().IsRetrying(), now);

        const float beatInterval =
            beats < kDenseHeartbeats ? kHeartbeatSeconds : kSettledHeartbeatSeconds;
        if (secs(now, lastBeat) >= beatInterval) {
            lastBeat = now;
            ++beats;
            LogHeartbeat();
        }

        std::this_thread::sleep_for(milliseconds(kPublishIntervalMs));
    }
}

}  // namespace pdht
