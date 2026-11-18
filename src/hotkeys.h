#ifndef SP_HOTKEYS_H
#define SP_HOTKEYS_H
#include "win32.h"
#include "input.h"
typedef struct {
    HANDLE thread,ready,cancel;
    DWORD thread_id,error;
    HWND target;
    UINT message;
    const SpProfile *profile;
    SpKeyState keys;
    SpKeyGate gate;
} SpKeyboard;
bool sp_keyboard_start(SpKeyboard *keyboard,HWND target,UINT message,const SpProfile *profile);
void sp_keyboard_stop(SpKeyboard *keyboard);
#endif
