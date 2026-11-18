#ifndef SP_INPUT_H
#define SP_INPUT_H
#include "config.h"
typedef struct { bool down[256], swallowed[768], pressed[768]; } SpKeyState;
typedef struct { bool suppress; size_t binding; } SpKeyDecision;
/* Zero-initialized, owned by the hook thread together with the reducer state. */
typedef struct { bool armed; uint64_t epoch; } SpKeyGate;
SpKeyDecision sp_key_event(SpKeyState *keys,const SpProfile *profile,
    unsigned vk,unsigned scan,bool down,bool injected);
unsigned sp_key_modifiers(const SpKeyState *keys);
/* Disarmed physical events are tracked, but always passed without matching. */
SpKeyDecision sp_key_gate_event(SpKeyGate *gate,SpKeyState *keys,const SpProfile *profile,
    unsigned vk,unsigned scan,bool down,bool injected);
/* Capture gate->epoch BEFORE checking physical key state outside the callback.
   Only an unchanged, all-up poll with no observed held slots may arm/reset. */
bool sp_key_gate_poll(SpKeyGate *gate,SpKeyState *keys,uint64_t epoch,bool all_up);
#endif
