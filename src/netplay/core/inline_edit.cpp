#include "netplay/core/inline_edit.h"

#include "logger.h"
#include "netplay/core/constants.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/menu_model.h"
#include "netplay/core/network_endpoint.h"
#include "netplay/core/text_utils.h"
#include "netplay/core/validation.h"

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace
{
using namespace netplay::constants;
using netplay::menu::MenuActionToString;
using netplay::menu::NetplayMenuAction;

size_t GetMaxLength(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
        return kInlineEditMaxPortLength;
    case NetplayMenuAction::JoinEditAddress:
        return kInlineEditMaxJoinAddressLength;
    case NetplayMenuAction::PlayerRoomsEditCode:
        return kInlineEditMaxRoomCodeLength;
    case NetplayMenuAction::NicknameEdit:
        return kInlineEditMaxNicknameLength;
    default:
        return 0;
    }
}

bool IsCharacterAllowed(NetplayMenuAction action, char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
        return c >= '0' && c <= '9';
    case NetplayMenuAction::JoinEditAddress:
        return std::isalnum(uc) != 0
            || c == '.'
            || c == ':'
            || c == '-'
            || c == '_'
            || c == '%'
            || c == '['
            || c == ']';
    case NetplayMenuAction::PlayerRoomsEditCode:
        return std::isalnum(uc) != 0 || c == '.' || c == '-' || c == '_';
    case NetplayMenuAction::NicknameEdit:
        // Allow printable ASCII (32-126) and non-ASCII bytes (UTF-8 lead/continuation)
        return (c >= 32 && c <= 126) || uc >= 0x80u;
    default:
        return false;
    }
}

// Erase the last UTF-8 codepoint from a string.
void EraseLastUtf8Codepoint(std::string& s)
{
    if (s.empty())
    {
        return;
    }

    size_t i = s.size();
    // Skip continuation bytes (10xxxxxx)
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0u) == 0x80u)
    {
        --i;
    }
    // Remove the lead byte
    if (i > 0)
    {
        --i;
    }
    s.erase(i);
}

size_t ClampCaretOffset(const std::string& text, size_t offset)
{
    if (offset >= text.size())
    {
        return text.size();
    }
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        --offset;
    }
    return offset;
}

size_t PrevUtf8Boundary(const std::string& text, size_t offset)
{
    offset = ClampCaretOffset(text, offset);
    if (offset == 0)
    {
        return 0;
    }
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        --offset;
    }
    return offset;
}

size_t NextUtf8Boundary(const std::string& text, size_t offset)
{
    offset = ClampCaretOffset(text, offset);
    if (offset >= text.size())
    {
        return text.size();
    }
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        ++offset;
    }
    return offset;
}

void TouchCaret(netplay::inline_edit::State* state)
{
    if (state == nullptr)
    {
        return;
    }
    state->caretVisible = true;
    state->lastCaretTick = GetTickCount();
}

void InsertBytesAtCaret(netplay::inline_edit::State* state, const std::string& bytes)
{
    if (state == nullptr || bytes.empty())
    {
        return;
    }
    const size_t caret = ClampCaretOffset(state->buffer, state->caretByteOffset);
    state->buffer.insert(caret, bytes);
    state->caretByteOffset = caret + bytes.size();
    TouchCaret(state);
}

bool EraseCodepointBeforeCaret(netplay::inline_edit::State* state)
{
    if (state == nullptr)
    {
        return false;
    }
    const size_t caret = ClampCaretOffset(state->buffer, state->caretByteOffset);
    if (caret == 0)
    {
        return false;
    }
    const size_t start = PrevUtf8Boundary(state->buffer, caret);
    state->buffer.erase(start, caret - start);
    state->caretByteOffset = start;
    TouchCaret(state);
    return true;
}

bool EraseCodepointAtCaret(netplay::inline_edit::State* state)
{
    if (state == nullptr)
    {
        return false;
    }
    const size_t caret = ClampCaretOffset(state->buffer, state->caretByteOffset);
    if (caret >= state->buffer.size())
    {
        return false;
    }
    const size_t end = NextUtf8Boundary(state->buffer, caret);
    state->buffer.erase(caret, end - caret);
    state->caretByteOffset = caret;
    TouchCaret(state);
    return true;
}

// Append a UTF-8 string to the edit buffer, respecting per-byte and per-action checks.
void TryAppendUtf8String(netplay::inline_edit::State* state, const std::string& utf8)
{
    if (state == nullptr || !state->active || utf8.empty())
    {
        return;
    }

    const size_t maxLength = GetMaxLength(state->action);
    for (char c : utf8)
    {
        if (!IsCharacterAllowed(state->action, c))
        {
            continue;
        }
        if (maxLength > 0 && state->buffer.size() >= maxLength)
        {
            break;
        }
        InsertBytesAtCaret(state, std::string(1, c));
    }
}

// Convert a wchar_t (from WM_CHAR) to UTF-8.
static std::string WcharToUtf8(wchar_t ch)
{
    if (ch == 0)
    {
        return {};
    }

    wchar_t buf[2] = {ch, L'\0'};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buf, 1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        return {};
    }
    std::vector<char> out(static_cast<size_t>(needed));
    if (WideCharToMultiByte(CP_UTF8, 0, buf, 1, out.data(), needed, nullptr, nullptr) <= 0)
    {
        return {};
    }
    return std::string(out.data(), static_cast<size_t>(needed));
}

// Drain WM_CHAR and WM_IME_CHAR messages from the thread message queue and append them as UTF-8.
// Returns true if any characters were consumed.
static bool DrainWmCharMessages(netplay::inline_edit::State* state)
{
    if (state == nullptr || !state->active)
    {
        return false;
    }

    bool consumed = false;
    MSG msg = {};

    // Drain WM_CHAR messages (generated by TranslateMessage / IME).
    // Only consume non-ASCII characters to avoid duplicating ASCII keys
    // already handled by the VK polling path.
    while (PeekMessageW(&msg, nullptr, WM_CHAR, WM_CHAR, PM_REMOVE))
    {
        const wchar_t ch = static_cast<wchar_t>(msg.wParam);
        if (ch < 128)
        {
            continue; // ASCII range is handled by VK polling
        }
        const std::string utf8 = WcharToUtf8(ch);
        if (!utf8.empty())
        {
            TryAppendUtf8String(state, utf8);
            consumed = true;
        }
    }

    // Also drain WM_IME_CHAR which is sent by some IMEs.
    while (PeekMessageW(&msg, nullptr, WM_IME_CHAR, WM_IME_CHAR, PM_REMOVE))
    {
        const wchar_t ch = static_cast<wchar_t>(msg.wParam);
        if (ch < 128)
        {
            continue;
        }
        const std::string utf8 = WcharToUtf8(ch);
        if (!utf8.empty())
        {
            TryAppendUtf8String(state, utf8);
            consumed = true;
        }
    }

    return consumed;
}

bool GetCommittedValue(NetplayMenuAction action, const netplay::inline_edit::Values& values, std::string* outValue)
{
    if (outValue == nullptr)
    {
        return false;
    }

    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
        *outValue = std::to_string(static_cast<unsigned>(values.hostPort));
        return true;
    case NetplayMenuAction::JoinEditAddress:
        *outValue = values.joinAddress;
        return true;
    case NetplayMenuAction::JoinEditPort:
        *outValue = std::to_string(static_cast<unsigned>(values.joinPort));
        return true;
    case NetplayMenuAction::NicknameEdit:
        *outValue = values.nickname;
        return true;
    case NetplayMenuAction::PlayerRoomsEditCode:
        *outValue = values.playerRoomsRoomCode;
        return true;
    default:
        return false;
    }
}

void PrimeKeyState(netplay::inline_edit::State* state)
{
    if (state == nullptr)
    {
        return;
    }
    netplay::input::PrimeKeyState(&state->keyDown);
}

void ClearError(netplay::inline_edit::State* state)
{
    if (state == nullptr)
    {
        return;
    }
    state->errorMessage.clear();
    state->errorExpireTick = 0;
}

void SetError(netplay::inline_edit::State* state, const char* error)
{
    if (state == nullptr)
    {
        return;
    }
    state->errorMessage = (error != nullptr) ? error : "";
    state->errorExpireTick = GetTickCount() + kInlineEditErrorDisplayMs;
}

void UpdateCaretBlink(netplay::inline_edit::State* state)
{
    if (state == nullptr || !state->active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (state->lastCaretTick == 0)
    {
        state->lastCaretTick = now;
        state->caretVisible = true;
        return;
    }

    if (now - state->lastCaretTick >= kInlineEditCaretBlinkMs)
    {
        state->lastCaretTick = now;
        state->caretVisible = !state->caretVisible;
    }

    if (!state->errorMessage.empty() && now >= state->errorExpireTick)
    {
        ClearError(state);
    }
}

bool ConsumeKeyEdge(netplay::inline_edit::State* state, int virtualKey)
{
    if (state == nullptr)
    {
        return false;
    }
    return netplay::input::ConsumeKeyEdge(&state->keyDown, virtualKey);
}

void TryAppendChar(netplay::inline_edit::State* state, char c)
{
    if (state == nullptr || !state->active)
    {
        return;
    }

    if (!IsCharacterAllowed(state->action, c))
    {
        return;
    }

    const size_t maxLength = GetMaxLength(state->action);
    if (maxLength == 0 || state->buffer.size() >= maxLength)
    {
        return;
    }

    InsertBytesAtCaret(state, std::string(1, c));
    ClearError(state);
}

void TryPasteText(netplay::inline_edit::State* state, HWND owner)
{
    if (state == nullptr || !state->active)
    {
        return;
    }

    std::string clipboardText;
    if (state->action == NetplayMenuAction::NicknameEdit)
    {
        if (!netplay::input::TryReadClipboardUtf8Text(owner, &clipboardText))
        {
            return;
        }
        TryAppendUtf8String(state, clipboardText);
    }
    else
    {
        if (!netplay::input::TryReadClipboardAsciiText(owner, &clipboardText))
        {
            return;
        }
        for (char c : clipboardText)
        {
            TryAppendChar(state, c);
        }
    }
}

bool Commit(netplay::inline_edit::State* state, netplay::inline_edit::Values* values)
{
    if (state == nullptr || values == nullptr || !state->active)
    {
        return false;
    }

    std::string value = netplay::text::TrimAscii(state->buffer);

    switch (state->action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
    {
        uint16_t parsedPort = 0;
        if (!netplay::validation::ParsePort(value, &parsedPort))
        {
            SetError(state, "INVALID PORT");
            mod::Log("InlineEdit: invalid port '%s'", value.c_str());
            return false;
        }

        if (state->action == NetplayMenuAction::HostEditPort)
        {
            values->hostPort = parsedPort;
        }
        else
        {
            values->joinPort = parsedPort;
        }
        break;
    }
    case NetplayMenuAction::JoinEditAddress:
    {
        netplay::network::RemoteHostInput parsedInput;
        if (!netplay::network::ParseRemoteHostInput(
                value,
                &parsedInput))
        {
            SetError(state, "INVALID ADDRESS");
            mod::Log("InlineEdit: invalid join address '%s'", value.c_str());
            return false;
        }
        value = parsedInput.host;
        values->joinAddress = value;
        break;
    }
    case NetplayMenuAction::NicknameEdit:
        if (!netplay::validation::IsValidNickname(value))
        {
            SetError(state, "INVALID NAME");
            mod::Log("InlineEdit: invalid nickname '%s'", value.c_str());
            return false;
        }
        values->nickname = value;
        break;
    case NetplayMenuAction::PlayerRoomsEditCode:
        if (!netplay::validation::IsValidLobbyRoomCode(value))
        {
            SetError(state, "INVALID ROOM CODE");
            mod::Log("InlineEdit: invalid room code '%s'", value.c_str());
            return false;
        }
        values->playerRoomsRoomCode = value;
        break;
    default:
        return false;
    }

    mod::Log("InlineEdit: commit action=%s value='%s'", MenuActionToString(state->action), value.c_str());
    netplay::inline_edit::ResetState(state);
    return true;
}
}

namespace netplay::inline_edit
{
bool IsInlineEditableAction(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditAddress:
    case NetplayMenuAction::JoinEditPort:
    case NetplayMenuAction::NicknameEdit:
    case NetplayMenuAction::PlayerRoomsEditCode:
        return true;
    default:
        return false;
    }
}

void ResetState(State* state)
{
    if (state == nullptr)
    {
        return;
    }
    *state = {};
}

void BeginEdit(State* state, NetplayMenuAction action, const Values& values)
{
    if (state == nullptr || !IsInlineEditableAction(action))
    {
        return;
    }

    std::string initialValue;
    (void)GetCommittedValue(action, values, &initialValue);

    state->active = true;
    state->action = action;
    state->buffer = initialValue;
    state->caretByteOffset = state->buffer.size();
    state->caretVisible = true;
    state->lastCaretTick = GetTickCount();
    ClearError(state);
    PrimeKeyState(state);

    mod::Log("InlineEdit: begin action=%s value='%s'", MenuActionToString(action), initialValue.c_str());
}

void CancelEdit(State* state)
{
    if (state == nullptr || !state->active)
    {
        return;
    }

    mod::Log("InlineEdit: cancel action=%s", MenuActionToString(state->action));
    ResetState(state);
}

bool GetDisplayValue(
    const State& state,
    const Values& values,
    NetplayMenuAction action,
    std::string* outValue,
    bool includeCaret)
{
    if (outValue == nullptr || !GetCommittedValue(action, values, outValue))
    {
        return false;
    }

    if (state.active && state.action == action)
    {
        *outValue = state.buffer;
        if (includeCaret && state.caretVisible)
        {
            const size_t caret = ClampCaretOffset(*outValue, state.caretByteOffset);
            outValue->insert(caret, 1, '_');
        }
    }
    return true;
}

InputResult HandleInput(
    State* state,
    Values* values,
    HWND owner,
    const uint8_t* inputBytes,
    bool* escapeDown)
{
    if (state == nullptr || values == nullptr || !state->active || inputBytes == nullptr)
    {
        return InputResult::NotEditing;
    }

    if (escapeDown != nullptr)
    {
        *escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    }

    UpdateCaretBlink(state);

    const bool confirmPressed = ConsumeKeyEdge(state, VK_RETURN);
    const bool cancelPressed = ConsumeKeyEdge(state, VK_ESCAPE);

    if (cancelPressed)
    {
        CancelEdit(state);
        return InputResult::Cancelled;
    }

    if (confirmPressed)
    {
        return Commit(state, values) ? InputResult::CommitSuccess : InputResult::CommitFailed;
    }

    bool changed = false;
    if (ConsumeKeyEdge(state, VK_LEFT))
    {
        state->caretByteOffset = PrevUtf8Boundary(state->buffer, state->caretByteOffset);
        TouchCaret(state);
    }
    if (ConsumeKeyEdge(state, VK_RIGHT))
    {
        state->caretByteOffset = NextUtf8Boundary(state->buffer, state->caretByteOffset);
        TouchCaret(state);
    }
    if (ConsumeKeyEdge(state, VK_HOME))
    {
        state->caretByteOffset = 0;
        TouchCaret(state);
    }
    if (ConsumeKeyEdge(state, VK_END))
    {
        state->caretByteOffset = state->buffer.size();
        TouchCaret(state);
    }

    if (ConsumeKeyEdge(state, VK_BACK))
    {
        if (EraseCodepointBeforeCaret(state))
        {
            ClearError(state);
            changed = true;
        }
    }
    if (ConsumeKeyEdge(state, VK_DELETE))
    {
        if (EraseCodepointAtCaret(state))
        {
            ClearError(state);
            changed = true;
        }
    }

    if (ConsumeKeyEdge(state, 'V'))
    {
        if (netplay::input::IsCtrlPressed())
        {
            const size_t before = state->buffer.size();
            TryPasteText(state, owner);
            changed = changed || state->buffer.size() != before;
        }
        else
        {
            char c = 0;
            if (netplay::input::TryTranslateVirtualKeyToAscii('V', &c))
            {
                const size_t before = state->buffer.size();
                TryAppendChar(state, c);
                changed = changed || state->buffer.size() != before;
            }
        }
    }

    auto handlePrintableKey = [&](int virtualKey)
    {
        if (!ConsumeKeyEdge(state, virtualKey))
        {
            return;
        }
        char c = 0;
        if (!netplay::input::TryTranslateVirtualKeyToAscii(virtualKey, &c))
        {
            return;
        }
        const size_t before = state->buffer.size();
        TryAppendChar(state, c);
        changed = changed || state->buffer.size() != before;
    };

    for (int vk = 'A'; vk <= 'Z'; ++vk)
    {
        if (vk == 'V')
        {
            continue;
        }
        handlePrintableKey(vk);
    }
    for (int vk = '0'; vk <= '9'; ++vk)
    {
        handlePrintableKey(vk);
    }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
    {
        handlePrintableKey(vk);
    }

    constexpr std::array<int, 11> kExtraKeys = {
        VK_SPACE,
        VK_OEM_PERIOD,
        VK_OEM_MINUS,
        VK_OEM_PLUS,
        VK_OEM_1,
        VK_OEM_2,
        VK_OEM_3,
        VK_OEM_4,
        VK_OEM_5,
        VK_OEM_6,
        VK_OEM_7,
    };
    for (int vk : kExtraKeys)
    {
        handlePrintableKey(vk);
    }
    handlePrintableKey(VK_DECIMAL);

    // Drain WM_CHAR messages for IME / Unicode input (nickname edit only).
    if (state->action == NetplayMenuAction::NicknameEdit)
    {
        if (DrainWmCharMessages(state))
        {
            changed = true;
        }
    }

    if (changed)
    {
        mod::Log("InlineEdit: action=%s buffer='%s'", MenuActionToString(state->action), state->buffer.c_str());
    }
    return InputResult::Consumed;
}
}
