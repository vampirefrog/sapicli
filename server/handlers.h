#pragma once

#include <string>
#include <vector>

namespace sapisrv {

struct Response {
    int status = 200;
    std::string status_text = "OK";
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

// GET /voices — returns JSON array of available SAPI voices.
Response handle_voices();

}  // namespace sapisrv
