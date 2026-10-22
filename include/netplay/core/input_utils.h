#pragma once

#include <array>
#include <string>
#include <windows.h>

namespace netplay::input
{
void PrimeKeyState(std::array<uint8_t, 256>* keyDown);
bool ConsumeKeyEdge(std::array<uint8_t, 256>* keyDown, int virtualKey);
bool IsCtrlPressed();
bool TryTranslateVirtualKeyToAscii(int virtualKey, char* outChar);
bool TryReadClipboardAsciiText(HWND owner, std::string* outText);
bool TryReadClipboardUtf8Text(HWND owner, std::string* outText);
}

