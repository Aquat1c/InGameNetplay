#include "netplay/core/inline_edit.h"

#include "logger.h"
#include "netplay/core/constants.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/menu_model.h"
#include "netplay/core/text_utils.h"
#include "netplay/core/validation.h"

#include <array>
#include <cctype>

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
        return std::isalnum(uc) != 0 || c == '.' || c == ':' || c == '-' || c == '_';
    case NetplayMenuAction::NicknameEdit:
        return c >= 32 && c <= 126;
    default:
        return false;
    }
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

    state->buffer.push_back(c);
    state->caretVisible = true;
    state->lastCaretTick = GetTickCount();
    ClearError(state);
}

void TryPasteText(netplay::inline_edit::State* state, HWND owner)
{
    if (state == nullptr || !state->active)
    {
        return;
    }

    std::string clipboardText;
    if (!netplay::input::TryReadClipboardAsciiText(owner, &clipboardText))
    {
        return;
    }

    for (char c : clipboardText)
    {
        TryAppendChar(state, c);
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
        if (!netplay::validation::IsValidJoinAddress(value))
        {
            SetError(state, "INVALID ADDRESS");
            mod::Log("InlineEdit: invalid join address '%s'", value.c_str());
            return false;
        }
        values->joinAddress = value;
        break;
    case NetplayMenuAction::NicknameEdit:
        if (!netplay::validation::IsValidNickname(value))
        {
            SetError(state, "INVALID NAME");
            mod::Log("InlineEdit: invalid nickname '%s'", value.c_str());
            return false;
        }
        values->nickname = value;
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
            outValue->push_back('_');
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
    if (ConsumeKeyEdge(state, VK_BACK) || ConsumeKeyEdge(state, VK_DELETE))
    {
        if (!state->buffer.empty())
        {
            state->buffer.pop_back();
            state->caretVisible = true;
            state->lastCaretTick = GetTickCount();
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

    if (changed)
    {
        mod::Log("InlineEdit: action=%s buffer='%s'", MenuActionToString(state->action), state->buffer.c_str());
    }
    return InputResult::Consumed;
}
}


