#pragma once

#include <vector>
#include <string>

// Minimal logging used by the loaders and the renderer. Messages are written to
// the debugger output and kept in a ring buffer the UI can display.
namespace Log {

enum class Level { Debug, Info, Warning, Error };

void Write(Level level, const char* format, ...);

// Snapshot of the most recent messages, oldest first.
std::vector<std::string> Recent();

void Clear();

} // namespace Log

#define LOG_DEBUG(...) ::Log::Write(::Log::Level::Debug,   __VA_ARGS__)
#define LOG_INFO(...)  ::Log::Write(::Log::Level::Info,    __VA_ARGS__)
#define LOG_WARN(...)  ::Log::Write(::Log::Level::Warning, __VA_ARGS__)
#define LOG_ERROR(...) ::Log::Write(::Log::Level::Error,   __VA_ARGS__)
