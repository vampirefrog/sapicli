#include "service.h"
#include "logs.h"

#include <cstdio>
#include <cwchar>
#include <cstdlib>

namespace {

void print_usage() {
    fwprintf(stderr,
        L"sapisrv \xe2\x80\x94 SAPI HTTP server\n"
        L"\n"
        L"Subcommands:\n"
        L"  console [--port=N]   Run in foreground (logs to stderr + file; Ctrl+C to stop)\n"
        L"  run     [--port=N]   Run as Windows Service (called by SCM, not by hand)\n"
        L"  install              Register the service (requires admin)\n"
        L"  uninstall            Remove the service (requires admin)\n"
        L"  logs                 Tail the current day's log file (like tail -f)\n"
        L"\n"
        L"--port defaults to %d.\n",
        sapisrv::kDefaultPort
    );
}

// Pull the value of `--port=N` (or `--port N`) out of argv. Returns
// kDefaultPort if the flag is missing. -1 on parse error.
int parse_port(int argc, wchar_t* argv[]) {
    for (int i = 2; i < argc; ++i) {
        const wchar_t* a = argv[i];
        const wchar_t* val = nullptr;
        if (!wcsncmp(a, L"--port=", 7))      val = a + 7;
        else if (!wcscmp(a, L"--port") && i + 1 < argc) val = argv[++i];
        if (!val) continue;
        wchar_t* end = nullptr;
        long p = wcstol(val, &end, 10);
        if (*end != L'\0' || p <= 0 || p > 65535) {
            fwprintf(stderr, L"Invalid port: %s\n", val);
            return -1;
        }
        return static_cast<int>(p);
    }
    return sapisrv::kDefaultPort;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    const wchar_t* cmd = argv[1];

    int port = parse_port(argc, argv);
    if (port < 0) return 1;

    if (!_wcsicmp(cmd, L"console")) return sapisrv::run_as_console(port);
    if (!_wcsicmp(cmd, L"run"))     return sapisrv::run_as_service(port);
    if (!_wcsicmp(cmd, L"install")) return sapisrv::install_service();
    if (!_wcsicmp(cmd, L"uninstall")) return sapisrv::uninstall_service();
    if (!_wcsicmp(cmd, L"logs"))    return sapisrv::log::run_logs_follow(sapisrv::log::default_log_dir());

    print_usage();
    return 1;
}
