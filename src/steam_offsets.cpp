// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "ue4.h"

// Steam build profiles. Append-only: a patch that shifts the binary adds a new
// entry below and goes to the top of kKnownProfiles in ue4.cpp, so a player who
// has not taken the patch keeps matching the profile they are already on.
namespace pdht::ue4 {

extern const BuildProfile kSteamProfile_20260604 = {
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

}  // namespace pdht::ue4
