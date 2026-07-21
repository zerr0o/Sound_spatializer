/*
 * Minimal non-offloaded WaveRT render filter for Sound Spatializer.
 *
 * The filter topology follows Microsoft's SysVAD HDMI loopback pattern, but
 * deliberately advertises one format only: stereo IEEE float at 48 kHz.
 */

#ifndef SOUND_SPATIALIZER_WAVTABLE_H
#define SOUND_SPATIALIZER_WAVTABLE_H

#include "../include/SoundSpatializerDriverContract.h"

#define SOUND_SPATIALIZER_DEVICE_MAX_CHANNELS       SOUND_SPATIALIZER_CHANNEL_COUNT
#define SOUND_SPATIALIZER_MAX_INPUT_SYSTEM_STREAMS  1
#define SOUND_SPATIALIZER_MAX_OUTPUT_LOOPBACK_STREAMS MAX_OUTPUT_LOOPBACK_STREAMS

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE SoundSpatializerSupportedFormats[] =
{
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                SOUND_SPATIALIZER_CHANNEL_COUNT,
                SOUND_SPATIALIZER_SAMPLE_RATE,
                SOUND_SPATIALIZER_SAMPLE_RATE * SOUND_SPATIALIZER_CHANNEL_COUNT * (SOUND_SPATIALIZER_BITS_PER_SAMPLE / 8),
                SOUND_SPATIALIZER_CHANNEL_COUNT * (SOUND_SPATIALIZER_BITS_PER_SAMPLE / 8),
                SOUND_SPATIALIZER_BITS_PER_SAMPLE,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX),
            },
            SOUND_SPATIALIZER_BITS_PER_SAMPLE,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
        },
    },
};

static MODE_AND_DEFAULT_FORMAT SoundSpatializerSupportedModes[] =
{
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,            &SoundSpatializerSupportedFormats[0].DataFormat },
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,        &SoundSpatializerSupportedFormats[0].DataFormat },
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_MEDIA,          &SoundSpatializerSupportedFormats[0].DataFormat },
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_MOVIE,          &SoundSpatializerSupportedFormats[0].DataFormat },
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS, &SoundSpatializerSupportedFormats[0].DataFormat },
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_NOTIFICATION,   &SoundSpatializerSupportedFormats[0].DataFormat },
};

static PIN_DEVICE_FORMATS_AND_MODES SoundSpatializerPinDeviceFormatsAndModes[] =
{
    {
        SystemRenderPin,
        SoundSpatializerSupportedFormats,
        SIZEOF_ARRAY(SoundSpatializerSupportedFormats),
        SoundSpatializerSupportedModes,
        SIZEOF_ARRAY(SoundSpatializerSupportedModes),
    },
    {
        RenderLoopbackPin,
        SoundSpatializerSupportedFormats,
        SIZEOF_ARRAY(SoundSpatializerSupportedFormats),
        NULL,
        0,
    },
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0,
    },
};

static KSDATARANGE_AUDIO SoundSpatializerStreamingDataRange =
{
    {
        sizeof(KSDATARANGE_AUDIO),
        KSDATARANGE_ATTRIBUTES,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
    },
    SOUND_SPATIALIZER_CHANNEL_COUNT,
    SOUND_SPATIALIZER_BITS_PER_SAMPLE,
    SOUND_SPATIALIZER_BITS_PER_SAMPLE,
    SOUND_SPATIALIZER_SAMPLE_RATE,
    SOUND_SPATIALIZER_SAMPLE_RATE,
};

static KSDATARANGE_AUDIO SoundSpatializerLoopbackDataRange =
{
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
    },
    SOUND_SPATIALIZER_CHANNEL_COUNT,
    SOUND_SPATIALIZER_BITS_PER_SAMPLE,
    SOUND_SPATIALIZER_BITS_PER_SAMPLE,
    SOUND_SPATIALIZER_SAMPLE_RATE,
    SOUND_SPATIALIZER_SAMPLE_RATE,
};

static PKSDATARANGE SoundSpatializerStreamingDataRangePointers[] =
{
    PKSDATARANGE(&SoundSpatializerStreamingDataRange),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

static PKSDATARANGE SoundSpatializerLoopbackDataRangePointers[] =
{
    PKSDATARANGE(&SoundSpatializerLoopbackDataRange),
};

static KSDATARANGE SoundSpatializerBridgeDataRange =
{
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE),
};

static PKSDATARANGE SoundSpatializerBridgeDataRangePointers[] =
{
    &SoundSpatializerBridgeDataRange,
};

static PCPIN_DESCRIPTOR SoundSpatializerWavePins[] =
{
    {
        SOUND_SPATIALIZER_MAX_INPUT_SYSTEM_STREAMS,
        SOUND_SPATIALIZER_MAX_INPUT_SYSTEM_STREAMS,
        0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(SoundSpatializerStreamingDataRangePointers),
            SoundSpatializerStreamingDataRangePointers,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0,
        },
    },
    {
        SOUND_SPATIALIZER_MAX_OUTPUT_LOOPBACK_STREAMS,
        SOUND_SPATIALIZER_MAX_OUTPUT_LOOPBACK_STREAMS,
        0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(SoundSpatializerLoopbackDataRangePointers),
            SoundSpatializerLoopbackDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSNODETYPE_AUDIO_LOOPBACK,
            NULL,
            0,
        },
    },
    {
        0,
        0,
        0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(SoundSpatializerBridgeDataRangePointers),
            SoundSpatializerBridgeDataRangePointers,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0,
        },
    },
};

static PCPROPERTY_ITEM SoundSpatializerVolumeProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(SoundSpatializerVolumeAutomation, SoundSpatializerVolumeProperties);

static PCPROPERTY_ITEM SoundSpatializerMuteProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(SoundSpatializerMuteAutomation, SoundSpatializerMuteProperties);

static PCPROPERTY_ITEM SoundSpatializerPeakProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_PEAKMETER2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(SoundSpatializerPeakAutomation, SoundSpatializerPeakProperties);

static PCNODE_DESCRIPTOR SoundSpatializerWaveNodes[] =
{
    { 0, NULL,                                &KSNODETYPE_SUM,       NULL },
    { 0, &SoundSpatializerVolumeAutomation,   &KSNODETYPE_VOLUME,    &KSAUDFNAME_WAVE_VOLUME },
    { 0, &SoundSpatializerMuteAutomation,     &KSNODETYPE_MUTE,      &KSAUDFNAME_WAVE_MUTE },
    { 0, &SoundSpatializerPeakAutomation,     &KSNODETYPE_PEAKMETER, &KSAUDFNAME_PEAKMETER },
};

C_ASSERT(KSNODE_WAVE_SUM == 0);
C_ASSERT(KSNODE_WAVE_VOLUME == 1);
C_ASSERT(KSNODE_WAVE_MUTE == 2);
C_ASSERT(KSNODE_WAVE_PEAKMETER == 3);

static PCCONNECTION_DESCRIPTOR SoundSpatializerWaveConnections[] =
{
    { PCFILTER_NODE,             KSPIN_WAVE_RENDER2_SINK_SYSTEM,   KSNODE_WAVE_SUM,       1 },
    { KSNODE_WAVE_SUM,           0,                                KSNODE_WAVE_VOLUME,    1 },
    { KSNODE_WAVE_VOLUME,        0,                                KSNODE_WAVE_MUTE,      1 },
    { KSNODE_WAVE_MUTE,          0,                                KSNODE_WAVE_PEAKMETER, 1 },
    { KSNODE_WAVE_PEAKMETER,     2,                                PCFILTER_NODE,          KSPIN_WAVE_RENDER2_SINK_LOOPBACK },
    { KSNODE_WAVE_PEAKMETER,     0,                                PCFILTER_NODE,          KSPIN_WAVE_RENDER2_SOURCE },
};

static PCPROPERTY_ITEM SoundSpatializerWaveFilterProperties[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter,
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(SoundSpatializerWaveFilterAutomation, SoundSpatializerWaveFilterProperties);

static PCFILTER_DESCRIPTOR SoundSpatializerWaveMiniportFilterDescriptor =
{
    0,
    &SoundSpatializerWaveFilterAutomation,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(SoundSpatializerWavePins),
    SoundSpatializerWavePins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(SoundSpatializerWaveNodes),
    SoundSpatializerWaveNodes,
    SIZEOF_ARRAY(SoundSpatializerWaveConnections),
    SoundSpatializerWaveConnections,
    0,
    NULL,
};

#endif // SOUND_SPATIALIZER_WAVTABLE_H
