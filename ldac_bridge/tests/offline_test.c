// Offline checks that need no Bluetooth hardware:
//  1. LDAC encoder: packet sizes, frames per packet and resulting bitrate per quality mode
//  2. Jitter buffer: simulated clock drift between capture and Bluetooth timer, checks that
//     the buffer stays at the target latency without dropouts
//  3. WASAPI capture: start default device, pull 1 s at 48 kHz, report level and buffer stats

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "ldacBT.h"
#include "pcm_capture.h"

static int test_ldac(int sample_rate, int eqmid, int mtu){
    HANDLE_LDAC_BT h = ldacBT_get_handle();
    if (ldacBT_init_handle_encode(h, mtu, eqmid, LDACBT_CHANNEL_MODE_STEREO, LDACBT_SMPL_FMT_F32, sample_rate) != 0){
        printf("  init failed: %d\n", ldacBT_get_error_code(h));
        ldacBT_free_handle(h);
        return 1;
    }
    unsigned char out[LDACBT_MAX_NBYTES];
    float pcm[LDACBT_ENC_LSU * 2];
    long total_bytes = 0, packets = 0, frames = 0, max_packet = 0;
    double phase = 0;
    int calls = sample_rate * 2 / LDACBT_ENC_LSU;   // 2 seconds
    for (int c = 0; c < calls; c++){
        for (int i = 0; i < LDACBT_ENC_LSU; i++){
            float s = 0.5f * (float) sin(phase);
            phase += 2 * M_PI * 1000.0 / sample_rate;
            pcm[2*i] = s; pcm[2*i+1] = s;
        }
        int used = 0, sz = 0, fn = 0;
        if (ldacBT_encode(h, pcm, &used, out, &sz, &fn) != 0){
            printf("  encode error %d\n", ldacBT_get_error_code(h));
            ldacBT_free_handle(h);
            return 1;
        }
        if (sz > 0){
            total_bytes += sz; packets++; frames += fn;
            if (sz > max_packet) max_packet = sz;
        }
    }
    printf("  %6d Hz eqmid %d mtu %4d: %4ld kbps, %3ld pkt/s, %.1f frames/pkt, max packet %ld bytes\n",
           sample_rate, eqmid, mtu, total_bytes * 8 / 2 / 1000, packets / 2,
           packets ? (double) frames / packets : 0.0, max_packet);
    ldacBT_free_handle(h);
    return (max_packet <= mtu) ? 0 : 1;
}

// Capture delivers 10 ms chunks on a clock off by drift_ppm; the Bluetooth side reads
// 128 frames per encode from a 4 ms timer, like ldac_source.c. 10 simulated minutes.
static int test_drift(int drift_ppm, uint32_t latency_ms){
    const uint32_t rate = 48000;
    pcm_capture_test_init(rate);
    pcm_capture_set_latency_ms(latency_ms);
    pcm_capture_set_output_rate(rate);

    static float chunk[480 * 2];
    float out[128 * 2];
    const double capture_period_us = 10000.0 / (1.0 + drift_ppm * 1e-6);
    double next_capture_us = 0.0;
    double frames_due = 0.0;
    uint32_t underruns = 0, overruns = 0, min_ms = 1000, max_ms = 0;
    double sum_ms = 0.0, sum_correction = 0.0;
    long samples = 0;
    int correction = 0;
    for (uint64_t t_us = 0; t_us < 600000000ULL; t_us += 4000){
        while (next_capture_us <= (double) t_us){
            pcm_capture_test_push(chunk, 480);
            next_capture_us += capture_period_us;
        }
        frames_due += rate * 0.004;
        while (frames_due >= 128.0){
            pcm_capture_read(out, 128);
            frames_due -= 128.0;
        }
        uint32_t u, o, ms;
        float peak;
        pcm_capture_get_stats(&u, &o, &ms, &peak, &correction);
        if (t_us < 60000000ULL) continue;       // settle for the first minute
        underruns += u;
        overruns += o;
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        sum_ms += ms;
        sum_correction += correction;
        samples++;
    }
    double mean_ms = sum_ms / samples;
    double mean_correction = sum_correction / samples;
    bool ok = underruns == 0 && overruns == 0 && fabs(mean_ms - latency_ms) < 5.0 && fabs(mean_correction - drift_ppm) < 50.0;
    printf("  drift %+5d ppm, target %3u ms: buffer %.1f ms (%u..%u), correction %+.0f ppm (mean), underruns %u, overruns %u  %s\n",
           drift_ppm, (unsigned) latency_ms, mean_ms, (unsigned) min_ms, (unsigned) max_ms, mean_correction,
           (unsigned) underruns, (unsigned) overruns, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void){
    int failures = 0;
    printf("LDAC encoder (libldac version 0x%06x)\n", ldacBT_get_version());
    const int rates[] = { 44100, 48000, 96000 };
    for (int r = 0; r < 3; r++){
        for (int q = LDACBT_EQMID_HQ; q <= LDACBT_EQMID_MQ; q++){
            failures += test_ldac(rates[r], q, 1005 - 12 - 1);
        }
    }
    // smaller MTU as used by some headsets
    failures += test_ldac(48000, LDACBT_EQMID_HQ, 679);

    printf("\nJitter buffer drift compensation\n");
    const int drifts[] = { 0, 300, -300, 1000, -1000 };
    for (int d = 0; d < 5; d++){
        failures += test_drift(drifts[d], 30);
    }
    failures += test_drift(200, 15);
    pcm_capture_set_latency_ms(30);

    printf("\nWASAPI capture\n");
    if (!pcm_capture_start(NULL)){
        printf("  capture start failed\n");
        return 1;
    }
    pcm_capture_set_output_rate(48000);
    float buf[128 * 2];
    float peak = 0;
    // read at real time like the Bluetooth timer, for 1 s
    ULONGLONG start = GetTickCount64();
    uint64_t frames_read = 0;
    while (GetTickCount64() - start < 1000){
        Sleep(4);
        uint64_t frames_due = (GetTickCount64() - start) * 48;
        while (frames_read + 128 <= frames_due){
            pcm_capture_read(buf, 128);
            frames_read += 128;
            for (int j = 0; j < 256; j++) if (fabsf(buf[j]) > peak) peak = fabsf(buf[j]);
        }
    }
    uint32_t under, over, ms;
    float device_peak;
    int drift;
    pcm_capture_get_stats(&under, &over, &ms, &device_peak, &drift);
    printf("  1 s read: peak %.3f, buffered %u ms, underruns %u, overruns %u\n", peak, ms, under, over);
    pcm_capture_stop();

    printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
