#include "netplay/core/options_keybinds.h"

#include "netplay/core/input_utils.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <mmsystem.h>
#include <windows.h>

namespace netplay::options::keybinds
{
namespace
{
struct BindingEntry
{
    int virtualKey = 0;
    const char* dikName = nullptr;
    const char* displayName = nullptr;
};

constexpr int kMaxPadButtons = 32;

constexpr BindingEntry kBindingTable[] = {
    {VK_ESCAPE, "DIK_ESCAPE", "Esc"},
    {'1', "DIK_1", "1"},
    {'2', "DIK_2", "2"},
    {'3', "DIK_3", "3"},
    {'4', "DIK_4", "4"},
    {'5', "DIK_5", "5"},
    {'6', "DIK_6", "6"},
    {'7', "DIK_7", "7"},
    {'8', "DIK_8", "8"},
    {'9', "DIK_9", "9"},
    {'0', "DIK_0", "0"},
    {VK_OEM_MINUS, "DIK_MINUS", "-"},
    {VK_OEM_PLUS, "DIK_EQUALS", "="},
    {VK_BACK, "DIK_BACK", "Backspace"},
    {VK_TAB, "DIK_TAB", "Tab"},
    {'Q', "DIK_Q", "Q"},
    {'W', "DIK_W", "W"},
    {'E', "DIK_E", "E"},
    {'R', "DIK_R", "R"},
    {'T', "DIK_T", "T"},
    {'Y', "DIK_Y", "Y"},
    {'U', "DIK_U", "U"},
    {'I', "DIK_I", "I"},
    {'O', "DIK_O", "O"},
    {'P', "DIK_P", "P"},
    {VK_OEM_4, "DIK_LBRACKET", "["},
    {VK_OEM_6, "DIK_RBRACKET", "]"},
    {VK_RETURN, "DIK_RETURN", "Enter"},
    {VK_LCONTROL, "DIK_LCONTROL", "Left Ctrl"},
    {'A', "DIK_A", "A"},
    {'S', "DIK_S", "S"},
    {'D', "DIK_D", "D"},
    {'F', "DIK_F", "F"},
    {'G', "DIK_G", "G"},
    {'H', "DIK_H", "H"},
    {'J', "DIK_J", "J"},
    {'K', "DIK_K", "K"},
    {'L', "DIK_L", "L"},
    {VK_OEM_1, "DIK_SEMICOLON", ";"},
    {VK_OEM_7, "DIK_APOSTROPHE", "'"},
    {VK_OEM_3, "DIK_GRAVE", "`"},
    {VK_LSHIFT, "DIK_LSHIFT", "Left Shift"},
    {VK_OEM_5, "DIK_BACKSLASH", "\\"},
    {'Z', "DIK_Z", "Z"},
    {'X', "DIK_X", "X"},
    {'C', "DIK_C", "C"},
    {'V', "DIK_V", "V"},
    {'B', "DIK_B", "B"},
    {'N', "DIK_N", "N"},
    {'M', "DIK_M", "M"},
    {VK_OEM_COMMA, "DIK_COMMA", ","},
    {VK_OEM_PERIOD, "DIK_PERIOD", "."},
    {VK_OEM_2, "DIK_SLASH", "/"},
    {VK_RSHIFT, "DIK_RSHIFT", "Right Shift"},
    {VK_MULTIPLY, "DIK_MULTIPLY", "Numpad *"},
    {VK_LMENU, "DIK_LMENU", "Left Alt"},
    {VK_SPACE, "DIK_SPACE", "Space"},
    {VK_CAPITAL, "DIK_CAPITAL", "Caps Lock"},
    {VK_F1, "DIK_F1", "F1"},
    {VK_F2, "DIK_F2", "F2"},
    {VK_F3, "DIK_F3", "F3"},
    {VK_F4, "DIK_F4", "F4"},
    {VK_F5, "DIK_F5", "F5"},
    {VK_F6, "DIK_F6", "F6"},
    {VK_F7, "DIK_F7", "F7"},
    {VK_F8, "DIK_F8", "F8"},
    {VK_F9, "DIK_F9", "F9"},
    {VK_F10, "DIK_F10", "F10"},
    {VK_NUMLOCK, "DIK_NUMLOCK", "Num Lock"},
    {VK_SCROLL, "DIK_SCROLL", "Scroll Lock"},
    {VK_NUMPAD7, "DIK_NUMPAD7", "Numpad 7"},
    {VK_NUMPAD8, "DIK_NUMPAD8", "Numpad 8"},
    {VK_NUMPAD9, "DIK_NUMPAD9", "Numpad 9"},
    {VK_SUBTRACT, "DIK_SUBTRACT", "Numpad -"},
    {VK_NUMPAD4, "DIK_NUMPAD4", "Numpad 4"},
    {VK_NUMPAD5, "DIK_NUMPAD5", "Numpad 5"},
    {VK_NUMPAD6, "DIK_NUMPAD6", "Numpad 6"},
    {VK_ADD, "DIK_ADD", "Numpad +"},
    {VK_NUMPAD1, "DIK_NUMPAD1", "Numpad 1"},
    {VK_NUMPAD2, "DIK_NUMPAD2", "Numpad 2"},
    {VK_NUMPAD3, "DIK_NUMPAD3", "Numpad 3"},
    {VK_NUMPAD0, "DIK_NUMPAD0", "Numpad 0"},
    {VK_DECIMAL, "DIK_DECIMAL", "Numpad ."},
#ifdef VK_OEM_102
    {VK_OEM_102, "DIK_OEM_102", "OEM 102"},
#endif
    {VK_F11, "DIK_F11", "F11"},
    {VK_F12, "DIK_F12", "F12"},
    {VK_F13, "DIK_F13", "F13"},
    {VK_F14, "DIK_F14", "F14"},
    {VK_F15, "DIK_F15", "F15"},
    {VK_PAUSE, "DIK_PAUSE", "Pause"},
    {VK_SNAPSHOT, "DIK_SYSRQ", "Print Screen"},
    {VK_HOME, "DIK_HOME", "Home"},
    {VK_UP, "DIK_UP", "Up"},
    {VK_PRIOR, "DIK_PRIOR", "Page Up"},
    {VK_LEFT, "DIK_LEFT", "Left"},
    {VK_RIGHT, "DIK_RIGHT", "Right"},
    {VK_END, "DIK_END", "End"},
    {VK_DOWN, "DIK_DOWN", "Down"},
    {VK_NEXT, "DIK_NEXT", "Page Down"},
    {VK_INSERT, "DIK_INSERT", "Insert"},
    {VK_DELETE, "DIK_DELETE", "Delete"},
    {VK_LWIN, "DIK_LWIN", "Left Win"},
    {VK_RWIN, "DIK_RWIN", "Right Win"},
    {VK_APPS, "DIK_APPS", "Menu"},
    {VK_SLEEP, "DIK_SLEEP", "Sleep"},
#ifdef VK_BROWSER_SEARCH
    {VK_BROWSER_SEARCH, "DIK_WEBSEARCH", "Browser Search"},
    {VK_BROWSER_FAVORITES, "DIK_WEBFAVORITES", "Browser Favorites"},
    {VK_BROWSER_REFRESH, "DIK_WEBREFRESH", "Browser Refresh"},
    {VK_BROWSER_STOP, "DIK_WEBSTOP", "Browser Stop"},
    {VK_BROWSER_FORWARD, "DIK_WEBFORWARD", "Browser Forward"},
    {VK_BROWSER_BACK, "DIK_WEBBACK", "Browser Back"},
    {VK_BROWSER_HOME, "DIK_WEBHOME", "Browser Home"},
#endif
#ifdef VK_VOLUME_MUTE
    {VK_VOLUME_MUTE, "DIK_MUTE", "Mute"},
    {VK_MEDIA_PLAY_PAUSE, "DIK_PLAYPAUSE", "Play/Pause"},
    {VK_MEDIA_STOP, "DIK_MEDIASTOP", "Media Stop"},
    {VK_VOLUME_DOWN, "DIK_VOLUMEDOWN", "Volume Down"},
    {VK_VOLUME_UP, "DIK_VOLUMEUP", "Volume Up"},
    {VK_MEDIA_NEXT_TRACK, "DIK_NEXTTRACK", "Next Track"},
    {VK_MEDIA_PREV_TRACK, "DIK_PREVTRACK", "Previous Track"},
#endif
#ifdef VK_LAUNCH_APP1
    {VK_LAUNCH_APP1, "DIK_MYCOMPUTER", "My Computer"},
    {VK_LAUNCH_APP2, "DIK_CALCULATOR", "Calculator"},
    {VK_LAUNCH_MAIL, "DIK_MAIL", "Mail"},
    {VK_LAUNCH_MEDIA_SELECT, "DIK_MEDIASELECT", "Media Select"},
#endif
    {VK_RCONTROL, "DIK_RCONTROL", "Right Ctrl"},
    {VK_RMENU, "DIK_RMENU", "Right Alt"},
};

std::string ToUpperAscii(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string PrettifyToken(std::string_view token)
{
    std::string out;
    out.reserve(token.size() + 4);
    bool newWord = true;
    for (char c : token)
    {
        if (c == '_')
        {
            if (!out.empty() && out.back() != ' ')
            {
                out.push_back(' ');
            }
            newWord = true;
            continue;
        }

        if (!newWord && std::isupper(static_cast<unsigned char>(c)) != 0)
        {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) != 0)
        {
            out.push_back(newWord
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else
        {
            out.push_back(c);
        }
        newWord = false;
    }
    return out;
}

const BindingEntry* FindByDikName(std::string_view dikName)
{
    for (const BindingEntry& entry : kBindingTable)
    {
        if (dikName == entry.dikName)
        {
            return &entry;
        }
    }
    return nullptr;
}

uint32_t ReadActivePadButtons()
{
    const UINT deviceCount = joyGetNumDevs();
    uint32_t buttons = 0;
    for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex)
    {
        JOYINFOEX info = {};
        info.dwSize = sizeof(info);
        info.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(deviceIndex, &info) == JOYERR_NOERROR)
        {
            buttons |= info.dwButtons;
        }
    }
    return buttons;
}
} // namespace

bool IsKeyboardBindingValue(const std::string& value)
{
    return StartsWith(ToUpperAscii(value), "DIK_");
}

bool IsPadBindingValue(const std::string& value)
{
    const std::string normalized = ToUpperAscii(value);
    if (!StartsWith(normalized, "BUTTON_"))
    {
        return false;
    }

    if (normalized.size() <= 7)
    {
        return false;
    }

    return std::all_of(
        normalized.begin() + 7,
        normalized.end(),
        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

bool IsBindableValue(const std::string& value)
{
    return IsKeyboardBindingValue(value) || IsPadBindingValue(value);
}

std::string NormalizeBindingValue(const std::string& value)
{
    if (!IsBindableValue(value))
    {
        return value;
    }
    return ToUpperAscii(value);
}

std::string FormatBindingValue(const std::string& value)
{
    const std::string normalized = NormalizeBindingValue(value);
    if (IsPadBindingValue(normalized))
    {
        return "Pad Button " + normalized.substr(7);
    }

    if (!IsKeyboardBindingValue(normalized))
    {
        return value;
    }

    if (const BindingEntry* entry = FindByDikName(normalized))
    {
        return entry->displayName;
    }

    return PrettifyToken(normalized.substr(4));
}

void PrimePadButtonState(std::array<uint32_t, 32>* buttonsDown)
{
    if (buttonsDown == nullptr)
    {
        return;
    }

    buttonsDown->fill(0);
    const uint32_t buttons = ReadActivePadButtons();
    for (int buttonIndex = 0; buttonIndex < static_cast<int>(buttonsDown->size()); ++buttonIndex)
    {
        (*buttonsDown)[static_cast<size_t>(buttonIndex)] = (buttons & (1u << buttonIndex)) != 0 ? 1u : 0u;
    }
}

bool TryCaptureKeyboardBinding(
    std::array<uint8_t, 256>* keyDown,
    std::string* outValue,
    std::string* outDisplayValue)
{
    if (keyDown == nullptr || outValue == nullptr || outDisplayValue == nullptr)
    {
        return false;
    }

    for (const BindingEntry& entry : kBindingTable)
    {
        if (netplay::input::ConsumeKeyEdge(keyDown, entry.virtualKey))
        {
            *outValue = entry.dikName;
            *outDisplayValue = entry.displayName;
            return true;
        }
    }

    return false;
}

bool TryCapturePadBinding(
    std::array<uint32_t, 32>* buttonsDown,
    std::string* outValue,
    std::string* outDisplayValue)
{
    if (buttonsDown == nullptr || outValue == nullptr || outDisplayValue == nullptr)
    {
        return false;
    }

    const uint32_t buttons = ReadActivePadButtons();
    const int maxButtons = (std::min)(kMaxPadButtons, static_cast<int>(buttonsDown->size()));
    for (int buttonIndex = 0; buttonIndex < maxButtons; ++buttonIndex)
    {
        const bool down = (buttons & (1u << buttonIndex)) != 0;
        const size_t stateIndex = static_cast<size_t>(buttonIndex);
        const bool pressed = down && (*buttonsDown)[stateIndex] == 0;
        (*buttonsDown)[stateIndex] = down ? 1u : 0u;
        if (!pressed)
        {
            continue;
        }

        *outValue = "BUTTON_" + std::to_string(buttonIndex + 1);
        *outDisplayValue = "Pad Button " + std::to_string(buttonIndex + 1);
        return true;
    }

    return false;
}
} // namespace netplay::options::keybinds
