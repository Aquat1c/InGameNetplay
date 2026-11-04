#include <cstdio>
#include <windows.h>

extern "C" __declspec(dllexport) int __cdecl WriteNativeLog(const wchar_t* path, const char* text)
{
    FILE* file = _wfopen(path, L"w");
    if (file == nullptr) return 0;
    const bool wrote = std::fputs(text, file) >= 0;
    return std::fclose(file) == 0 && wrote ? 1 : 0;
}
