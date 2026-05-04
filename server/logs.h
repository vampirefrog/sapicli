#pragma once

#include <string>

namespace sapisrv::log {

// Initialize logging. Creates the log directory if missing. Logs go to both
// stderr (useful in console mode) and a rolling file
//   <log_dir>\sapisrv-YYYYMMDD.log
// The file is rotated when the date changes.
//
// init() must be called once at process start. Safe to call again on a
// different log_dir but unusual.
void init(const std::wstring& log_dir);

void info (const char* fmt, ...);
void warn (const char* fmt, ...);
void error(const char* fmt, ...);

// Subcommand: open the current day's log file, seek near the end, stream new
// content to stdout indefinitely. Polls for updates and rolls to the next
// day's file across midnight. Returns when the global stop_event is signaled.
int run_logs_follow(const std::wstring& log_dir);

// Default log dir resolution: $SAPISRV_LOGS_DIR if set, else
// %ProgramData%\sapicli\logs.
std::wstring default_log_dir();

}  // namespace sapisrv::log
