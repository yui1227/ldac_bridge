// ldac_bridge: stream Windows audio to Bluetooth headphones using LDAC
//
// BTstack A2DP Source with a single Sony LDAC stream endpoint.
// PCM comes from pcm_capture (WASAPI), is encoded with libldac and sent as RTP media packets.

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "ldacBT.h"
#include "media_keys.h"
#include "pcm_capture.h"
#include "sony_battery.h"

#define A2DP_CODEC_VENDOR_ID_SONY   0x0000012D
#define A2DP_SONY_CODEC_LDAC        0x00AA

#define LDAC_CODEC_INFO_LEN         8
#define LDAC_PCM_FRAMES_PER_ENCODE  LDACBT_ENC_LSU
#define LDAC_MEDIA_HEADER_LEN       1

#define AUDIO_TIMER_MS              4
#define RECONNECT_DELAY_MS          5000
#define STATS_INTERVAL_MS           5000
#define INQUIRY_DURATION_1280MS     8

#define LAST_DEVICE_FILE            "last_device.txt"

// ABR: backlog of PCM not yet sent, in ms
#define ABR_BACKLOG_DEGRADE_MS      60
#define ABR_BACKLOG_GOOD_MS         15
#define ABR_DEGRADE_COOLDOWN_MS     1000
#define ABR_IMPROVE_AFTER_MS        10000

// command line options
static const char * opt_name_filter;
static const char * opt_audio_device;
static int          opt_eqmid = LDACBT_EQMID_HQ;
static bool         opt_abr;
static uint32_t     opt_rate;          // 0 = follow capture device
static bool         opt_sony_debug;
static bool         opt_have_addr;
static bd_addr_t    opt_addr;

static bd_addr_t remote_addr;
static bool      remote_addr_valid;
static bool      scan_active;
static bool      shutting_down;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint8_t  sdp_a2dp_source_service_buffer[150];
static uint8_t  sdp_avrcp_target_service_buffer[200];
static uint8_t  sdp_avrcp_controller_service_buffer[200];

// Absolute volume: Windows endpoint volume <-> headset volume (AVRCP 0..127)
#define VOLUME_POLL_MS          100
#define AVRCP_VOLUME_MAX        127
#define AVRCP_VOLUME_UNKNOWN    0xFF
static btstack_timer_source_t volume_timer;
static uint8_t volume_last_synced = AVRCP_VOLUME_UNKNOWN;   // last value both sides agreed on

// AVRCP: headset buttons arrive as pass-through commands to our target role
static uint16_t                avrcp_cid;
static avrcp_playback_status_t avrcp_playback_status = AVRCP_PLAYBACK_STATUS_STOPPED;

static btstack_timer_source_t reconnect_timer;

// Local LDAC endpoint: all rates LDAC supports up to 96 kHz, stereo only
static uint8_t ldac_local_capabilities[LDAC_CODEC_INFO_LEN] = {
    0x2D, 0x01, 0x00, 0x00,     // vendor id Sony (LE)
    0xAA, 0x00,                 // codec id LDAC (LE)
    LDACBT_SAMPLING_FREQ_044100 | LDACBT_SAMPLING_FREQ_048000 | LDACBT_SAMPLING_FREQ_088200 | LDACBT_SAMPLING_FREQ_096000,
    LDACBT_CHANNEL_MODE_STEREO,
};
static uint8_t ldac_local_configuration[LDAC_CODEC_INFO_LEN];
static uint8_t ldac_set_configuration[LDAC_CODEC_INFO_LEN];

typedef struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    bool     remote_has_ldac;
    uint8_t  remote_ldac_freq_bitmap;
    uint8_t  remote_ldac_channel_bitmap;

    uint32_t sampling_rate;
    bool     streaming;

    HANDLE_LDAC_BT ldac;
    int      max_media_payload_size;

    btstack_timer_source_t audio_timer;
    uint32_t time_last_tick_ms;
    uint32_t acc_samples_remainder;
    uint32_t samples_due;

    uint32_t rtp_timestamp;         // samples encoded so far
    uint32_t packet_rtp_timestamp;  // timestamp of first sample of pending packet
    bool     packet_started;
    uint8_t  packet[LDAC_MEDIA_HEADER_LEN + LDACBT_MAX_NBYTES];
    uint16_t packet_len;
    bool     packet_ready;

    uint32_t packets_sent;
    uint32_t bytes_sent;
    uint32_t time_stats_ms;

    uint32_t abr_time_last_degrade_ms;
    uint32_t abr_time_good_since_ms;
} ldac_stream_t;

static ldac_stream_t stream;

static const char * eqmid_name(int eqmid){
    switch (eqmid){
        case LDACBT_EQMID_HQ: return "HQ 990 kbps";
        case LDACBT_EQMID_SQ: return "SQ 660 kbps";
        case LDACBT_EQMID_MQ: return "MQ 330 kbps";
        default:              return "?";
    }
}

static uint32_t ldac_freq_bit_to_rate(uint8_t bit){
    switch (bit){
        case LDACBT_SAMPLING_FREQ_044100: return 44100;
        case LDACBT_SAMPLING_FREQ_048000: return 48000;
        case LDACBT_SAMPLING_FREQ_088200: return 88200;
        case LDACBT_SAMPLING_FREQ_096000: return 96000;
        case LDACBT_SAMPLING_FREQ_176400: return 176400;
        case LDACBT_SAMPLING_FREQ_192000: return 192000;
        default:                          return 0;
    }
}

static uint8_t ldac_rate_to_freq_bit(uint32_t rate){
    switch (rate){
        case 44100:  return LDACBT_SAMPLING_FREQ_044100;
        case 48000:  return LDACBT_SAMPLING_FREQ_048000;
        case 88200:  return LDACBT_SAMPLING_FREQ_088200;
        case 96000:  return LDACBT_SAMPLING_FREQ_096000;
        default:     return 0;
    }
}

// Pick sampling rate: requested rate, else capture device rate, else a sensible fallback
static uint8_t choose_ldac_freq_bit(uint8_t remote_bitmap){
    uint8_t common = remote_bitmap & ldac_local_capabilities[6];
    uint8_t wanted = ldac_rate_to_freq_bit(opt_rate ? opt_rate : pcm_capture_device_rate());
    if (common & wanted) return wanted;
    const uint8_t fallback[] = { LDACBT_SAMPLING_FREQ_048000, LDACBT_SAMPLING_FREQ_044100,
                                 LDACBT_SAMPLING_FREQ_096000, LDACBT_SAMPLING_FREQ_088200 };
    for (unsigned i = 0; i < sizeof(fallback); i++){
        if (common & fallback[i]) return fallback[i];
    }
    return 0;
}

// ---- persistence of last connected headset ----

static void save_last_device(const bd_addr_t addr){
    FILE * f = fopen(LAST_DEVICE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", bd_addr_to_str(addr));
    fclose(f);
}

static bool load_last_device(bd_addr_t addr){
    FILE * f = fopen(LAST_DEVICE_FILE, "r");
    if (!f) return false;
    char line[32] = {0};
    bool ok = fgets(line, sizeof(line), f) != NULL && sscanf_bd_addr(line, addr) != 0;
    fclose(f);
    return ok;
}

// ---- connection management ----

static void start_scanning(void){
    printf("Scanning for headphones%s%s%s... (put them into pairing mode)\n",
           opt_name_filter ? " named '" : "", opt_name_filter ? opt_name_filter : "", opt_name_filter ? "'" : "");
    scan_active = true;
    gap_inquiry_start(INQUIRY_DURATION_1280MS);
}

static void connect_to_remote(void){
    if (shutting_down || stream.a2dp_cid != 0) return;
    printf("Connecting to %s...\n", bd_addr_to_str(remote_addr));
    uint8_t status = a2dp_source_establish_stream(remote_addr, &stream.a2dp_cid);
    if (status != ERROR_CODE_SUCCESS){
        printf("establish_stream failed, status 0x%02x\n", status);
        stream.a2dp_cid = 0;
    }
}

static void reconnect_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    connect_to_remote();
}

static void schedule_reconnect(void){
    if (shutting_down || !remote_addr_valid) return;
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, &reconnect_timer_handler);
    btstack_run_loop_set_timer(&reconnect_timer, RECONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&reconnect_timer);
}

// ---- LDAC encoding and sending ----

static void ldac_encoder_close(void){
    if (stream.ldac){
        ldacBT_free_handle(stream.ldac);
        stream.ldac = NULL;
    }
}

static bool ldac_encoder_open(void){
    ldac_encoder_close();
    stream.ldac = ldacBT_get_handle();
    if (!stream.ldac){
        printf("LDAC: cannot allocate encoder\n");
        return false;
    }
    int mtu = stream.max_media_payload_size - LDAC_MEDIA_HEADER_LEN;
    if (mtu > LDACBT_MAX_NBYTES) mtu = LDACBT_MAX_NBYTES;
    int eqmid = opt_eqmid;
    int result = ldacBT_init_handle_encode(stream.ldac, mtu, eqmid, LDACBT_CHANNEL_MODE_STEREO,
                                           LDACBT_SMPL_FMT_F32, (int) stream.sampling_rate);
    if (result != 0){
        printf("LDAC: init failed, error %d (mtu %d)\n", ldacBT_get_error_code(stream.ldac), mtu);
        ldac_encoder_close();
        return false;
    }
    printf("LDAC: encoder ready, %u Hz, %s, mtu %d%s\n", (unsigned) stream.sampling_rate,
           eqmid_name(ldacBT_get_eqmid(stream.ldac)), mtu, opt_abr ? ", adaptive bitrate" : "");
    return true;
}

static void ldac_send_pending_packet(void){
    if (!stream.packet_ready) return;
    uint8_t status = a2dp_source_stream_send_media_payload_rtp(stream.a2dp_cid, stream.local_seid, 0,
                                                               stream.packet_rtp_timestamp,
                                                               stream.packet, stream.packet_len);
    if (status == ERROR_CODE_SUCCESS){
        stream.packets_sent++;
        stream.bytes_sent += stream.packet_len;
    }
    stream.packet_ready = false;
}

// Encode PCM until one transport packet is ready, then request to send it
static void ldac_pump(void){
    while (stream.streaming && !stream.packet_ready && stream.samples_due >= LDAC_PCM_FRAMES_PER_ENCODE){
        float pcm[LDAC_PCM_FRAMES_PER_ENCODE * 2];
        pcm_capture_read(pcm, LDAC_PCM_FRAMES_PER_ENCODE);

        if (!stream.packet_started){
            stream.packet_rtp_timestamp = stream.rtp_timestamp;
            stream.packet_started = true;
        }

        int pcm_used = 0;
        int stream_size = 0;
        int frame_num = 0;
        if (ldacBT_encode(stream.ldac, pcm, &pcm_used, &stream.packet[LDAC_MEDIA_HEADER_LEN], &stream_size, &frame_num) != 0){
            printf("LDAC: encode error %d\n", ldacBT_get_error_code(stream.ldac));
        }
        stream.samples_due -= LDAC_PCM_FRAMES_PER_ENCODE;
        stream.rtp_timestamp += LDAC_PCM_FRAMES_PER_ENCODE;

        if (stream_size > 0){
            // media payload header: number of LDAC frames in this packet
            stream.packet[0] = (uint8_t)(frame_num & 0x0F);
            stream.packet_len = (uint16_t)(LDAC_MEDIA_HEADER_LEN + stream_size);
            stream.packet_ready = true;
            stream.packet_started = false;
            a2dp_source_stream_endpoint_request_can_send_now(stream.a2dp_cid, stream.local_seid);
        }
    }
}

static void abr_update(uint32_t now){
    if (!opt_abr) return;
    uint32_t backlog_ms = stream.samples_due * 1000 / stream.sampling_rate;
    if (backlog_ms > ABR_BACKLOG_DEGRADE_MS){
        stream.abr_time_good_since_ms = now;
        if ((now - stream.abr_time_last_degrade_ms) > ABR_DEGRADE_COOLDOWN_MS && ldacBT_get_eqmid(stream.ldac) != LDACBT_EQMID_MQ){
            ldacBT_alter_eqmid_priority(stream.ldac, LDACBT_EQMID_INC_CONNECTION);
            stream.abr_time_last_degrade_ms = now;
            printf("LDAC ABR: link congested (%u ms backlog), now %s\n", (unsigned) backlog_ms, eqmid_name(ldacBT_get_eqmid(stream.ldac)));
        }
    } else if (backlog_ms < ABR_BACKLOG_GOOD_MS){
        if ((now - stream.abr_time_good_since_ms) > ABR_IMPROVE_AFTER_MS && ldacBT_get_eqmid(stream.ldac) != opt_eqmid){
            ldacBT_alter_eqmid_priority(stream.ldac, LDACBT_EQMID_INC_QUALITY);
            stream.abr_time_good_since_ms = now;
            printf("LDAC ABR: link good, now %s\n", eqmid_name(ldacBT_get_eqmid(stream.ldac)));
        }
    } else {
        stream.abr_time_good_since_ms = now;
    }
}

static void print_stats(uint32_t now){
    if ((now - stream.time_stats_ms) < STATS_INTERVAL_MS) return;
    uint32_t elapsed = now - stream.time_stats_ms;
    uint32_t underruns, overruns, buffered_ms;
    float peak;
    pcm_capture_get_stats(&underruns, &overruns, &buffered_ms, &peak);
    char peak_text[16] = "silent";
    if (peak > 0.00001f) snprintf(peak_text, sizeof(peak_text), "%.1f dBFS", 20.0f * log10f(peak));
    float volume = 0.0f;
    bool muted = false;
    bool have_volume = pcm_capture_get_volume(&volume, &muted);
    printf("[stats] %s, %u kbps on air, %u pkt/s, buffer %u ms, underruns %u, overruns %u, peak %s, Windows volume %s%d%%\n",
           eqmid_name(ldacBT_get_eqmid(stream.ldac)),
           (unsigned)(stream.bytes_sent * 8 / elapsed), (unsigned)(stream.packets_sent * 1000 / elapsed),
           (unsigned) buffered_ms, (unsigned) underruns, (unsigned) overruns, peak_text,
           muted ? "muted " : "", have_volume ? (int) lroundf(volume * 100) : -1);
    stream.bytes_sent = 0;
    stream.packets_sent = 0;
    stream.time_stats_ms = now;
}

static void audio_timer_handler(btstack_timer_source_t * timer){
    btstack_run_loop_set_timer(timer, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(timer);

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t elapsed = now - stream.time_last_tick_ms;
    stream.time_last_tick_ms = now;

    // convert elapsed wall time to samples without drift
    uint32_t total = elapsed * stream.sampling_rate + stream.acc_samples_remainder;
    stream.samples_due += total / 1000;
    stream.acc_samples_remainder = total % 1000;

    // never accumulate more than 200 ms, e.g. after the radio was busy
    uint32_t max_due = stream.sampling_rate / 5;
    if (stream.samples_due > max_due) stream.samples_due = max_due;

    abr_update(now);
    ldac_pump();
    print_stats(now);
}

static void audio_start(void){
    stream.max_media_payload_size = a2dp_max_media_payload_size(stream.a2dp_cid, stream.local_seid);
    if (!ldac_encoder_open()) return;

    pcm_capture_set_output_rate(stream.sampling_rate);

    uint32_t now = btstack_run_loop_get_time_ms();
    stream.time_last_tick_ms = now;
    stream.time_stats_ms = now;
    stream.abr_time_last_degrade_ms = now;
    stream.abr_time_good_since_ms = now;
    stream.acc_samples_remainder = 0;
    stream.samples_due = 0;
    stream.packet_ready = false;
    stream.packet_started = false;
    stream.packets_sent = 0;
    stream.bytes_sent = 0;
    stream.streaming = true;

    btstack_run_loop_remove_timer(&stream.audio_timer);
    btstack_run_loop_set_timer_handler(&stream.audio_timer, &audio_timer_handler);
    btstack_run_loop_set_timer(&stream.audio_timer, AUDIO_TIMER_MS);
    btstack_run_loop_add_timer(&stream.audio_timer);
}

static void audio_stop(void){
    stream.streaming = false;
    stream.packet_ready = false;
    btstack_run_loop_remove_timer(&stream.audio_timer);
    ldac_encoder_close();
}

// ---- AVRCP: headset buttons -> Windows media keys ----

static void avrcp_set_status(avrcp_playback_status_t status){
    avrcp_playback_status = status;
    if (avrcp_cid){
        avrcp_target_set_playback_status(avrcp_cid, status);
    }
}

// ---- absolute volume ----

static uint8_t windows_volume_as_avrcp(bool * ok){
    float level;
    bool muted;
    *ok = pcm_capture_get_volume(&level, &muted);
    if (!*ok) return 0;
    if (muted) return 0;
    return (uint8_t) lroundf(level * AVRCP_VOLUME_MAX);
}

// Windows -> headset: poll the endpoint volume (also covers keyboard volume keys and mute)
static void volume_timer_handler(btstack_timer_source_t * ts){
    if (avrcp_cid == 0) return;
    btstack_run_loop_set_timer(ts, VOLUME_POLL_MS);
    btstack_run_loop_add_timer(ts);

    bool ok;
    uint8_t volume = windows_volume_as_avrcp(&ok);
    if (!ok || volume == volume_last_synced) return;
    // a previous AVRCP command may still be pending; then retry on the next tick
    if (avrcp_controller_set_absolute_volume(avrcp_cid, volume) == ERROR_CODE_SUCCESS){
        volume_last_synced = volume;
    }
}

// headset -> Windows
static void volume_from_headset(uint8_t volume){
    volume &= 0x7F;
    if (volume == volume_last_synced) return;
    volume_last_synced = volume;
    pcm_capture_set_volume((float) volume / AVRCP_VOLUME_MAX);
    printf("Volume: %u%% (set on headset)\n", (unsigned)(volume * 100 / AVRCP_VOLUME_MAX));
}

static void volume_sync_start(void){
    volume_last_synced = AVRCP_VOLUME_UNKNOWN;   // first tick pushes the Windows volume to the headset
    avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
    btstack_run_loop_remove_timer(&volume_timer);
    btstack_run_loop_set_timer_handler(&volume_timer, &volume_timer_handler);
    btstack_run_loop_set_timer(&volume_timer, VOLUME_POLL_MS);
    btstack_run_loop_add_timer(&volume_timer);
}

static void volume_sync_stop(void){
    btstack_run_loop_remove_timer(&volume_timer);
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (hci_event_avrcp_meta_get_subevent_code(packet)){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS){
                printf("AVRCP: connection failed, status 0x%02x\n", avrcp_subevent_connection_established_get_status(packet));
                avrcp_cid = 0;
                break;
            }
            avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            avrcp_target_support_event(avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_target_set_now_playing_info(avrcp_cid, NULL, 0);
            avrcp_target_set_playback_status(avrcp_cid, avrcp_playback_status);
            volume_sync_start();
            printf("AVRCP: connected, headset buttons control Windows media, volume synced\n");
            break;
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            avrcp_cid = 0;
            volume_sync_stop();
            break;
        default:
            break;
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (hci_event_avrcp_meta_get_subevent_code(packet)){
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
            avrcp_target_play_status(avrcp_cid, 0xFFFFFFFF, 0xFFFFFFFF, avrcp_playback_status);
            break;

        case AVRCP_SUBEVENT_OPERATION: {
            // act on press only, release is reported separately
            if (avrcp_subevent_operation_get_button_pressed(packet) == 0) break;
            avrcp_operation_id_t op = (avrcp_operation_id_t) avrcp_subevent_operation_get_operation_id(packet);
            printf("AVRCP: headset button %s\n", avrcp_operation2str(op));
            switch (op){
                // Windows only has a toggle key; the headset picks PLAY or PAUSE from our reported status
                case AVRCP_OPERATION_ID_PLAY:
                    media_key_send(MEDIA_KEY_PLAY_PAUSE);
                    avrcp_set_status(AVRCP_PLAYBACK_STATUS_PLAYING);
                    break;
                case AVRCP_OPERATION_ID_PAUSE:
                    media_key_send(MEDIA_KEY_PLAY_PAUSE);
                    avrcp_set_status(AVRCP_PLAYBACK_STATUS_PAUSED);
                    break;
                case AVRCP_OPERATION_ID_STOP:
                    media_key_send(MEDIA_KEY_STOP);
                    avrcp_set_status(AVRCP_PLAYBACK_STATUS_STOPPED);
                    break;
                case AVRCP_OPERATION_ID_FORWARD:
                    media_key_send(MEDIA_KEY_NEXT);
                    break;
                case AVRCP_OPERATION_ID_BACKWARD:
                    media_key_send(MEDIA_KEY_PREVIOUS);
                    break;
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (hci_event_avrcp_meta_get_subevent_code(packet)){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            // the initial (interim) value is ignored: Windows volume wins on connect
            if (avrcp_subevent_notification_volume_changed_get_command_type(packet) == AVRCP_CTYPE_RESPONSE_INTERIM) break;
            volume_from_headset(avrcp_subevent_notification_volume_changed_get_absolute_volume(packet));
            break;
        default:
            break;
    }
}

// ---- A2DP events ----

static void handle_codec_capability(uint8_t * packet){
    uint8_t remote_seid = a2dp_subevent_signaling_media_codec_other_capability_get_remote_seid(packet);
    uint16_t len = a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
    const uint8_t * info = a2dp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
    if (len < LDAC_CODEC_INFO_LEN) return;
    uint32_t vendor_id = little_endian_read_32(info, 0);
    uint16_t codec_id  = little_endian_read_16(info, 4);
    if (vendor_id != A2DP_CODEC_VENDOR_ID_SONY || codec_id != A2DP_SONY_CODEC_LDAC){
        printf("  remote codec: vendor 0x%08" PRIx32 " codec 0x%04x (ignored)\n", vendor_id, codec_id);
        return;
    }
    stream.remote_has_ldac = true;
    stream.remote_seid = remote_seid;
    stream.remote_ldac_freq_bitmap = info[6];
    stream.remote_ldac_channel_bitmap = info[7];
    printf("  remote codec: LDAC (seid %u), rates:%s%s%s%s%s%s\n", remote_seid,
           (info[6] & LDACBT_SAMPLING_FREQ_044100) ? " 44.1k" : "",
           (info[6] & LDACBT_SAMPLING_FREQ_048000) ? " 48k" : "",
           (info[6] & LDACBT_SAMPLING_FREQ_088200) ? " 88.2k" : "",
           (info[6] & LDACBT_SAMPLING_FREQ_096000) ? " 96k" : "",
           (info[6] & LDACBT_SAMPLING_FREQ_176400) ? " 176.4k" : "",
           (info[6] & LDACBT_SAMPLING_FREQ_192000) ? " 192k" : "");
}

static void handle_capabilities_complete(void){
    if (!stream.remote_has_ldac){
        printf("This device does not offer LDAC. Disconnecting.\n");
        a2dp_source_disconnect(stream.a2dp_cid);
        return;
    }
    uint8_t freq_bit = choose_ldac_freq_bit(stream.remote_ldac_freq_bitmap);
    if (!freq_bit || (stream.remote_ldac_channel_bitmap & LDACBT_CHANNEL_MODE_STEREO) == 0){
        printf("No common LDAC configuration (rates 0x%02x, channels 0x%02x)\n",
               stream.remote_ldac_freq_bitmap, stream.remote_ldac_channel_bitmap);
        a2dp_source_disconnect(stream.a2dp_cid);
        return;
    }
    memcpy(ldac_set_configuration, ldac_local_capabilities, 6);
    ldac_set_configuration[6] = freq_bit;
    ldac_set_configuration[7] = LDACBT_CHANNEL_MODE_STEREO;
    printf("Selecting LDAC %u Hz stereo\n", (unsigned) ldac_freq_bit_to_rate(freq_bit));
    uint8_t status = a2dp_source_set_config_other(stream.a2dp_cid, stream.local_seid, stream.remote_seid,
                                                  ldac_set_configuration, LDAC_CODEC_INFO_LEN);
    if (status != ERROR_CODE_SUCCESS){
        printf("set_config failed, status 0x%02x\n", status);
    }
}

static void a2dp_source_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    bd_addr_t address;
    uint8_t status;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)){
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, address);
            status = a2dp_subevent_signaling_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP: connection to %s failed, status 0x%02x\n", bd_addr_to_str(address), status);
                stream.a2dp_cid = 0;
                schedule_reconnect();
                break;
            }
            stream.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            stream.remote_has_ldac = false;
            memcpy(remote_addr, address, 6);
            remote_addr_valid = true;
            save_last_device(address);
            btstack_run_loop_remove_timer(&reconnect_timer);
            printf("A2DP: connected to %s, querying codecs...\n", bd_addr_to_str(address));
            // headset may also connect AVRCP itself; a duplicate request is simply rejected
            if (avrcp_cid == 0){
                uint16_t cid;
                avrcp_connect(address, &cid);
            }
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            printf("  remote codec: SBC\n");
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY:
            printf("  remote codec: AAC\n");
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            handle_codec_capability(packet);
            break;

        case A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE:
            handle_capabilities_complete();
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION: {
            const uint8_t * info = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);
            uint16_t len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
            if (len < LDAC_CODEC_INFO_LEN) break;
            stream.sampling_rate = ldac_freq_bit_to_rate(info[6]);
            printf("A2DP: LDAC configured, %u Hz\n", (unsigned) stream.sampling_rate);
            break;
        }

        case A2DP_SUBEVENT_SIGNALING_DELAY_REPORT:
            printf("A2DP: headset reports %u.%u ms delay\n",
                   a2dp_subevent_signaling_delay_report_get_delay_100us(packet) / 10,
                   a2dp_subevent_signaling_delay_report_get_delay_100us(packet) % 10);
            break;

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP: stream failed, status 0x%02x\n", status);
                break;
            }
            printf("A2DP: stream established, starting...\n");
            a2dp_source_start_stream(stream.a2dp_cid, stream.local_seid);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            printf("A2DP: streaming LDAC. Play something on Windows. Ctrl-C to quit.\n");
            audio_start();
            avrcp_set_status(AVRCP_PLAYBACK_STATUS_PLAYING);
            sony_battery_connect(remote_addr);
            break;

        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            ldac_send_pending_packet();
            ldac_pump();
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            printf("A2DP: stream suspended by headset\n");
            audio_stop();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            printf("A2DP: stream released\n");
            audio_stop();
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            printf("A2DP: disconnected\n");
            audio_stop();
            sony_battery_disconnect();
            stream.a2dp_cid = 0;
            schedule_reconnect();
            break;

        default:
            break;
    }
}

// ---- GAP / pairing ----

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t address;

    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
                if (remote_addr_valid){
                    connect_to_remote();
                } else {
                    start_scanning();
                }
            } else if (btstack_event_state_get_state(packet) == HCI_STATE_HALTING){
                shutting_down = true;
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, address);
            gap_pin_code_response(address, "0000");
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            printf("Pairing with %s\n", bd_addr_to_str(address));
            gap_ssp_confirmation_response(address);
            break;

        case GAP_EVENT_INQUIRY_RESULT: {
            if (!scan_active) break;
            gap_event_inquiry_result_get_bd_addr(packet, address);
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            char name[240] = "";
            if (gap_event_inquiry_result_get_name_available(packet)){
                int len = gap_event_inquiry_result_get_name_len(packet);
                if (len > (int) sizeof(name) - 1) len = sizeof(name) - 1;
                memcpy(name, gap_event_inquiry_result_get_name(packet), len);
                name[len] = 0;
            }
            bool is_audio = (cod & 0x200400) == 0x200400;   // service: audio, major class: audio/video
            printf("  found %s cod %06" PRIx32 " '%s'%s\n", bd_addr_to_str(address), cod, name, is_audio ? " [audio]" : "");
            bool match = opt_name_filter ? (strstr(name, opt_name_filter) != NULL) : is_audio;
            if (match){
                scan_active = false;
                gap_inquiry_stop();
                memcpy(remote_addr, address, 6);
                remote_addr_valid = true;
                connect_to_remote();
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (scan_active){
                gap_inquiry_start(INQUIRY_DURATION_1280MS);
            }
            break;

        default:
            break;
    }
}

// ---- setup ----

static void print_usage(const char * exe){
    printf("Usage: %s [options]\n", exe);
    printf("  --addr XX:XX:XX:XX:XX:XX  connect to this headset (default: last used, else scan)\n");
    printf("  --name TEXT               when scanning, pick the first device whose name contains TEXT\n");
    printf("  --scan                    ignore last used headset and scan\n");
    printf("  --quality hq|sq|mq|abr    LDAC 990 / 660 / 330 kbps, or adaptive starting at 990 (default hq)\n");
    printf("  --rate 44100|48000|96000  LDAC sampling rate (default: same as the captured device)\n");
    printf("  --device TEXT             capture this playback (loopback) or recording device (default: default playback)\n");
    printf("  --list-devices            list audio devices and exit\n");
    printf("  --sony-debug              print raw Sony control protocol frames (battery levels)\n");
}

// Returns 0 to continue, >0 to exit successfully, <0 on error. Called before Bluetooth starts.
int ldac_bridge_setup(int argc, const char * argv[]){
    bool force_scan = false;
    for (int i = 1; i < argc; i++){
        const char * a = argv[i];
        const char * v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0){
            print_usage(argv[0]);
            return 1;
        } else if (strcmp(a, "--list-devices") == 0){
            pcm_capture_list_devices();
            return 1;
        } else if (strcmp(a, "--scan") == 0){
            force_scan = true;
        } else if (strcmp(a, "--sony-debug") == 0){
            opt_sony_debug = true;
        } else if (strcmp(a, "--addr") == 0 && v){
            if (sscanf_bd_addr(v, opt_addr) == 0){ printf("Invalid address '%s'\n", v); return -1; }
            opt_have_addr = true; i++;
        } else if (strcmp(a, "--name") == 0 && v){
            opt_name_filter = v; i++;
        } else if (strcmp(a, "--device") == 0 && v){
            opt_audio_device = v; i++;
        } else if (strcmp(a, "--rate") == 0 && v){
            opt_rate = (uint32_t) atoi(v); i++;
            if (!ldac_rate_to_freq_bit(opt_rate)){ printf("Unsupported rate %s\n", v); return -1; }
        } else if (strcmp(a, "--quality") == 0 && v){
            if      (strcmp(v, "hq") == 0)  opt_eqmid = LDACBT_EQMID_HQ;
            else if (strcmp(v, "sq") == 0)  opt_eqmid = LDACBT_EQMID_SQ;
            else if (strcmp(v, "mq") == 0)  opt_eqmid = LDACBT_EQMID_MQ;
            else if (strcmp(v, "abr") == 0) { opt_eqmid = LDACBT_EQMID_HQ; opt_abr = true; }
            else { printf("Unknown quality '%s'\n", v); return -1; }
            i++;
        } else {
            printf("Unknown option '%s'\n", a);
            print_usage(argv[0]);
            return -1;
        }
    }

    if (opt_have_addr){
        memcpy(remote_addr, opt_addr, 6);
        remote_addr_valid = true;
    } else if (!force_scan && !opt_name_filter){
        remote_addr_valid = load_last_device(remote_addr);
    }

    if (!pcm_capture_start(opt_audio_device)) return -1;
    return 0;
}

int btstack_main(int argc, const char * argv[]){
    UNUSED(argc);
    UNUSED(argv);

    // headset should be the peripheral; allow role switch
    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // Secure Simple Pairing, no MITM (headphones have no display)
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);

    l2cap_init();
    sony_battery_init(opt_sony_debug);

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_source_packet_handler);

    avdtp_stream_endpoint_t * endpoint = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            ldac_local_capabilities, sizeof(ldac_local_capabilities),
            ldac_local_configuration, sizeof(ldac_local_configuration));
    if (!endpoint){
        printf("A2DP: cannot create LDAC stream endpoint\n");
        return 1;
    }
    stream.local_seid = avdtp_local_seid(endpoint);
    avdtp_source_register_delay_reporting_category(stream.local_seid);

    sdp_init();
    memset(sdp_a2dp_source_service_buffer, 0, sizeof(sdp_a2dp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_service_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_service_buffer);

    // AVRCP Target: receive play/pause/next/previous from the headset
    avrcp_init();
    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);
    // avrcp_connect sets up both roles, so the controller must be initialized too (events unused)
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(),
                                   AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);
    // Controller record (category 2) tells the headset we drive its absolute volume
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    gap_set_local_name("LDAC Bridge 00:00:00:00:00:00");
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}
