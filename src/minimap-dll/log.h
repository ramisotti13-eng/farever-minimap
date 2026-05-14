#pragma once
#include <string>

namespace farevermod {

// Open the log file (truncating). Called once from DllMain.
void log_open();
void log_close();

// Append a timestamped line. Cheap, sync, no formatting library.
void log_line(const std::string& s);

// Convenience: printf-style.
void logf(const char* fmt, ...);

}  // namespace farevermod
