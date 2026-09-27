// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "mod.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "cameraunlock/config/defaults_file.h"
#include "cameraunlock/input/key_binding_registration.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/math/smoothing_utils.h"
#include "cameraunlock/memory/pe_fingerprint.h"
#include "ue4.h"
#include "version.h"

namespace pdht {

namespace log = cameraunlock::logging;
using cameraunlock::TrackingMode;

// Deliberately leaked, so no destructor is registered in the module's onexit
// table. A function-local `static Mod instance` would be destroyed by the CRT
// during DLL_PROCESS_DETACH - after DllMain has returned, still on the loader
// lock - and ~Mod would then join three threads the OS has already killed and
// unhook detours while suspending every thread in a terminating process. The
// module is pinned (see DllMain), so this outlives the process either way.
Mod& Mod::Get() {
    static Mod* instance = new Mod();
    return *instance;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

void Mod::Run() {
    if (m_started) return;
    m_started = true;

    // Truncates, so the file only ever covers the session being played, and
    // files the outgoing one away as HeadTracking.prev.log first: this hook
    // detours a game-thread function, so the session worth reading is often the
    // one that just crashed and the user relaunches before sending the file.
    log::Open(ExeDir() + L"\\HeadTracking.log");
    log::Line("=== %s v%s ===", kModName, kVersion);
    LogGameModuleFingerprint();
    LoadConfig();

    m_session = std::make_unique<cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver>>(m_receiver);
    ApplyConfigToSession();
    StartReceiver();
    RegisterHotkeys();

    m_cameraHook = std::make_unique<CameraHook>(*this);
    m_cameraHook->Install();

    log::Line("Bootstrap complete. Tracking %s.", m_enabled.load() ? "ENABLED" : "disabled");
}

// The first thing any "it stopped working after a patch" report needs: which
// build the mod is actually looking at, whether or not a profile matched it.
void Mod::LogGameModuleFingerprint() const {
    std::uintptr_t base = 0, end = 0;
    if (!ue4::GetGameModule(base, end)) return;
    cameraunlock::memory::PeFingerprint fp{};
    if (!cameraunlock::memory::ReadPeFingerprint(reinterpret_cast<void*>(base), fp)) return;
    log::Line("Game module base=0x%llX size=0x%X fingerprint TDS=0x%08X SOI=0x%08X CSUM=0x%08X",
              static_cast<unsigned long long>(base), static_cast<unsigned>(end - base),
              fp.TimeDateStamp, fp.SizeOfImage, fp.CheckSum);
}

void Mod::LoadConfig() {
    m_config = config::Load(ExeDir(), cameraunlock::config::DefaultsFile::PerUser());
    m_enabled.store(m_config.enableOnStartup);
    m_worldSpaceYaw.store(m_config.worldSpaceYaw);
    log::Line("Config: port=%d enableOnStartup=%d rotation=%d position=%d localSmoothing=%.2f "
              "remoteSmoothing=%.2f worldSpaceYaw=%d fovOffset=%.1f",
              m_config.udpPort, m_config.enableOnStartup, m_config.rotationEnabled,
              m_config.positionEnabled, m_config.localSmoothing, m_config.remoteSmoothing,
              m_config.worldSpaceYaw, m_config.fovOffsetDegrees);
}

void Mod::StartReceiver() {
    m_receiver.SetLog([](const std::string& msg) { log::Line("[UDP] %s", msg.c_str()); });
    const auto port = static_cast<uint16_t>(m_config.udpPort);
    if (m_receiver.Start(port)) {
        log::Line("UDP receiver listening on port %u", port);
    } else {
        log::Line("UDP receiver could not bind port %u immediately; retrying in background", port);
    }
}

void Mod::ApplyConfigToSession() {
    // Sensitivity and inversion stay at the processors' identity defaults: the
    // tracker shapes the pose, and the mod applies it as it arrives.
    cameraunlock::PositionSettings ps{};
    ps.limit_x = m_config.limitX;
    ps.limit_y = m_config.limitY;
    ps.limit_y_down = m_config.limitYDown;
    ps.limit_z = m_config.limitZ;
    ps.limit_z_back = m_config.limitZBack;
    m_session->GetPositionProcessor().SetSettings(ps);

    // After SetSettings: the session writes both smoothing values into the
    // position settings as well, so a later settings rebuild would drop them.
    // The connection flag that picks between them is fed by the session from
    // the receiver's source address every update. That wiring is compile-time
    // detected, so a receiver adapter that forgets IsRemoteConnection() would
    // pin every remote user to LocalSmoothing rather than fail to build.
    static_assert(decltype(m_session)::element_type::kHasRemoteConnection,
                  "receiver must expose IsRemoteConnection() or remote smoothing never applies");
    m_session->SetLocalSmoothing(m_config.localSmoothing);
    m_session->SetRemoteSmoothing(m_config.remoteSmoothing);

    m_session->SetMode(config::StartupTrackingMode(m_config));
}

void Mod::LogConnectionChange() {
    const bool isRemote = m_session->IsRemoteConnection();
    if (m_remoteConnectionKnown && isRemote == m_isRemoteConnection) return;
    m_remoteConnectionKnown = true;
    m_isRemoteConnection = isRemote;

    const double effective = cameraunlock::math::GetEffectiveSmoothing(
        m_config.localSmoothing, m_config.remoteSmoothing, isRemote);
    log::Line("Tracker connection is %s; smoothing=%.2f",
              isRemote ? "remote" : "local", effective);
}

// The table's hotkey codec only lets through a list this parser reads.
static std::vector<cameraunlock::input::KeyBinding> Bindings(const char* key, const std::string& list) {
    const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(list);
    if (!parsed.ok()) throw std::logic_error(std::string(key) + "='" + list + "': " + parsed.error);
    return parsed.bindings;
}

void Mod::RegisterHotkeys() {
    using cameraunlock::input::RegisterKeyBindings;

    // Each list holds every key that fires its action, the Ctrl+Shift chord
    // included. A key without modifiers stays silent while Ctrl and Shift are
    // both held, so Ctrl+Shift+<key> reaches only a binding that names the
    // chord.
    RegisterKeyBindings(m_hotkeys, Bindings("ToggleKey", m_config.toggleKey), [this]() { ToggleTracking(); });
    RegisterKeyBindings(m_hotkeys, Bindings("CycleTrackingModeKey", m_config.cycleTrackingModeKey),
                        [this]() { CycleTrackingMode(); });
    RegisterKeyBindings(m_hotkeys, Bindings("YawModeKey", m_config.yawModeKey), [this]() { ToggleYawMode(); });

    m_hotkeys.Start();
    log::Line("Hotkeys: Toggle=[%s]  Cycle mode=[%s]  Yaw mode=[%s]", m_config.toggleKey.c_str(),
              m_config.cycleTrackingModeKey.c_str(), m_config.yawModeKey.c_str());
}

// End changes this session only; EnableOnStartup decides the next one.
void Mod::ToggleTracking() {
    bool now = !m_enabled.load();
    m_enabled.store(now);
    log::Line("Tracking %s", now ? "ENABLED" : "disabled");
}

// The next mode is computed from the one the camera worker last applied, so two
// presses before it runs are one step, as they always were. The worker applies
// it; this thread saves it.
void Mod::CycleTrackingMode() {
    const auto next = static_cast<TrackingMode>((static_cast<int>(m_session->GetMode()) + 1) % 3);
    m_desiredMode.store(static_cast<int>(next));
    m_modeChangeRequested.store(true);
    config::SaveTrackingMode(next);
}

void Mod::ApplyPendingModeChange() {
    if (!m_session) return;
    if (!m_modeChangeRequested.exchange(false)) return;
    const auto mode = static_cast<TrackingMode>(m_desiredMode.load());
    m_session->SetMode(mode);
    switch (mode) {
        case TrackingMode::RotationAndPosition:
            log::Line("Tracking mode: rotation and position");
            break;
        case TrackingMode::RotationOnly:
            log::Line("Tracking mode: rotation only (position disabled)");
            break;
        case TrackingMode::PositionOnly:
            log::Line("Tracking mode: position only (rotation disabled)");
            break;
    }
}

void Mod::ToggleYawMode() {
    const bool now = !m_worldSpaceYaw.load();
    m_worldSpaceYaw.store(now);
    log::Line("Yaw mode: %s", now ? "world-space (horizon-locked)" : "camera-local");
    config::SaveWorldSpaceYaw(now);
}

}  // namespace pdht
