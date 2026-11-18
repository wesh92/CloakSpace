#ifndef SP_PROCESS_H
#define SP_PROCESS_H
#include "win32.h"
#define SP_COMMAND_CAP 32768u
#define SP_OUTPUT_CAP 8192u
#define SP_CONTAINER_CAP 256u

typedef struct {
    DWORD error, exit_code;
    bool timed_out, cancelled, truncated;
    size_t out_size, err_size;
    char out[SP_OUTPUT_CAP], err[SP_OUTPUT_CAP];
} SpProcessResult;

bool sp_commandline(wchar_t *out, size_t capacity, const wchar_t *const *argv, size_t argc);
/* Search absolute PATH entries only, never the current/application directory implicitly. */
bool sp_find_executable(const wchar_t *name, wchar_t *out, size_t capacity);
bool sp_capture(const wchar_t *exe, const wchar_t *const *argv, size_t argc,
                DWORD timeout_ms, HANDLE cancel, SpProcessResult *result);
bool sp_spawn(const wchar_t *exe, const wchar_t *const *argv, size_t argc, DWORD *error);
bool sp_parse_containers(const char *text, size_t length, char first[SP_CONTAINER_CAP], size_t *count);
#endif
