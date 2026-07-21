#pragma once

// Stable contract shared by the kernel package and user-mode discovery code.
// Changing any identifier in this file requires a contract-version migration.

#include <guiddef.h>
#include <devpropdef.h>

#define SOUND_SPATIALIZER_AUDIO_HARDWARE_ID       L"ROOT\\SOUNDSPATIALIZER_AUDIO"
#define SOUND_SPATIALIZER_AUDIO_SERVICE_NAME      L"SoundSpatializerAudio"
#define SOUND_SPATIALIZER_AUDIO_ENDPOINT_NAME     L"Sound Spatializer"
#define SOUND_SPATIALIZER_WAVE_REFERENCE_NAME     L"WaveSoundSpatializer"
#define SOUND_SPATIALIZER_TOPOLOGY_REFERENCE_NAME L"TopologySoundSpatializer"

#define SOUND_SPATIALIZER_DRIVER_CONTRACT_VERSION 1u
#define SOUND_SPATIALIZER_SAMPLE_RATE              48000u
#define SOUND_SPATIALIZER_CHANNEL_COUNT            2u
#define SOUND_SPATIALIZER_BITS_PER_SAMPLE          32u

// {EF58434D-ADA7-47E2-A2C4-4E8C58BA3E0B}
// INF-created device interface used to find the owning PnP device without
// relying on a localized friendly name. It is not an IOCTL surface in v1.
static const GUID GUID_DEVINTERFACE_SOUND_SPATIALIZER_AUDIO =
{
    0xef58434d, 0xada7, 0x47e2,
    { 0xa2, 0xc4, 0x4e, 0x8c, 0x58, 0xba, 0x3e, 0x0b }
};

// {B01E7F02-85B0-4CF9-B53D-75DFD2B05E07}
static const GUID SOUND_SPATIALIZER_ENDPOINT_PROPERTY_SET =
{
    0xb01e7f02, 0x85b0, 0x4cf9,
    { 0xb5, 0x3d, 0x75, 0xdf, 0xd2, 0xb0, 0x5e, 0x07 }
};

// These keys are placed below EP\0 by the INF and propagated to the MMDevice
// property store. Values are VT_UI4 / DEVPROP_TYPE_UINT32.
static const DEVPROPKEY DEVPKEY_SoundSpatializer_EndpointMarker =
{
    {
        0xb01e7f02, 0x85b0, 0x4cf9,
        { 0xb5, 0x3d, 0x75, 0xdf, 0xd2, 0xb0, 0x5e, 0x07 }
    },
    2
};

static const DEVPROPKEY DEVPKEY_SoundSpatializer_ContractVersion =
{
    {
        0xb01e7f02, 0x85b0, 0x4cf9,
        { 0xb5, 0x3d, 0x75, 0xdf, 0xd2, 0xb0, 0x5e, 0x07 }
    },
    3
};

