// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
//
// The INI is the only place a user hands this mod raw numbers, and everything
// downstream of Config::Load trusts them: sensitivities multiply into the head
// pose, the limits become the clamp bounds in PositionProcessor, and the pose
// ends up in the renderer's ViewRotationMatrix and ViewOrigin. Nothing further
// down re-checks any of it - InjectHeadPose's orthonormality guard only tests
// the CLEAN basis it read back from the engine, so a non-finite pose is written
// out every frame with no log line and nothing to see but a broken view.
//
// So these are boundary tests. Each writes a real INI, loads it, and asserts the
// value that reaches the rest of the mod. Deterministic: temp files are written
// and deleted per case, and no game is involved.
//
// Built only when PDHT_BUILD_TESTS=ON.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "config.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    if (std::isfinite(actual) && std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("FAIL: %s (got %g, expected %g)\n", what, static_cast<double>(actual),
                static_cast<double>(expected));
}

void CheckEq(int actual, int expected, const char* what) {
    if (actual == expected) return;
    ++g_failures;
    std::printf("FAIL: %s (got %d, expected %d)\n", what, actual, expected);
}

// GetPrivateProfileString, which IniReader is built on, only reads a real file
// on disk, so each case gets one. Named per case so a crashed run leaves
// something identifiable behind rather than clobbering a shared name.
class TempIni {
public:
    explicit TempIni(const char* caseName, const std::string& body) {
        char dir[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, dir);
        m_path = std::string(dir) + "pdht_test_" + caseName + ".ini";
        FILE* f = nullptr;
        fopen_s(&f, m_path.c_str(), "wb");
        if (!f) {
            ++g_failures;
            std::printf("FAIL: could not write %s\n", m_path.c_str());
            return;
        }
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
    }
    ~TempIni() { DeleteFileA(m_path.c_str()); }

    TempIni(const TempIni&) = delete;
    TempIni& operator=(const TempIni&) = delete;

    const std::string& Path() const { return m_path; }

private:
    std::string m_path;
};

pdht::Config LoadFrom(const char* caseName, const std::string& body) {
    TempIni ini(caseName, body);
    pdht::Config config;
    config.Load(ini.Path());
    return config;
}

// Nothing in a healthy config may be touched. This is the test that keeps the
// validation from quietly becoming a behaviour change.
void TestValidConfigIsUntouched() {
    std::printf("-- a valid config passes through unchanged\n");
    const pdht::Config c = LoadFrom("valid",
        "[Network]\n"
        "Port=5555\n"
        "[Tracking]\n"
        "EnableOnStartup=false\n"
        "YawSensitivity=1.5\n"
        "PitchSensitivity=0.8\n"
        "RollSensitivity=2.25\n"
        "InvertPitch=true\n"
        "WorldSpaceYaw=false\n"
        "LocalSmoothing=0.05\n"
        "RemoteSmoothing=0.4\n"
        "[Position]\n"
        "Enabled=false\n"
        "SensitivityX=2.0\n"
        "SensitivityY=0.5\n"
        "SensitivityZ=3.5\n"
        "LimitX=0.25\n"
        "LimitY=0.15\n"
        "LimitZ=0.45\n"
        "LimitZBack=0.08\n"
        "[Camera]\n"
        "FovOffset=12.5\n"
        "[Controls]\n"
        "KeyToggle=0x70\n"
        "KeyCycleMode=0x71\n"
        "KeyYawMode=0x72\n");

    CheckEq(c.port, 5555, "an in-range port is kept");
    Check(!c.enableOnStartup, "EnableOnStartup=false is kept");
    CheckNear(c.yawSensitivity, 1.5f, 0.0f, "in-range yaw sensitivity is kept");
    CheckNear(c.pitchSensitivity, 0.8f, 0.0f, "in-range pitch sensitivity is kept");
    CheckNear(c.rollSensitivity, 2.25f, 0.0f, "in-range roll sensitivity is kept");
    Check(c.invertPitch, "InvertPitch=true is kept");
    Check(!c.worldSpaceYaw, "WorldSpaceYaw=false is kept");
    CheckNear(c.localSmoothing, 0.05f, 0.0f, "in-range local smoothing is kept");
    CheckNear(c.remoteSmoothing, 0.4f, 0.0f, "in-range remote smoothing is kept");
    Check(!c.positionEnabled, "Position Enabled=false is kept");
    CheckNear(c.positionSensitivityX, 2.0f, 0.0f, "in-range position sensitivity x is kept");
    CheckNear(c.positionSensitivityY, 0.5f, 0.0f, "in-range position sensitivity y is kept");
    CheckNear(c.positionSensitivityZ, 3.5f, 0.0f, "in-range position sensitivity z is kept");
    CheckNear(c.limitX, 0.25f, 0.0f, "in-range limit x is kept");
    CheckNear(c.limitY, 0.15f, 0.0f, "in-range limit y is kept");
    CheckNear(c.limitZ, 0.45f, 0.0f, "in-range limit z is kept");
    CheckNear(c.limitZBack, 0.08f, 0.0f, "in-range limit z back is kept");
    CheckNear(c.fovOffsetDegrees, 12.5f, 0.0f, "in-range fov offset is kept");
    CheckEq(c.keyToggle, 0x70, "a valid toggle key is kept");
    CheckEq(c.keyCycleMode, 0x71, "a valid cycle key is kept");
    CheckEq(c.keyYawMode, 0x72, "a valid yaw key is kept");
}

void TestMissingFileKeepsDefaults() {
    std::printf("-- a missing file keeps every default\n");
    const pdht::Config defaults;
    pdht::Config c;
    c.Load("Z:\\pdht-does-not-exist\\nothing-here.ini");
    CheckEq(c.port, defaults.port, "a missing file keeps the default port");
    CheckNear(c.yawSensitivity, defaults.yawSensitivity, 0.0f,
              "a missing file keeps the default sensitivity");
    CheckNear(c.limitZ, defaults.limitZ, 0.0f, "a missing file keeps the default limit z");
    CheckEq(c.keyToggle, defaults.keyToggle, "a missing file keeps the default toggle key");
}

// IniReader::ReadInt is the one reader that does NOT honour its default on a
// present-but-unparseable value: GetPrivateProfileIntA yields 0 and the caller
// gets that 0. Port 0 binds an ephemeral port, so no tracker packet can ever
// arrive while the whole log still reads as healthy.
void TestUnparseablePortFallsBackToDefault() {
    std::printf("-- an unparseable port does not become port 0\n");
    const uint16_t expected = pdht::Config{}.port;

    const pdht::Config empty = LoadFrom("port_empty", "[Network]\nPort=\n");
    CheckEq(empty.port, expected, "an empty port falls back to the default");

    const pdht::Config quoted = LoadFrom("port_quoted", "[Network]\nPort=\"4242\" ; note\n");
    CheckEq(quoted.port, expected, "a quoted port with a trailing comment falls back");

    const pdht::Config word = LoadFrom("port_word", "[Network]\nPort=default\n");
    CheckEq(word.port, expected, "a non-numeric port falls back to the default");

    const pdht::Config zero = LoadFrom("port_zero", "[Network]\nPort=0\n");
    CheckEq(zero.port, expected, "an explicit port 0 falls back rather than binding any port");
}

// 70000 truncated to 4464 in the uint16_t cast, so the mod listened on a port
// nobody was sending to and said nothing about it.
void TestOutOfRangePortFallsBackToDefault() {
    std::printf("-- an out-of-range port does not wrap\n");
    const uint16_t expected = pdht::Config{}.port;
    CheckEq(LoadFrom("port_high", "[Network]\nPort=70000\n").port, expected,
            "a port above 65535 falls back instead of wrapping");
    CheckEq(LoadFrom("port_neg", "[Network]\nPort=-1\n").port, expected,
            "a negative port falls back instead of wrapping to 65535");
    CheckEq(LoadFrom("port_low", "[Network]\nPort=80\n").port, expected,
            "a privileged port falls back");
}

// strtod accepts "nan" and "inf", and overflows 1e400 to +inf. Any of the three
// reaching the pose puts a non-finite basis and view origin into the renderer
// every frame.
void TestNonFiniteFloatsNeverEscape() {
    std::printf("-- nan / inf / overflow never reach the pose\n");
    const pdht::Config defaults;
    const pdht::Config c = LoadFrom("nonfinite",
        "[Tracking]\n"
        "YawSensitivity=nan\n"
        "PitchSensitivity=inf\n"
        "RollSensitivity=-inf\n"
        "LocalSmoothing=nan\n"
        "RemoteSmoothing=1e400\n"
        "[Position]\n"
        "SensitivityX=nan\n"
        "SensitivityY=inf\n"
        "SensitivityZ=1e400\n"
        "LimitX=nan\n"
        "LimitY=inf\n"
        "LimitZ=-inf\n"
        "LimitZBack=nan\n"
        "[Camera]\n"
        "FovOffset=nan\n");

    Check(std::isfinite(c.yawSensitivity) && std::isfinite(c.pitchSensitivity)
              && std::isfinite(c.rollSensitivity),
          "no rotation sensitivity survives as non-finite");
    Check(std::isfinite(c.localSmoothing) && std::isfinite(c.remoteSmoothing),
          "neither smoothing value survives as non-finite");
    Check(std::isfinite(c.positionSensitivityX) && std::isfinite(c.positionSensitivityY)
              && std::isfinite(c.positionSensitivityZ),
          "no position sensitivity survives as non-finite");
    Check(std::isfinite(c.limitX) && std::isfinite(c.limitY) && std::isfinite(c.limitZ)
              && std::isfinite(c.limitZBack),
          "no position limit survives as non-finite");
    Check(std::isfinite(c.fovOffsetDegrees), "the fov offset does not survive as non-finite");

    // NaN falls back to the default rather than to a range endpoint: there is no
    // reading of "nan" that means "as far as this axis goes".
    CheckNear(c.yawSensitivity, defaults.yawSensitivity, 0.0f,
              "a NaN sensitivity falls back to the default, not to a limit");
    CheckNear(c.limitX, defaults.limitX, 0.0f,
              "a NaN limit falls back to the default, not to a limit");
    CheckNear(c.fovOffsetDegrees, defaults.fovOffsetDegrees, 0.0f,
              "a NaN fov offset falls back to the default");
}

// PositionProcessor clamps as Clamp(v, -limit, limit). With a negative limit the
// bounds invert and Clamp returns its lower bound for every input, which pins
// the lean at a fixed offset instead of freeing it.
void TestPositionLimitsStayPositive() {
    std::printf("-- position limits stay positive and bounded\n");
    const pdht::Config c = LoadFrom("limits",
        "[Position]\n"
        "LimitX=-0.3\n"
        "LimitY=0\n"
        "LimitZ=50\n"
        "LimitZBack=-1e9\n");
    Check(c.limitX > 0.0f, "a negative limit x is corrected to a positive one");
    Check(c.limitY > 0.0f, "a zero limit y is corrected to a positive one");
    Check(c.limitZ > 0.0f && c.limitZ <= 0.5f, "an absurd limit z is clamped into range");
    Check(c.limitZBack > 0.0f, "a hugely negative limit z back is corrected to a positive one");
}

void TestSmoothingStaysInUnitRange() {
    std::printf("-- smoothing stays in 0..1\n");
    const pdht::Config c = LoadFrom("smoothing",
        "[Tracking]\nLocalSmoothing=-2.5\nRemoteSmoothing=9.0\n");
    // Outside 0..1 the smoothing speed lerp runs off both ends: a negative
    // smoothing gives a speed above 50 and a value above 1 gives a negative one,
    // which turns the exponential blend into divergence rather than smoothing.
    CheckNear(c.localSmoothing, 0.0f, 0.0f, "a negative local smoothing clamps to 0");
    CheckNear(c.remoteSmoothing, 1.0f, 0.0f, "a remote smoothing above 1 clamps to 1");
}

void TestSensitivitiesStayInRange() {
    std::printf("-- sensitivities stay in their documented ranges\n");
    const pdht::Config c = LoadFrom("sensitivity",
        "[Tracking]\nYawSensitivity=1000\nPitchSensitivity=-4\n"
        "[Position]\nSensitivityX=-1\nSensitivityZ=1000\n");
    CheckNear(c.yawSensitivity, 3.0f, 0.0f, "an absurd yaw sensitivity clamps to the maximum");
    CheckNear(c.pitchSensitivity, 0.1f, 0.0f,
              "a negative pitch sensitivity clamps to the minimum");
    CheckNear(c.positionSensitivityX, 0.0f, 0.0f,
              "a negative position sensitivity clamps to zero");
    CheckNear(c.positionSensitivityZ, 5.0f, 0.0f,
              "an absurd position sensitivity clamps to the maximum");
}

void TestFovOffsetRangeIsUnchanged() {
    std::printf("-- the fov offset range is unchanged\n");
    const pdht::Config c = LoadFrom("fov", "[Camera]\nFovOffset=400\n");
    CheckNear(c.fovOffsetDegrees, 60.0f, 0.0f, "a huge fov offset clamps to the maximum");
    const pdht::Config d = LoadFrom("fov_neg", "[Camera]\nFovOffset=-400\n");
    CheckNear(d.fovOffsetDegrees, -40.0f, 0.0f, "a hugely negative fov offset clamps to the minimum");
}

// strtod parses a PREFIX and stops. Every one of these used to be accepted
// silently: `0,15` became 0.0 (inside range, so no "corrected to" line fired),
// and text that parses to nothing returned the default, which EQUALS the
// default, so the correction test could never fire either. The user's edit was
// discarded with nothing in the log naming the key.
void TestPartiallyParseableFloatsAreRejected() {
    std::printf("-- a float that does not wholly parse is rejected, not truncated\n");
    const pdht::Config c = LoadFrom("partial_float",
        "[Tracking]\nLocalSmoothing=0,15\nRemoteSmoothing=0.5abc\n"
        "YawSensitivity=one point five\n"
        "[Camera]\nFovOffset=10 20\n");
    const pdht::Config defaults;
    CheckNear(c.localSmoothing, defaults.localSmoothing, 0.0f,
              "a decimal comma is rejected rather than read as 0");
    CheckNear(c.remoteSmoothing, defaults.remoteSmoothing, 0.0f,
              "trailing garbage after a number is rejected");
    CheckNear(c.yawSensitivity, defaults.yawSensitivity, 0.0f,
              "words where a number belongs are rejected");
    CheckNear(c.fovOffsetDegrees, defaults.fovOffsetDegrees, 0.0f,
              "two numbers where one belongs are rejected");
}

// A trailing comment is the documented, supported form on every key type. The
// core's ReadBool compares the WHOLE value against its whitelist, so
// `Enabled=0 ; note` matched nothing and left positional tracking fully ON.
void TestTrailingCommentsSurviveOnEveryType() {
    std::printf("-- trailing comments do not silently discard a value\n");
    const pdht::Config c = LoadFrom("trailing_comment",
        "[Network]\nPort=5555 ; my port\n"
        "[Tracking]\nEnableOnStartup=0 ; off for now\nWorldSpaceYaw=0 # camera local\n"
        "LocalSmoothing=0.4 ; a little\n"
        "[Position]\nEnabled=0 ; no leaning\n"
        "[Controls]\nKeyToggle=0x70 ; F1\n");
    CheckEq(c.port, 5555, "a commented port is read");
    Check(!c.enableOnStartup, "a commented EnableOnStartup=0 actually disables");
    Check(!c.worldSpaceYaw, "a commented WorldSpaceYaw=0 actually selects camera-local yaw");
    CheckNear(c.localSmoothing, 0.4f, 1e-6f, "a commented smoothing is read");
    Check(!c.positionEnabled, "a commented [Position] Enabled=0 actually disables position");
    CheckEq(c.keyToggle, 0x70, "a commented key code is read");
}

// The whole point of a bool key is that a wrong value is visible. Silently
// keeping the default meant `Enabled=off` left positional tracking on with
// nothing anywhere to say so.
void TestBoolsAcceptTheirWholeVocabulary() {
    std::printf("-- bool keys accept their documented spellings and reject the rest\n");
    const pdht::Config yes = LoadFrom("bool_yes",
        "[Tracking]\nEnableOnStartup=TRUE\nInvertPitch=Yes\n"
        "[Position]\nEnabled=on\n");
    Check(yes.enableOnStartup, "TRUE is true");
    Check(yes.invertPitch, "Yes is true");
    Check(yes.positionEnabled, "on is true");

    const pdht::Config no = LoadFrom("bool_no",
        "[Tracking]\nEnableOnStartup=False\nWorldSpaceYaw=NO\n"
        "[Position]\nEnabled=Off\n");
    Check(!no.enableOnStartup, "False is false");
    Check(!no.worldSpaceYaw, "NO is false");
    Check(!no.positionEnabled, "Off is false");

    const pdht::Config junk = LoadFrom("bool_junk",
        "[Tracking]\nEnableOnStartup=disabled\n[Position]\nEnabled=nope\n");
    const pdht::Config defaults;
    Check(junk.enableOnStartup == defaults.enableOnStartup,
          "an unrecognised bool keeps the default");
    Check(junk.positionEnabled == defaults.positionEnabled,
          "an unrecognised [Position] Enabled keeps the default");
}

// A key code that parses into a DIFFERENT valid key is the failure the range
// check cannot see: `KeyToggle=End` used to read 'E', stop at 'n', and bind
// 0x0E - an unassigned VK that silently never fires.
void TestKeyCodesRejectNonHexText() {
    std::printf("-- key codes reject text that is not a hex code\n");
    const pdht::Config defaults;
    const pdht::Config c = LoadFrom("key_text",
        "[Controls]\nKeyToggle=End\nKeyCycleMode=PageUp\nKeyYawMode=0xZZ\n");
    CheckEq(c.keyToggle, defaults.keyToggle, "a key NAME is rejected, not read as 0x0E");
    CheckEq(c.keyCycleMode, defaults.keyCycleMode, "another key name is rejected");
    CheckEq(c.keyYawMode, defaults.keyYawMode, "a malformed hex literal is rejected");

    const pdht::Config ok = LoadFrom("key_forms",
        "[Controls]\nKeyToggle=0x70\nKeyCycleMode=0X71\nKeyYawMode=72\n");
    CheckEq(ok.keyToggle, 0x70, "a 0x-prefixed code is read");
    CheckEq(ok.keyCycleMode, 0x71, "an 0X-prefixed code is read");
    CheckEq(ok.keyYawMode, 0x72, "a bare hex code is read");
}

// Inclusive, because a user who reads "range 1024 to 65535" in the INI and types
// 1024 must get 1024.
void TestRangeEndpointsAreInclusive() {
    std::printf("-- documented range endpoints are accepted\n");
    const pdht::Config lo = LoadFrom("endpoints_lo",
        "[Network]\nPort=1024\n"
        "[Tracking]\nYawSensitivity=0.1\nLocalSmoothing=0.0\n"
        "[Position]\nSensitivityX=0.0\nLimitZ=0.01\n"
        "[Camera]\nFovOffset=-40\n");
    CheckEq(lo.port, 1024, "the lowest documented port is accepted");
    CheckNear(lo.yawSensitivity, 0.1f, 0.0f, "the lowest documented sensitivity is accepted");
    CheckNear(lo.localSmoothing, 0.0f, 0.0f, "zero smoothing is accepted");
    CheckNear(lo.positionSensitivityX, 0.0f, 0.0f, "zero position sensitivity is accepted");
    CheckNear(lo.limitZ, 0.01f, 0.0f, "the smallest documented limit is accepted");
    CheckNear(lo.fovOffsetDegrees, -40.0f, 0.0f, "the lowest documented fov offset is accepted");

    const pdht::Config hi = LoadFrom("endpoints_hi",
        "[Network]\nPort=65535\n"
        "[Tracking]\nYawSensitivity=3.0\nRemoteSmoothing=1.0\n"
        "[Position]\nSensitivityZ=5.0\nLimitZ=0.5\n"
        "[Camera]\nFovOffset=60\n");
    CheckEq(hi.port, 65535, "the highest documented port is accepted");
    CheckNear(hi.yawSensitivity, 3.0f, 0.0f, "the highest documented sensitivity is accepted");
    CheckNear(hi.remoteSmoothing, 1.0f, 0.0f, "full smoothing is accepted");
    CheckNear(hi.positionSensitivityZ, 5.0f, 0.0f, "the highest position sensitivity is accepted");
    CheckNear(hi.limitZ, 0.5f, 0.0f, "the largest documented limit is accepted");
    CheckNear(hi.fovOffsetDegrees, 60.0f, 0.0f, "the highest documented fov offset is accepted");
}

// The vertical clamp is [-limitYDown, +limitY]. LimitY used to be the only key,
// so it bounded the upward lean while the downward one stayed at the core
// struct's own 0.20 default whatever the user typed.
void TestVerticalLimitsAreBothConfigurable() {
    std::printf("-- LimitY and LimitYDown are independent, and LimitY alone is symmetric\n");
    const pdht::Config both = LoadFrom("limit_y_both",
        "[Position]\nLimitY=0.30\nLimitYDown=0.05\n");
    CheckNear(both.limitY, 0.30f, 0.0f, "LimitY is the upward limit");
    CheckNear(both.limitYDown, 0.05f, 0.0f, "LimitYDown is the downward limit");

    const pdht::Config mirrored = LoadFrom("limit_y_one", "[Position]\nLimitY=0.05\n");
    CheckNear(mirrored.limitY, 0.05f, 0.0f, "LimitY alone is read");
    CheckNear(mirrored.limitYDown, 0.05f, 0.0f,
              "LimitY alone mirrors into the downward limit rather than leaving it at 0.20");
}

// WarnRetiredSmoothingKey is load-bearing for upgraders: the old single value
// carried a hidden 0.15 floor, so migrating it would hand a local user smoothing
// they never chose.
void TestRetiredSmoothingKeyIsIgnored() {
    std::printf("-- the retired [Tracking] Smoothing key is ignored, not applied\n");
    const pdht::Config defaults;
    const pdht::Config c = LoadFrom("retired_smoothing",
        "[Tracking]\nSmoothing=0.8\n[Position]\nSmoothing=0.8\n");
    CheckNear(c.localSmoothing, defaults.localSmoothing, 0.0f,
              "the retired key does not become LocalSmoothing");
    CheckNear(c.remoteSmoothing, defaults.remoteSmoothing, 0.0f,
              "the retired key does not become RemoteSmoothing");
}

// 0 is not a virtual key, and it doubles as the hotkey poller's unset sentinel.
void TestKeyCodesStayVirtualKeys() {
    std::printf("-- hotkeys stay inside the virtual key range\n");
    const pdht::Config defaults;
    const pdht::Config c = LoadFrom("keys",
        "[Controls]\nKeyToggle=0\nKeyCycleMode=0x1FF\nKeyYawMode=0xFF\n");
    CheckEq(c.keyToggle, defaults.keyToggle, "key code 0 falls back to the default");
    CheckEq(c.keyCycleMode, defaults.keyCycleMode, "a key code past 0xFE falls back");
    CheckEq(c.keyYawMode, defaults.keyYawMode, "the reserved 0xFF falls back");
}

}  // namespace

int main() {
    std::printf("=== pdht config boundary validation ===\n");
    TestValidConfigIsUntouched();
    TestMissingFileKeepsDefaults();
    TestUnparseablePortFallsBackToDefault();
    TestOutOfRangePortFallsBackToDefault();
    TestNonFiniteFloatsNeverEscape();
    TestPositionLimitsStayPositive();
    TestSmoothingStaysInUnitRange();
    TestSensitivitiesStayInRange();
    TestFovOffsetRangeIsUnchanged();
    TestKeyCodesStayVirtualKeys();
    TestPartiallyParseableFloatsAreRejected();
    TestTrailingCommentsSurviveOnEveryType();
    TestBoolsAcceptTheirWholeVocabulary();
    TestKeyCodesRejectNonHexText();
    TestRangeEndpointsAreInclusive();
    TestVerticalLimitsAreBothConfigurable();
    TestRetiredSmoothingKeyIsIgnored();
    std::printf("=== %s ===\n", g_failures == 0 ? "all boundaries held" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
