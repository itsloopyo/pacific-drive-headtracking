// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock

// The differential test for the conversion of PacificDriveHeadTracking.ini.
//
//   Oracle     the dev pre-release's reader and startup code (21a4f1e), the
//              newest published build (oracle/oracle_reader.cpp)
//   Import     the frozen reader in src/legacy_config/, through the startup
//              code the commit that froze it ran it through
//
// Comparison 1, oracle against import, is what a player sees change that the
// conversion did not cause: commits since 21a4f1e that change how the file is
// read. There are none. The import's reader is 21a4f1e's with literal
// defaults, and every core source both compile holds the same bytes at
// 21a4f1e's pin and this one, so every field must agree bit for bit.
//
// Inputs: the file the dev build shipped (its installer ZIP, its Nexus ZIP and
// its launcher manifest seed hold the same bytes), the same file with CRLF line
// ends, no file, an empty file, core's mutation corpus over the shipped file,
// the shipped file with each of the three key codes set to every code from 0x00
// to 0xFF, and the shipped file with LimitY raised and LimitYDown removed.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "legacy_config/legacy_config.h"
#include "oracle/oracle_reader.h"

#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/config/testing/ini_mutations.h"

namespace {

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;
namespace legacy = pdht::legacy;
namespace testing = cameraunlock::config::testing;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
}

std::string ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

// ---- Scratch folders ---------------------------------------------------------
//
// One folder per reading: GetPrivateProfileString, which every reader here sits
// on, is free to cache the file it last read. `game` stands for the folder the
// game exe runs from. Every folder lives under one root for the run, removed
// once at the end.

void RemoveTree(const fs::path& root) {
    if (!fs::exists(root)) return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    fs::remove_all(root);
}

const fs::path& ScratchRoot() {
    static const fs::path root = [] {
        wchar_t temp[MAX_PATH + 1] = {};
        if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
        fs::path r = fs::path(temp) / ("pdht_diff_" + std::to_string(GetCurrentProcessId()));
        RemoveTree(r);
        return r;
    }();
    return root;
}

class Scratch {
public:
    Scratch() {
        static unsigned s_next = 0;
        root_ = ScratchRoot() / std::to_string(s_next++);
        fs::create_directories(root_ / "game");
    }

    fs::path game() const { return root_ / "game"; }
    fs::path legacy() const { return game() / "PacificDriveHeadTracking.ini"; }

    void WriteLegacy(const std::string& bytes) const { WriteFileBytes(legacy(), bytes); }

private:
    fs::path root_;
};

// ---- What a reading does -------------------------------------------------------
//
// A Record names everything the running mod acts on after reading the file:
// `field.*` the settings, `start.*` the state the session starts in, `hotkey.*`
// the bindings that can fire, each as `modifiers:code` (Ctrl 1, Shift 2, as
// cameraunlock::input::KeyModifiers numbers them) in ascending order. Floats are
// their bits.

using Record = std::map<std::string, std::string>;

std::string Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(bits));
    return text;
}

std::string Flag(bool value) { return value ? "1" : "0"; }

const char* const kActionNames[] = {"Toggle", "CycleTrackingMode", "YawMode"};

// The bindings a set of HotkeyPoller registrations can fire. The poller skips
// code 0, and GetAsyncKeyState reports no code above 0xFF or below 0 down.
void AddHotkeys(Record& r, const std::vector<pdht_oracle::Registration>& registrations) {
    std::map<int, std::vector<std::pair<unsigned, int>>> byAction;
    for (int action = 0; action < 3; ++action) byAction[action];
    for (const auto& [action, vk, modifiers] : registrations) {
        if (vk < 0x01 || vk > 0xFF) continue;
        byAction[action].push_back({modifiers, vk});
    }
    for (auto& [action, items] : byAction) {
        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        std::string text;
        for (const auto& [modifiers, vk] : items) {
            char item[32];
            std::snprintf(item, sizeof(item), "%s%u:0x%02X", text.empty() ? "" : " ", modifiers, static_cast<unsigned>(vk));
            text += item;
        }
        r[std::string("hotkey.") + kActionNames[action]] = text;
    }
}

const char* ModeName(int mode) {
    switch (mode) {
        case 0: return "RotationAndPosition";
        case 1: return "RotationOnly";
        case 2: return "PositionOnly";
        default: return "none";
    }
}

Record ObserveOracle(const pdht_oracle::Published& g) {
    Record r;
    r["field.port"] = std::to_string(g.port);
    r["field.rot.yaw_sensitivity"] = Bits(g.yaw_sens);
    r["field.rot.pitch_sensitivity"] = Bits(g.pitch_sens);
    r["field.rot.roll_sensitivity"] = Bits(g.roll_sens);
    r["field.rot.invert_yaw"] = Flag(g.invert_yaw);
    r["field.rot.invert_pitch"] = Flag(g.invert_pitch);
    r["field.rot.invert_roll"] = Flag(g.invert_roll);
    r["field.local_smoothing"] = Bits(g.local_smoothing);
    r["field.remote_smoothing"] = Bits(g.remote_smoothing);
    r["field.fov_offset"] = Bits(g.fov_offset);
    r["field.pos.sensitivity_x"] = Bits(g.pos_sens_x);
    r["field.pos.sensitivity_y"] = Bits(g.pos_sens_y);
    r["field.pos.sensitivity_z"] = Bits(g.pos_sens_z);
    r["field.pos.limit_x"] = Bits(g.limit_x);
    r["field.pos.limit_y"] = Bits(g.limit_y);
    r["field.pos.limit_y_down"] = Bits(g.limit_y_down);
    r["field.pos.limit_z"] = Bits(g.limit_z);
    r["field.pos.limit_z_back"] = Bits(g.limit_z_back);
    r["start.enabled"] = Flag(g.tracking_enabled);
    r["start.mode"] = ModeName(g.tracking_mode);
    r["start.world_space_yaw"] = Flag(g.world_space_yaw);
    AddHotkeys(r, g.hotkeys);
    return r;
}

// The frozen reader's settings through the startup code of the commit that
// froze it, which is 21a4f1e's: src/mod.cpp and src/camera_hook.cpp were
// unchanged since the dev build.
Record ObserveImport(const legacy::Config& c) {
    pdht_oracle::Published g;
    g.port = c.port;
    g.tracking_enabled = c.enableOnStartup;
    g.world_space_yaw = c.worldSpaceYaw;
    g.yaw_sens = c.yawSensitivity;
    g.pitch_sens = c.pitchSensitivity;
    g.roll_sens = c.rollSensitivity;
    g.invert_yaw = c.invertYaw;
    g.invert_pitch = c.invertPitch;
    g.invert_roll = c.invertRoll;
    g.local_smoothing = c.localSmoothing;
    g.remote_smoothing = c.remoteSmoothing;
    g.fov_offset = c.fovOffsetDegrees;
    g.tracking_mode = c.positionEnabled ? 0 : 1;
    g.pos_sens_x = c.positionSensitivityX;
    g.pos_sens_y = c.positionSensitivityY;
    g.pos_sens_z = c.positionSensitivityZ;
    g.limit_x = c.limitX;
    g.limit_y = c.limitY;
    g.limit_y_down = c.limitYDown;
    g.limit_z = c.limitZ;
    g.limit_z_back = c.limitZBack;
    g.hotkeys = {{pdht_oracle::kToggle, c.keyToggle, 0},
                 {pdht_oracle::kCycleMode, c.keyCycleMode, 0},
                 {pdht_oracle::kYawMode, c.keyYawMode, 0},
                 {pdht_oracle::kToggle, 0x59, 3},
                 {pdht_oracle::kCycleMode, 0x47, 3},
                 {pdht_oracle::kYawMode, 0x48, 3}};
    return ObserveOracle(g);
}

std::vector<std::string> Differences(const Record& a, const Record& b) {
    std::vector<std::string> out;
    for (const auto& [name, value] : a) {
        const auto it = b.find(name);
        if (it == b.end()) {
            out.push_back(name + " only on the left");
        } else if (it->second != value) {
            out.push_back(name + ": " + value + " / " + it->second);
        }
    }
    for (const auto& [name, value] : b) {
        if (a.find(name) == a.end()) out.push_back(name + " only on the right");
    }
    return out;
}

// ---- Inputs --------------------------------------------------------------------

fs::path DataPath(const char* name) {
    return fs::path(PDHT_SOURCE_DIR) / "tests" / "config_differential" / "data" / name;
}

// What the dev build shipped: the file every player who installed it holds,
// edited or not.
std::string Shipped() { return ReadFileBytes(DataPath("shipped-dev.ini")); }

const char* const kShippedName = "dev shipped file";

std::string Crlf(const std::string& lf) {
    std::string out;
    for (const char c : lf) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// Every key the frozen reader reads, and how the corpus varies each one. The
// reader refuses a port outside 1024-65535 and a key code outside 0x01-0xFE,
// and clamps the rotation sensitivities into 0.1-3, the position ones into 0-5,
// the limits into 0.01-0.5, the smoothing pair into 0-1 and FovOffset into -40
// to 60.
std::vector<testing::MutationKey> CorpusKeys() {
    return {
        {"Network", "Port", "5771", {"80", "70000"}},
        {"Tracking", "EnableOnStartup", "0", {}},
        {"Tracking", "YawSensitivity", "0.5", {"0.05", "4"}},
        {"Tracking", "PitchSensitivity", "0.5", {"0.05", "4"}},
        {"Tracking", "RollSensitivity", "0.5", {"0.05", "4"}},
        {"Tracking", "InvertYaw", "1", {}},
        {"Tracking", "InvertPitch", "1", {}},
        {"Tracking", "InvertRoll", "1", {}},
        {"Tracking", "WorldSpaceYaw", "0", {}},
        {"Tracking", "LocalSmoothing", "0.3", {"-0.5", "1.5"}},
        {"Tracking", "RemoteSmoothing", "0.6", {"-0.5", "1.5"}},
        {"Position", "Enabled", "0", {}},
        {"Position", "SensitivityX", "0.5", {"-1", "6"}},
        {"Position", "SensitivityY", "0.5", {"-1", "6"}},
        {"Position", "SensitivityZ", "0.5", {"-1", "6"}},
        {"Position", "LimitX", "0.45", {"0.001", "0.8"}},
        {"Position", "LimitY", "0.35", {"0.001", "0.8"}},
        {"Position", "LimitYDown", "0.15", {"0.001", "0.8"}},
        {"Position", "LimitZ", "0.25", {"0.001", "0.8"}},
        {"Position", "LimitZBack", "0.15", {"0.001", "0.8"}},
        {"Camera", "FovOffset", "15.0", {"-45", "75"}},
        {"Controls", "KeyToggle", "0x2E", {"0x100"}, true},
        {"Controls", "KeyCycleMode", "0x2D", {"0x100"}, true},
        {"Controls", "KeyYawMode", "0x24", {"0x100"}, true},
    };
}

std::vector<cfg::LegacyKey> CorpusReads() {
    std::vector<cfg::LegacyKey> keys;
    for (const legacy::Key& key : legacy::ReadKeys()) keys.push_back({key.section, key.key});
    return keys;
}

struct Input {
    std::string name;
    bool present;
    std::string bytes;
};

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) throw std::logic_error("'" + from + "' is not in the text");
    return text.replace(at, from.size(), to);
}

std::vector<Input> Inputs() {
    std::vector<Input> inputs = {
        {kShippedName, true, Shipped()},
        {"dev shipped file, CRLF", true, Crlf(Shipped())},
        {"no file", false, {}},
        {"empty file", true, {}},
        {"LimitY=0.35 without LimitYDown", true,
         Replace(Replace(Shipped(), "LimitY=0.20\n", "LimitY=0.35\n"), "LimitYDown=0.20\n", "")},
    };
    for (testing::IniMutation& m : testing::GenerateIniMutations(Shipped(), CorpusReads(), CorpusKeys())) {
        inputs.push_back({"corpus: " + m.name, true, std::move(m.bytes)});
    }
    for (const char* key : {"KeyToggle=0x23", "KeyCycleMode=0x21", "KeyYawMode=0x22"}) {
        const std::string name(key, std::strchr(key, '=') - key);
        for (int code = 0x00; code <= 0xFF; ++code) {
            char text[48];
            std::snprintf(text, sizeof(text), "%s=0x%02X", name.c_str(), static_cast<unsigned>(code));
            inputs.push_back({text, true, Replace(Shipped(), key, text)});
        }
    }
    return inputs;
}

// ---- Comparison 1 ------------------------------------------------------------------

void Compare(const std::vector<Input>& inputs) {
    int compared = 0;
    for (const Input& input : inputs) {
        const std::string& name = input.name;
        Scratch s;
        if (input.present) s.WriteLegacy(input.bytes);
        const Record oracle = ObserveOracle(pdht_oracle::Read(s.legacy().string()));
        Check(input.present ? ReadFileBytes(s.legacy()) == input.bytes : !fs::exists(s.legacy()),
              name + ": the dev build leaves the file as it was and writes none where there was none");

        Scratch t;
        if (input.present) t.WriteLegacy(input.bytes);
        legacy::Config read;
        const bool present = legacy::Load(t.legacy().string(), read);
        Check(present == input.present, name + ": the frozen reader finds the file exactly when it is there");
        Check(input.present || !fs::exists(t.legacy()), name + ": the frozen reader writes no file");
        const Record imported = ObserveImport(read);

        const std::vector<std::string> diff = Differences(oracle, imported);
        for (const std::string& d : diff) std::printf("  comparison 1, %s: %s\n", name.c_str(), d.c_str());
        Check(diff.empty(), name + ": comparison 1, the oracle and the import agree");
        ++compared;
    }
    std::printf("comparison 1: %d inputs\n", compared);
}

}  // namespace

int main() {
    // Unbuffered, so the lines before an exception reach the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::vector<Input> inputs = Inputs();
        Compare(inputs);
        RemoveTree(ScratchRoot());
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
