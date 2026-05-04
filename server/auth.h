#pragma once

#include <cstdint>
#include <string>

namespace sapisrv {

struct AuthRequest {
    std::string api_key;     // from "Authorization: Bearer <key>" or ?api_key=...
    std::string source_ip;   // dotted-quad / v6 textual; used as the bucket key for unauth
};

struct AuthDecision {
    bool allowed = true;
    int status = 200;            // 401 missing/invalid key (only if a key was supplied)
                                 // 429 rate limited
    const char* status_text = "OK";
    std::string body;            // JSON body for an error response
    int retry_after_seconds = 0; // populated for 429
};

// Load %ProgramData%\sapicli\keys.json (or path override). Missing file =
// public tier only with default qps/burst. Bad JSON throws.
//
// Format:
//   {
//     "public_tier": { "qps": 0.5, "burst": 3 },
//     "keys": {
//       "<key-string>": { "name": "...", "qps": 10, "burst": 50 },
//       ...
//     }
//   }
void load_auth_config(const std::wstring& keys_json_path);

// Evaluate one request against the auth + rate-limit policy. Safe to call
// concurrently from worker threads.
AuthDecision check_auth(const AuthRequest& req);

}  // namespace sapisrv
