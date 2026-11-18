#include "input.h"
unsigned sp_key_modifiers(const SpKeyState *k) {
    return ((k->down[0x5b]||k->down[0x5c])?SP_MOD_WIN:0)|
        ((k->down[0xa2]||k->down[0xa3])?SP_MOD_CTRL:0)|
        ((k->down[0xa0]||k->down[0xa1])?SP_MOD_SHIFT:0)|
        ((k->down[0xa4]||k->down[0xa5])?SP_MOD_ALT:0);
}
SpKeyDecision sp_key_event(SpKeyState *k,const SpProfile *p,unsigned vk,unsigned scan,bool down,bool injected) {
    SpKeyDecision d={false,SIZE_MAX};
    if (injected || vk>255) return d;
    unsigned slot=scan && scan<512?scan:512+vk;
    bool was_down=k->pressed[slot];
    k->pressed[slot]=down;
    k->down[vk]=down;
    if (k->swallowed[slot]) {
        d.suppress=true; if (!down) k->swallowed[slot]=false;
        return d;
    }
    if (!down || was_down) return d; /* Never capture an app-visible press on repeat. */
    d.binding=sp_match_binding(p,vk,scan,sp_key_modifiers(k));
    if (d.binding!=SIZE_MAX) { d.suppress=true; k->swallowed[slot]=true; }
    return d;
}
SpKeyDecision sp_key_gate_event(SpKeyGate *g,SpKeyState *k,const SpProfile *p,
        unsigned vk,unsigned scan,bool down,bool injected) {
    if (g->armed) return sp_key_event(k,p,vk,scan,down,injected);
    if (!injected) {
        ++g->epoch;
        /* A passed down must block arming even before async state catches up.
           Scan identity survives a layout/VK change while the key is held. */
        if (scan && scan<512) k->pressed[scan]=down;
        else if (vk<256) k->pressed[512+vk]=down;
    }
    return (SpKeyDecision){false,SIZE_MAX};
}
bool sp_key_gate_poll(SpKeyGate *g,SpKeyState *k,uint64_t epoch,bool all_up) {
    if (g->armed) return true;
    if (!all_up || g->epoch!=epoch) return false;
    for (size_t i=0;i<sizeof(k->pressed)/sizeof(k->pressed[0]);++i)
        if (k->pressed[i]) return false;
    *k=(SpKeyState){0};
    g->armed=true;
    return true;
}
