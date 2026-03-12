#pragma once

#include <array>
#include <string>

namespace netplay::options::keybinds
{
bool IsKeyboardBindingValue(const std::string& value);
bool IsPadBindingValue(const std::string& value);
bool IsBindableValue(const std::string& value);
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
