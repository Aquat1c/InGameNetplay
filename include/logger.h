#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle);
void ShutdownLogger();
void Log(const char* fmt, ...);
}
