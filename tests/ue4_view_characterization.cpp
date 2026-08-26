// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
//
// Characterization harness for the view math in ue4.cpp - the one part of this
// mod that is pure and therefore testable off a running game. It does two jobs:
//
//   - Asserts the invariants the injection depends on (the basis stays
//     orthonormal, a rejected layout writes nothing, the aim point is where the
//     clean forward projects, a zero pose is the identity).
//   - Prints a deterministic report of every derived number. Restructuring that
//     file must leave the report byte-identical; for code with no other way to
//     observe it, that is what "no behaviour change" means.
//
// Built only when PDHT_BUILD_TESTS=ON, so nothing in the shipped .asi or in CI
// depends on it.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "ue4.h"

namespace {

using pdht::ue4::AimScreenOffset;
using pdht::ue4::HeadPose;
using pdht::ue4::InjectStatus;
using pdht::ue4::ViewProjection;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    if (std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("FAIL: %s (got %.6f, expected %.6f)\n", what, actual, expected);
}

// FSceneViewProjectionData as the hook sees it: ViewOrigin +0x00, the row-major
// ViewRotationMatrix +0x10, ProjectionMatrix +0x50, ViewRect +0x90.
struct ProjectionBuffer {
    alignas(16) unsigned char bytes[0xB0];

    std::uintptr_t Address() { return reinterpret_cast<std::uintptr_t>(bytes); }
    float* Origin() { return reinterpret_cast<float*>(bytes + 0x00); }
    float* Matrix() { return reinterpret_cast<float*>(bytes + 0x10); }
    float& ProjXX() { return *reinterpret_cast<float*>(bytes + 0x50); }
    float& ProjYY() { return *reinterpret_cast<float*>(bytes + 0x64); }
    std::int32_t* Rect() { return reinterpret_cast<std::int32_t*>(bytes + 0x90); }
};

constexpr float kDegToRad = 0.01745329252f;

// The matrix columns are the camera's world-space axes.
void SetBasis(ProjectionBuffer& pd, const float right[3], const float up[3],
              const float fwd[3]) {
    float* m = pd.Matrix();
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = right[0];  m[4] = right[1];  m[8]  = right[2];
    m[1] = up[0];     m[5] = up[1];     m[9]  = up[2];
    m[2] = fwd[0];    m[6] = fwd[1];    m[10] = fwd[2];
    m[15] = 1.0f;
}

// A camera at the origin looking along world +X with world +Z up: 90 degrees
// horizontal, 16:9 vertical, on a 1920x1080 viewport.
ProjectionBuffer MakeLevelView() {
    ProjectionBuffer pd{};
    std::memset(pd.bytes, 0, sizeof(pd.bytes));
    const float right[3] = {0.0f, 1.0f, 0.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float fwd[3] = {1.0f, 0.0f, 0.0f};
    SetBasis(pd, right, up, fwd);
    pd.ProjXX() = 1.0f;
    pd.ProjYY() = 16.0f / 9.0f;
    pd.Rect()[0] = 0;
    pd.Rect()[1] = 0;
    pd.Rect()[2] = 1920;
    pd.Rect()[3] = 1080;
    return pd;
}

// Straight down: fwd is anti-parallel to world up, so a world-space yaw is a
// pure spin about the view axis and the aim point must not move at all. This is
// the case a camera-local yaw gets catastrophically wrong.
ProjectionBuffer MakeDownView() {
    ProjectionBuffer pd = MakeLevelView();
    const float right[3] = {0.0f, 1.0f, 0.0f};
    const float up[3] = {1.0f, 0.0f, 0.0f};
    const float fwd[3] = {0.0f, 0.0f, -1.0f};
    SetBasis(pd, right, up, fwd);
    return pd;
}

// The same view pitched down 60 degrees, which is where world-space and
// camera-local yaw stop agreeing.
ProjectionBuffer MakePitchedView() {
    ProjectionBuffer pd = MakeLevelView();
    const float a = -60.0f * kDegToRad;
    const float right[3] = {0.0f, 1.0f, 0.0f};
    const float up[3] = {-std::sin(a), 0.0f, std::cos(a)};
    const float fwd[3] = {std::cos(a), 0.0f, std::sin(a)};
    SetBasis(pd, right, up, fwd);
    return pd;
}

HeadPose MakePose(float pitch, float yaw, float roll) {
    HeadPose pose{};
    pose.pitch = pitch;
    pose.yaw = yaw;
    pose.roll = roll;
    pose.worldSpaceYaw = true;
    return pose;
}

float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void CheckOrthonormal(const ViewProjection& view, const char* what) {
    char label[160];
    std::snprintf(label, sizeof(label), "%s: tracked basis is orthonormal", what);
    const bool ok = std::fabs(Dot3(view.trackedRight, view.trackedRight) - 1.0f) < 1e-4f
                 && std::fabs(Dot3(view.trackedUp, view.trackedUp) - 1.0f) < 1e-4f
                 && std::fabs(Dot3(view.trackedFwd, view.trackedFwd) - 1.0f) < 1e-4f
                 && std::fabs(Dot3(view.trackedRight, view.trackedUp)) < 1e-4f
                 && std::fabs(Dot3(view.trackedRight, view.trackedFwd)) < 1e-4f
                 && std::fabs(Dot3(view.trackedUp, view.trackedFwd)) < 1e-4f;
    Check(ok, label);
}

void ReportInject(const char* label, ProjectionBuffer pd, const HeadPose& pose) {
    AimScreenOffset aim{};
    ViewProjection view{};
    const InjectStatus status = pdht::ue4::InjectHeadPose(pd.Address(), pose, aim, view);
    std::printf("%-26s status=%d aim=(%.4f,%.4f valid=%d) fwd=(%.6f,%.6f,%.6f) "
                "up=(%.6f,%.6f,%.6f) right=(%.6f,%.6f,%.6f) origin=(%.4f,%.4f,%.4f) "
                "proj=(%.6f,%.6f -> %.6f,%.6f)\n",
                label, static_cast<int>(status), aim.x, aim.y, aim.valid ? 1 : 0,
                view.trackedFwd[0], view.trackedFwd[1], view.trackedFwd[2],
                view.trackedUp[0], view.trackedUp[1], view.trackedUp[2],
                view.trackedRight[0], view.trackedRight[1], view.trackedRight[2],
                pd.Origin()[0], pd.Origin()[1], pd.Origin()[2],
                view.cleanProjXX, view.cleanProjYY, view.trackedProjXX, view.trackedProjYY);
    if (status == InjectStatus::Ok) CheckOrthonormal(view, label);
}

void ReportReproject(const char* label, const ViewProjection& view, float x, float y) {
    float outX = 0.0f;
    float outY = 0.0f;
    const bool ok = pdht::ue4::ReprojectScreenPoint(view, x, y, outX, outY);
    std::printf("%-26s in=(%.1f,%.1f) ok=%d out=(%.4f,%.4f)\n", label, x, y, ok ? 1 : 0,
                outX, outY);
}

void TestFovDerivation() {
    std::printf("-- FovDegreesFromProjectionScale\n");
    for (float scale : {0.0f, -1.0f, 0.5f, 1.0f, 1.428148f, 2.414214f}) {
        std::printf("   scale=%.6f -> %.6f deg\n", scale,
                    pdht::ue4::FovDegreesFromProjectionScale(scale));
    }
    CheckNear(pdht::ue4::FovDegreesFromProjectionScale(1.0f), 90.0f, 1e-3f,
              "projXX of 1 is a 90 degree horizontal FOV");
    CheckNear(pdht::ue4::FovDegreesFromProjectionScale(0.0f), 0.0f, 0.0f,
              "a non-positive scale reports 0 rather than dividing by it");
}

void TestZeroPoseIsIdentity() {
    std::printf("-- zero pose\n");
    ProjectionBuffer pd = MakeLevelView();
    float before[16];
    std::memcpy(before, pd.Matrix(), sizeof(before));

    AimScreenOffset aim{};
    ViewProjection view{};
    const InjectStatus status =
        pdht::ue4::InjectHeadPose(pd.Address(), MakePose(0.0f, 0.0f, 0.0f), aim, view);
    Check(status == InjectStatus::Ok, "a level view injects cleanly");
    Check(std::memcmp(before, pd.Matrix(), sizeof(before)) == 0,
          "a zero pose leaves the view rotation matrix byte-identical");
    Check(aim.valid, "clean aim is in front of an untracked view");
    CheckNear(aim.x, 0.0f, 1e-4f, "clean aim projects to the screen centre in x");
    CheckNear(aim.y, 0.0f, 1e-4f, "clean aim projects to the screen centre in y");
    ReportInject("zero", MakeLevelView(), MakePose(0.0f, 0.0f, 0.0f));
}

void TestRotationAxes() {
    std::printf("-- rotation\n");
    ReportInject("yaw+20 world", MakeLevelView(), MakePose(0.0f, 20.0f, 0.0f));
    ReportInject("pitch+15", MakeLevelView(), MakePose(15.0f, 0.0f, 0.0f));
    ReportInject("roll+10", MakeLevelView(), MakePose(0.0f, 0.0f, 10.0f));
    ReportInject("yaw+20 pitch+15 roll+10", MakeLevelView(), MakePose(15.0f, 20.0f, 10.0f));

    // Yaw about world up on a level camera: forward swings in the world XY plane.
    AimScreenOffset aim{};
    ViewProjection view{};
    ProjectionBuffer pd = MakeLevelView();
    pdht::ue4::InjectHeadPose(pd.Address(), MakePose(0.0f, 20.0f, 0.0f), aim, view);
    CheckNear(view.trackedFwd[0], std::cos(20.0f * kDegToRad), 1e-5f, "yaw turns forward in x");
    CheckNear(view.trackedFwd[1], std::sin(20.0f * kDegToRad), 1e-5f, "yaw turns forward in y");
    CheckNear(view.trackedFwd[2], 0.0f, 1e-5f, "yaw about world up keeps forward level");

    ProjectionBuffer pitched = MakeLevelView();
    pdht::ue4::InjectHeadPose(pitched.Address(), MakePose(15.0f, 0.0f, 0.0f), aim, view);
    Check(view.trackedFwd[2] > 0.0f, "positive pitch looks up, not down");
}

// The aim offset in closed form, against MakeLevelView (M00 = 1, M11 = 16/9,
// 1920x1080, so both half-extents come out at 960 px).
//
// These exist because the harness used to assert only that a ZERO pose lands at
// the screen centre, which is invariant under a sign flip in either axis, under
// a flipped roll direction, and under the FOV clamps being deleted. All four of
// those were injected into ue4.cpp deliberately and the suite still passed. The
// roll direction in particular is the exact thing that shipped wrong on the
// first build of this mod.
void TestAimProjection() {
    std::printf("-- aim projection\n");
    const float halfW = 960.0f;            // M00 * width / 2
    const float halfH = (16.0f / 9.0f) * 540.0f;  // M11 * height / 2

    AimScreenOffset aim{};
    ViewProjection view{};

    // Pure yaw: the aim point sweeps horizontally and stays on the horizon.
    ProjectionBuffer yawed = MakeLevelView();
    pdht::ue4::InjectHeadPose(yawed.Address(), MakePose(0.0f, 20.0f, 0.0f), aim, view);
    CheckNear(aim.x, -std::tan(20.0f * kDegToRad) * halfW, 1e-2f,
              "yaw moves the aim point by -tan(yaw) of the half-width");
    CheckNear(aim.y, 0.0f, 1e-3f, "yaw alone does not move the aim point vertically");

    // Pure pitch: purely vertical, and DOWN the screen for a positive (upward)
    // pitch, because looking up puts what you are aiming at lower in frame.
    ProjectionBuffer pitchedOnly = MakeLevelView();
    pdht::ue4::InjectHeadPose(pitchedOnly.Address(), MakePose(15.0f, 0.0f, 0.0f), aim, view);
    CheckNear(aim.x, 0.0f, 1e-3f, "pitch alone does not move the aim point horizontally");
    CheckNear(aim.y, std::tan(15.0f * kDegToRad) * halfH, 1e-2f,
              "pitch moves the aim point by +tan(pitch) of the half-height");

    // Pure roll leaves it dead centre: roll is applied about the forward axis,
    // which is the axis clean aim already lies on.
    for (float roll : {5.0f, 15.0f, 30.0f, 45.0f}) {
        ProjectionBuffer rolled = MakeLevelView();
        pdht::ue4::InjectHeadPose(rolled.Address(), MakePose(0.0f, 0.0f, roll), aim, view);
        CheckNear(aim.x, 0.0f, 1e-3f, "roll alone leaves the aim point centred in x");
        CheckNear(aim.y, 0.0f, 1e-3f, "roll alone leaves the aim point centred in y");
    }

    // Pitch + roll: this camera composes roll OUTERMOST, so the offset rotates
    // about the centre at constant radius rather than staying vertical. Both the
    // radius and the direction are pinned - the radius alone would pass with the
    // roll applied the wrong way round.
    const float radius = std::tan(15.0f * kDegToRad) * halfW;  // halfW == halfH here
    for (float roll : {0.0f, 10.0f, 30.0f, -30.0f}) {
        ProjectionBuffer pr = MakeLevelView();
        pdht::ue4::InjectHeadPose(pr.Address(), MakePose(15.0f, 0.0f, roll), aim, view);
        CheckNear(std::sqrt(aim.x * aim.x + aim.y * aim.y), radius, 1e-2f,
                  "pitch+roll rotates the aim offset at constant radius");
        CheckNear(aim.x, -radius * std::sin(roll * kDegToRad), 1e-2f,
                  "pitch+roll puts the aim offset on the correct side (x)");
        CheckNear(aim.y, radius * std::cos(roll * kDegToRad), 1e-2f,
                  "pitch+roll puts the aim offset on the correct side (y)");
    }

    // World-space yaw looking straight down is a pure spin about the view axis:
    // the world turns and the aim point does not move. A camera-local yaw here
    // sweeps it hundreds of pixels across the screen.
    for (float yaw : {5.0f, 40.0f, -40.0f}) {
        ProjectionBuffer down = MakeDownView();
        pdht::ue4::InjectHeadPose(down.Address(), MakePose(0.0f, yaw, 0.0f), aim, view);
        Check(aim.valid, "the aim point is still in front of a straight-down view");
        CheckNear(aim.x, 0.0f, 1e-3f, "world yaw looking down does not move the aim point (x)");
        CheckNear(aim.y, 0.0f, 1e-3f, "world yaw looking down does not move the aim point (y)");
    }

    // The crosshair is where the reprojection sends the view centre. If those two
    // ever disagree the crosshair and the HUD it sits among are computed from
    // different views.
    ProjectionBuffer both = MakeLevelView();
    pdht::ue4::InjectHeadPose(both.Address(), MakePose(15.0f, 20.0f, 10.0f), aim, view);
    float cx = 0.0f;
    float cy = 0.0f;
    Check(pdht::ue4::ReprojectScreenPoint(view, 960.0f, 540.0f, cx, cy),
          "the view centre reprojects on a combined pose");
    CheckNear(cx, 960.0f + aim.x, 1e-2f, "the aim offset IS the centre's reprojection (x)");
    CheckNear(cy, 540.0f + aim.y, 1e-2f, "the aim offset IS the centre's reprojection (y)");

    // Edge-on. The perspective divide runs away here - at 89.94 degrees it
    // reaches ~10^6 px, and that number goes straight into a widget's
    // RenderTransform.Translation - so the offset is clamped to the frame.
    for (float yaw : {80.0f, 89.9f, 100.0f, 179.0f}) {
        ProjectionBuffer edge = MakeLevelView();
        pdht::ue4::InjectHeadPose(edge.Address(), MakePose(0.0f, yaw, 0.0f), aim, view);
        Check(aim.valid, "an edge-on aim point still resolves to somewhere on screen");
        Check(std::fabs(aim.x) <= 960.0f + 1e-2f && std::fabs(aim.y) <= 540.0f + 1e-2f,
              "an edge-on aim point is clamped to the frame, not left to diverge");
    }
}

void TestWorldVersusLocalYaw() {
    std::printf("-- yaw mode\n");
    HeadPose world = MakePose(0.0f, 25.0f, 0.0f);
    HeadPose local = world;
    local.worldSpaceYaw = false;
    ReportInject("pitched yaw world", MakePitchedView(), world);
    ReportInject("pitched yaw local", MakePitchedView(), local);

    AimScreenOffset aim{};
    ViewProjection worldView{};
    ViewProjection localView{};
    ProjectionBuffer a = MakePitchedView();
    ProjectionBuffer b = MakePitchedView();
    pdht::ue4::InjectHeadPose(a.Address(), world, aim, worldView);
    pdht::ue4::InjectHeadPose(b.Address(), local, aim, localView);
    Check(std::fabs(worldView.trackedFwd[2] - localView.trackedFwd[2]) > 1e-3f,
          "world-space and camera-local yaw differ once the camera is pitched");
}

void TestPositionOffsetUsesCleanAxes() {
    std::printf("-- position\n");
    HeadPose pose = MakePose(0.0f, 90.0f, 0.0f);
    pose.offsetRight = 10.0f;
    pose.offsetUp = 5.0f;
    pose.offsetForward = 20.0f;
    ReportInject("lean under 90 yaw", MakeLevelView(), pose);

    // Clean right is world +Y, clean up world +Z, clean forward world +X. The 90
    // degree yaw must not rotate the lean: it follows the body, not the head.
    ProjectionBuffer pd = MakeLevelView();
    AimScreenOffset aim{};
    ViewProjection view{};
    pdht::ue4::InjectHeadPose(pd.Address(), pose, aim, view);
    CheckNear(pd.Origin()[0], 20.0f, 1e-4f, "forward lean lands on the clean forward axis");
    CheckNear(pd.Origin()[1], 10.0f, 1e-4f, "right lean lands on the clean right axis");
    CheckNear(pd.Origin()[2], 5.0f, 1e-4f, "up lean lands on the clean up axis");
}

void TestFovOffset() {
    std::printf("-- fov offset\n");
    for (float offset : {0.0f, 20.0f, -20.0f, 400.0f, -400.0f}) {
        HeadPose pose = MakePose(0.0f, 0.0f, 0.0f);
        pose.fovOffsetDegrees = offset;
        char label[64];
        std::snprintf(label, sizeof(label), "fov %+.0f", offset);
        ReportInject(label, MakeLevelView(), pose);
    }

    // The offset is a uniform scale about the centre, so whatever aspect ratio
    // the engine folded into the two elements has to survive it.
    ProjectionBuffer pd = MakeLevelView();
    HeadPose pose = MakePose(0.0f, 0.0f, 0.0f);
    pose.fovOffsetDegrees = 20.0f;
    AimScreenOffset aim{};
    ViewProjection view{};
    pdht::ue4::InjectHeadPose(pd.Address(), pose, aim, view);
    CheckNear(view.trackedProjYY / view.trackedProjXX, view.cleanProjYY / view.cleanProjXX,
              1e-5f, "a FOV offset preserves the aspect ratio");
    CheckNear(pdht::ue4::FovDegreesFromProjectionScale(view.trackedProjXX), 110.0f, 1e-3f,
              "the offset is added to the FOV the game asked for");
    // An independently computed number, not view.trackedProjXX: comparing the
    // struct against the value the same call reported asserts the implementation
    // against itself. 110 degrees horizontal is 1/tan(55).
    CheckNear(pd.ProjXX(), 1.0f / std::tan(55.0f * kDegToRad), 1e-5f,
              "the offset projection is what gets written back");

    // The clamps are what keep tan() off its asymptote. Deleting both of them
    // left every other assertion in this suite passing while an unclamped 360
    // degrees produced M00 = 1.1e7 and 180 degrees produced M00 = -4.4e-8.
    for (float offset : {400.0f, -400.0f}) {
        ProjectionBuffer clamped = MakeLevelView();
        HeadPose extreme = MakePose(0.0f, 0.0f, 0.0f);
        extreme.fovOffsetDegrees = offset;
        ViewProjection clampedView{};
        pdht::ue4::InjectHeadPose(clamped.Address(), extreme, aim, clampedView);
        CheckNear(pdht::ue4::FovDegreesFromProjectionScale(clampedView.trackedProjXX),
                  offset > 0.0f ? 170.0f : 20.0f, 1e-3f,
                  "an absurd FOV offset is clamped to the usable range");
        Check(clampedView.trackedProjXX > 0.0f,
              "a clamped FOV offset still leaves a positive projection scale");
    }

    // A projection element outside the perspective band is not a half-tangent
    // reciprocal, and treating it as one rescaled it by up to a factor of a
    // million before writing it back.
    ProjectionBuffer ortho = MakeLevelView();
    ortho.ProjXX() = 0.0002f;
    HeadPose orthoPose = MakePose(0.0f, 0.0f, 0.0f);
    orthoPose.fovOffsetDegrees = 20.0f;
    ViewProjection orthoView{};
    pdht::ue4::InjectHeadPose(ortho.Address(), orthoPose, aim, orthoView);
    CheckNear(ortho.ProjXX(), 0.0002f, 0.0f,
              "a non-perspective projection is left exactly as the engine built it");
}

void TestLayoutRejection() {
    std::printf("-- layout rejection\n");
    ProjectionBuffer pd = MakeLevelView();
    pd.Matrix()[0] = 4.0f;  // right is no longer unit length
    float before[16];
    std::memcpy(before, pd.Matrix(), sizeof(before));

    AimScreenOffset aim{};
    ViewProjection view{};
    const InjectStatus status =
        pdht::ue4::InjectHeadPose(pd.Address(), MakePose(0.0f, 20.0f, 0.0f), aim, view);
    Check(status == InjectStatus::NotOrthonormal, "a non-orthonormal basis is rejected");
    Check(std::memcmp(before, pd.Matrix(), sizeof(before)) == 0,
          "a rejected layout is left completely alone");
    Check(!view.valid, "a rejected layout publishes no view state");
    Check(!aim.valid, "a rejected layout publishes no aim point");
}

void TestReprojection() {
    std::printf("-- reprojection\n");
    ProjectionBuffer pd = MakeLevelView();
    AimScreenOffset aim{};
    ViewProjection identity{};
    pdht::ue4::InjectHeadPose(pd.Address(), MakePose(0.0f, 0.0f, 0.0f), aim, identity);

    ReportReproject("identity centre", identity, 960.0f, 540.0f);
    ReportReproject("identity corner", identity, 100.0f, 200.0f);

    float x = 0.0f;
    float y = 0.0f;
    Check(pdht::ue4::ReprojectScreenPoint(identity, 123.0f, 456.0f, x, y),
          "an unrotated view reprojects");
    CheckNear(x, 123.0f, 1e-2f, "an unrotated view is the identity map in x");
    CheckNear(y, 456.0f, 1e-2f, "an unrotated view is the identity map in y");

    ProjectionBuffer rolled = MakeLevelView();
    ViewProjection rollView{};
    pdht::ue4::InjectHeadPose(rolled.Address(), MakePose(0.0f, 0.0f, 30.0f), aim, rollView);
    ReportReproject("roll centre", rollView, 960.0f, 540.0f);
    ReportReproject("roll corner", rollView, 1500.0f, 300.0f);
    Check(pdht::ue4::ReprojectScreenPoint(rollView, 960.0f, 540.0f, x, y),
          "the view centre reprojects under roll");
    CheckNear(x, 960.0f, 1e-2f, "roll leaves the view centre where it is (x)");
    CheckNear(y, 540.0f, 1e-2f, "roll leaves the view centre where it is (y)");

    ProjectionBuffer turned = MakeLevelView();
    ViewProjection turnedView{};
    pdht::ue4::InjectHeadPose(turned.Address(), MakePose(0.0f, 100.0f, 0.0f), aim, turnedView);
    Check(!pdht::ue4::ReprojectScreenPoint(turnedView, 960.0f, 540.0f, x, y),
          "a point behind the tracked view is refused rather than mirrored");

    ViewProjection invalid = identity;
    invalid.valid = false;
    Check(!pdht::ue4::ReprojectScreenPoint(invalid, 960.0f, 540.0f, x, y),
          "an invalid view reprojects nothing");

    ViewProjection degenerate = identity;
    degenerate.rect[2] = degenerate.rect[0];
    Check(!pdht::ue4::ReprojectScreenPoint(degenerate, 960.0f, 540.0f, x, y),
          "a zero-width rect reprojects nothing");
}

void TestProfileRegistry() {
    std::printf("-- build registry\n");
    Check(pdht::ue4::kKnownProfileCount >= 1, "at least one build profile is registered");
    for (std::size_t i = 0; i < pdht::ue4::kKnownProfileCount; ++i) {
        const pdht::ue4::BuildProfile& p = *pdht::ue4::kKnownProfiles[i];
        std::printf("   %-24s TDS=0x%08X SOI=0x%08X CSUM=0x%08X proj=0x%zX "
                    "srt=0x%zX vps=0x%zX spiv=0x%zX pov=0x%zX\n",
                    p.name, p.fingerprint.TimeDateStamp, p.fingerprint.SizeOfImage,
                    p.fingerprint.CheckSum, p.getProjectionDataRva,
                    p.setRenderTranslationRva, p.getViewportScaleRva,
                    p.setPositionInViewportRva, p.povRotationOffset);
    }
}

}  // namespace

int main() {
    std::printf("=== pdht view characterization ===\n");
    TestFovDerivation();
    TestZeroPoseIsIdentity();
    TestRotationAxes();
    TestAimProjection();
    TestWorldVersusLocalYaw();
    TestPositionOffsetUsesCleanAxes();
    TestFovOffset();
    TestLayoutRejection();
    TestReprojection();
    TestProfileRegistry();
    std::printf("=== %s ===\n", g_failures == 0 ? "all invariants held" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
