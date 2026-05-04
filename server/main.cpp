#include "service.h"
#include "logs.h"

#include <cstdio>
#include <cwchar>

namespace {

void print_usage() {
    fwprintf(stderr,
        L"sapisrv \xe2\x80\x94 SAPI HTTP server\n"
        L"\n"
        L"Subcommands:\n"
        L"  console      Run in foreground (logs to stderr + file; Ctrl+C to stop)\n"
        L"  run          Run as Windows Service (called by SCM, not by hand)\n"
        L"  install      Register the service (requires admin)\n"
        L"  uninstall    Remove the service (requires admin)\n"
        L"  logs         Tail the current day's log file (like tail -f)\n"
    );
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    const wchar_t* cmd = argv[1];

    if (!_wcsicmp(cmd, L"console")) return sapisrv::run_as_console();
    if (!_wcsicmp(cmd, L"run"))     return sapisrv::run_as_service();
    if (!_wcsicmp(cmd, L"install")) return sapisrv::install_service();
    if (!_wcsicmp(cmd, L"uninstall")) return sapisrv::uninstall_service();
    if (!_wcsicmp(cmd, L"logs"))    return sapisrv::log::run_logs_follow(sapisrv::log::default_log_dir());

    print_usage();
    return 1;
}
