#pragma once

#include <windows.h>

namespace sapisrv {

constexpr const wchar_t* kServiceName = L"sapisrv";
constexpr const wchar_t* kServiceDisplay = L"SAPI HTTP Server";

// Event signaled when the server should shut down (SCM stop, Ctrl+C, etc.).
HANDLE stop_event();

constexpr int kDefaultPort = 8080;

// Run the server: HTTP loop, request handlers. Returns when stop_event is set.
int run_server(int port);

// Service-mode entry. Hooks SCM, calls run_server() between RUNNING/STOPPED.
int run_as_service(int port);

// Console-mode entry. Installs Ctrl+C handler, calls run_server().
int run_as_console(int port);

// Service install/uninstall via SCM. Requires admin.
int install_service();
int uninstall_service();

}  // namespace sapisrv
