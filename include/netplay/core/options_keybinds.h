#pragma once

#include <array>
#include <string>

namespace netplay::options::keybinds
{
bool IsKeyboardBindingValue(const std::string& value);
bool IsPadBindingValue(const std::string& value);
bool IsBindableValue(const std::string& value);
// Resolves a keyboard binding value (e.g. "DIK_F1") to a Win32 virtual-key code
// suitable for GetAsyncKeyState. Returns 0 if the value is not a known keyboard
// binding (e.g. a pad binding or unknown name).
int VirtualKeyForBindingValue(const std::string& value);
std::string NormalizeBindingValue(const std::string& value);
std::string FormatBindingValue(const std::string& value);
void PrimePadButtonState(std::array<uint32_t, 32>* buttonsDown);
bool TryCaptureKeyboardBinding(
    std::array<uint8_t, 256>* keyDown,
    std::string* outValue,
    std::string* outDisplayValue);
bool TryCapturePadBinding(
    std::array<uint32_t, 32>* buttonsDown,
    std::string* outValue,
    std::string* outDisplayValue);
}
