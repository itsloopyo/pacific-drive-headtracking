// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cstddef>
#include <cstdint>

#include "cameraunlock/memory/pe_fingerprint.h"

// UE4-specific types and reflection access for Pacific Drive (project
// "PenDriverPro", UE 4.27). Unlike cameraunlock-core/unreal/ue_math.h (which
// targets UE5 Large World Coordinates and uses doubles), UE4 ships float
// FVector/FRotator - 12 bytes each. Reading them as doubles overflows the
// engine's buffers, so every in-memory UE4 struct here is float-based.
namespace pdht::ue4 {

struct FRotator {  // degrees; Pitch about Y, Yaw about Z, Roll about X
    float Pitch, Yaw, Roll;
};

// Per-build profile. The two reflection globals (GUObjectArray, FNamePool) are
// not pinned: InitReflection brute-force-discovers them at runtime and
// self-validates the layout before any access, which is patch-resilient (a
// shifted binary is re-discovered, not stranded). The fingerprint is kept for
// the diagnostic log line and the optional POV-offset pins.
//
//   povRotationOffset - byte offset of the cached FMinimalViewInfo's Rotation
//       within APlayerCameraManager. Read-only: this is the view point the
//       gameplay simulation uses, and the mod never writes it. The FOV sits 12
//       bytes past it, which is the whole of what gets read. 0 => discover at
//       runtime.
//   getProjectionDataRva - RVA of ULocalPlayer::GetProjectionData, which builds
//       the renderer's view transform and is called by nothing in the gameplay
//       simulation. Hooked so the head pose lands in the rendered view only.
//       0 => no hook (mod stays in read-only discovery).
//   setRenderTranslationRva - RVA of UWidget::SetRenderTranslation(FVector2D),
//       used to move the crosshair to the aim point. 0 => no compensation.
//   getViewportScaleRva - RVA of UWidgetLayoutLibrary::GetViewportScale(UObject*),
//       the UMG DPI scale that converts the pixel offset to Slate units.
//       0 => no compensation.
//   setPositionInViewportRva - RVA of
//       UUserWidget::SetPositionInViewport(FVector2D, bool). Objective markers are
//       widgets added straight to the viewport and positioned through this, which
//       writes a Slate-side slot rather than any UMG field - so this is the only
//       place their screen position can be reached. 0 => markers stay put.
struct BuildProfile {
    const char* name;
    cameraunlock::memory::PeFingerprint fingerprint;
    std::size_t povRotationOffset;
    std::size_t getProjectionDataRva;
    std::size_t setRenderTranslationRva;
    std::size_t getViewportScaleRva;
    std::size_t setPositionInViewportRva;
};

extern const BuildProfile* const kKnownProfiles[];
extern const std::size_t kKnownProfileCount;

// Module base/end of the running game exe.
bool GetGameModule(std::uintptr_t& base, std::uintptr_t& end);

// Fingerprints the running module and returns the matching profile, or nullptr
// on an unknown build (discovery is still attempted - it self-validates).
const BuildProfile* MatchProfile(void* moduleBase);

// Brute-force-discovers FNamePool (anchored on the id-0 name "None") and
// GUObjectArray (validated by resolving the first object's class name), then
// installs the layout via cameraunlock::unreal::SetRuntime. Returns false (and
// the caller stays dormant) if discovery does not check out. objCountOut
// reports the live UObject count.
bool InitReflection(std::uintptr_t base, std::uintptr_t end, int& objCountOut);

// After InitReflection: the live APlayerCameraManager (or game subclass), or 0
// if not spawned yet (e.g. still at the main menu). Class-name scan - may return
// a stale instance from a streamed-out level.
std::uintptr_t FindCameraManager();

// The local PlayerController (non-CDO), or 0 if not spawned yet.
std::uintptr_t FindPlayerController();

// ClassPrivate of an object, or 0 if it cannot be read. One read: this is how a
// cached object pointer gets re-checked between frames. A full UObject scan
// touches every object header in the process - hundreds of thousands of
// scattered cache misses - and doing that on a timer is exactly what a periodic
// hitch looks like, so scan once and validate from then on.
std::uintptr_t ClassOf(std::uintptr_t obj);

// First non-CDO object whose class name contains @p classNameFragment, or 0.
// Unlike FindRuntimeInstances this does not require the transient package: level
// actors such as AWorldSettings are outered into the map package, and the
// fragment match picks up game subclasses.
std::uintptr_t FindLiveInstance(const char* classNameFragment);

// The UClass object with this exact name, or 0. Classes exist from startup, well
// before any instance of them does, so this is what property offsets are looked
// up against.
std::uintptr_t FindClassByName(const char* className);

// Byte offset of a UPROPERTY within instances of @p classObject, searched up the
// SuperStruct chain. UE4.25+ keeps properties in a separate FField list off
// UStruct::ChildProperties rather than in the UObject array, so this is the only
// way to reach them. Self-validated by the caller against an offset that is
// known independently. False when the class has no such property.
bool FindPropertyOffset(std::uintptr_t classObject, const char* propertyName,
                        std::size_t& offsetOut);

// Whether @p obj is still a live object, by looking it up in the global object
// array through its own InternalIndex and checking the array's flags. This is
// the check a cached pointer needs: comparing ClassPrivate only proves the
// memory has not been reused yet, so a destroyed object keeps passing. Actors
// are marked PendingKill the moment Destroy() runs, which makes this immediate
// for them rather than waiting on garbage collection.
bool IsObjectAlive(std::uintptr_t obj);

// Runtime instances of an already-resolved UClass, i.e. objects the game
// created rather than templates loaded off disk. One pointer compare per object:
// matching on the class NAME instead would resolve an FName and allocate a
// string for every object in the process, which at a few hundred thousand
// objects is a visible hitch when it runs on a timer.
int FindInstancesOfClass(std::uintptr_t classObject, std::uintptr_t* out, int maxInstances);

// The ACTIVE camera manager: the local PlayerController's PlayerCameraManager
// member (found by scanning the controller for a pointer to a
// PlayerCameraManager-class object - no hardcoded offset). pcOut/offOut report
// the controller instance and the member offset it was found at. Returns 0 if
// no controller/manager yet.
std::uintptr_t FindActiveCameraManager(std::uintptr_t& pcOut, std::size_t& offOut);

// Reads an FRotator (3 floats) at cameraManager+rotOffset. SEH-guarded.
bool ReadPovRotation(std::uintptr_t cameraManager, std::size_t rotOffset,
                     FRotator& out);
// Reads the cached POV's horizontal FOV in degrees. FMinimalViewInfo is
// Location, Rotation, FOV, so it sits 12 bytes past the rotation. This is the
// FOV the game asked for, which is only worth reading as a cross-check: what is
// actually rendered comes off the projection matrix. SEH-guarded.
bool ReadPovFov(std::uintptr_t cameraManager, std::size_t rotOffset, float& out);

// A head pose in the engine's own conventions, ready for injection: degrees for
// the rotation, centimetres along the clean view axes for the offset. Tracker
// sign, axis and unit conversion happens once at the boundary, before this.
struct HeadPose {
    float pitch;
    float yaw;
    float roll;
    float offsetRight;
    float offsetUp;
    float offsetForward;
    // True: yaw turns the view about world up, so the horizon stays level at any
    // game-camera pitch. False: yaw turns it about the camera's own up axis,
    // which leans the horizon once the camera is pitched.
    bool worldSpaceYaw;
    // Degrees ADDED to whatever horizontal FOV the game asked for this frame,
    // applied to the rendered projection only. Additive rather than absolute so
    // the game keeps its own FOV behaviour - Pacific Drive uses a wider FOV in
    // the car than on foot and widens it further with speed, and pinning one
    // number would flatten all of that and reframe the car interior it was
    // authored around. 0 leaves the projection exactly as the engine built it.
    float fovOffsetDegrees;
};

// FSceneViewProjectionData (UE4.27) is what ULocalPlayer::GetProjectionData
// fills in for the renderer:
//   +0x00 FVector  ViewOrigin          (12 bytes; FMatrix alignment pads to 16)
//   +0x10 FMatrix  ViewRotationMatrix  (row-major, world -> view)
//   +0x50 FMatrix  ProjectionMatrix
//   +0x90 FIntRect ViewRect
//   +0xA0 FIntRect ConstrainedViewRect
//
// The engine builds ViewRotationMatrix as FInverseRotationMatrix(Rotation)
// times an axis swap, and the swap leaves its columns holding the camera's
// world-space axes: column 0 right, column 1 up, column 2 forward. So the pose
// goes in by rotating those three axes and writing them back - no FRotator
// composition and no gimbal lock. Yaw is applied about world up by default so
// the horizon stays level when the game camera is pitched; HeadPose::worldSpaceYaw
// selects the camera's own up axis instead.
enum class InjectStatus {
    Ok,
    ReadFailed,
    NotOrthonormal,  // the offsets above do not describe a view rotation matrix
    WriteFailed,
};

// Where the clean aim direction lands on screen once the head pose is applied,
// in pixels from the centre of the view. This is the point the game's own
// interaction trace is pointing at, which is where the crosshair belongs.
struct AimScreenOffset {
    float x;
    float y;
    bool valid;  // false when the aim point is behind the tracked view
};

// The clean and tracked view bases for one frame, enough to map any screen point
// from the view the game thinks it has into the one the player is looking
// through. Captured while the view transform is built and read back on the same
// thread.
//
// A screen point cannot be corrected by adding the centre's displacement: under
// pure ROLL the centre does not move at all, so a translation leaves every marker
// exactly where it was while the world rotates underneath it. The mapping is a
// reprojection - unproject through the clean basis, project through the tracked
// one - which is exact for rotation at any screen position.
struct ViewProjection {
    float cleanRight[3];
    float cleanUp[3];
    float cleanFwd[3];
    float trackedRight[3];
    float trackedUp[3];
    float trackedFwd[3];
    // ProjectionMatrix [0][0] and [1][1] before and after the FOV offset. They
    // differ only when FovOffset is set, and the two are not interchangeable:
    // the game projected its HUD with the FOV it asked for, so a screen point
    // has to be UNprojected through the clean pair and reprojected through the
    // tracked one. Using one pair for both would silently drop the FOV change
    // out of every marker's position.
    float cleanProjXX;
    float cleanProjYY;
    float trackedProjXX;
    float trackedProjYY;
    std::int32_t rect[4];  // Min.X, Min.Y, Max.X, Max.Y
    bool valid;
};

// UWidget::RenderTransform, an FWidgetTransform laid out Translation (FVector2D),
// Scale (FVector2D), Shear, Angle. Known two ways on this build: the whole body
// of UWidget::SetRenderTranslation is `RenderTransform.Translation = arg;
// UpdateRenderTransform();` with the store at +0x90, and UE property reflection
// puts UWidget::RenderTransform at the same place - GameState::Initialize
// refuses to run at all if those two ever disagree. Scale therefore sits at
// +0x98, which is how the FOV correction reaches a widget: write the scale, then
// call SetRenderTranslation, whose UpdateRenderTransform() pushes the whole
// transform to Slate.
constexpr std::size_t kRenderTransformOffset = 0x90;
constexpr std::size_t kRenderTransformScaleOffset = kRenderTransformOffset + 8;

// The horizontal FOV the projection matrix actually renders, in degrees, from
// its [0][0] element (pass [1][1] for the vertical). This is exact by
// definition and independent of how the engine built the matrix: a view-space
// direction lands at NDC 1 - the edge of the viewport - when its tangent is
// 1/projXX, so the rendered half-angle IS atan(1/projXX). Deriving it from the
// engine's FOV field instead would have to guess at the aspect-ratio
// multipliers UE folds into the same element.
float FovDegreesFromProjectionScale(float projScale);

// Reads ProjectionMatrix [0][0] and [1][1] out of the projection data without
// touching anything. This is how the mod knows what FOV the game is rendering,
// and it runs on every hook call rather than only while tracking, so the log
// reports the FOV whether or not a tracker is connected. SEH-guarded.
bool ReadProjectionScale(std::uintptr_t projectionData, float& projXX, float& projYY);

// Maps a screen point in PIXELS from the clean view into the tracked view.
// False when the result would be behind the tracked view or the rect is
// degenerate, in which case the caller should leave the point alone.
bool ReprojectScreenPoint(const ViewProjection& view, float xIn, float yIn,
                          float& xOut, float& yOut);

// Composes @p pose into the render view transform at @p projectionData, and
// reports where clean aim projects into the result. The position offset is
// applied along the CLEAN view axes, so leaning follows body orientation rather
// than where the head is currently looking.
InjectStatus InjectHeadPose(std::uintptr_t projectionData, const HeadPose& pose,
                            AimScreenOffset& aimOut, ViewProjection& viewOut);

}  // namespace pdht::ue4
