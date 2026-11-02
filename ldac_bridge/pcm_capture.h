// WASAPI audio capture for ldac_bridge
//
// Captures either a render device in loopback mode (what you hear) or a
// recording device (e.g. "CABLE Output" of VB-Cable) on a background thread.
// The Bluetooth side pulls interleaved stereo float frames at its own rate.

#ifndef PCM_CAPTURE_H
#define PCM_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

// Print all render and capture endpoints with their names
void pcm_capture_list_devices(void);

// Start capture thread. device_filter: case-insensitive substring of the
// endpoint name, or NULL for loopback of the default render device.
// Returns false if no matching device could be opened.
bool pcm_capture_start(const char * device_filter);

void pcm_capture_stop(void);

// Sample rate delivered by the device (valid after pcm_capture_start)
uint32_t pcm_capture_device_rate(void);

// Set rate for pcm_capture_read; resets the resampler and jitter buffer
void pcm_capture_set_output_rate(uint32_t sample_rate);

// Jitter buffer target in ms (default 30, clamped to 10..500). Lower is less
// latency but more dropouts when the system is busy.
void pcm_capture_set_latency_ms(uint32_t ms);
uint32_t pcm_capture_get_latency_ms(void);

// Read num_frames stereo float frames at output rate. Missing data is
// replaced by silence. Never blocks. Clock drift is compensated by adjusting
// the resampling ratio slightly, keeping the buffer at the target latency.
void pcm_capture_read(float * stereo_out, int num_frames);

// Statistics since last call. peak: highest absolute captured sample (1.0 = full scale),
// drift_ppm: current resampling correction (positive: consuming faster than nominal)
void pcm_capture_get_stats(uint32_t * underruns, uint32_t * overruns, uint32_t * buffered_ms, float * peak, int * drift_ppm);

// Tests only: run the jitter buffer without a device. Call init instead of
// pcm_capture_start, then feed frames as if captured at device_rate.
void pcm_capture_test_init(uint32_t device_rate);
void pcm_capture_test_push(const float * stereo_frames, uint32_t num_frames);

// Windows volume of the playback endpoint (the loopback device, or the default
// playback device when capturing a recording device). level: 0.0 .. 1.0
bool pcm_capture_get_volume(float * level, bool * muted);
void pcm_capture_set_volume(float level);

#endif
