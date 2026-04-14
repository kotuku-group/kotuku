#pragma once

// keymaptable.h
// (C) Copyright 2001-2022 Paul Manias

#include <kotuku/system/keys.h>

// Key mapping translation table.  This is used to get the numeric value of keys referred to in the keymap
// configuration files.  Because this is a lookup table, it must be arranged in the same order as that specified in
// the system/keyboard.h file.

static const char * const glKeymapTable[K_LIST_END+1] = {
   "",
   "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
   "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
   "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
   "REVERSEQUOTE", "MINUS", "EQUALS", "LSQUARE", "RSQUARE", "SEMICOLON",
   "QUOTE", "COMMA", "PERIOD", "SLASH", "BACKSLASH", "SPACE",
   "NP0", "NP1", "NP2", "NP3", "NP4", "NP5", "NP6", "NP7", "NP8", "NP9",
   "NPMULTIPLY", "NPPLUS", "NPBAR", "NPMINUS", "NPDOT", "NPDIVIDE",
   "LCONTROL", "RCONTROL", "HELP", "LSHIFT", "RSHIFT", "CAPSLOCK", "PRINT",
   "LALT", "RALT", "LCOMMAND", "RCOMMAND", "F1", "F2", "F3", "F4", "F5", "F6",
   "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16", "F17",
   "MACRO", "NPPLUSMINUS", "LESSGREATER", "UP", "DOWN", "RIGHT", "LEFT", "SCROLL", "PAUSE",
   "WAKE", "SLEEP", "POWER", "BACKSPACE", "TAB", "ENTER", "ESCAPE", "DELETE",
   "CLEAR", "HOME", "PAGEUP", "PAGEDOWN", "END", "SELECT", "EXECUTE",
   "INSERT", "UNDO", "REDO", "MENU", "FIND", "CANCEL", "BREAK", "NUMLOCK",
   "PRTSCR", "NPENTER", "SYSRQ", "F18", "F19", "F20",
   "WINCONTROL", "VOLUMEUP", "VOLUMEDOWN", "BACK",
   "CALL", "ENDCALL", "CAMERA", "AT",
   "PLUS", "LENSFOCUS", "STOP", "NEXT",
   "PREVIOUS", "FORWARD", "REWIND", "MUTE",
   "STAR", "POUND", "PLAY",
   0
};

const static char glCharTable[K_LIST_END+1] = {
   0,
   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
   'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
   '`', '-', '=', '[', ']', ';',
   '\'', ',', '.', '/', '\\', ' ',
   '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
   '*', '+', '-', '-', '.', '/',
   '.', '.', '.', '.', '.', '.', '.',
   '.', '.', '.', '.', '.', '.', '.', '.', '.', '.',
   '.', '.', '.', '.', '.', '.', '.', '.', '.', '.', '.',
   '.', '+', '<', '.', '.', '.', '.', '.', '.',
   '.', '.', '.', '.', '\t', '\n', '\e', '.',
   '.', '.', '.', '.', '.', '.', '.',
   '.', '.', '.', '.', '.', '.', '.', '.',
   '.', '\n', '.', '.', '.', '.',
   0
};
