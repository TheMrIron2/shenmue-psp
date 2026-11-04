#pragma once

namespace shenmue::psp {

void InitBootLog(int argc, char **argv);
void BootLog(const char *format, ...);
void BootLogHeap(const char *stage);

} // namespace shenmue::psp
