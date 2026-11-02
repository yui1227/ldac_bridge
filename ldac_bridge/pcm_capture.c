// WASAPI audio capture for ldac_bridge

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <mmreg.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pcm_capture.h"

// GUIDs defined locally to avoid depending on libuuid contents
static const CLSID CLSID_MMDeviceEnumerator_ = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID   IID_IMMDeviceEnumerator_  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID   IID_IAudioClient_         = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID   IID_IAudioCaptureClient_  = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const IID   IID_IAudioEndpointVolume_ = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
static const GUID  SUBTYPE_IEEE_FLOAT_       = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID  SUBTYPE_PCM_              = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY PKEY_FriendlyName_  = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

#define LOOPBACK_FLAG       0x00020000  // AUDCLNT_STREAMFLAGS_LOOPBACK
#define RING_FRAMES         (192000 * 2)
#define PREBUFFER_MS        50
#define MAX_BUFFER_MS       250

typedef enum { SAMPLE_F32, SAMPLE_S16, SAMPLE_S24, SAMPLE_S32 } sample_type_t;

static float            ring[RING_FRAMES][2];
static uint32_t         ring_read;
static uint32_t         ring_count;
static CRITICAL_SECTION ring_lock;

static HANDLE           capture_thread;
static volatile LONG    capture_running;
static IMMDevice *      capture_device;
static bool             capture_loopback;
static uint32_t         device_rate;
static int              device_channels;
static sample_type_t    device_sample_type;
static HANDLE           capture_ready_event;
static bool             capture_ok;
static IAudioEndpointVolume * endpoint_volume;   // used from the main (Bluetooth) thread only
static float            stat_peak;

// consumer side (Bluetooth thread)
static uint32_t output_rate;
static double   resample_step;
static double   resample_phase;
static float    resample_prev[2];
static float    resample_cur[2];
static bool     jitter_running;
static uint32_t stat_underruns;
static uint32_t stat_overruns;

static void wide_to_utf8(const wchar_t * in, char * out, int out_len){
    out[0] = 0;
    if (in == NULL) return;
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, out_len, NULL, NULL);
}

static bool contains_case_insensitive(const char * haystack, const char * needle){
    size_t n = strlen(needle);
    for (const char * p = haystack; *p; p++){
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return true;
    }
    return n == 0;
}

static void get_device_name(IMMDevice * device, char * name, int name_len){
    name[0] = 0;
    IPropertyStore * store = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) return;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_FriendlyName_, &value)) && value.vt == VT_LPWSTR){
        wide_to_utf8(value.pwszVal, name, name_len);
    }
    PropVariantClear(&value);
    IPropertyStore_Release(store);
}

static IMMDeviceEnumerator * create_enumerator(void){
    IMMDeviceEnumerator * enumerator = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **) &enumerator);
    return SUCCEEDED(hr) ? enumerator : NULL;
}

// Search render endpoints (loopback) first, then capture endpoints
static IMMDevice * find_device(IMMDeviceEnumerator * enumerator, const char * filter, bool * is_loopback){
    const EDataFlow flows[2] = { eRender, eCapture };
    for (int f = 0; f < 2; f++){
        IMMDeviceCollection * collection = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, flows[f], DEVICE_STATE_ACTIVE, &collection))) continue;
        UINT count = 0;
        IMMDeviceCollection_GetCount(collection, &count);
        for (UINT i = 0; i < count; i++){
            IMMDevice * device = NULL;
            if (FAILED(IMMDeviceCollection_Item(collection, i, &device))) continue;
            char name[256];
            get_device_name(device, name, sizeof(name));
            if (contains_case_insensitive(name, filter)){
                IMMDeviceCollection_Release(collection);
                *is_loopback = (flows[f] == eRender);
                return device;
            }
            IMMDevice_Release(device);
        }
        IMMDeviceCollection_Release(collection);
    }
    return NULL;
}

void pcm_capture_list_devices(void){
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator * enumerator = create_enumerator();
    if (!enumerator){
        printf("Cannot create audio device enumerator\n");
        return;
    }
    const EDataFlow flows[2] = { eRender, eCapture };
    const char * titles[2] = { "Playback devices (captured via loopback):", "Recording devices:" };
    for (int f = 0; f < 2; f++){
        printf("%s\n", titles[f]);
        IMMDeviceCollection * collection = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, flows[f], DEVICE_STATE_ACTIVE, &collection))) continue;
        UINT count = 0;
        IMMDeviceCollection_GetCount(collection, &count);
        for (UINT i = 0; i < count; i++){
            IMMDevice * device = NULL;
            if (FAILED(IMMDeviceCollection_Item(collection, i, &device))) continue;
            char name[256];
            get_device_name(device, name, sizeof(name));
            printf("  - %s\n", name);
            IMMDevice_Release(device);
        }
        IMMDeviceCollection_Release(collection);
    }
    IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize();
}

static void ring_push(const float * frames, uint32_t num_frames){
    EnterCriticalSection(&ring_lock);
    for (uint32_t i = 0; i < num_frames; i++){
        if (ring_count == RING_FRAMES){
            // drop oldest
            ring_read = (ring_read + 1) % RING_FRAMES;
            ring_count--;
        }
        uint32_t w = (ring_read + ring_count) % RING_FRAMES;
        ring[w][0] = frames[2*i];
        ring[w][1] = frames[2*i+1];
        ring_count++;
        float l = fabsf(frames[2*i]);
        float r = fabsf(frames[2*i+1]);
        if (l > stat_peak) stat_peak = l;
        if (r > stat_peak) stat_peak = r;
    }
    LeaveCriticalSection(&ring_lock);
}

static float read_sample(const BYTE * p, sample_type_t type){
    switch (type){
        case SAMPLE_F32: return *(const float *) p;
        case SAMPLE_S16: return (float)(*(const int16_t *) p) / 32768.0f;
        case SAMPLE_S24: {
            int32_t v = (int32_t)((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24);
            return (float) v / 2147483648.0f;
        }
        case SAMPLE_S32: return (float)(*(const int32_t *) p) / 2147483648.0f;
        default:         return 0.0f;
    }
}

static int sample_size(sample_type_t type){
    switch (type){
        case SAMPLE_S16: return 2;
        case SAMPLE_S24: return 3;
        default:         return 4;
    }
}

static bool parse_format(const WAVEFORMATEX * wf){
    device_rate = wf->nSamplesPerSec;
    device_channels = wf->nChannels;
    WORD tag = wf->wFormatTag;
    WORD bits = wf->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE){
        const WAVEFORMATEXTENSIBLE * ext = (const WAVEFORMATEXTENSIBLE *) wf;
        if (IsEqualGUID(&ext->SubFormat, &SUBTYPE_IEEE_FLOAT_))  tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(&ext->SubFormat, &SUBTYPE_PCM_))    tag = WAVE_FORMAT_PCM;
        else return false;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32){ device_sample_type = SAMPLE_F32; return true; }
    if (tag == WAVE_FORMAT_PCM){
        switch (bits){
            case 16: device_sample_type = SAMPLE_S16; return true;
            case 24: device_sample_type = SAMPLE_S24; return true;
            case 32: device_sample_type = SAMPLE_S32; return true;
            default: return false;
        }
    }
    return false;
}

static DWORD WINAPI capture_thread_main(LPVOID arg){
    (void) arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IAudioClient * client = NULL;
    IAudioCaptureClient * capture = NULL;
    WAVEFORMATEX * mix_format = NULL;
    float * convert_buffer = NULL;
    HRESULT hr;

    hr = IMMDevice_Activate(capture_device, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **) &client);
    if (FAILED(hr)) { printf("Audio: Activate failed 0x%08lx\n", hr); goto done; }

    hr = IAudioClient_GetMixFormat(client, &mix_format);
    if (FAILED(hr) || !parse_format(mix_format)){ printf("Audio: unsupported mix format\n"); goto done; }

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, capture_loopback ? LOOPBACK_FLAG : 0,
                                 1000000 /* 100 ms */, 0, mix_format, NULL);
    if (FAILED(hr)) { printf("Audio: Initialize failed 0x%08lx\n", hr); goto done; }

    hr = IAudioClient_GetService(client, &IID_IAudioCaptureClient_, (void **) &capture);
    if (FAILED(hr)) { printf("Audio: GetService failed 0x%08lx\n", hr); goto done; }

    convert_buffer = (float *) malloc(sizeof(float) * 2 * device_rate);   // up to 1 s per packet
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) { printf("Audio: Start failed 0x%08lx\n", hr); goto done; }

    capture_ok = true;
    SetEvent(capture_ready_event);

    int bytes_per_sample = sample_size(device_sample_type);
    int bytes_per_frame  = bytes_per_sample * device_channels;

    while (InterlockedCompareExchange(&capture_running, 1, 1)){
        UINT32 packet_frames = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &packet_frames))) break;
        if (packet_frames == 0){
            Sleep(3);
            continue;
        }
        BYTE * data;
        UINT32 num_frames;
        DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &num_frames, &flags, NULL, NULL))) break;
        if (num_frames > device_rate) num_frames = device_rate;
        for (UINT32 i = 0; i < num_frames; i++){
            float l = 0.0f, r = 0.0f;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0){
                const BYTE * frame = data + i * bytes_per_frame;
                l = read_sample(frame, device_sample_type);
                r = device_channels > 1 ? read_sample(frame + bytes_per_sample, device_sample_type) : l;
            }
            convert_buffer[2*i]   = l;
            convert_buffer[2*i+1] = r;
        }
        IAudioCaptureClient_ReleaseBuffer(capture, num_frames);
        ring_push(convert_buffer, num_frames);
    }

    IAudioClient_Stop(client);

done:
    if (!capture_ok) SetEvent(capture_ready_event);
    free(convert_buffer);
    if (mix_format) CoTaskMemFree(mix_format);
    if (capture) IAudioCaptureClient_Release(capture);
    if (client) IAudioClient_Release(client);
    CoUninitialize();
    return 0;
}

bool pcm_capture_start(const char * device_filter){
    InitializeCriticalSection(&ring_lock);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator * enumerator = create_enumerator();
    if (!enumerator){
        printf("Audio: cannot create device enumerator\n");
        return false;
    }
    if (device_filter){
        capture_device = find_device(enumerator, device_filter, &capture_loopback);
        if (!capture_device){
            printf("Audio: no device matching '%s' (use --list-devices)\n", device_filter);
        }
    } else {
        capture_loopback = true;
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &capture_device))){
            capture_device = NULL;
            printf("Audio: no default playback device\n");
        }
    }
    if (!capture_device){
        IMMDeviceEnumerator_Release(enumerator);
        return false;
    }

    // volume control: the captured playback device itself, else the default playback device
    IMMDevice * volume_device = NULL;
    if (capture_loopback){
        volume_device = capture_device;
        IMMDevice_AddRef(volume_device);
    } else if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &volume_device))){
        volume_device = NULL;
    }
    if (volume_device){
        if (FAILED(IMMDevice_Activate(volume_device, &IID_IAudioEndpointVolume_, CLSCTX_ALL, NULL, (void **) &endpoint_volume))){
            endpoint_volume = NULL;
        }
        IMMDevice_Release(volume_device);
    }
    IMMDeviceEnumerator_Release(enumerator);

    char name[256];
    get_device_name(capture_device, name, sizeof(name));

    capture_ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&capture_running, 1);
    capture_thread = CreateThread(NULL, 0, capture_thread_main, NULL, 0, NULL);
    WaitForSingleObject(capture_ready_event, 5000);
    if (!capture_ok){
        InterlockedExchange(&capture_running, 0);
        return false;
    }
    SetThreadPriority(capture_thread, THREAD_PRIORITY_TIME_CRITICAL);
    printf("Audio: capturing '%s' (%s), %u Hz, %d ch\n", name, capture_loopback ? "loopback" : "recording",
           (unsigned) device_rate, device_channels);
    return true;
}

void pcm_capture_stop(void){
    if (!capture_thread) return;
    InterlockedExchange(&capture_running, 0);
    WaitForSingleObject(capture_thread, 2000);
    CloseHandle(capture_thread);
    capture_thread = NULL;
}

uint32_t pcm_capture_device_rate(void){
    return device_rate;
}

void pcm_capture_set_output_rate(uint32_t sample_rate){
    EnterCriticalSection(&ring_lock);
    output_rate = sample_rate;
    resample_step = (double) device_rate / (double) sample_rate;
    resample_phase = 1.0;
    memset(resample_prev, 0, sizeof(resample_prev));
    memset(resample_cur, 0, sizeof(resample_cur));
    jitter_running = false;
    LeaveCriticalSection(&ring_lock);
}

void pcm_capture_read(float * stereo_out, int num_frames){
    const uint32_t prebuffer = device_rate * PREBUFFER_MS / 1000;
    const uint32_t max_buffer = device_rate * MAX_BUFFER_MS / 1000;

    EnterCriticalSection(&ring_lock);

    if (!jitter_running && ring_count >= prebuffer){
        jitter_running = true;
    }
    if (jitter_running && ring_count > max_buffer){
        // input clock faster than Bluetooth clock or stall: drop back to target latency
        uint32_t drop = ring_count - prebuffer;
        ring_read = (ring_read + drop) % RING_FRAMES;
        ring_count -= drop;
        stat_overruns++;
    }

    for (int i = 0; i < num_frames; i++){
        if (jitter_running){
            while (resample_phase >= 1.0){
                if (ring_count == 0){
                    // input clock slower or source stopped: wait for refill
                    jitter_running = false;
                    stat_underruns++;
                    break;
                }
                resample_prev[0] = resample_cur[0];
                resample_prev[1] = resample_cur[1];
                resample_cur[0] = ring[ring_read][0];
                resample_cur[1] = ring[ring_read][1];
                ring_read = (ring_read + 1) % RING_FRAMES;
                ring_count--;
                resample_phase -= 1.0;
            }
        }
        if (!jitter_running){
            stereo_out[2*i] = 0.0f;
            stereo_out[2*i+1] = 0.0f;
            continue;
        }
        float t = (float) resample_phase;
        stereo_out[2*i]   = resample_prev[0] + (resample_cur[0] - resample_prev[0]) * t;
        stereo_out[2*i+1] = resample_prev[1] + (resample_cur[1] - resample_prev[1]) * t;
        resample_phase += resample_step;
    }

    LeaveCriticalSection(&ring_lock);
}

void pcm_capture_get_stats(uint32_t * underruns, uint32_t * overruns, uint32_t * buffered_ms, float * peak){
    EnterCriticalSection(&ring_lock);
    *underruns = stat_underruns;
    *overruns = stat_overruns;
    *buffered_ms = device_rate ? ring_count * 1000 / device_rate : 0;
    *peak = stat_peak;
    stat_underruns = 0;
    stat_overruns = 0;
    stat_peak = 0.0f;
    LeaveCriticalSection(&ring_lock);
}

bool pcm_capture_get_volume(float * level, bool * muted){
    if (!endpoint_volume) return false;
    BOOL mute = FALSE;
    if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(endpoint_volume, level))) return false;
    if (FAILED(IAudioEndpointVolume_GetMute(endpoint_volume, &mute))) return false;
    *muted = mute != FALSE;
    return true;
}

void pcm_capture_set_volume(float level){
    if (!endpoint_volume) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint_volume, level, NULL);
}
