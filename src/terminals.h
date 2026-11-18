#ifndef SP_TERMINALS_H
#define SP_TERMINALS_H
#include "config.h"
#include "process.h"
/* Called on a worker thread, with immutable configuration; never touches desktop windows. */
bool sp_terminal_run(const SpProfile *profile,const SpBinding *binding,HANDLE cancel,
                     wchar_t *message,size_t capacity);
#endif
