#include "hotkeys.h"
static SpKeyboard *current;
static LRESULT CALLBACK keyboard_proc(int code,WPARAM wp,LPARAM lp) {
    if (code<0 || !current) return CallNextHookEx(NULL,code,wp,lp);
    const KBDLLHOOKSTRUCT *event=(const KBDLLHOOKSTRUCT *)lp;
    bool down=wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN;
    bool up=wp==WM_KEYUP||wp==WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(NULL,code,wp,lp);
    unsigned scan=(event->scanCode&0xff)|((event->flags&LLKHF_EXTENDED)?0x100:0);
    SpKeyDecision decision=sp_key_gate_event(&current->gate,&current->keys,current->profile,event->vkCode,scan,
        down,(event->flags&LLKHF_INJECTED)!=0);
    if (decision.binding!=SIZE_MAX) {
        HWND active=GetForegroundWindow();
        if (!PostMessageW(current->target,current->message,(WPARAM)decision.binding,(LPARAM)active)) {
            unsigned slot=scan && scan<512?scan:512+event->vkCode;
            current->keys.swallowed[slot]=false; return CallNextHookEx(NULL,code,wp,lp);
        }
        /* Mask Start/Alt menu activation after consuming the primary key.
           VK E8 is unassigned. These injected mask events never enter our matcher. */
        unsigned mods=sp_key_modifiers(&current->keys);
        if (mods&(SP_MOD_WIN|SP_MOD_ALT)) {
            INPUT mask[2]={{0},{0}};
            mask[0].type=mask[1].type=INPUT_KEYBOARD;
            mask[0].ki.wVk=mask[1].ki.wVk=0xe8;
            mask[1].ki.dwFlags=KEYEVENTF_KEYUP;
            (void)SendInput(2,mask,sizeof(INPUT));
        }
    }
    return decision.suppress?1:CallNextHookEx(NULL,code,wp,lp);
}
static void poll_neutral(SpKeyboard *k) {
    if (k->gate.armed) return;
    uint64_t epoch=k->gate.epoch;
    bool all_up=true;
    /* Never seed VK latches. The hook is installed before these outside-callback
       checks; reentrant physical events invalidate this poll via the epoch. */
    for (int vk=1;vk<256;++vk)
        if (GetAsyncKeyState(vk)&0x8000) all_up=false;
    (void)sp_key_gate_poll(&k->gate,&k->keys,epoch,all_up);
}
static DWORD WINAPI keyboard_thread(void *ctx) {
    SpKeyboard *k=ctx; MSG msg;
    /* Cancellation exists before CreateThread: stopping never depends on a queue. */
    if (WaitForSingleObject(k->cancel,0)==WAIT_OBJECT_0) {
        k->error=ERROR_CANCELLED; SetEvent(k->ready); return 0;
    }
    PeekMessageW(&msg,NULL,0,0,PM_NOREMOVE);

    if (WaitForSingleObject(k->cancel,0)==WAIT_OBJECT_0) {
        k->error=ERROR_CANCELLED; SetEvent(k->ready); return 0;
    }
    current=k;
    HHOOK hook=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard_proc,GetModuleHandleW(NULL),0);
    k->error=hook?0:GetLastError(); SetEvent(k->ready);
    if (hook) {
        poll_neutral(k);
        for (;;) {
            DWORD wait=MsgWaitForMultipleObjects(1,&k->cancel,FALSE,k->gate.armed?INFINITE:25,QS_ALLINPUT);
            if (wait!=WAIT_OBJECT_0+1 && wait!=WAIT_TIMEOUT) break;
            while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
                if (msg.message==WM_QUIT || WaitForSingleObject(k->cancel,0)==WAIT_OBJECT_0)
                    goto finished;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            poll_neutral(k);
        }
finished:
        UnhookWindowsHookEx(hook);
    }
    current=NULL; return 0;
}
bool sp_keyboard_start(SpKeyboard *k,HWND target,UINT message,const SpProfile *profile) {
    ZeroMemory(k,sizeof(*k)); k->target=target; k->message=message; k->profile=profile;
    k->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!k->ready) { k->error=GetLastError(); return false; }
    k->cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!k->cancel) { k->error=GetLastError(); sp_keyboard_stop(k); return false; }
    k->thread=CreateThread(NULL,0,keyboard_thread,k,0,&k->thread_id);
    if (!k->thread) { k->error=GetLastError(); sp_keyboard_stop(k); return false; }
    DWORD wait=WaitForSingleObject(k->ready,3000);
    DWORD error=wait==WAIT_OBJECT_0?k->error:wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
    if (error) { sp_keyboard_stop(k); k->error=error; return false; }
    return true;
}
void sp_keyboard_stop(SpKeyboard *k) {
    if (k->thread) {
        SetEvent(k->cancel);
        /* Joining is mandatory before the caller may release k or its profile. */
        WaitForSingleObject(k->thread,INFINITE);
        CloseHandle(k->thread); k->thread=NULL;
    }
    if (k->ready) { CloseHandle(k->ready); k->ready=NULL; }
    if (k->cancel) { CloseHandle(k->cancel); k->cancel=NULL; }
}
