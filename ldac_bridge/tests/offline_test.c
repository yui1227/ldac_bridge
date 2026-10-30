// Offline checks that need no Bluetooth hardware:
//  1. LDAC encoder: packet sizes, frames per packet and resulting bitrate per quality mode
//  2. WASAPI capture: start default device, pull 1 s at 48 kHz, report level and buffer stats

#include <math.h>
#include <stdio.h>
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

    printf("\nWASAPI capture\n");
    if (!pcm_capture_start(NULL)){
        printf("  capture start failed\n");
        return 1;
    }
    pcm_capture_set_output_rate(48000);
    float buf[480 * 2];
    float peak = 0;
    for (int i = 0; i < 100; i++){       // 100 x 10 ms
        Sleep(10);
        pcm_capture_read(buf, 480);
        for (int j = 0; j < 960; j++) if (fabsf(buf[j]) > peak) peak = fabsf(buf[j]);
    }
    uint32_t under, over, ms;
    pcm_capture_get_stats(&under, &over, &ms);
    printf("  1 s read: peak %.3f, buffered %u ms, underruns %u, overruns %u\n", peak, ms, under, over);
    pcm_capture_stop();

    printf("\n%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
