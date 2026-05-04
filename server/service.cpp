#include "service.h"
#include "logs.h"

#include <cstdio>
#include <cstring>

namespace sapisrv {

namespace {

HANDLE g_stop_event = nullptr;
SERVICE_STATUS g_status{};
SERVICE_STATUS_HANDLE g_status_handle = nullptr;

void set_service_state(DWORD state, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;
    g_status.dwWin32ExitCode = 0;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwWaitHint = wait_hint;
    static DWORD checkpoint = 1;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    if (g_status_handle) SetServiceStatus(g_status_handle, &g_status);
}

void WINAPI service_ctrl_handler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        set_service_state(SERVICE_STOP_PENDING, 5000);
        if (g_stop_event) SetEvent(g_stop_event);
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerW(kServiceName, service_ctrl_handler);
    if (!g_status_handle) return;
    set_service_state(SERVICE_START_PENDING, 3000);
    set_service_state(SERVICE_RUNNING);
    run_server();
    set_service_state(SERVICE_STOPPED);
}

BOOL WINAPI console_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT
            || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_stop_event) SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

bool ensure_stop_event() {
    if (!g_stop_event)
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_stop_event != nullptr;
}

}  // namespace

HANDLE stop_event() { return g_stop_event; }

int run_as_service() {
    if (!ensure_stop_event()) return 1;
    log::init(log::default_log_dir());
    log::info("sapisrv service starting");
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), service_main },
        { nullptr, nullptr },
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD e = GetLastError();
        log::error("StartServiceCtrlDispatcher failed: %lu", e);
        return 1;
    }
    log::info("sapisrv service stopped");
    return 0;
}

int run_as_console() {
    if (!ensure_stop_event()) return 1;
    log::init(log::default_log_dir());
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    log::info("sapisrv console mode (Ctrl+C to stop)");
    int rc = run_server();
    log::info("sapisrv console mode stopped");
    return rc;
}

int install_service() {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        fwprintf(stderr, L"GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }
    WCHAR cmdline[MAX_PATH + 16];
    _snwprintf_s(cmdline, _countof(cmdline), _TRUNCATE, L"\"%s\" run", path);

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!manager) {
        fwprintf(stderr, L"OpenSCManager failed: %lu (need admin)\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = CreateServiceW(manager, kServiceName, kServiceDisplay,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, cmdline, nullptr, nullptr, nullptr,
        L"NT AUTHORITY\\NetworkService", nullptr);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            fwprintf(stderr, L"Service '%s' already installed.\n", kServiceName);
        } else {
            fwprintf(stderr, L"CreateService failed: %lu\n", e);
        }
        CloseServiceHandle(manager);
        return e == ERROR_SERVICE_EXISTS ? 0 : 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(manager);
    fwprintf(stderr, L"Installed service '%s' (path: %s)\n", kServiceName, cmdline);
    return 0;
}

int uninstall_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!manager) {
        fwprintf(stderr, L"OpenSCManager failed: %lu (need admin)\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(manager, kServiceName, SERVICE_STOP | DELETE);
    if (!svc) {
        fwprintf(stderr, L"OpenService failed: %lu\n", GetLastError());
        CloseServiceHandle(manager);
        return 1;
    }
    SERVICE_STATUS s{};
    ControlService(svc, SERVICE_CONTROL_STOP, &s);
    if (!DeleteService(svc)) {
        fwprintf(stderr, L"DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(manager);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(manager);
    fwprintf(stderr, L"Uninstalled service '%s'\n", kServiceName);
    return 0;
}

}  // namespace sapisrv
