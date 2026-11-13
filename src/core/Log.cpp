#include "core/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

constexpr size_t kMaxMessages = 4096;

std::mutex             g_mutex;
std::deque<std::string> g_messages;
std::FILE*             g_file = nullptr;

// Mirrors the log next to the executable. A map editor spends most of its time
// chewing through undocumented binary data, so a file that survives a crash is
// worth more than the debugger output alone.
std::FILE* LogFile() {
    if (!g_file) {
        char path[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, path, MAX_PATH)) {
            if (char* slash = std::strrchr(path, '\\')) slash[1] = '\0';
            std::strncat(path, "FRSWorldEditor.log", MAX_PATH - std::strlen(path) - 1);
            g_file = std::fopen(path, "w");
        }
    }
    return g_file;
}

const char* LevelTag(Log::Level level) {
    switch (level) {
        case Log::Level::Debug:   return "[debug] ";
        case Log::Level::Info:    return "[info ] ";
        case Log::Level::Warning: return "[warn ] ";
        case Log::Level::Error:   return "[error] ";
    }
    return "[?????] ";
}

} // namespace

namespace Log {

void Write(Level level, const char* format, ...) {
    char body[1024];

    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    std::string line = LevelTag(level);
    line += body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_messages.push_back(line);
        if (g_messages.size() > kMaxMessages) g_messages.pop_front();

        line += '\n';
        if (std::FILE* file = LogFile()) {
            std::fputs(line.c_str(), file);
            std::fflush(file);
        }
    }

    OutputDebugStringA(line.c_str());
}

std::vector<std::string> Recent() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return { g_messages.begin(), g_messages.end() };
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_messages.clear();
}

} // namespace Log
