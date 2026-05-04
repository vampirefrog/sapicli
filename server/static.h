#pragma once

#include "handlers.h"

#include <string>

namespace sapisrv {

// Resolve and serve a static file from the configured www_root. Path is the
// URL absolute path (e.g. L"/" maps to "index.html"). Rejects path traversal.
// Sends a 404 if the file isn't found.
void handle_static(const std::wstring& url_path, StreamWriter& out);

// Default www_root: env override SAPISRV_WWW_DIR, else <exe_dir>\www.
std::wstring default_www_dir();

}  // namespace sapisrv
