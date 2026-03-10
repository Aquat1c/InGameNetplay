#pragma once

#include <cstddef>
#include <cstdint>

namespace igcr::input {

struct DIKeyNameEntry {
    uint8_t code;
    const char* name;
};

// Adapted from efz-training-mode keyboard mapping table for consistent DIK naming.
inline constexpr DIKeyNameEntry kDIKeyNameTable[] = {
    {0x01, "ESC"},
    {0x02, "1"},
    {0x03, "2"},
    {0x04, "3"},
    {0x05, "4"},
    {0x06, "5"},
    {0x07, "6"},
    {0x08, "7"},
    {0x09, "8"},
    {0x0A, "9"},
    {0x0B, "0"},
    {0x0C, "MINUS"},
    {0x0D, "EQUALS"},
    {0x0E, "BACKSPACE"},
    {0x0F, "TAB"},
    {0x10, "Q"},
    {0x11, "W"},
    {0x12, "E"},
    {0x13, "R"},
    {0x14, "T"},
    {0x15, "Y"},
    {0x16, "U"},
    {0x17, "I"},
    {0x18, "O"},
    {0x19, "P"},
    {0x1A, "LBRACKET"},
    {0x1B, "RBRACKET"},
    {0x1C, "ENTER"},
    {0x1D, "LCTRL"},
    {0x1E, "A"},
    {0x1F, "S"},
    {0x20, "D"},
    {0x21, "F"},
    {0x22, "G"},
    {0x23, "H"},
    {0x24, "J"},
    {0x25, "K"},
    {0x26, "L"},
    {0x27, "SEMICOLON"},
    {0x28, "APOSTROPHE"},
    {0x29, "GRAVE"},
    {0x2A, "LSHIFT"},
    {0x2B, "BACKSLASH"},
    {0x2C, "Z"},
    {0x2D, "X"},
    {0x2E, "C"},
    {0x2F, "V"},
    {0x30, "B"},
    {0x31, "N"},
    {0x32, "M"},
    {0x33, "COMMA"},
    {0x34, "PERIOD"},
    {0x35, "SLASH"},
    {0x36, "RSHIFT"},
    {0x37, "NUM*"},
    {0x38, "LALT"},
    {0x39, "SPACE"},
    {0x3A, "CAPSLOCK"},
    {0x3B, "F1"},
    {0x3C, "F2"},
    {0x3D, "F3"},
    {0x3E, "F4"},
    {0x3F, "F5"},
    {0x40, "F6"},
    {0x41, "F7"},
    {0x42, "F8"},
    {0x43, "F9"},
    {0x44, "F10"},
    {0x45, "NUMLOCK"},
    {0x46, "SCROLLLOCK"},
    {0x47, "NUM7"},
    {0x48, "NUM8"},
    {0x49, "NUM9"},
    {0x4A, "NUM-"},
    {0x4B, "NUM4"},
    {0x4C, "NUM5"},
    {0x4D, "NUM6"},
    {0x4E, "NUM+"},
    {0x4F, "NUM1"},
    {0x50, "NUM2"},
    {0x51, "NUM3"},
    {0x52, "NUM0"},
    {0x53, "NUM."},
    {0x56, "OEM102"},
    {0x57, "F11"},
    {0x58, "F12"},
    {0x64, "F13"},
    {0x65, "F14"},
    {0x66, "F15"},
    {0x70, "KANA"},
    {0x73, "ABNT_C1"},
    {0x79, "CONVERT"},
    {0x7B, "NOCONVERT"},
    {0x7D, "YEN"},
    {0x7E, "ABNT_C2"},
    {0x8D, "NUM="},
    {0x90, "PREVTRACK"},
    {0x91, "AT"},
    {0x92, "COLON"},
    {0x93, "UNDERLINE"},
    {0x94, "KANJI"},
    {0x95, "STOP"},
    {0x96, "AX"},
    {0x97, "UNLABELED"},
    {0x99, "NEXTTRACK"},
    {0x9C, "NUMENTER"},
    {0x9D, "RCTRL"},
    {0xA0, "MUTE"},
    {0xA1, "CALC"},
    {0xA2, "PLAYPAUSE"},
    {0xA4, "MEDIASTOP"},
    {0xAE, "VOLDOWN"},
    {0xB0, "VOLUP"},
    {0xB2, "WEBHOME"},
    {0xB3, "NUMCOMMA"},
    {0xB5, "NUM/"},
    {0xB7, "SYSRQ"},
    {0xB8, "RALT"},
    {0xC5, "PAUSE"},
    {0xC7, "HOME"},
    {0xC8, "UP"},
    {0xC9, "PGUP"},
    {0xCB, "LEFT"},
    {0xCD, "RIGHT"},
    {0xCF, "END"},
    {0xD0, "DOWN"},
    {0xD1, "PGDN"},
    {0xD2, "INS"},
    {0xD3, "DEL"},
    {0xDB, "LWIN"},
    {0xDC, "RWIN"},
    {0xDD, "APPS"},
    {0xDE, "POWER"},
    {0xDF, "SLEEP"},
    {0xE3, "WAKE"},
    {0xE5, "WEBSEARCH"},
    {0xE6, "WEBFAV"},
    {0xE7, "WEBREFRESH"},
    {0xE8, "WEBSTOP"},
    {0xE9, "WEBFORWARD"},
    {0xEA, "WEBBACK"},
    {0xEB, "MYCOMPUTER"},
    {0xEC, "MAIL"},
    {0xED, "MEDIASELECT"},
};

inline const char* LookupDIKeyName(const uint8_t code) {
    for (const auto& entry : kDIKeyNameTable) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return nullptr;
}

}  // namespace igcr::input


/* -------------------------------------------------------------------------------------------------------------------------------
; *                                                   APPENDIX: SCAN KEY CODES
; */
;/*
; * Pad buttons take the form BUTTON_1 up to BUTTON_32 on the pad used by player 1.
; * See the game controller settings in the windows control panel for the button numbers.
; */

;/*
; * Keyboard Scan Key Reference.
; */
;DIK_ESCAPE
;DIK_1
;DIK_2
;DIK_3
;DIK_4
;DIK_5
;DIK_6
;DIK_7
;DIK_8
;DIK_9
;DIK_0
;DIK_MINUS          /* - on main keyboard */
;DIK_EQUALS
;DIK_BACK           /* backspace */
;DIK_TAB
;DIK_Q
;DIK_W
;DIK_E
;DIK_R
;DIK_T
;DIK_Y
;DIK_U
;DIK_I
;DIK_O
;DIK_P
;DIK_LBRACKET
;DIK_RBRACKET
;DIK_RETURN         /* Enter on main keyboard */
;DIK_LCONTROL
;DIK_A
;DIK_S
;DIK_D
;DIK_F
;DIK_G
;DIK_H
;DIK_J
;DIK_K
;DIK_L
;DIK_SEMICOLON
;DIK_APOSTROPHE
;DIK_GRAVE          /* accent grave */
;DIK_LSHIFT
;DIK_BACKSLASH
;DIK_Z
;DIK_X
;DIK_C
;DIK_V
;DIK_B
;DIK_N
;DIK_M
;DIK_COMMA
;DIK_PERIOD         /* . on main keyboard */
;DIK_SLASH          /* / on main keyboard */
;DIK_RSHIFT
;DIK_MULTIPLY       /* * on numeric keypad */
;DIK_LMENU          /* left Alt */
;DIK_SPACE
;DIK_CAPITAL
;DIK_F1
;DIK_F2
;DIK_F3
;DIK_F4
;DIK_F5
;DIK_F6
;DIK_F7
;DIK_F8
;DIK_F9
;DIK_F10
;DIK_NUMLOCK
;DIK_SCROLL         /* Scroll Lock */
;DIK_NUMPAD7
;DIK_NUMPAD8
;DIK_NUMPAD9
;DIK_SUBTRACT       /* - on numeric keypad */
;DIK_NUMPAD4
;DIK_NUMPAD5
;DIK_NUMPAD6
;DIK_ADD            /* + on numeric keypad */
;DIK_NUMPAD1
;DIK_NUMPAD2
;DIK_NUMPAD3
;DIK_NUMPAD0
;DIK_DECIMAL        /* . on numeric keypad */
;DIK_OEM_102        /* <> or \| on RT 102-key keyboard (Non-U.S.) */
;DIK_F11
;DIK_F12
;DIK_F13            /* (NEC PC98) */
;DIK_F14            /* (NEC PC98) */
;DIK_F15            /* (NEC PC98) */
;DIK_KANA           /* (Japanese keyboard) */
;DIK_ABNT_C1        /* /? on Brazilian keyboard */
;DIK_CONVERT        /* (Japanese keyboard) */
;DIK_NOCONVERT      /* (Japanese keyboard) */
;DIK_YEN            /* (Japanese keyboard) */
;DIK_ABNT_C2        /* Numpad . on Brazilian keyboard */
;DIK_NUMPADEQUALS   /* = on numeric keypad (NEC PC98) */
;DIK_PREVTRACK      /* Previous Track (DIK_CIRCUMFLEX on Japanese keyboard) */
;DIK_AT             /* (NEC PC98) */
;DIK_COLON          /* (NEC PC98) */
;DIK_UNDERLINE      /* (NEC PC98) */
;DIK_KANJI          /* (Japanese keyboard) */
;DIK_STOP           /* (NEC PC98) */
;DIK_AX             /* (Japan AX) */
;DIK_UNLABELED      /* (J3100) */
;DIK_NEXTTRACK      /* Next Track */
;DIK_NUMPADENTER    /* Enter on numeric keypad */
;DIK_RCONTROL
;DIK_MUTE           /* Mute */
;DIK_CALCULATOR     /* Calculator */
;DIK_PLAYPAUSE      /* Play / Pause */
;DIK_MEDIASTOP      /* Media Stop */
;DIK_VOLUMEDOWN     /* Volume - */
;DIK_VOLUMEUP       /* Volume + */
;DIK_WEBHOME        /* Web home */
;DIK_NUMPADCOMMA    /* , on numeric keypad (NEC PC98) */
;DIK_DIVIDE         /* / on numeric keypad */
;DIK_SYSRQ
;DIK_RMENU          /* right Alt */
;DIK_PAUSE          /* Pause */
;DIK_HOME           /* Home on arrow keypad */
;DIK_UP             /* UpArrow on arrow keypad */
;DIK_PRIOR          /* PgUp on arrow keypad */
;DIK_LEFT           /* LeftArrow on arrow keypad */
;DIK_RIGHT          /* RightArrow on arrow keypad */
;DIK_END            /* End on arrow keypad */
;DIK_DOWN           /* DownArrow on arrow keypad */
;DIK_NEXT           /* PgDn on arrow keypad */
;DIK_INSERT         /* Insert on arrow keypad */
;DIK_DELETE         /* Delete on arrow keypad */
;DIK_LWIN           /* Left Windows key */
;DIK_RWIN           /* Right Windows key */
;DIK_APPS           /* AppMenu key */
;DIK_POWER          /* System Power */
;DIK_SLEEP          /* System Sleep */
;DIK_WAKE           /* System Wake */
;DIK_WEBSEARCH      /* Web Search */
;DIK_WEBFAVORITES   /* Web Favorites */
;DIK_WEBREFRESH     /* Web Refresh */
;DIK_WEBSTOP        /* Web Stop */
;DIK_WEBFORWARD     /* Web Forward */
;DIK_WEBBACK        /* Web Back */
;DIK_MYCOMPUTER     /* My Computer */
;DIK_MAIL           /* Mail */
;DIK_MEDIASELECT    /* Media Select */
;/*
; *  Alternate names for keys, to facilitate transition from DOS.
; */
;DIK_BACK            /* backspace */
;DIK_MULTIPLY        /* * on numeric keypad */
;DIK_LMENU           /* left Alt */
;DIK_CAPITAL         /* CapsLock */
;DIK_SUBTRACT        /* - on numeric keypad */
;DIK_ADD             /* + on numeric keypad */
;DIK_DECIMAL         /* . on numeric keypad */
;DIK_DIVIDE          /* / on numeric keypad */
;DIK_RMENU           /* right Alt */
;DIK_UP              /* UpArrow on arrow keypad */
;DIK_PRIOR           /* PgUp on arrow keypad */
;DIK_LEFT            /* LeftArrow on arrow keypad */
;DIK_RIGHT           /* RightArrow on arrow keypad */
;DIK_DOWN            /* DownArrow on arrow keypad */
;DIK_NEXT            /* PgDn on arrow keypad */
;/*
; * Names for keys originally not used on US keyboards.
; */
;DIK_PREVTRACK       /* Japanese keyboard */ 
