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

// Read num_frames stereo float frames at output rate. Missing data is
// replaced by silence. Never blocks.
void pcm_capture_read(float * stereo_out, int num_frames);

// Statistics since last call
void pcm_capture_get_stats(uint32_t * underruns, uint32_t * overruns, uint32_t * buffered_ms);

#endif
