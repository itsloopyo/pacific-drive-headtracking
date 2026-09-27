// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock

// The differential test for the conversion of PacificDriveHeadTracking.ini.
//
//   Oracle     the dev pre-release's reader and startup code (21a4f1e), the
//              newest published build (oracle/oracle_reader.cpp)
//   Import     the frozen reader in src/legacy_config/, through the startup
//              code the commit that froze it ran it through
//   Migration  the config owner's Load in a folder holding only the input as
//              PacificDriveHeadTracking.ini, which imports it into a new
//              CameraUnlock.ini, then the canonical reader and table on it,
//              through this build's startup code
//
// Comparison 1, oracle against import, is what a player sees change that the
// conversion did not cause: commits since 21a4f1e that change how the file is
// read. There are none. The import's reader is 21a4f1e's with literal
// defaults, and every core source both compile holds the same bytes at
// 21a4f1e's pin and this one, so every field must agree bit for bit.
//
// Comparison 2, import against migration, is the proof for the conversion; see
// the section of that name below for what it allows.
//
// Inputs: the file the dev build shipped (its installer ZIP, its Nexus ZIP and
// its launcher manifest seed hold the same bytes), the same file with CRLF line
// ends, no file, an empty file, core's mutation corpus over the shipped file,
// the shipped file with each of the three key codes set to every code from 0x00
// to 0xFF, and the shipped file with LimitY raised and LimitYDown removed.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "config.h"
#include "legacy_config/legacy_config.h"
#include "oracle/oracle_reader.h"

#include "cameraunlock/config/canonical_ini.h"
#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/config/testing/ini_mutations.h"
#include "cameraunlock/input/key_bindings.h"

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
// game exe runs from, and Defaults.ini sits in `global` beside it. Every folder
// lives under one root for the run, removed once at the end.

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
    fs::path canonical() const { return game() / "CameraUnlock.ini"; }
    fs::path defaults() const { return root_ / "global" / "Defaults.ini"; }

    void WriteLegacy(const std::string& bytes) const { WriteFileBytes(legacy(), bytes); }
    void WriteDefaults(const std::string& bytes) const {
        fs::create_directories(defaults().parent_path());
        WriteFileBytes(defaults(), bytes);
    }

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
const char* const kShippedCrlfName = "dev shipped file, CRLF";

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
        {kShippedCrlfName, true, Crlf(Shipped())},
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

// ---- Comparison 2 ------------------------------------------------------------------
//
// The migration: the config owner's Load in a game folder holding only the input
// as PacificDriveHeadTracking.ini, which imports it through config::Import into a
// new CameraUnlock.ini, then the canonical reader and table on that file. It must
// start the mod exactly as the import did, apart from the changes core's
// data/config-format.json approves:
//
//   pose_shaping  a sensitivity or inversion the player set away from the
//                 shipped identity is dropped, and the session runs at
//                 identity. Every shipped value is identity, so nothing folds.
//   N3            a key code on Ctrl, Shift or Alt alone is unbound and keeps
//                 its Ctrl+Shift chord.
//
// The reader keeps every number finite and inside a range the canonical rows
// hold, and every key code inside 0x01-0xFE, so N1, N2 and a deferred import are
// out of reach.
//
// A row the player never changed from what the dev build shipped follows
// Defaults.ini: the import lists it in follows_defaults_ini, the tracking mode
// pair as one unit, and the migration writes it `default`. The test derives the
// untouched rows from what the frozen reader read and holds the import's list to
// them on every input. The shipped file (either line ending), an empty file and
// no file leave every row untouched and give the committed file byte for byte.
//
// Each input with a file migrates three times: over a Defaults.ini the owner
// creates with the built-in values, from a read-only PacificDriveHeadTracking.ini,
// and over a Defaults.ini that differs from the built-in value on every global
// row. The first two give the settings the import read, since the dev build
// shipped the built-in values. Over the third an untouched row takes
// Defaults.ini's value and a changed row keeps the player's.

using Drop = std::tuple<cfg::DropRule, std::string, std::string>;

// The settings the running mod acts on from a canonical Config, through this
// build's startup code: mod.cpp ApplyConfigToSession, which leaves the
// processors at identity sensitivity and inversion, and RegisterHotkeys, which
// puts each list through ParseKeyBindings and RegisterKeyBindings.
Record ObserveCanonical(const pdht::Config& c) {
    pdht_oracle::Published g;
    g.port = c.udpPort;
    g.tracking_enabled = c.enableOnStartup;
    g.world_space_yaw = c.worldSpaceYaw;
    g.yaw_sens = 1.0f;
    g.pitch_sens = 1.0f;
    g.roll_sens = 1.0f;
    g.local_smoothing = c.localSmoothing;
    g.remote_smoothing = c.remoteSmoothing;
    g.fov_offset = c.fovOffsetDegrees;
    g.tracking_mode = static_cast<int>(pdht::config::StartupTrackingMode(c));
    g.pos_sens_x = 1.0f;
    g.pos_sens_y = 1.0f;
    g.pos_sens_z = 1.0f;
    g.limit_x = c.limitX;
    g.limit_y = c.limitY;
    g.limit_y_down = c.limitYDown;
    g.limit_z = c.limitZ;
    g.limit_z_back = c.limitZBack;
    const std::pair<int, const std::string*> lists[] = {
        {pdht_oracle::kToggle, &c.toggleKey},
        {pdht_oracle::kCycleMode, &c.cycleTrackingModeKey},
        {pdht_oracle::kYawMode, &c.yawModeKey},
    };
    for (const auto& [action, list] : lists) {
        const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(*list);
        Check(parsed.ok(), "a migrated key list parses: " + *list);
        for (const cameraunlock::input::KeyBinding& b : parsed.bindings) {
            g.hotkeys.push_back({action, b.vk, static_cast<unsigned>(b.modifiers)});
        }
    }
    return ObserveOracle(g);
}

// The drops the approved changes call for, from what the frozen reader read, and
// the settings the session then runs on: comparison 2's whole allowance.
struct Allowed {
    std::vector<Drop> dropped;
    Record observed;
};

bool ModifierKey(int vk) { return (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5); }

Allowed ApplyApprovedChanges(const legacy::Config& read) {
    Allowed a;
    legacy::Config c = read;
    const auto shaping = [&a](auto& value, auto shipped, const char* section, const char* key) {
        if (value != shipped) a.dropped.push_back({cfg::DropRule::PoseShaping, section, key});
        value = shipped;
    };
    shaping(c.yawSensitivity, 1.0f, "Tracking", "YawSensitivity");
    shaping(c.pitchSensitivity, 1.0f, "Tracking", "PitchSensitivity");
    shaping(c.rollSensitivity, 1.0f, "Tracking", "RollSensitivity");
    shaping(c.invertYaw, false, "Tracking", "InvertYaw");
    shaping(c.invertPitch, false, "Tracking", "InvertPitch");
    shaping(c.invertRoll, false, "Tracking", "InvertRoll");
    shaping(c.positionSensitivityX, 1.0f, "Position", "SensitivityX");
    shaping(c.positionSensitivityY, 1.0f, "Position", "SensitivityY");
    shaping(c.positionSensitivityZ, 1.0f, "Position", "SensitivityZ");

    const auto key = [&a](int& vk, const char* name) {
        if (!ModifierKey(vk)) return;
        a.dropped.push_back({cfg::DropRule::ModifierKey, "Controls", name});
        vk = 0;
    };
    key(c.keyToggle, "KeyToggle");
    key(c.keyCycleMode, "KeyCycleMode");
    key(c.keyYawMode, "KeyYawMode");

    std::sort(a.dropped.begin(), a.dropped.end());
    a.observed = ObserveImport(c);
    return a;
}

std::vector<std::string> CanonicalDiagnostics(const std::string& bytes, pdht::Config& out) {
    std::vector<std::string> found;
    const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(bytes);
    for (const cfg::CanonicalDiagnostic& d : doc.diagnostics) found.push_back("reader: " + cfg::DescribeCanonicalDiagnostic(d));
    const cfg::ConfigTable<pdht::Config> table = pdht::config::Table();
    out = table.defaults();
    for (const cfg::CanonicalDiagnostic& d : cfg::ApplyCanonical(doc, table, out).diagnostics) {
        found.push_back("table: " + cfg::DescribeCanonicalDiagnostic(d));
    }
    return found;
}

bool AsciiCrlf(const std::string& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c > 0x7E) return false;
        if (c == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n')) return false;
        if (c == '\n' && (i == 0 || bytes[i - 1] != '\r')) return false;
        if (c < 0x20 && c != '\r' && c != '\n') return false;
    }
    return !bytes.empty() && bytes.back() == '\n';
}

// A file's bytes, last write time and attributes, which no load may change.
struct FileState {
    std::string bytes;
    unsigned long long written = 0;
    DWORD attributes = 0;
    bool operator==(const FileState& other) const {
        return bytes == other.bytes && written == other.written && attributes == other.attributes;
    }
};

std::optional<FileState> StateOf(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
        throw std::runtime_error("cannot read the attributes of " + path.string());
    }
    FileState state;
    state.bytes = ReadFileBytes(path);
    state.written = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    state.attributes = data.dwFileAttributes;
    return state;
}

std::set<std::string> Names(const fs::path& folder) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder)) names.insert(entry.path().filename().string());
    return names;
}

bool LogSays(const std::vector<std::string>& log, const std::string& text) {
    for (const std::string& line : log) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

// A Defaults.ini holding a value other than the built-in one on every global row
// the table binds, so a migration that wrote `default` where the imported value
// is not what `default` gives would read back differently over it.
const char* const kSkewedDefaults =
    "[CameraUnlock]\r\nConfigFormat=1\r\n\r\n"
    "[Network]\r\nUdpPort=5252\r\n\r\n"
    "[General]\r\nEnableOnStartup=false\r\nWorldSpaceYaw=false\r\nRotationEnabled=false\r\n\r\n"
    "[Smoothing]\r\nLocalSmoothing=0.5\r\nRemoteSmoothing=0.5\r\n\r\n"
    "[Position]\r\nPositionEnabled=true\r\nPositionLimitX=0.5\r\nPositionLimitY=0.5\r\nPositionLimitYDown=0.5\r\n"
    "PositionLimitZ=0.5\r\nPositionLimitZBack=0.5\r\n\r\n"
    "[Hotkeys]\r\nToggleKey=F8\r\nCycleTrackingModeKey=F9\r\nYawModeKey=F10\r\n";

using cfg::schema::Concept;

// Every row the table binds that follows Defaults.ini.
const std::set<Concept>& GlobalRows() {
    static const std::set<Concept> rows = {
        Concept::UdpPort,          Concept::EnableOnStartup,    Concept::WorldSpaceYaw,
        Concept::RotationEnabled,  Concept::PositionEnabled,    Concept::LocalSmoothing,
        Concept::RemoteSmoothing,  Concept::PositionLimitX,     Concept::PositionLimitY,
        Concept::PositionLimitYDown, Concept::PositionLimitZ,   Concept::PositionLimitZBack,
        Concept::ToggleKey,        Concept::CycleTrackingModeKey, Concept::YawModeKey,
    };
    return rows;
}

// The rows the player never changed from what the dev build shipped, the mode
// pair as one unit.
std::set<Concept> UntouchedRows(const legacy::Config& l) {
    const legacy::Config d;
    std::set<Concept> changed;
    const auto differs = [&changed](bool different, std::initializer_list<Concept> rows) {
        if (different) changed.insert(rows);
    };
    differs(l.port != d.port, {Concept::UdpPort});
    differs(l.enableOnStartup != d.enableOnStartup, {Concept::EnableOnStartup});
    differs(l.worldSpaceYaw != d.worldSpaceYaw, {Concept::WorldSpaceYaw});
    differs(l.positionEnabled != d.positionEnabled, {Concept::RotationEnabled, Concept::PositionEnabled});
    differs(l.localSmoothing != d.localSmoothing, {Concept::LocalSmoothing});
    differs(l.remoteSmoothing != d.remoteSmoothing, {Concept::RemoteSmoothing});
    differs(l.limitX != d.limitX, {Concept::PositionLimitX});
    differs(l.limitY != d.limitY, {Concept::PositionLimitY});
    differs(l.limitYDown != d.limitYDown, {Concept::PositionLimitYDown});
    differs(l.limitZ != d.limitZ, {Concept::PositionLimitZ});
    differs(l.limitZBack != d.limitZBack, {Concept::PositionLimitZBack});
    differs(l.keyToggle != d.keyToggle, {Concept::ToggleKey});
    differs(l.keyCycleMode != d.keyCycleMode, {Concept::CycleTrackingModeKey});
    differs(l.keyYawMode != d.keyYawMode, {Concept::YawModeKey});
    std::set<Concept> untouched;
    for (const Concept row : GlobalRows()) {
        if (changed.count(row) == 0) untouched.insert(row);
    }
    return untouched;
}

std::string ConceptNames(const std::set<Concept>& rows) {
    std::string text;
    for (const Concept row : rows) {
        text += (text.empty() ? "" : ", ") + std::string(cfg::schema::kConcepts[static_cast<std::size_t>(row)].name);
    }
    return text.empty() ? "none" : text;
}

// `row`'s field copied from `from` into `to`.
void CopyRow(Concept row, const pdht::Config& from, pdht::Config& to) {
    switch (row) {
        case Concept::UdpPort: to.udpPort = from.udpPort; break;
        case Concept::EnableOnStartup: to.enableOnStartup = from.enableOnStartup; break;
        case Concept::WorldSpaceYaw: to.worldSpaceYaw = from.worldSpaceYaw; break;
        case Concept::RotationEnabled: to.rotationEnabled = from.rotationEnabled; break;
        case Concept::PositionEnabled: to.positionEnabled = from.positionEnabled; break;
        case Concept::LocalSmoothing: to.localSmoothing = from.localSmoothing; break;
        case Concept::RemoteSmoothing: to.remoteSmoothing = from.remoteSmoothing; break;
        case Concept::PositionLimitX: to.limitX = from.limitX; break;
        case Concept::PositionLimitY: to.limitY = from.limitY; break;
        case Concept::PositionLimitYDown: to.limitYDown = from.limitYDown; break;
        case Concept::PositionLimitZ: to.limitZ = from.limitZ; break;
        case Concept::PositionLimitZBack: to.limitZBack = from.limitZBack; break;
        case Concept::ToggleKey: to.toggleKey = from.toggleKey; break;
        case Concept::CycleTrackingModeKey: to.cycleTrackingModeKey = from.cycleTrackingModeKey; break;
        case Concept::YawModeKey: to.yawModeKey = from.yawModeKey; break;
        default: throw std::logic_error("no field for a row the table does not bind");
    }
}

// The settings kSkewedDefaults gives, each checked to differ from the built-in
// value, the tracking mode pair taken together.
pdht::Config SkewedConfig() {
    pdht::Config skewed;
    const std::vector<std::string> diagnostics = CanonicalDiagnostics(kSkewedDefaults, skewed);
    if (!diagnostics.empty()) throw std::logic_error("the skewed Defaults.ini draws " + diagnostics.front());
    const pdht::Config builtin = pdht::config::Table().defaults();
    for (const Concept row : GlobalRows()) {
        pdht::Config probe = builtin;
        CopyRow(row, skewed, probe);
        if (row == Concept::RotationEnabled || row == Concept::PositionEnabled) {
            CopyRow(Concept::RotationEnabled, skewed, probe);
            CopyRow(Concept::PositionEnabled, skewed, probe);
        }
        if (Differences(ObserveCanonical(probe), ObserveCanonical(builtin)).empty()) {
            throw std::logic_error(std::string("the skewed Defaults.ini leaves ") +
                                   cfg::schema::kConcepts[static_cast<std::size_t>(row)].name + " at the built-in value");
        }
    }
    return skewed;
}

// The folder beside this executable the migrated files are written to, for
// lint-migrated.mjs, which CTest runs after this test.
fs::path MigratedFolder() {
    std::vector<wchar_t> exe(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) throw std::runtime_error("cannot find this executable's path");
        if (length < exe.size()) return fs::path(std::wstring(exe.data(), length)).parent_path() / "migrated";
        exe.resize(exe.size() * 2);
    }
}

cfg::ConfigOwnerOptions<pdht::Config> OwnerOptions(const Scratch& s) {
    return pdht::config::OwnerOptions(s.game().wstring(), cfg::DefaultsFile::At(s.defaults().wstring()));
}

// Runs the owner's Load in `s`, whose game folder holds the input as
// PacificDriveHeadTracking.ini or nothing, checks what a load must do beyond
// comparison 2, and returns the settings the session runs on. A file it creates
// by migrating goes into `migratedFiles`.
pdht::Config Migrate(const Input& input, const Scratch& s, const std::string& label,
                     std::set<std::string>& migratedFiles) {
    const std::optional<FileState> legacyBefore = StateOf(s.legacy());
    const std::optional<FileState> defaultsBefore = StateOf(s.defaults());

    const cfg::ConfigLoadResult<pdht::Config> loaded = cfg::ConfigOwner<pdht::Config>(OwnerOptions(s)).Load();
    const cfg::ConfigLoadStatus want = input.present ? cfg::ConfigLoadStatus::Migrated : cfg::ConfigLoadStatus::Created;
    if (loaded.status != want) {
        std::printf("  %s: %s, %s\n", label.c_str(), cfg::ConfigLoadStatusName(loaded.status), loaded.reason.c_str());
    }
    Check(loaded.status == want, label + ": the load is " + cfg::ConfigLoadStatusName(want));
    Check(StateOf(s.legacy()) == legacyBefore,
          label + ": a load leaves PacificDriveHeadTracking.ini's bytes, write time and attributes");
    Check(!defaultsBefore || StateOf(s.defaults()) == defaultsBefore, label + ": a load leaves Defaults.ini as it was");
    if (loaded.status != want) return loaded.config;

    Check(Names(s.game()) == (input.present ? std::set<std::string>{"CameraUnlock.ini", "PacificDriveHeadTracking.ini"}
                                            : std::set<std::string>{"CameraUnlock.ini"}),
          label + ": the game folder holds PacificDriveHeadTracking.ini and CameraUnlock.ini and nothing else");

    const std::string migrated = ReadFileBytes(s.canonical());
    Check(cfg::HasCanonicalStamp(migrated), label + ": CameraUnlock.ini carries the stamp");
    Check(AsciiCrlf(migrated), label + ": CameraUnlock.ini is ASCII with CRLF line ends");
    pdht::Config reread;
    const std::vector<std::string> diagnostics = CanonicalDiagnostics(migrated, reread);
    for (const std::string& d : diagnostics) std::printf("  %s: CameraUnlock.ini, %s\n", label.c_str(), d.c_str());
    Check(diagnostics.empty(), label + ": CameraUnlock.ini reads with no diagnostic");
    if (input.present) migratedFiles.insert(migrated);

    // The next start reads CameraUnlock.ini, imports nothing and writes nothing.
    const std::optional<FileState> created = StateOf(s.canonical());
    const cfg::ConfigLoadResult<pdht::Config> again = cfg::ConfigOwner<pdht::Config>(OwnerOptions(s)).Load();
    Check(again.status == cfg::ConfigLoadStatus::Canonical, label + ": the next start reads CameraUnlock.ini");
    Check(Differences(ObserveCanonical(again.config), ObserveCanonical(loaded.config)).empty(),
          label + ": the next start runs on the same settings");
    Check(StateOf(s.canonical()) == created && StateOf(s.legacy()) == legacyBefore,
          label + ": the next start changes neither file");
    Check(!input.present || LogSays(again.log, "is left as it was and is not read"),
          label + ": the next start logs that PacificDriveHeadTracking.ini is not read");
    return loaded.config;
}

void ImportAgainstMigration(const std::vector<Input>& inputs) {
    const std::string committed = ReadFileBytes(fs::path(PDHT_SOURCE_DIR) / "PacificDriveHeadTracking.ini");
    const cfg::ConfigTable<pdht::Config> table = pdht::config::Table();
    const pdht::Config skewedValues = SkewedConfig();
    std::set<std::string> migratedFiles;
    int compared = 0;
    int dropping = 0;
    int allUntouched = 0;
    int modeChanged = 0;
    int modifierKeys = 0;
    for (const Input& input : inputs) {
        const std::string& name = input.name;

        // The import, run as the owner runs it but on a read-only copy: it reads
        // what the frozen reader reads, drops what the approved changes drop, and
        // writes nothing.
        cfg::ImportResult imported;
        legacy::Config read;
        {
            Scratch ro;
            if (input.present) {
                ro.WriteLegacy(input.bytes);
                SetFileAttributesW(ro.legacy().c_str(), FILE_ATTRIBUTE_READONLY);
            }
            const std::optional<FileState> before = StateOf(ro.legacy());
            pdht::Config unused = table.defaults();
            imported = pdht::config::Import().run({ro.legacy().wstring(), ro.legacy().string(), false}, unused);
            const std::set<std::string> left = input.present ? std::set<std::string>{"PacificDriveHeadTracking.ini"}
                                                             : std::set<std::string>{};
            Check(StateOf(ro.legacy()) == before && Names(ro.game()) == left,
                  name + ": the import leaves a read-only folder as it was");
            legacy::Load(ro.legacy().string(), read);
        }
        Check(imported.status == (input.present ? cfg::ImportStatus::Imported : cfg::ImportStatus::Absent),
              name + ": the import reads every input, as the published build did");

        const std::set<Concept> follows(imported.follows_defaults_ini.begin(), imported.follows_defaults_ini.end());
        Check(follows.size() == imported.follows_defaults_ini.size(), name + ": follows_defaults_ini names a row twice");
        const std::set<Concept> untouched = UntouchedRows(read);
        Check(follows == untouched, name + ": follows Defaults.ini " + ConceptNames(follows) + ", the player left " +
                                        ConceptNames(untouched) + " untouched");
        if (untouched == GlobalRows()) ++allUntouched;
        if (untouched.count(Concept::RotationEnabled) == 0) ++modeChanged;
        const bool unedited = name == kShippedName || name == kShippedCrlfName || name == "no file" ||
                              name == "empty file";
        if (unedited) Check(untouched == GlobalRows(), name + ": a file no player edited leaves a row changed");

        const Allowed allowed = ApplyApprovedChanges(read);
        if (!allowed.dropped.empty()) ++dropping;
        for (const Drop& d : allowed.dropped) {
            if (std::get<0>(d) == cfg::DropRule::ModifierKey) ++modifierKeys;
        }
        std::vector<Drop> dropped;
        for (const cfg::DroppedValue& d : imported.dropped) dropped.push_back({d.rule, d.section, d.key});
        std::sort(dropped.begin(), dropped.end());
        Check(dropped == allowed.dropped, name + ": the import drops exactly what the approved changes drop");
        Check(imported.pose_shaping.size() == 9, name + ": the import records all nine pose-shaping settings");
        for (const cfg::PoseShapingValue& p : imported.pose_shaping) {
            const bool changed = std::find(allowed.dropped.begin(), allowed.dropped.end(),
                                           Drop{cfg::DropRule::PoseShaping, p.section, p.key}) != allowed.dropped.end();
            Check(p.folded != changed, name + ": [" + p.section + "] " + p.key + " folds exactly when it is the shipped value");
        }

        // Over a Defaults.ini the owner creates with the built-in values.
        pdht::Config migrated;
        {
            Scratch s;
            if (input.present) s.WriteLegacy(input.bytes);
            migrated = Migrate(input, s, name, migratedFiles);
            const std::vector<std::string> diff = Differences(allowed.observed, ObserveCanonical(migrated));
            for (const std::string& d : diff) std::printf("  comparison 2, %s: %s\n", name.c_str(), d.c_str());
            Check(diff.empty(), name + ": comparison 2, the migration runs as the import read, less the approved changes");

            // Over the built-in values the table's own defaults stand for Defaults.ini.
            const std::string bytes = ReadFileBytes(s.canonical());
            pdht::Config reread;
            CanonicalDiagnostics(bytes, reread);
            Check(Differences(ObserveCanonical(reread), ObserveCanonical(migrated)).empty(),
                  name + ": CameraUnlock.ini reads back as the settings the session runs on");
            for (const Concept row : follows) {
                const std::string key = cfg::schema::kConcepts[static_cast<std::size_t>(row)].key;
                Check(bytes.find("\r\n" + key + "=default\r\n") != std::string::npos,
                      name + ": " + key + " is not written default");
            }

            // Fresh equals upgrade: the file the dev build shipped, an empty file
            // and no file at all end as the committed file.
            if (unedited) {
                Check(bytes == committed, name + ": gives the committed file byte for byte");
            }
        }

        if (!input.present) {
            ++compared;
            continue;
        }

        // From a read-only PacificDriveHeadTracking.ini, which keeps its attribute.
        {
            Scratch ro;
            ro.WriteLegacy(input.bytes);
            SetFileAttributesW(ro.legacy().c_str(), FILE_ATTRIBUTE_READONLY);
            const pdht::Config c = Migrate(input, ro, name + " (read-only)", migratedFiles);
            Check(Differences(allowed.observed, ObserveCanonical(c)).empty(),
                  name + ": a read-only PacificDriveHeadTracking.ini imports as a writable one does");
            Check((GetFileAttributesW(ro.legacy().c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
                  name + ": PacificDriveHeadTracking.ini keeps its read-only attribute");
        }

        // Over a Defaults.ini that differs everywhere, an untouched row takes its
        // value and a changed row keeps the player's.
        {
            Scratch skewed;
            skewed.WriteLegacy(input.bytes);
            skewed.WriteDefaults(kSkewedDefaults);
            const pdht::Config c = Migrate(input, skewed, name + " (skewed Defaults.ini)", migratedFiles);
            pdht::Config want = migrated;
            for (const Concept row : follows) CopyRow(row, skewedValues, want);
            const std::vector<std::string> diff = Differences(ObserveCanonical(want), ObserveCanonical(c));
            for (const std::string& d : diff) std::printf("  comparison 2, %s (skewed Defaults.ini): %s\n", name.c_str(), d.c_str());
            Check(diff.empty(), name + ": over a Defaults.ini that differs everywhere, the untouched rows take its "
                                       "values and the changed rows keep the player's");
        }
        ++compared;
    }
    std::printf("comparison 2: %d inputs, %d with a value the approved changes drop\n", compared, dropping);
    Check(dropping > 0, "the inputs reach the approved drops");
    std::printf("  %d inputs left every row at the dev build's default, %d changed the tracking mode, %d put a key on "
                "a modifier key alone\n", allUntouched, modeChanged, modifierKeys);
    Check(allUntouched > 0 && modeChanged > 0 && allUntouched < compared,
          "the inputs both leave rows untouched and change them, the tracking mode among them");
    Check(modifierKeys > 0, "the inputs reach a key code on a modifier key alone");

    // Core's canonical config lint runs over these next (lint-migrated.mjs).
    const fs::path lint = MigratedFolder();
    fs::remove_all(lint);
    fs::create_directories(lint);
    std::size_t n = 0;
    for (const std::string& file : migratedFiles) {
        WriteFileBytes(lint / (std::to_string(n++) + ".ini"), file);
    }
    std::printf("%zu distinct migrated files written to %s\n", migratedFiles.size(), lint.string().c_str());
}

}  // namespace

int main() {
    // Unbuffered, so the lines before an exception reach the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::vector<Input> inputs = Inputs();
        Compare(inputs);
        ImportAgainstMigration(inputs);
        RemoveTree(ScratchRoot());
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
