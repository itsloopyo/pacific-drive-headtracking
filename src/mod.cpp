// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "mod.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include "cameraunlock/input/chord_hotkeys.h"
#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/math/smoothing_utils.h"
#include "cameraunlock/memory/pe_fingerprint.h"
#include "ue4.h"
#include "version.h"

namespace pdht {

namespace log = cameraunlock::logging;
using cameraunlock::TrackingMode;

// Chord cluster letters (Ctrl+Shift+<letter>), per doctrine ordering.
static constexpr int kVkY = 0x59;  // toggle tracking
static constexpr int kVkG = 0x47;  // cycle tracking mode
static constexpr int kVkH = 0x48;  // toggle yaw mode

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

// IniReader takes a narrow path and hands it to GetPrivateProfileStringA, which
// decodes with the process ANSI code page. So the conversion has to be CP_ACP,
// not CP_UTF8: a UTF-8 path arrives as ANSI mojibake, the file is never found,
// and because Config::Load reported that identically to a genuinely absent file,
// the whole INI silently stopped applying - port, FOV offset, hotkeys, limits.
// A Steam library under a non-Latin folder name reproduces it. The log path is
// wide and so was never affected, which is what made the log look healthy.
//
// CP_ACP cannot represent every path either. WideCharToMultiByte reports that
// through the default-char flag, and a path it cannot round-trip is worth saying
// out loud rather than becoming another silent default-config session.
static std::string WideToAnsi(const std::wstring& wide, bool& lossy) {
    lossy = false;
    const int n =
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    BOOL usedDefault = FALSE;
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), n, "?", &usedDefault);
    lossy = usedDefault != FALSE;
    return out;
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
    bool lossy = false;
    const std::string iniPath =
        WideToAnsi(ExeDir() + L"\\PacificDriveHeadTracking.ini", lossy);
    if (lossy) {
        log::Line("The game directory contains characters this system's ANSI code page "
                  "cannot represent, so PacificDriveHeadTracking.ini cannot be opened by "
                  "path. The mod runs on defaults.");
    }
    m_config.Load(iniPath);
    m_enabled.store(m_config.enableOnStartup);
    m_worldSpaceYaw.store(m_config.worldSpaceYaw);
    log::Line("Config: port=%u enableOnStartup=%d position=%d localSmoothing=%.2f remoteSmoothing=%.2f "
              "worldSpaceYaw=%d",
              m_config.port, m_config.enableOnStartup, m_config.positionEnabled,
              m_config.localSmoothing, m_config.remoteSmoothing, m_config.worldSpaceYaw);
}

void Mod::StartReceiver() {
    m_receiver.SetLog([](const std::string& msg) { log::Line("[UDP] %s", msg.c_str()); });
    if (m_receiver.Start(m_config.port)) {
        log::Line("UDP receiver listening on port %u", m_config.port);
    } else {
        log::Line("UDP receiver could not bind port %u immediately; retrying in background", m_config.port);
    }
}

void Mod::ApplyConfigToSession() {
    auto& proc = m_session->GetProcessor();
    cameraunlock::SensitivitySettings sens{};
    sens.yaw = m_config.yawSensitivity;
    sens.pitch = m_config.pitchSensitivity;
    sens.roll = m_config.rollSensitivity;
    sens.invert_yaw = m_config.invertYaw;
    sens.invert_pitch = m_config.invertPitch;
    sens.invert_roll = m_config.invertRoll;
    proc.SetSensitivity(sens);

    cameraunlock::PositionSettings ps{};
    ps.sensitivity_x = m_config.positionSensitivityX;
    ps.sensitivity_y = m_config.positionSensitivityY;
    ps.sensitivity_z = m_config.positionSensitivityZ;
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

    m_session->SetMode(m_config.positionEnabled ? TrackingMode::RotationAndPosition
                                                : TrackingMode::RotationOnly);
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

void Mod::RegisterHotkeys() {
    using cameraunlock::input::ChordGuarded;
    using cameraunlock::input::NavGuarded;

    auto toggle = [this]() { ToggleTracking(); };
    auto cycleMode = [this]() { CycleTrackingMode(); };
    auto yawMode = [this]() { ToggleYawMode(); };

    m_hotkeys.AddHotkey(m_config.keyToggle, NavGuarded(toggle));
    m_hotkeys.AddHotkey(m_config.keyCycleMode, NavGuarded(cycleMode));
    m_hotkeys.AddHotkey(m_config.keyYawMode, NavGuarded(yawMode));

    m_hotkeys.AddHotkey(kVkY, ChordGuarded(toggle));
    m_hotkeys.AddHotkey(kVkG, ChordGuarded(cycleMode));
    m_hotkeys.AddHotkey(kVkH, ChordGuarded(yawMode));

    m_hotkeys.Start();
    // The keys ACTUALLY bound, not the defaults. Hardcoding the default names
    // meant a user who rebound to F10 read a log claiming Toggle=End, concluded
    // their edit had been ignored, and had no way to see that a value like
    // `KeyToggle=F1` had bound 0xF1 rather than the F1 key.
    using cameraunlock::input::VirtualKeyToString;
    log::Line("Hotkeys: Toggle=%s(0x%X)/Ctrl+Shift+Y  Cycle mode=%s(0x%X)/Ctrl+Shift+G  "
              "Yaw mode=%s(0x%X)/Ctrl+Shift+H",
              VirtualKeyToString(m_config.keyToggle), m_config.keyToggle,
              VirtualKeyToString(m_config.keyCycleMode), m_config.keyCycleMode,
              VirtualKeyToString(m_config.keyYawMode), m_config.keyYawMode);
}

void Mod::ToggleTracking() {
    bool now = !m_enabled.load();
    m_enabled.store(now);
    log::Line("Tracking %s", now ? "ENABLED" : "disabled");
}

void Mod::CycleTrackingMode() {
    m_modeCycleRequested.store(true);
}

void Mod::ApplyPendingModeChange() {
    if (!m_session) return;
    if (!m_modeCycleRequested.exchange(false)) return;
    switch (m_session->CycleMode()) {
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
}

}  // namespace pdht
