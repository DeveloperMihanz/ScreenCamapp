// ScreenCam Virtual Camera - identifiers shared by all native components.
// Copyright (c) ScreenCam contributors. MIT License.
#pragma once

#include <guiddef.h>

// CLSID of the ScreenCam Media Foundation media source (COM in-proc server,
// ScreenCam.VirtualCameraSource.dll). The Windows Camera Frame Server creates
// this class when an application opens "ScreenCam Virtual Camera".
// {DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}
static const GUID CLSID_ScreenCamMediaSource =
{ 0xdbbe7e28, 0xd5a6, 0x4c61, { 0x80, 0xe9, 0xc5, 0x1b, 0x85, 0xf9, 0x82, 0xf2 } };

#define SCREENCAM_SOURCE_CLSID_STRING L"{DBBE7E28-D5A6-4C61-80E9-C51B85F982F2}"
#define SCREENCAM_CAMERA_FRIENDLY_NAME L"ScreenCam Virtual Camera"
#define SCREENCAM_SOURCE_DESCRIPTION L"ScreenCam Virtual Camera Media Source"
