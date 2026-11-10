#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>

namespace Aetheron {

static std::mutex g_log_mutex;
static FILE* g_log = nullptr;
static std::set<std::string> g_once;
static LARGE_INTEGER g_t0, g_freq;

void LogInit(const char* trace_path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log) fclose(g_log);
    g_log = fopen(trace_path, "w");
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
}

static void VLog(const char* fmt, va_list ap) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart ? g_freq.QuadPart : 1);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (g_log) {
        fprintf(g_log, "[%10.4f] %s\n", t, buf);
        fflush(g_log);
    }
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    VLog(fmt, ap);
    va_end(ap);
}

void LogOnce(const std::string& key, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_once.insert(key).second) return;
    va_list ap;
    va_start(ap, fmt);
    VLog(fmt, ap);
    va_end(ap);
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log) fclose(g_log);
    g_log = nullptr;
}

}  // namespace Aetheron
