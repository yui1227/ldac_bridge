// Sony headphones battery levels over the proprietary RFCOMM protocol
//
// Frame:   0x3E | escaped( type, seq, len[4, BE], payload[len], checksum ) | 0x3C
//          checksum = sum of the unescaped bytes before it
//          0x3C/0x3D/0x3E inside a frame become 0x3D, (byte & 0xEF)
// Types:   0x01 ACK (seq = 1 - acknowledged seq), 0x0C / 0x0E commands
// Every command must be ACKed; only one of our commands is in flight at a time.
//
// Battery requests and replies (code, battery type, level, charging[, level, charging]):
//   protocol v1: request 0x10, reply 0x11, notify 0x13, types single 0x00, dual 0x01, case 0x02
//   protocol v2: request 0x22, reply 0x23, notify 0x25, types (seen on WF-1000XM6):
//                single 0x00, dual 0x01, case 0x02, dual 0x09 and case 0x0A (same layout plus
//                trailing extra bytes). Notifications use 0x09.
// An earbud that sits in the case is reported with level 0.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sony_battery.h"

#define FRAME_START   0x3E
#define FRAME_END     0x3C
#define FRAME_ESCAPE  0x3D
#define ESCAPE_MASK   0xEF

#define TYPE_ACK      0x01
#define TYPE_CMD_1    0x0C
#define TYPE_CMD_2    0x0E

#define CMD_INIT_REQUEST    0x00
#define CMD_INIT_REPLY      0x01
#define V1_BATTERY_REQUEST  0x10
#define V1_BATTERY_REPLY    0x11
#define V1_BATTERY_NOTIFY   0x13
#define V2_BATTERY_REQUEST  0x22
#define V2_BATTERY_REPLY    0x23
#define V2_BATTERY_NOTIFY   0x25

#define BATTERY_TYPE_SINGLE     0x00
#define BATTERY_TYPE_DUAL       0x01
#define BATTERY_TYPE_CASE       0x02
#define V2_BATTERY_TYPE_DUAL_EX 0x09
#define V2_BATTERY_TYPE_CASE_EX 0x0A

#define MAX_PAYLOAD         64
#define MAX_FRAME           (2 * (MAX_PAYLOAD + 7) + 2)
#define TX_QUEUE_SIZE       12
#define RX_BUFFER_SIZE      1024
#define ACK_TIMEOUT_MS      1500
#define SDP_RETRY_MS        500
#define POLL_INTERVAL_MS    60000

// Service UUIDs of the Sony control channel (Gadgetbridge)
static const uint8_t sony_uuid_v2[16] = { 0x95, 0x6C, 0x7B, 0x26, 0xD4, 0x9A, 0x4B, 0xA8, 0xB0, 0x3F, 0xB1, 0x7D, 0x39, 0x3C, 0xB6, 0xE2 };
static const uint8_t sony_uuid_v1[16] = { 0x96, 0xCC, 0x20, 0x3E, 0x50, 0x68, 0x46, 0xAD, 0xB3, 0x2D, 0xE3, 0x16, 0xF5, 0xE0, 0x69, 0xBA };

typedef enum {
    STATE_IDLE,
    STATE_SDP_LIST,     // debug: list all RFCOMM services
    STATE_SDP_V2,
    STATE_SDP_V1,
    STATE_CONNECTING,
    STATE_OPEN,
} sony_state_t;

typedef struct {
    uint8_t len;
    uint8_t data[MAX_PAYLOAD];
} command_t;

static bool         debug_enabled;
static sony_state_t state;
static bd_addr_t    remote;
static uint8_t      rfcomm_channel;
static uint16_t     rfcomm_cid;
static int          protocol_version;   // 1 or 2, from the UUID that matched

static btstack_timer_source_t sdp_retry_timer;
static btstack_timer_source_t ack_timer;
static btstack_timer_source_t poll_timer;

// raw frames waiting for CAN_SEND_NOW (ACKs and commands)
static uint8_t  out_frames[TX_QUEUE_SIZE][MAX_FRAME];
static uint16_t out_frame_len[TX_QUEUE_SIZE];
static int      out_head, out_count;

// commands waiting for the previous command's ACK
static command_t cmd_queue[TX_QUEUE_SIZE];
static int       cmd_head, cmd_count;
static bool      waiting_for_ack;
static uint8_t   tx_seq;

static uint8_t  rx_buffer[RX_BUFFER_SIZE];
static uint16_t rx_len;
static bool     rx_in_frame;

// battery state, -1 = unknown
static int  level_left = -1, level_right = -1, level_case = -1, level_single = -1;
static bool charging_left, charging_right, charging_case, charging_single;

static void sdp_query_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size);
static void start_sdp_query(void);

static void dump(const char * prefix, const uint8_t * data, uint16_t len){
    printf("[sony] %s", prefix);
    for (uint16_t i = 0; i < len; i++) printf(" %02x", data[i]);
    printf("\n");
}

// ---- framing ----

static uint16_t encode_frame(uint8_t type, uint8_t seq, const uint8_t * payload, uint8_t payload_len, uint8_t * out){
    uint8_t body[MAX_PAYLOAD + 7];
    uint16_t n = 0;
    body[n++] = type;
    body[n++] = seq;
    big_endian_store_32(body, n, payload_len);
    n += 4;
    memcpy(&body[n], payload, payload_len);
    n += payload_len;
    uint8_t checksum = 0;
    for (uint16_t i = 0; i < n; i++) checksum += body[i];
    body[n++] = checksum;

    uint16_t pos = 0;
    out[pos++] = FRAME_START;
    for (uint16_t i = 0; i < n; i++){
        uint8_t b = body[i];
        if (b == FRAME_START || b == FRAME_END || b == FRAME_ESCAPE){
            out[pos++] = FRAME_ESCAPE;
            out[pos++] = b & ESCAPE_MASK;
        } else {
            out[pos++] = b;
        }
    }
    out[pos++] = FRAME_END;
    return pos;
}

static void queue_frame(uint8_t type, uint8_t seq, const uint8_t * payload, uint8_t payload_len){
    if (out_count == TX_QUEUE_SIZE){
        printf("[sony] tx queue full, dropping frame\n");
        return;
    }
    int slot = (out_head + out_count) % TX_QUEUE_SIZE;
    out_frame_len[slot] = encode_frame(type, seq, payload, payload_len, out_frames[slot]);
    out_count++;
    rfcomm_request_can_send_now_event(rfcomm_cid);
}

static void send_next_command(void);

static void ack_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (debug_enabled) printf("[sony] no ACK, continuing\n");
    waiting_for_ack = false;
    send_next_command();
}

static void send_next_command(void){
    if (state != STATE_OPEN || waiting_for_ack || cmd_count == 0) return;
    command_t * cmd = &cmd_queue[cmd_head];
    cmd_head = (cmd_head + 1) % TX_QUEUE_SIZE;
    cmd_count--;
    if (debug_enabled) dump("tx cmd", cmd->data, cmd->len);
    queue_frame(TYPE_CMD_1, tx_seq, cmd->data, cmd->len);
    waiting_for_ack = true;
    btstack_run_loop_remove_timer(&ack_timer);
    btstack_run_loop_set_timer_handler(&ack_timer, &ack_timeout_handler);
    btstack_run_loop_set_timer(&ack_timer, ACK_TIMEOUT_MS);
    btstack_run_loop_add_timer(&ack_timer);
}

static void queue_command(uint8_t b0, uint8_t b1){
    if (cmd_count == TX_QUEUE_SIZE) return;
    command_t * cmd = &cmd_queue[(cmd_head + cmd_count) % TX_QUEUE_SIZE];
    cmd->data[0] = b0;
    cmd->data[1] = b1;
    cmd->len = 2;
    cmd_count++;
    send_next_command();
}

// ---- battery ----

static void format_level(char * out, size_t out_size, const char * name, int level, bool charging, bool earbud){
    if (level < 0){
        out[0] = 0;
    } else if (level == 0 && earbud){
        snprintf(out, out_size, "%s in case", name);
    } else {
        snprintf(out, out_size, "%s %d%%%s", name, level, charging ? " (charging)" : "");
    }
}

// print only when something changed, replies to polls are usually identical
static void print_battery(void){
    static char last_line[128];
    char parts[4][40];
    format_level(parts[0], sizeof(parts[0]), "L", level_left, charging_left, true);
    format_level(parts[1], sizeof(parts[1]), "R", level_right, charging_right, true);
    format_level(parts[2], sizeof(parts[2]), "Case", level_case, charging_case, false);
    format_level(parts[3], sizeof(parts[3]), "Headset", (level_left < 0 && level_right < 0) ? level_single : -1, charging_single, false);

    char line[128] = "Battery: ";
    bool first = true;
    for (int i = 0; i < 4; i++){
        if (!parts[i][0]) continue;
        if (!first) strcat(line, " | ");
        strcat(line, parts[i]);
        first = false;
    }
    if (first || strcmp(line, last_line) == 0) return;
    strcpy(last_line, line);
    printf("%s\n", line);
}

static void request_battery(void){
    uint8_t request = (protocol_version == 2) ? V2_BATTERY_REQUEST : V1_BATTERY_REQUEST;
    queue_command(request, BATTERY_TYPE_DUAL);
    queue_command(request, BATTERY_TYPE_CASE);
}

static void poll_timer_handler(btstack_timer_source_t * ts){
    if (state != STATE_OPEN) return;
    request_battery();
    btstack_run_loop_set_timer(ts, POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

// payload: code, battery type, level, charging[, level, charging]
static void handle_battery(const uint8_t * p, uint16_t len){
    if (len < 4) return;
    switch (p[1]){
        case BATTERY_TYPE_DUAL:
        case V2_BATTERY_TYPE_DUAL_EX:
            if (len < 6) return;
            level_left = p[2];
            charging_left = p[3] != 0;
            level_right = p[4];
            charging_right = p[5] != 0;
            break;
        case BATTERY_TYPE_CASE:
        case V2_BATTERY_TYPE_CASE_EX:
            level_case = p[2];
            charging_case = p[3] != 0;
            break;
        case BATTERY_TYPE_SINGLE:
            level_single = p[2];
            charging_single = p[3] != 0;
            break;
        default:
            return;
    }
    print_battery();
}

static void handle_command(const uint8_t * p, uint16_t len){
    if (len == 0) return;
    switch (p[0]){
        case CMD_INIT_REPLY:
            printf("Sony control channel ready (protocol v%d)\n", protocol_version);
            request_battery();
            btstack_run_loop_remove_timer(&poll_timer);
            btstack_run_loop_set_timer_handler(&poll_timer, &poll_timer_handler);
            btstack_run_loop_set_timer(&poll_timer, POLL_INTERVAL_MS);
            btstack_run_loop_add_timer(&poll_timer);
            break;
        case V1_BATTERY_REPLY:
        case V1_BATTERY_NOTIFY:
        case V2_BATTERY_REPLY:
        case V2_BATTERY_NOTIFY:
            handle_battery(p, len);
            break;
        default:
            break;
    }
}

static void handle_frame(const uint8_t * frame, uint16_t len){
    if (len < 7) return;
    uint8_t checksum = 0;
    for (uint16_t i = 0; i < len - 1; i++) checksum += frame[i];
    if (checksum != frame[len - 1]){
        if (debug_enabled) dump("rx bad checksum", frame, len);
        return;
    }
    uint8_t type = frame[0];
    uint8_t seq = frame[1];
    uint32_t payload_len = big_endian_read_32(frame, 2);
    if (payload_len + 7 != len) return;
    const uint8_t * payload = &frame[6];

    if (type == TYPE_ACK){
        tx_seq = seq;
        waiting_for_ack = false;
        btstack_run_loop_remove_timer(&ack_timer);
        send_next_command();
        return;
    }

    if (debug_enabled){
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "rx type %02x:", type);
        dump(prefix, payload, (uint16_t) payload_len);
    }
    queue_frame(TYPE_ACK, (uint8_t)(1 - seq), NULL, 0);
    if (type == TYPE_CMD_1 || type == TYPE_CMD_2){
        handle_command(payload, (uint16_t) payload_len);
    }
}

static void handle_rx_bytes(const uint8_t * data, uint16_t len){
    // frames may be split across RFCOMM packets, so the escape state persists
    static bool escape = false;
    for (uint16_t i = 0; i < len; i++){
        uint8_t b = data[i];
        if (b == FRAME_START){
            rx_in_frame = true;
            rx_len = 0;
            escape = false;
            continue;
        }
        if (!rx_in_frame) continue;
        if (b == FRAME_END){
            rx_in_frame = false;
            handle_frame(rx_buffer, rx_len);
            continue;
        }
        if (b == FRAME_ESCAPE){
            escape = true;
            continue;
        }
        if (escape){
            b |= (uint8_t) ~ESCAPE_MASK;
            escape = false;
        }
        if (rx_len < RX_BUFFER_SIZE) rx_buffer[rx_len++] = b;
    }
}

// ---- RFCOMM ----

static void reset_channel_state(void){
    rfcomm_cid = 0;
    out_head = out_count = 0;
    cmd_head = cmd_count = 0;
    waiting_for_ack = false;
    tx_seq = 0;
    rx_len = 0;
    rx_in_frame = false;
    btstack_run_loop_remove_timer(&ack_timer);
    btstack_run_loop_remove_timer(&poll_timer);
}

static void rfcomm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    if (packet_type == RFCOMM_DATA_PACKET){
        if (debug_enabled) dump("rx raw", packet, size);
        handle_rx_bytes(packet, size);
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)){
        case RFCOMM_EVENT_CHANNEL_OPENED:
            if (rfcomm_event_channel_opened_get_status(packet) != ERROR_CODE_SUCCESS){
                printf("[sony] RFCOMM connection failed, status 0x%02x\n", rfcomm_event_channel_opened_get_status(packet));
                state = STATE_IDLE;
                rfcomm_cid = 0;
                break;
            }
            rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            state = STATE_OPEN;
            if (debug_enabled) printf("[sony] RFCOMM open, cid 0x%02x\n", rfcomm_cid);
            queue_command(CMD_INIT_REQUEST, 0x00);
            break;

        case RFCOMM_EVENT_CAN_SEND_NOW:
            if (out_count == 0) break;
            rfcomm_send(rfcomm_cid, out_frames[out_head], out_frame_len[out_head]);
            out_head = (out_head + 1) % TX_QUEUE_SIZE;
            out_count--;
            if (out_count) rfcomm_request_can_send_now_event(rfcomm_cid);
            break;

        case RFCOMM_EVENT_CHANNEL_CLOSED:
            if (debug_enabled) printf("[sony] RFCOMM closed\n");
            reset_channel_state();
            state = STATE_IDLE;
            break;

        default:
            break;
    }
}

// ---- SDP: find the RFCOMM channel of the Sony service ----

static void sdp_retry_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    start_sdp_query();
}

static void start_sdp_query(void){
    uint8_t status;
    switch (state){
        case STATE_SDP_LIST:
            status = sdp_client_query_rfcomm_channel_and_name_for_service_class_uuid(&sdp_query_handler, remote, BLUETOOTH_PROTOCOL_RFCOMM);
            break;
        case STATE_SDP_V2:
            status = sdp_client_query_rfcomm_channel_and_name_for_uuid128(&sdp_query_handler, remote, sony_uuid_v2);
            break;
        case STATE_SDP_V1:
            status = sdp_client_query_rfcomm_channel_and_name_for_uuid128(&sdp_query_handler, remote, sony_uuid_v1);
            break;
        default:
            return;
    }
    if (status == ERROR_CODE_SUCCESS) return;
    // SDP client is shared with A2DP/AVRCP setup, try again shortly
    btstack_run_loop_remove_timer(&sdp_retry_timer);
    btstack_run_loop_set_timer_handler(&sdp_retry_timer, &sdp_retry_handler);
    btstack_run_loop_set_timer(&sdp_retry_timer, SDP_RETRY_MS);
    btstack_run_loop_add_timer(&sdp_retry_timer);
}

static void sdp_query_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    switch (hci_event_packet_get_type(packet)){
        case SDP_EVENT_QUERY_RFCOMM_SERVICE:
            if (state == STATE_SDP_LIST){
                printf("[sony] RFCOMM service channel %u '%s'\n",
                       sdp_event_query_rfcomm_service_get_rfcomm_channel(packet),
                       sdp_event_query_rfcomm_service_get_name(packet));
            } else if (rfcomm_channel == 0){
                rfcomm_channel = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
            }
            break;

        case SDP_EVENT_QUERY_COMPLETE:
            if (sdp_event_query_complete_get_status(packet) == SDP_QUERY_BUSY){
                start_sdp_query();
                break;
            }
            switch (state){
                case STATE_SDP_LIST:
                    state = STATE_SDP_V2;
                    start_sdp_query();
                    break;
                case STATE_SDP_V2:
                case STATE_SDP_V1:
                    if (rfcomm_channel){
                        protocol_version = (state == STATE_SDP_V2) ? 2 : 1;
                        if (debug_enabled) printf("[sony] control service v%d on RFCOMM channel %u\n", protocol_version, rfcomm_channel);
                        state = STATE_CONNECTING;
                        rfcomm_create_channel(&rfcomm_packet_handler, remote, rfcomm_channel, &rfcomm_cid);
                    } else if (state == STATE_SDP_V2){
                        state = STATE_SDP_V1;
                        start_sdp_query();
                    } else {
                        printf("[sony] headset has no Sony control service, battery levels unavailable\n");
                        state = STATE_IDLE;
                    }
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
}

// ---- public ----

void sony_battery_init(bool debug){
    debug_enabled = debug;
    rfcomm_init();
}

void sony_battery_connect(bd_addr_t addr){
    if (state != STATE_IDLE) return;
    memcpy(remote, addr, 6);
    rfcomm_channel = 0;
    level_left = level_right = level_case = level_single = -1;
    state = debug_enabled ? STATE_SDP_LIST : STATE_SDP_V2;
    start_sdp_query();
}

void sony_battery_disconnect(void){
    btstack_run_loop_remove_timer(&sdp_retry_timer);
    if (rfcomm_cid && state == STATE_OPEN){
        rfcomm_disconnect(rfcomm_cid);
    }
    reset_channel_state();
    state = STATE_IDLE;
}
