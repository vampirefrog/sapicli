#pragma once

#include <windows.h>

namespace sapisrv {

constexpr const wchar_t* kServiceName = L"sapisrv";
constexpr const wchar_t* kServiceDisplay = L"SAPI HTTP Server";

// Event signaled when the server should shut down (SCM stop, Ctrl+C, etc.).
HANDLE stop_event();

// Run the server: HTTP loop, request handlers. Returns when stop_event is set.
int run_server();

// Service-mode entry. Hooks SCM, calls run_server() between RUNNING/STOPPED.
int run_as_service();

// Console-mode entry. Installs Ctrl+C handler, calls run_server().
int run_as_console();

// Service install/uninstall via SCM. Requires admin.
int install_service();
int uninstall_service();

}  // namespace sapisrv
