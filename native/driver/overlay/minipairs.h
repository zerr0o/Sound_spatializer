/*
 * Sound Spatializer endpoint overlay for the Microsoft SysVAD sample.
 *
 * This header intentionally exposes exactly one render endpoint. The generic
 * adapter, topology and WaveRT implementations are supplied by the pinned
 * Windows-driver-samples dependency.
 */

#ifndef SOUND_SPATIALIZER_MINIPAIRS_H
#define SOUND_SPATIALIZER_MINIPAIRS_H

#include "speakertopo.h"
#include "speakertoptable.h"
#include "soundspatializerwavtable.h"

NTSTATUS
CreateMiniportWaveRTSYSVAD(
    _Out_ PUNKNOWN*,
    _In_ REFCLSID,
    _In_opt_ PUNKNOWN,
    _In_ POOL_FLAGS,
    _In_ PUNKNOWN,
    _In_opt_ PVOID,
    _In_ PENDPOINT_MINIPAIR);

NTSTATUS
CreateMiniportTopologySYSVAD(
    _Out_ PUNKNOWN*,
    _In_ REFCLSID,
    _In_opt_ PUNKNOWN,
    _In_ POOL_FLAGS,
    _In_ PUNKNOWN,
    _In_opt_ PVOID,
    _In_ PENDPOINT_MINIPAIR);

// A 2 ms transport floor and a 128-frame default processing quantum are
// advertised to IAudioClient3. Hardware and the Windows audio engine remain
// free to select a larger shared-mode period.
static KSAUDIO_PACKETSIZE_CONSTRAINTS2 SoundSpatializerPacketConstraints =
{
    2 * HNSTIME_PER_MILLISECOND,
    FILE_BYTE_ALIGNMENT,
    0,
    1,
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        128,
        0,
    },
};

static const SYSVAD_DEVPROPERTY SoundSpatializerWaveInterfaceProperties[] =
{
    {
        &DEVPKEY_KsAudio_PacketSize_Constraints2,
        DEVPROP_TYPE_BINARY,
        sizeof(SoundSpatializerPacketConstraints),
        &SoundSpatializerPacketConstraints,
    },
};

static PHYSICALCONNECTIONTABLE SoundSpatializerTopologyConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,
        KSPIN_WAVE_RENDER2_SOURCE,
        CONNECTIONTYPE_WAVE_OUTPUT,
    },
};

static ENDPOINT_MINIPAIR SoundSpatializerMiniports =
{
    // The non-offload WaveRT graph follows SysVAD's render2/HDMI graph (SUM,
    // volume, mute, peak). This device type selects the matching generic
    // property handlers; the endpoint form factor remains Speakers in the INF.
    eHdmiRenderDevice,
    L"TopologySoundSpatializer",
    NULL,
    CreateMiniportTopologySYSVAD,
    &SpeakerTopoMiniportFilterDescriptor,
    0,
    NULL,
    L"WaveSoundSpatializer",
    NULL,
    CreateMiniportWaveRTSYSVAD,
    &SoundSpatializerWaveMiniportFilterDescriptor,
    ARRAYSIZE(SoundSpatializerWaveInterfaceProperties),
    SoundSpatializerWaveInterfaceProperties,
    SOUND_SPATIALIZER_DEVICE_MAX_CHANNELS,
    SoundSpatializerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SoundSpatializerPinDeviceFormatsAndModes),
    SoundSpatializerTopologyConnections,
    SIZEOF_ARRAY(SoundSpatializerTopologyConnections),
    ENDPOINT_LOOPBACK_SUPPORTED,
    NULL,
    0,
    NULL,
};

static PENDPOINT_MINIPAIR g_RenderEndpoints[] =
{
    &SoundSpatializerMiniports,
};

#define g_cRenderEndpoints SIZEOF_ARRAY(g_RenderEndpoints)

// adapter.cpp takes the address even when the count is zero, so retain a
// one-slot sentinel without exposing a capture endpoint.
static PENDPOINT_MINIPAIR g_CaptureEndpoints[1] = { NULL };
// Keep this as a runtime value.  A literal zero makes the upstream adapter's
// unsigned loop comparison trigger C4296 under the WDK's mandatory warning
// policy, even though the loop is intentionally empty.
static ULONG g_cCaptureEndpoints = 0;

#define g_MaxMiniports ((g_cRenderEndpoints + g_cCaptureEndpoints) * 2)

#endif // SOUND_SPATIALIZER_MINIPAIRS_H
