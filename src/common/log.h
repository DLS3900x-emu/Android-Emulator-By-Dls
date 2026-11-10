// Thread-safe logging to trace.log (host-side diagnostics).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

namespace Aetheron {

void LogInit(const char* trace_path);
void Log(const char* fmt, ...);
// Logs a message only the first time a given key is seen (avoids flooding).
void LogOnce(const std::string& key, const char* fmt, ...);
void LogClose();

}  // namespace Aetheron
