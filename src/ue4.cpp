// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "ue4.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace pdht::ue4 {

// Known builds, newest first (top = diagnostic primary). Append-only: a patch
// that shifts the binary adds a new entry, never edits an existing one.
// Fingerprint captured 2026-06-04 from the live Steam build.
static const BuildProfile kSteamProfile_20260604 = {
    "steam-win64-20260604",
    {0x698D49C3u, 0x05B01000u, 0x05826D8Cu},  // TimeDateStamp, SizeOfImage, CheckSum
    // CameraCachePrivate.POV.Rotation in APlayerCameraManager. Read-only - this
    // is the view point APlayerController::GetPlayerViewPoint returns, so every
    // interaction trace and AI vision check goes through it, and the whole point
    // of hooking the render side instead is that the mod never writes it.
    //
    // From UE property reflection (CameraCachePrivate at +0x1AE0) plus the
    // 16-byte alignment FMinimalViewInfo inherits from FPostProcessSettings,
    // which puts FCameraCacheEntry::POV at entry+0x10, i.e. Location at +0x1AF0
    // and Rotation 12 bytes past it. An earlier session found live POV data at
    // +0xEA0 by change-detection and labelled it CameraCachePrivate; reflection
    // says +0xE90 is ViewTarget, so +0xEA0 was ViewTarget.POV. Both track the
    // view - UpdateCamera computes into the view target and the cache is filled
    // from it - but only the cache is what gameplay reads, so that is the one
    // worth watching.
    0x1AFC,  // povRotationOffset
    // ULocalPlayer::GetProjectionData. Found 2026-08-25 by walking
    // ULocalPlayer::GetPrivateStaticClass (the only referrer of the UTF-16
    // "ULocalPlayer" at rva 0x4941718) -> InternalConstructor -> the default
    // constructor at 0x2df27a0, which stores the class vtable (rva 0x478e648);
    // slot 89 is the only entry that writes an FIntRect pair at +0x90/+0xA0 of
    // its fourth argument, which is FSceneViewProjectionData::SetViewRectangle.
    0x2dfb9d0,  // getProjectionDataRva
    // UWidget::SetRenderTranslation and UWidgetLayoutLibrary::GetViewportScale.
    // Found 2026-08-25 by reading UFunction::Func (instance offset +0xD8) for
    // /Script/UMG.Widget.SetRenderTranslation and
    // /Script/UMG.WidgetLayoutLibrary.GetViewportScale out of the running
    // process, then decompiling the exec thunks the pointers led to. The first
    // is literally { this->RenderTransform.Translation = arg; UpdateRenderTransform(); }.
    0x2852260,  // setRenderTranslationRva
    0x281bd30,  // getViewportScaleRva
    // UUserWidget::SetPositionInViewport(FVector2D, bool). Found 2026-08-26 from
    // the native-function registration table: the ANSI name
    // "SetPositionInViewport" sits at rva 0x45015F8, the table entry pointing at
    // it is 0x4506280, and the qword after that entry is the exec thunk
    // (0x2889000) whose single call is this native.
    0x2851cd0,  // setPositionInViewportRva
};

const BuildProfile* const kKnownProfiles[] = {
    &kSteamProfile_20260604,
};
const std::size_t kKnownProfileCount =
    sizeof(kKnownProfiles) / sizeof(kKnownProfiles[0]);

bool GetGameModule(std::uintptr_t& base, std::uintptr_t& end) {
    HMODULE h = GetModuleHandleW(nullptr);
    if (!h) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    base = reinterpret_cast<std::uintptr_t>(h);
    end = base + nt->OptionalHeader.SizeOfImage;
    return true;
}

const BuildProfile* MatchProfile(void* moduleBase) {
    cameraunlock::memory::PeFingerprint running{};
    if (!cameraunlock::memory::ReadPeFingerprint(moduleBase, running)) {
        return nullptr;
    }
    for (std::size_t i = 0; i < kKnownProfileCount; ++i) {
        if (kKnownProfiles[i]->fingerprint.Matches(running)) {
            return kKnownProfiles[i];
        }
    }
    return nullptr;
}

namespace {

constexpr std::size_t kProjViewOrigin = 0x00;
constexpr std::size_t kProjViewRotationMatrix = 0x10;
// ProjectionMatrix[0][0] and [1][1] are the horizontal and vertical
// half-tangent reciprocals, so they are both the perspective divide and the
// live FOV: whatever the game asked for this frame is already in them, ahead of
// any FOV setting the game does or does not expose, and no engine FOV field has
// to be found or trusted to read it.
constexpr std::size_t kProjMatrixXX = 0x50;
constexpr std::size_t kProjMatrixYY = 0x64;
constexpr std::size_t kProjViewRect = 0x90;

constexpr float kDegToRad = 0.01745329252f;
constexpr float kRadToDeg = 57.2957795131f;

// The offset FOV is clamped to this before it becomes a projection matrix. Not
// a taste range - it is what keeps tan() away from the asymptote and the near
// plane out of the player's face if a config file asks for something absurd.
constexpr float kMinFovDegrees = 20.0f;
constexpr float kMaxFovDegrees = 170.0f;

// ProjectionMatrix[0][0] is only the reciprocal half-tangent of a PERSPECTIVE
// view. GetProjectionData also builds orthographic and otherwise degenerate
// views, and for those the number means nothing - but atan(1/x) happily returns
// ~180 degrees for a tiny one, the offset then clamps to kMaxFovDegrees, and the
// element is rewritten scaled by up to a factor of a million. This band is 5.7
// to 169 degrees horizontal, wider than any real game view, and anything outside
// it is left exactly as the engine built it.
constexpr float kMinPerspectiveProjScale = 0.1f;
constexpr float kMaxPerspectiveProjScale = 20.0f;

// Below this the aim direction is edge-on to the view plane and the perspective
// divide runs away: measured at 89.94 degrees of head yaw the divide produced
// 916,721 pixels, and that number goes straight into RenderTransform.Translation.
// cos(80 degrees).
constexpr float kMinAimForward = 0.1736f;

struct Vec3 {
    float x, y, z;
};

float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 RotateAboutAxis(const Vec3& v, const Vec3& axis, float degrees) {
    const float rad = degrees * kDegToRad;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float d = Dot(axis, v) * (1.0f - c);
    const Vec3 cross{axis.y * v.z - axis.z * v.y,
                     axis.z * v.x - axis.x * v.z,
                     axis.x * v.y - axis.y * v.x};
    return {v.x * c + cross.x * s + axis.x * d,
            v.y * c + cross.y * s + axis.y * d,
            v.z * c + cross.z * s + axis.z * d};
}

// A view rotation matrix's columns are orthonormal. Anything else means the
// offsets are not pointing at one, and writing rotated axes back would corrupt
// whatever struct is really there.
constexpr float kBasisTolerance = 0.01f;

bool IsUnitLength(const Vec3& v) {
    return std::fabs(std::sqrt(Dot(v, v)) - 1.0f) <= kBasisTolerance;
}

bool IsOrthonormalBasis(const Vec3& right, const Vec3& up, const Vec3& fwd) {
    if (!IsUnitLength(right) || !IsUnitLength(up) || !IsUnitLength(fwd)) return false;
    return std::fabs(Dot(right, up)) <= kBasisTolerance
        && std::fabs(Dot(right, fwd)) <= kBasisTolerance
        && std::fabs(Dot(up, fwd)) <= kBasisTolerance;
}

struct ViewParams {
    float matrix[16];
    float origin[3];
    // As the engine built them, i.e. the FOV the game asked for.
    float projXX;
    float projYY;
    // After the FOV offset. Equal to the two above when the offset is 0.
    float trackedProjXX;
    float trackedProjYY;
    std::int32_t rect[4];  // Min.X, Min.Y, Max.X, Max.Y
};

bool ReadProjection(std::uintptr_t pd, ViewParams& v) {
    __try {
        std::memcpy(v.matrix, reinterpret_cast<const void*>(pd + kProjViewRotationMatrix),
                    sizeof(v.matrix));
        std::memcpy(v.origin, reinterpret_cast<const void*>(pd + kProjViewOrigin),
                    sizeof(v.origin));
        std::memcpy(&v.projXX, reinterpret_cast<const void*>(pd + kProjMatrixXX),
                    sizeof(v.projXX));
        std::memcpy(&v.projYY, reinterpret_cast<const void*>(pd + kProjMatrixYY),
                    sizeof(v.projYY));
        std::memcpy(v.rect, reinterpret_cast<const void*>(pd + kProjViewRect), sizeof(v.rect));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Clean aim in the tracked view, perspective-divided through the projection
// matrix the engine just built, then scaled to pixels from the view centre.
//
// The aim point is taken at infinity: the interaction trace starts at the clean
// view origin, and once the head has also LEANED that ray projects to a line
// rather than a point, so there is no single correct pixel without the hit
// distance. The residual is exactly (lean / distance) in NDC, which is zero
// whenever the player is not leaning and worst for a near interaction at full
// lean - at the shipped LimitX of 0.30 m and a 1 m in-car interaction (glovebox,
// radio, ignition) it is 0.30 NDC, i.e. 288 px off centre on a 1920-wide frame
// at 90 degrees horizontal. Taking the hit distance from the game's own
// highlight system is the fix, and it is not done yet.
AimScreenOffset ProjectCleanAim(const ViewParams& v, const Vec3& cleanFwd,
                                const Vec3& right, const Vec3& up, const Vec3& fwd) {
    AimScreenOffset out{0.0f, 0.0f, false};
    const float width = static_cast<float>(v.rect[2] - v.rect[0]);
    const float height = static_cast<float>(v.rect[3] - v.rect[1]);
    if (width <= 0.0f || height <= 0.0f) return out;

    const float dx = Dot(cleanFwd, right);
    const float dy = Dot(cleanFwd, up);
    const float vz = Dot(cleanFwd, fwd);

    // The tracked pair, because this is a position in the frame being rendered:
    // a wider FOV puts the same aim direction closer to the centre in pixels.
    float ndcX = 0.0f;
    float ndcY = 0.0f;
    if (vz > kMinAimForward) {
        ndcX = dx * v.trackedProjXX / vz;
        ndcY = dy * v.trackedProjYY / vz;
    } else {
        // Edge-on or behind. The divide is meaningless here, so point at the
        // screen edge along the direction the aim lies in - which is what the
        // doctrine asks for when the aim point leaves the view. Recentring
        // instead would put the crosshair where the player is NOT aiming, and
        // letting the divide run puts it a million pixels away.
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1e-6f) return out;  // exactly opposite the view: no direction
        ndcX = dx / len;
        ndcY = dy / len;
    }

    // Clamp to the edge of the frame. Past 80 degrees the divide alone still
    // reaches thousands of pixels, and the crosshair belongs on screen.
    const float extent = std::fmax(std::fabs(ndcX), std::fabs(ndcY));
    if (extent > 1.0f) {
        ndcX /= extent;
        ndcY /= extent;
    }

    out.x = ndcX * 0.5f * width;
    out.y = -ndcY * 0.5f * height;  // NDC y is up, screen y is down
    out.valid = true;
    return out;
}

bool WriteProjection(std::uintptr_t pd, const Vec3& right, const Vec3& up, const Vec3& fwd,
                     const float* origin, float projXX, float projYY) {
    __try {
        *reinterpret_cast<float*>(pd + kProjMatrixXX) = projXX;
        *reinterpret_cast<float*>(pd + kProjMatrixYY) = projYY;
        auto* m = reinterpret_cast<float*>(pd + kProjViewRotationMatrix);
        m[0] = right.x;  m[4] = right.y;  m[8] = right.z;
        m[1] = up.x;     m[5] = up.y;     m[9] = up.z;
        m[2] = fwd.x;    m[6] = fwd.y;    m[10] = fwd.z;
        auto* o = reinterpret_cast<float*>(pd + kProjViewOrigin);
        o[0] = origin[0];
        o[1] = origin[1];
        o[2] = origin[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A FOV change is a uniform scale of the whole frame about its centre, so both
// elements move by the same ratio and the aspect ratio is preserved whatever the
// engine folded into them.
void ApplyFovOffset(ViewParams& v, float offsetDegrees) {
    v.trackedProjXX = v.projXX;
    v.trackedProjYY = v.projYY;
    if (offsetDegrees == 0.0f || v.projYY <= 0.0f) return;
    // Not just "> 0": a projection element outside the perspective band is not a
    // half-tangent reciprocal at all, and treating it as one rewrites it scaled
    // by orders of magnitude.
    if (v.projXX < kMinPerspectiveProjScale || v.projXX > kMaxPerspectiveProjScale) return;

    float fov = FovDegreesFromProjectionScale(v.projXX) + offsetDegrees;
    if (fov < kMinFovDegrees) fov = kMinFovDegrees;
    if (fov > kMaxFovDegrees) fov = kMaxFovDegrees;
    v.trackedProjXX = 1.0f / std::tan(0.5f * fov * kDegToRad);
    v.trackedProjYY = v.projYY * (v.trackedProjXX / v.projXX);
}

}  // namespace

float FovDegreesFromProjectionScale(float projScale) {
    if (projScale <= 0.0f) return 0.0f;
    return 2.0f * std::atan(1.0f / projScale) * kRadToDeg;
}

bool ReadProjectionScale(std::uintptr_t projectionData, float& projXX, float& projYY) {
    __try {
        projXX = *reinterpret_cast<const float*>(projectionData + kProjMatrixXX);
        projYY = *reinterpret_cast<const float*>(projectionData + kProjMatrixYY);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReprojectScreenPoint(const ViewProjection& view, float xIn, float yIn,
                          float& xOut, float& yOut) {
    if (!view.valid) return false;
    const float width = static_cast<float>(view.rect[2] - view.rect[0]);
    const float height = static_cast<float>(view.rect[3] - view.rect[1]);
    if (width <= 0.0f || height <= 0.0f) return false;
    if (view.cleanProjXX == 0.0f || view.cleanProjYY == 0.0f) return false;
    if (view.trackedProjXX == 0.0f || view.trackedProjYY == 0.0f) return false;

    // Screen -> NDC -> a direction in the CLEAN view, treating the point as
    // infinitely far away. That is exact for a rotation of the view; a positional
    // lean leaves a small residual, the same one the aim offset has.
    const float ndcX = 2.0f * (xIn - static_cast<float>(view.rect[0])) / width - 1.0f;
    const float ndcY = 1.0f - 2.0f * (yIn - static_cast<float>(view.rect[1])) / height;
    const float localX = ndcX / view.cleanProjXX;
    const float localY = ndcY / view.cleanProjYY;

    float dir[3];
    for (int i = 0; i < 3; ++i) {
        dir[i] = view.cleanRight[i] * localX + view.cleanUp[i] * localY + view.cleanFwd[i];
    }

    auto dot3 = [](const float* a, const float* b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const float viewZ = dot3(dir, view.trackedFwd);
    if (viewZ <= 0.001f) return false;  // behind the tracked view

    const float outNdcX = dot3(dir, view.trackedRight) * view.trackedProjXX / viewZ;
    const float outNdcY = dot3(dir, view.trackedUp) * view.trackedProjYY / viewZ;
    xOut = static_cast<float>(view.rect[0]) + 0.5f * (outNdcX + 1.0f) * width;
    yOut = static_cast<float>(view.rect[1]) + 0.5f * (1.0f - outNdcY) * height;
    return true;
}

InjectStatus InjectHeadPose(std::uintptr_t projectionData, const HeadPose& pose,
                            AimScreenOffset& aimOut, ViewProjection& viewOut) {
    aimOut = AimScreenOffset{0.0f, 0.0f, false};
    viewOut.valid = false;
    ViewParams v{};
    if (!ReadProjection(projectionData, v)) return InjectStatus::ReadFailed;
    ApplyFovOffset(v, pose.fovOffsetDegrees);
    const float* m = v.matrix;
    float* const origin = v.origin;

    const Vec3 cleanRight{m[0], m[4], m[8]};
    const Vec3 cleanUp{m[1], m[5], m[9]};
    const Vec3 cleanFwd{m[2], m[6], m[10]};
    if (!IsOrthonormalBasis(cleanRight, cleanUp, cleanFwd)) return InjectStatus::NotOrthonormal;

    // World-space yaw turns the whole basis about world up: at a steep
    // game-camera pitch a camera-local yaw leans the horizon, and UE's own
    // FRotator yaw is world-up too, so this matches what the engine would have
    // produced. Camera-local yaw turns it about the basis' own up instead, which
    // leaves up untouched and is the leaning behaviour some players want.
    constexpr Vec3 kWorldUp{0.0f, 0.0f, 1.0f};
    const Vec3 yawAxis = pose.worldSpaceYaw ? kWorldUp : cleanUp;
    Vec3 right = RotateAboutAxis(cleanRight, yawAxis, pose.yaw);
    Vec3 up = RotateAboutAxis(cleanUp, yawAxis, pose.yaw);
    Vec3 fwd = RotateAboutAxis(cleanFwd, yawAxis, pose.yaw);

    // Negated: a positive rotation about the right axis pitches the view down,
    // and positive pitch has to look up.
    fwd = RotateAboutAxis(fwd, right, -pose.pitch);
    up = RotateAboutAxis(up, right, -pose.pitch);

    right = RotateAboutAxis(right, fwd, pose.roll);
    up = RotateAboutAxis(up, fwd, pose.roll);

    // Along the CLEAN axes: the offset is where the head has moved relative to
    // the body, not relative to where it is currently looking.
    origin[0] += cleanRight.x * pose.offsetRight + cleanUp.x * pose.offsetUp
               + cleanFwd.x * pose.offsetForward;
    origin[1] += cleanRight.y * pose.offsetRight + cleanUp.y * pose.offsetUp
               + cleanFwd.y * pose.offsetForward;
    origin[2] += cleanRight.z * pose.offsetRight + cleanUp.z * pose.offsetUp
               + cleanFwd.z * pose.offsetForward;

    aimOut = ProjectCleanAim(v, cleanFwd, right, up, fwd);

    auto storeVec = [](float* dst, const Vec3& src) {
        dst[0] = src.x;
        dst[1] = src.y;
        dst[2] = src.z;
    };
    storeVec(viewOut.cleanRight, cleanRight);
    storeVec(viewOut.cleanUp, cleanUp);
    storeVec(viewOut.cleanFwd, cleanFwd);
    storeVec(viewOut.trackedRight, right);
    storeVec(viewOut.trackedUp, up);
    storeVec(viewOut.trackedFwd, fwd);
    viewOut.cleanProjXX = v.projXX;
    viewOut.cleanProjYY = v.projYY;
    viewOut.trackedProjXX = v.trackedProjXX;
    viewOut.trackedProjYY = v.trackedProjYY;
    for (int i = 0; i < 4; ++i) viewOut.rect[i] = v.rect[i];
    viewOut.valid = true;

    return WriteProjection(projectionData, right, up, fwd, origin, v.trackedProjXX,
                           v.trackedProjYY)
               ? InjectStatus::Ok
               : InjectStatus::WriteFailed;
}

}  // namespace pdht::ue4
