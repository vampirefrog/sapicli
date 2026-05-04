#pragma once

#include <cstdint>
#include <string>

namespace sapisrv {

struct HttpConfig {
    std::wstring url_prefix = L"http://+:8080/";
    int worker_threads = 4;
};

// Run the HTTP server. Blocks until stop_event() is signaled. Returns 0 on
// clean shutdown, non-zero on init failure.
int run_http_server(const HttpConfig& cfg);

}  // namespace sapisrv
