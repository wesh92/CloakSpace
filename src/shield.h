#ifndef SP_SHIELD_H
#define SP_SHIELD_H
#include <stdbool.h>
#include <stddef.h>

/* A blocked down owns its up, even after focus/mode changes. A down that was
   already delivered owns an unblocked up; blocking repeats must not latch it. */
typedef struct { bool pressed,swallowed; } SpShieldPress;
static inline bool sp_shield_press(SpShieldPress *key,bool down,bool block) {
    if (down) {
        if (!key->pressed) { key->pressed=true; key->swallowed=block; }
        return key->swallowed || block;
    }
    bool swallowed=key->swallowed;
    key->pressed=key->swallowed=false;
    return swallowed;
}
static inline unsigned sp_shield_key(unsigned vk,unsigned scan) {
    return scan && scan<512?scan:vk<256?512+vk:768;
}
#endif
