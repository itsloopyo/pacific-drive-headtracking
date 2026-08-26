// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <atomic>
#include <memory>

#include "cameraunlock/input/hotkey_poller.h"
#include "cameraunlock/protocol/udp_receiver.h"
#include "cameraunlock/tracking/head_tracking_session.h"
#include "camera_hook.h"
#include "config.h"

namespace pdht {

// Mod singleton. Owns the full lifecycle: log, config, UDP receiver, the
// shared HeadTrackingSession pipeline, hotkeys, and the camera hook. Created
// on the bootstrap thread spawned from DllMain.
class Mod {
public:
    static Mod& Get();

    // Bootstrap entry point (runs on its own thread). Opens the log, loads
    // config, starts the receiver + hotkeys, and installs the camera hook.
    //
    // There is no matching Shutdown(): the module is pinned so it is never
    // unloaded, and the only DLL_PROCESS_DETACH that can still fire is the one
    // where the OS has already killed every thread. See DllMain.
    void Run();

    bool TrackingEnabled() const { return m_enabled.load(); }
    // True when head yaw turns the view about world up (horizon-locked) rather
    // than about the camera's own up axis. Read on the game thread by the
    // camera hook, flipped by the hotkey thread.
    bool WorldSpaceYaw() const { return m_worldSpaceYaw.load(); }
    void ToggleTracking();
    // Called from the hotkey thread. Raises a request rather than touching the
    // pipeline: HeadTrackingSession::SetMode resets the position processor's
    // smoothing and the interpolator when position goes off, and the camera
    // worker is inside Session().Update() reading exactly that state.
    void CycleTrackingMode();
    // Consumes a pending CycleTrackingMode(). Called by the camera worker, which
    // is the thread that owns the pipeline.
    void ApplyPendingModeChange();
    void ToggleYawMode();

    // Logs which smoothing parameter is in force whenever the session switches
    // between a local and a remote tracker. The session itself does the
    // selection, from the receiver's source address.
    void LogConnectionChange();

    const Config& GetConfig() const { return m_config; }
    const cameraunlock::UdpReceiver& Receiver() const { return m_receiver; }
    cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver>& Session() { return *m_session; }

private:
    void LogGameModuleFingerprint() const;
    void LoadConfig();
    void StartReceiver();
    void ApplyConfigToSession();
    void RegisterHotkeys();

    Config m_config;
    cameraunlock::UdpReceiver m_receiver;
    std::unique_ptr<cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver>> m_session;
    cameraunlock::input::HotkeyPoller m_hotkeys;
    std::unique_ptr<CameraHook> m_cameraHook;

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_worldSpaceYaw{true};
    std::atomic<bool> m_modeCycleRequested{false};
    bool m_started = false;
    bool m_isRemoteConnection = false;
    // Tri-state: false/false is indistinguishable from a local tracker, so a
    // plain equality check never reports the (common) local case at all.
    bool m_remoteConnectionKnown = false;
};

}  // namespace pdht
