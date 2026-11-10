#pragma once

namespace shenmue::psp {

void InitBootLog(int argc, char **argv);
void BootLog(const char *format, ...);

// Truncate the log file(s): called once per boot so a run starts clean.
void ResetBootLog();
void BootLogHeap(const char *stage);

} // namespace shenmue::psp
