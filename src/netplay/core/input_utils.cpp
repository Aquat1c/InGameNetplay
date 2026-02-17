#include "netplay/core/input_utils.h"

#include <vector>

namespace netplay::input
{
void PrimeKeyState(std::array<uint8_t, 256>* keyDown)
{
    if (keyDown == nullptr)
    {
        return;
    }

    keyDown->fill(0);
    for (int vk = 0; vk < 256; ++vk)
    {
        (*keyDown)[static_cast<size_t>(vk)] = (GetAsyncKeyState(vk) & 0x8000) != 0 ? 1u : 0u;
    }
}

bool ConsumeKeyEdge(std::array<uint8_t, 256>* keyDown, int virtualKey)
{
    if (keyDown == nullptr || virtualKey < 0 || virtualKey >= 256)
    {
        return false;
    }

    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const size_t keyIndex = static_cast<size_t>(virtualKey);
    const bool pressed = down && (*keyDown)[keyIndex] == 0;
    (*keyDown)[keyIndex] = down ? 1u : 0u;
    return pressed;
}

bool IsCtrlPressed()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool TryTranslateVirtualKeyToAscii(int virtualKey, char* outChar)
{
    if (outChar == nullptr)
    {
        return false;
    }

    if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
    {
        *outChar = static_cast<char>('0' + (virtualKey - VK_NUMPAD0));
        return true;
    }
    if (virtualKey == VK_DECIMAL)
    {
        *outChar = '.';
        return true;
    }

    BYTE keyboardState[256] = {};
    if (GetKeyboardState(keyboardState) == FALSE)
    {
        return false;
    }

    WORD translated = 0;
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    const int result = ToAscii(
        static_cast<UINT>(virtualKey),
        scanCode,
        keyboardState,
        &translated,
        0);
    if (result != 1)
    {
        return false;
    }

    const char c = static_cast<char>(translated & 0xFF);
    if (c < 32 || c > 126)
    {
        return false;
    }
    *outChar = c;
    return true;
}

bool TryReadClipboardAsciiText(HWND owner, std::string* outText)
{
    if (outText == nullptr || OpenClipboard(owner) == FALSE)
    {
        return false;
    }

    std::string clipboardText;

    HANDLE textHandle = GetClipboardData(CF_TEXT);
    if (textHandle != nullptr)
    {
        const char* text = reinterpret_cast<const char*>(GlobalLock(textHandle));
        if (text != nullptr)
        {
            clipboardText = text;
            GlobalUnlock(textHandle);
        }
    }

    if (clipboardText.empty())
    {
        HANDLE unicodeHandle = GetClipboardData(CF_UNICODETEXT);
        if (unicodeHandle != nullptr)
        {
            const wchar_t* text = reinterpret_cast<const wchar_t*>(GlobalLock(unicodeHandle));
            if (text != nullptr)
            {
                const int needed = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 1)
                {
                    std::vector<char> converted(static_cast<size_t>(needed));
                    if (WideCharToMultiByte(CP_ACP, 0, text, -1, converted.data(), needed, nullptr, nullptr) > 0)
                    {
                        clipboardText.assign(converted.data());
                    }
                }
                GlobalUnlock(unicodeHandle);
            }
        }
    }

    CloseClipboard();
    *outText = clipboardText;
    return !clipboardText.empty();
}
}



