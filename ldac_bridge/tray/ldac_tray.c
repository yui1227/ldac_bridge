// ldac_tray: background launcher for ldac_bridge.exe
//
// - runs ldac_bridge.exe hidden and restarts it when it exits (crash, USB lost after sleep)
// - tray icon with connection state and battery in the tooltip
// - Windows notification when the headset connects, with battery levels
// - ldac_tray.exe --install / --uninstall: start at logon (HKCU Run key)
//
// ldac_bridge.exe reports state as "@event ..." lines on stdout; everything is also
// written to ldac_bridge.log next to the executables.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_NAME            L"LDAC Bridge"
#define RUN_KEY             L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define INSTANCE_MUTEX      L"Local\\ldac_tray_instance"
#define QUIT_EVENT_NAME     L"Local\\ldac_bridge_quit"

#define WM_TRAY             (WM_APP + 1)
#define WM_BRIDGE_EVENT     (WM_APP + 2)   // lParam: malloc'ed UTF-8 line
#define WM_BRIDGE_EXITED    (WM_APP + 3)   // wParam: exit code

#define TIMER_NOTIFY        1
#define TIMER_RESTART       2
#define TIMER_KILL          3

#define NOTIFY_WAIT_FOR_BATTERY_MS  4000
#define RESTART_MIN_DELAY_MS        2000
#define RESTART_MAX_DELAY_MS        60000
#define STABLE_RUN_MS               60000
#define KILL_TIMEOUT_MS             3000
#define LOG_MAX_BYTES               (5 * 1024 * 1024)

#define MENU_STATUS         1
#define MENU_OPEN_LOG       2
#define MENU_RESTART        3
#define MENU_EXIT           4

static wchar_t exe_dir[MAX_PATH];
static wchar_t bridge_path[MAX_PATH];
static wchar_t log_path[MAX_PATH];

static HWND            hwnd;
static NOTIFYICONDATAW tray;
static UINT            wm_taskbar_created;

static HANDLE  quit_event;
static HANDLE  child_process;
static DWORD   child_start_tick;
static DWORD   restart_delay = RESTART_MIN_DELAY_MS;
static bool    exiting;
static HANDLE  log_file = INVALID_HANDLE_VALUE;

// state shown in tooltip and notifications
static wchar_t status_text[64] = L"啟動中…";
static wchar_t device_name[64];
static wchar_t battery_text[96];
static bool    connected;
static bool    notify_pending;

// ---- helpers ----

static void utf8_to_wide(const char * in, wchar_t * out, int out_len){
    out[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len);
}

static void update_tooltip(void){
    if (connected && battery_text[0]){
        swprintf(tray.szTip, sizeof(tray.szTip) / sizeof(wchar_t), L"%ls\n%ls\n%ls", APP_NAME, status_text, battery_text);
    } else {
        swprintf(tray.szTip, sizeof(tray.szTip) / sizeof(wchar_t), L"%ls\n%ls", APP_NAME, status_text);
    }
    tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}

static void show_notification(const wchar_t * title, const wchar_t * text){
    tray.uFlags = NIF_INFO;
    tray.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy(tray.szInfoTitle, title, sizeof(tray.szInfoTitle) / sizeof(wchar_t) - 1);
    tray.szInfoTitle[sizeof(tray.szInfoTitle) / sizeof(wchar_t) - 1] = 0;
    wcsncpy(tray.szInfo, text, sizeof(tray.szInfo) / sizeof(wchar_t) - 1);
    tray.szInfo[sizeof(tray.szInfo) / sizeof(wchar_t) - 1] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}

static void show_connected_notification(void){
    notify_pending = false;
    KillTimer(hwnd, TIMER_NOTIFY);
    wchar_t title[128];
    swprintf(title, 128, L"已連上 %ls", device_name);
    show_notification(title, battery_text[0] ? battery_text : L"LDAC 串流中");
}

static void append_level(wchar_t * out, size_t out_len, const wchar_t * name, int level, int charging, bool earbud){
    if (level < 0) return;
    wchar_t part[40];
    if (level == 0 && earbud){
        swprintf(part, 40, L"%ls 在充電盒", name);
    } else {
        swprintf(part, 40, L"%ls %d%%%ls", name, level, charging ? L" (充電中)" : L"");
    }
    if (out[0]) wcsncat(out, L"｜", out_len - wcslen(out) - 1);
    wcsncat(out, part, out_len - wcslen(out) - 1);
}

// ---- events from ldac_bridge.exe ----

static void handle_bridge_event(const char * line){
    if (strncmp(line, "@event connected ", 17) == 0){
        // "@event connected <addr> <name>"
        const char * name = strchr(line + 17, ' ');
        utf8_to_wide(name ? name + 1 : "耳機", device_name, 64);
        connected = true;
        battery_text[0] = 0;
        swprintf(status_text, 64, L"已連上 %ls (LDAC)", device_name);
        update_tooltip();
        // give the battery query a moment so the notification can include it
        notify_pending = true;
        SetTimer(hwnd, TIMER_NOTIFY, NOTIFY_WAIT_FOR_BATTERY_MS, NULL);
    } else if (strncmp(line, "@event battery ", 15) == 0){
        int l = -1, r = -1, c = -1, h = -1, lc = 0, rc = 0, cc = 0;
        sscanf(line + 15, "L=%d R=%d C=%d H=%d Lc=%d Rc=%d Cc=%d", &l, &r, &c, &h, &lc, &rc, &cc);
        battery_text[0] = 0;
        size_t n = sizeof(battery_text) / sizeof(wchar_t);
        append_level(battery_text, n, L"左", l, lc, true);
        append_level(battery_text, n, L"右", r, rc, true);
        append_level(battery_text, n, L"充電盒", c, cc, false);
        if (l < 0 && r < 0) append_level(battery_text, n, L"電量", h, 0, false);
        update_tooltip();
        if (notify_pending) show_connected_notification();
    } else if (strcmp(line, "@event disconnected") == 0){
        connected = false;
        notify_pending = false;
        KillTimer(hwnd, TIMER_NOTIFY);
        wcscpy(status_text, L"已中斷連線，等待重新連線");
        update_tooltip();
    } else if (strcmp(line, "@event waiting") == 0){
        wcscpy(status_text, L"等待耳機連線（開啟耳機或從充電盒取出）");
        update_tooltip();
    } else if (strncmp(line, "@event fatal", 12) == 0){
        wcscpy(status_text, L"藍牙控制器無回應，重新啟動中");
        update_tooltip();
    }
}

// ---- child process ----

static void open_log(void){
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    log_file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    if (log_file != INVALID_HANDLE_VALUE && GetFileSizeEx(log_file, &size) && size.QuadPart > LOG_MAX_BYTES){
        CloseHandle(log_file);
        log_file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
}

// reads ldac_bridge stdout: copy to log, forward "@event" lines to the window
static DWORD WINAPI reader_thread(LPVOID arg){
    HANDLE pipe = (HANDLE) arg;
    char buffer[4096];
    char line[1024];
    size_t line_len = 0;
    DWORD n;
    while (ReadFile(pipe, buffer, sizeof(buffer), &n, NULL) && n > 0){
        DWORD written;
        if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, buffer, n, &written, NULL);
        for (DWORD i = 0; i < n; i++){
            char ch = buffer[i];
            if (ch == '\r') continue;
            if (ch != '\n'){
                if (line_len < sizeof(line) - 1) line[line_len++] = ch;
                continue;
            }
            line[line_len] = 0;
            if (strncmp(line, "@event ", 7) == 0){
                PostMessageW(hwnd, WM_BRIDGE_EVENT, 0, (LPARAM) _strdup(line));
            }
            line_len = 0;
        }
    }
    CloseHandle(pipe);
    return 0;
}

static DWORD WINAPI wait_thread(LPVOID arg){
    HANDLE process = (HANDLE) arg;
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    PostMessageW(hwnd, WM_BRIDGE_EXITED, (WPARAM) code, 0);
    return 0;
}

static void start_child(void){
    if (child_process || exiting) return;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE read_end, write_end;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    open_log();
    ResetEvent(quit_event);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    wchar_t cmdline[MAX_PATH + 4];
    swprintf(cmdline, MAX_PATH + 4, L"\"%ls\"", bridge_path);
    BOOL ok = CreateProcessW(bridge_path, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, exe_dir, &si, &pi);
    CloseHandle(write_end);
    if (!ok){
        CloseHandle(read_end);
        wcscpy(status_text, L"找不到 ldac_bridge.exe");
        update_tooltip();
        return;
    }
    CloseHandle(pi.hThread);
    child_process = pi.hProcess;
    child_start_tick = GetTickCount();
    wcscpy(status_text, L"連線中…");
    update_tooltip();
    CloseHandle(CreateThread(NULL, 0, reader_thread, read_end, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, wait_thread, child_process, 0, NULL));
}

// ask ldac_bridge to shut down cleanly; force it after a timeout
static void stop_child(void){
    if (!child_process) return;
    SetEvent(quit_event);
    SetTimer(hwnd, TIMER_KILL, KILL_TIMEOUT_MS, NULL);
}

static void on_child_exited(DWORD code){
    KillTimer(hwnd, TIMER_KILL);
    if (child_process){
        CloseHandle(child_process);
        child_process = NULL;
    }
    connected = false;
    notify_pending = false;
    if (exiting){
        DestroyWindow(hwnd);
        return;
    }
    // quick failures (e.g. controller not ready yet) back off, a long healthy run resets the delay
    DWORD ran = GetTickCount() - child_start_tick;
    if (ran > STABLE_RUN_MS) restart_delay = RESTART_MIN_DELAY_MS;
    if (code != 0 && ran < STABLE_RUN_MS) wcscpy(status_text, L"藍牙未就緒，稍後重試");
    else wcscpy(status_text, L"重新啟動中…");
    update_tooltip();
    SetTimer(hwnd, TIMER_RESTART, restart_delay, NULL);
    if (ran <= STABLE_RUN_MS){
        restart_delay *= 2;
        if (restart_delay > RESTART_MAX_DELAY_MS) restart_delay = RESTART_MAX_DELAY_MS;
    }
}

// ---- tray ----

static void add_tray_icon(void){
    ZeroMemory(&tray, sizeof(tray));
    tray.cbSize = sizeof(tray);
    tray.hWnd = hwnd;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!tray.hIcon) tray.hIcon = LoadIconW(NULL, (LPCWSTR) IDI_APPLICATION);
    wcscpy(tray.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &tray);
    update_tooltip();
}

static void show_menu(void){
    HMENU menu = CreatePopupMenu();
    wchar_t status[160];
    if (connected && battery_text[0]) swprintf(status, 160, L"%ls - %ls", status_text, battery_text);
    else wcsncpy(status, status_text, 160);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, MENU_STATUS, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_OPEN_LOG, L"開啟記錄檔");
    AppendMenuW(menu, MF_STRING, MENU_RESTART, L"重新啟動藍牙");
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"結束");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp){
    if (msg == wm_taskbar_created){
        // explorer restarted
        add_tray_icon();
        return 0;
    }
    switch (msg){
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)){
                case MENU_OPEN_LOG:
                    ShellExecuteW(NULL, L"open", log_path, NULL, NULL, SW_SHOWNORMAL);
                    break;
                case MENU_RESTART:
                    restart_delay = RESTART_MIN_DELAY_MS;
                    stop_child();
                    break;
                case MENU_EXIT:
                    exiting = true;
                    if (child_process) stop_child();
                    else DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            return 0;
        case WM_BRIDGE_EVENT: {
            char * line = (char *) lp;
            handle_bridge_event(line);
            free(line);
            return 0;
        }
        case WM_BRIDGE_EXITED:
            on_child_exited((DWORD) wp);
            return 0;
        case WM_TIMER:
            KillTimer(h, wp);
            if (wp == TIMER_NOTIFY && notify_pending) show_connected_notification();
            if (wp == TIMER_RESTART) start_child();
            if (wp == TIMER_KILL && child_process) TerminateProcess(child_process, 1);
            return 0;
        case WM_POWERBROADCAST:
            // USB Bluetooth is usually reset by sleep: start fresh after resume
            if (wp == PBT_APMRESUMEAUTOMATIC){
                restart_delay = RESTART_MIN_DELAY_MS;
                if (child_process) stop_child();
            }
            return TRUE;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &tray);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

// ---- autostart ----

static int install_autostart(bool install){
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS){
        MessageBoxW(NULL, L"無法開啟登錄機碼", APP_NAME, MB_ICONERROR);
        return 1;
    }
    LONG result;
    if (install){
        wchar_t self[MAX_PATH], value[MAX_PATH + 4];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        swprintf(value, MAX_PATH + 4, L"\"%ls\"", self);
        result = RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *) value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, APP_NAME);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    MessageBoxW(NULL, result == ERROR_SUCCESS ? (install ? L"已設定登入時自動啟動" : L"已取消自動啟動") : L"設定失敗",
                APP_NAME, result == ERROR_SUCCESS ? MB_ICONINFORMATION : MB_ICONERROR);
    return result == ERROR_SUCCESS ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd, int show){
    (void) prev;
    (void) show;

    if (wcsstr(cmd, L"--install"))   return install_autostart(true);
    if (wcsstr(cmd, L"--uninstall")) return install_autostart(false);

    CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t * sep = wcsrchr(exe_dir, L'\\');
    if (sep) *sep = 0;
    swprintf(bridge_path, MAX_PATH, L"%ls\\ldac_bridge.exe", exe_dir);
    swprintf(log_path, MAX_PATH, L"%ls\\ldac_bridge.log", exe_dir);

    quit_event = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT_NAME);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ldac_tray";
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"ldac_tray", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    add_tray_icon();
    start_child();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    return 0;
}
