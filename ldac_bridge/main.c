// Windows / WinUSB / Intel main for ldac_bridge
// Based on BTstack port/windows-winusb-intel/main.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <timeapi.h>

#include "btstack_config.h"

#include "btstack_chipset_intel_firmware.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_windows.h"
#include "btstack_stdin.h"
#include "btstack_stdin_windows.h"
#include "btstack_tlv_windows.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_windows_fs.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

int btstack_main(int argc, const char * argv[]);
int ldac_bridge_setup(int argc, const char * argv[]);

#define TLV_DB_PATH_PREFIX "btstack_"
#define TLV_DB_PATH_POSTFIX ".tlv"
static char tlv_db_path[MAX_PATH];
static const btstack_tlv_t * tlv_impl;
static btstack_tlv_windows_t   tlv_context;
static bd_addr_t             local_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;

static int main_argc;
static const char ** main_argv;
static const hci_transport_t * transport;
static bool shutdown_triggered;

static char exe_dir[MAX_PATH];
static char firmware_dir[MAX_PATH];

// Controller heartbeat: BTstack's WinUSB transport only logs USB errors (e.g. after
// sleep/resume or unplug) and then goes silent. Ask the controller regularly and exit
// when it stops answering, so the tray launcher restarts us.
#define HEARTBEAT_INTERVAL_MS   10000
#define HEARTBEAT_MAX_MISSED    3
#define EXIT_CODE_CONTROLLER_LOST 3

// Set by ldac_tray.exe to request a clean shutdown
#define QUIT_EVENT_NAME "Local\\ldac_bridge_quit"

static btstack_timer_source_t heartbeat_timer;
static bool heartbeat_pending;
static int  heartbeat_missed;
static btstack_context_callback_registration_t quit_callback;

static void heartbeat_handler(btstack_timer_source_t * ts){
    if (heartbeat_pending || !hci_can_send_command_packet_now()){
        heartbeat_missed++;
        if (heartbeat_missed >= HEARTBEAT_MAX_MISSED){
            printf("Bluetooth controller stopped responding (USB reset or sleep?), exiting\n");
            printf("@event fatal controller lost\n");
            exit(EXIT_CODE_CONTROLLER_LOST);
        }
    }
    if (hci_can_send_command_packet_now()){
        heartbeat_pending = true;
        hci_send_cmd(&hci_read_local_version_information);
    }
    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE &&
        hci_event_command_complete_get_command_opcode(packet) == hci_read_local_version_information.opcode){
        heartbeat_pending = false;
        heartbeat_missed = 0;
        return;
    }
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) return;
    switch (btstack_event_state_get_state(packet)){
        case HCI_STATE_WORKING:
            btstack_run_loop_set_timer_handler(&heartbeat_timer, &heartbeat_handler);
            btstack_run_loop_set_timer(&heartbeat_timer, HEARTBEAT_INTERVAL_MS);
            btstack_run_loop_add_timer(&heartbeat_timer);
            gap_local_bd_addr(local_addr);
            printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
            // setup TLV next to the executable, so pairing survives restarts
            snprintf(tlv_db_path, sizeof(tlv_db_path), "%s\\%s%s%s", exe_dir, TLV_DB_PATH_PREFIX,
                     bd_addr_to_str_with_delimiter(local_addr, '-'), TLV_DB_PATH_POSTFIX);
            tlv_impl = btstack_tlv_windows_init_instance(&tlv_context, tlv_db_path);
            btstack_tlv_set_instance(tlv_impl, &tlv_context);
            hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
            break;
        case HCI_STATE_OFF:
            btstack_tlv_windows_deinit(&tlv_context);
            if (!shutdown_triggered) break;
            btstack_stdin_reset();
            timeEndPeriod(1);
            exit(0);
            break;
        default:
            break;
    }
}

static void trigger_shutdown(void){
    printf("Shutting down...\n");
    shutdown_triggered = true;
    btstack_run_loop_remove_timer(&heartbeat_timer);
    hci_power_control(HCI_POWER_OFF);
}

static void quit_on_main_thread(void * context){
    UNUSED(context);
    trigger_shutdown();
}

static DWORD WINAPI quit_event_thread(LPVOID arg){
    HANDLE quit_event = (HANDLE) arg;
    WaitForSingleObject(quit_event, INFINITE);
    quit_callback.callback = &quit_on_main_thread;
    btstack_run_loop_execute_on_main_thread(&quit_callback);
    return 0;
}

static void intel_firmware_done(int result){
    if (result != 0){
        printf("Intel firmware setup failed (%d). Check that %s contains the matching ibt-*.sfi/.ddc files.\n",
               result, firmware_dir);
        exit(result);
    }
    printf("Intel controller ready.\n");

    hci_init(transport, NULL);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_main(main_argc, main_argv);
}

int main(int argc, const char * argv[]){

    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    main_argc = argc;
    main_argv = argv;

    // 1 ms timer resolution for smooth audio pacing
    timeBeginPeriod(1);

    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    char * last_sep = strrchr(exe_dir, '\\');
    if (last_sep) *last_sep = 0;
    snprintf(firmware_dir, sizeof(firmware_dir), "%s\\firmware", exe_dir);

    // keep last_device.txt etc. next to the executable
    SetCurrentDirectoryA(exe_dir);

    int setup = ldac_bridge_setup(argc, argv);
    if (setup != 0){
        timeEndPeriod(1);
        return setup > 0 ? 0 : 1;
    }

    printf("ldac_bridge booting up\n");

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    char pklg_path[MAX_PATH];
    snprintf(pklg_path, sizeof(pklg_path), "%s\\hci_dump.pklg", exe_dir);
    hci_dump_windows_fs_open(pklg_path, HCI_DUMP_PACKETLOGGER);
    hci_dump_init(hci_dump_windows_fs_get_instance());
    printf("Packet Log: %s\n", pklg_path);

    btstack_stdin_windows_init();
    btstack_stdin_window_register_ctrl_c_callback(&trigger_shutdown);

    HANDLE quit_event = CreateEventA(NULL, TRUE, FALSE, QUIT_EVENT_NAME);
    if (quit_event){
        ResetEvent(quit_event);
        CreateThread(NULL, 0, quit_event_thread, quit_event, 0, NULL);
    }

    transport = hci_transport_usb_instance();
    btstack_chipset_intel_set_firmware_path(firmware_dir);
    btstack_chipset_intel_download_firmware(transport, &intel_firmware_done);

    btstack_run_loop_execute();
    return 0;
}
