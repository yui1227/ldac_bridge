// Windows / WinUSB / Intel main for ldac_bridge
// Based on BTstack port/windows-winusb-intel/main.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <setupapi.h>
#include <timeapi.h>

#include "btstack_config.h"

#include "btstack_chipset_intel_firmware.h"
#include "btstack_chipset_realtek.h"
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
// sleep/resume or unplug) and then goes silent. Exit when no HCI event at all arrived
// for a while, so the tray launcher restarts us. While streaming the controller reports
// completed packets constantly; when idle, the periodic Read Local Version provides events.
// Note: "cannot send a command right now" is normal (USB transfer or packet buffer busy)
// and must not count as a failure.
#define HEARTBEAT_INTERVAL_MS   10000
#define HEARTBEAT_SILENCE_MS    30000
#define EXIT_CODE_CONTROLLER_LOST 3

// Set by ldac_tray.exe to request a clean shutdown
#define QUIT_EVENT_NAME "Local\\ldac_bridge_quit"

// Controller setup (firmware download, HCI init) normally takes a few seconds. A controller
// that stops answering during setup would otherwise leave us waiting forever.
#define INIT_TIMEOUT_MS         30000

static btstack_timer_source_t init_timer;
static btstack_timer_source_t heartbeat_timer;
static uint32_t heartbeat_last_event_ms;
static btstack_context_callback_registration_t quit_callback;

static void heartbeat_handler(btstack_timer_source_t * ts){
    uint32_t silent_ms = btstack_run_loop_get_time_ms() - heartbeat_last_event_ms;
    if (silent_ms > HEARTBEAT_SILENCE_MS){
        printf("Bluetooth controller silent for %u s (USB reset or sleep?), exiting\n", (unsigned)(silent_ms / 1000));
        printf("@event fatal controller lost\n");
        exit(EXIT_CODE_CONTROLLER_LOST);
    }
    if (hci_can_send_command_packet_now()){
        hci_send_cmd(&hci_read_local_version_information);
    }
    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void init_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    printf("Bluetooth controller did not finish setup within %u s, exiting\n", (unsigned)(INIT_TIMEOUT_MS / 1000));
    printf("@event fatal controller setup timeout\n");
    exit(EXIT_CODE_CONTROLLER_LOST);
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    heartbeat_last_event_ms = btstack_run_loop_get_time_ms();
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) return;
    switch (btstack_event_state_get_state(packet)){
        case HCI_STATE_WORKING:
            btstack_run_loop_remove_timer(&init_timer);
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

// ---- Bluetooth adapter selection ----
//
// BTstack's WinUSB transport opens the first USB device it can open with WinUSB. Find that
// device ourselves to know which chip it is: Intel needs its firmware loaded before HCI
// starts, Realtek gets its ROM patch during HCI init, other chips (e.g. CSR) need nothing.

typedef enum { ADAPTER_NONE, ADAPTER_INTEL, ADAPTER_REALTEK, ADAPTER_GENERIC } adapter_type_t;

#define USB_VENDOR_ID_INTEL     0x8087
#define USB_VENDOR_ID_REALTEK   0x0bda

static const GUID GUID_DEVINTERFACE_USB_DEVICE_ = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

// Realtek chips sold under another vendor ID (product ID must be in BTstack's Realtek table)
static const uint16_t realtek_rebranded[][2] = {
    { 0x0b05, 0x1d70 },     // ASUS USB-BT540 (RTL8761CU)
};

static adapter_type_t adapter_type;
static uint16_t       adapter_vendor_id;
static uint16_t       adapter_product_id;

static const char * adapter_type_name(adapter_type_t type){
    switch (type){
        case ADAPTER_INTEL:   return "Intel";
        case ADAPTER_REALTEK: return "Realtek";
        default:              return "generic";
    }
}

static adapter_type_t classify_adapter(uint16_t vendor_id, uint16_t product_id){
    if (vendor_id == USB_VENDOR_ID_INTEL) return ADAPTER_INTEL;
    if (vendor_id == USB_VENDOR_ID_REALTEK) return ADAPTER_REALTEK;
    for (size_t i = 0; i < sizeof(realtek_rebranded) / sizeof(realtek_rebranded[0]); i++){
        if (realtek_rebranded[i][0] == vendor_id && realtek_rebranded[i][1] == product_id) return ADAPTER_REALTEK;
    }
    return ADAPTER_GENERIC;
}

// true if any string of a REG_MULTI_SZ list contains needle
static bool multi_sz_contains(const char * list, const char * needle){
    for (const char * s = list; *s; s += strlen(s) + 1){
        if (strstr(s, needle)) return true;
    }
    return false;
}

static void find_bluetooth_adapter(void){
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE_, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    int found = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++){
        char service[64] = {0};
        char hardware_ids[512] = {0};
        char compatible_ids[1024] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_SERVICE, NULL, (BYTE *) service, sizeof(service) - 1, NULL)) continue;
        if (_stricmp(service, "WinUSB") != 0) continue;
        SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID, NULL, (BYTE *) hardware_ids, sizeof(hardware_ids) - 2, NULL);
        SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_COMPATIBLEIDS, NULL, (BYTE *) compatible_ids, sizeof(compatible_ids) - 2, NULL);
        unsigned vendor_id = 0, product_id = 0;
        const char * vid = strstr(hardware_ids, "VID_");
        const char * pid = strstr(hardware_ids, "PID_");
        if (!vid || !pid || sscanf(vid + 4, "%4x", &vendor_id) != 1 || sscanf(pid + 4, "%4x", &product_id) != 1) continue;
        adapter_type_t type = classify_adapter((uint16_t) vendor_id, (uint16_t) product_id);
        // USB class E0 = wireless controller (Bluetooth)
        if (type == ADAPTER_GENERIC && !multi_sz_contains(compatible_ids, "Class_E0")) continue;
        printf("Bluetooth adapter on WinUSB: %04X:%04X (%s)\n", vendor_id, product_id, adapter_type_name(type));
        if (found++ == 0){
            adapter_type = type;
            adapter_vendor_id = (uint16_t) vendor_id;
            adapter_product_id = (uint16_t) product_id;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found > 1){
        printf("Warning: %d Bluetooth adapters use WinUSB, the first one is used. Give the others back to Windows.\n", found);
    }
}

static void start_stack(void){
    hci_init(transport, NULL);

    if (adapter_type == ADAPTER_REALTEK){
        // ROM patch rtl*_fw.bin / rtl*_config.bin from the firmware folder, downloaded during HCI init
        btstack_chipset_realtek_set_firmware_folder_path(firmware_dir);
        btstack_chipset_realtek_set_config_folder_path(firmware_dir);
        btstack_chipset_realtek_set_product_id(adapter_product_id);
        hci_set_chipset(btstack_chipset_realtek_instance());
        // phase 1 (identify chip) runs before HCI Reset, phase 2 (ROM version, patch download) after it
        hci_enable_custom_pre_init();
    }

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_main(main_argc, main_argv);
}

static void intel_firmware_done(int result){
    if (result != 0){
        printf("Intel firmware setup failed (%d). Check that %s contains the matching ibt-*.sfi/.ddc files.\n",
               result, firmware_dir);
        exit(result);
    }
    printf("Intel controller ready.\n");
    start_stack();
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

    // keep the previous run's packet log: after an automatic restart it shows what went wrong
    char pklg_path[MAX_PATH];
    char pklg_prev_path[MAX_PATH];
    snprintf(pklg_path, sizeof(pklg_path), "%s\\hci_dump.pklg", exe_dir);
    snprintf(pklg_prev_path, sizeof(pklg_prev_path), "%s\\hci_dump.prev.pklg", exe_dir);
    MoveFileExA(pklg_path, pklg_prev_path, MOVEFILE_REPLACE_EXISTING);
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

    btstack_run_loop_set_timer_handler(&init_timer, &init_timeout_handler);
    btstack_run_loop_set_timer(&init_timer, INIT_TIMEOUT_MS);
    btstack_run_loop_add_timer(&init_timer);

    transport = hci_transport_usb_instance();
    find_bluetooth_adapter();
    switch (adapter_type){
        case ADAPTER_NONE:
            printf("No Bluetooth adapter uses the WinUSB driver. Assign WinUSB to your adapter with Zadig (see README).\n");
            timeEndPeriod(1);
            return 1;
        case ADAPTER_INTEL:
            btstack_chipset_intel_set_firmware_path(firmware_dir);
            btstack_chipset_intel_download_firmware(transport, &intel_firmware_done);
            break;
        default:
            start_stack();
            break;
    }

    btstack_run_loop_execute();
    return 0;
}
