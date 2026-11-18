#ifndef SP_LAUNCH_ARGS_H
#define SP_LAUNCH_ARGS_H
#include "config.h"
#include <wchar.h>

/* Windows Terminal is a second parser, not an argv-transparent relay. Use an
   explicit safe subset instead of pretending CRT quoting escapes its syntax.
   Keep this shared by pre-hook validation, dispatch and the launch worker. */
static inline const wchar_t *sp_terminal_value_error(const wchar_t *value,bool child) {
    if (!value || !*value) return L"Windows Terminal names and child arguments must be nonempty.";
    if (wcschr(value,L';')) return L"Windows Terminal values cannot contain ';', even when preceded by a backslash.";
    for (const wchar_t *p=value;*p;++p)
        if (*p<32 || *p==127) return L"Windows Terminal values cannot contain control characters.";
    if (child && wcschr(value,L'\"')) return L"Windows Terminal child arguments cannot contain embedded double quotes; use process.exec for exact argv.";
    size_t length=wcslen(value);
    if (child && wcschr(value,L' ') && value[length-1]==L'\\')
        return L"Windows Terminal child arguments containing spaces cannot end in a backslash; use forward slashes or process.exec.";
    return NULL;
}
static inline const wchar_t *sp_terminal_settings_error(const SpProfile *profile) {
    const wchar_t *error=sp_terminal_value_error(profile->quake_window,false);
    if (error) return error;
    const wchar_t *values[]={profile->terminal_profile,profile->container_profile};
    for (size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        if (!values[i]) continue; /* Omitted: let Terminal choose its default. */
        error=sp_terminal_value_error(values[i],false);
        if (error) return error;
    }
    if (profile->container_directory) {
        error=sp_terminal_value_error(profile->container_directory,true);
        if (error) return error;
        if (profile->container_directory[0]!=L'/')
            return L"container-directory must be an absolute POSIX path inside the container (leading '/').";
    }
    return NULL;
}
static inline const wchar_t *sp_launch_binding_error(const SpBinding *binding) {
    bool quake=!wcscmp(binding->action,L"terminal.quake");
    bool container=!wcscmp(binding->action,L"terminal.container");
    bool process=!wcscmp(binding->action,L"process.exec");
    if (!quake && !container && !process) return NULL;
    if (binding->argc>SP_MAX_ARGS || (!container && !binding->argc)) return L"Invalid launch argument count (maximum 12; executable/profile required except for terminal.container).";
    if (!binding->argc) return NULL; /* Container command defaults to /bin/sh. */
    for (size_t i=0;i<binding->argc;++i)
        if (!binding->args[i]) return L"Launch arguments must not be NULL.";
    if (!*binding->args[0]) return L"Required executable/profile argument must be nonempty.";
    if (quake || container) for (size_t i=0;i<binding->argc;++i) {
        const wchar_t *error=sp_terminal_value_error(binding->args[i],container || i!=0);
        if (error) return error;
    }
    /* Later process.exec args intentionally allow empty/quoted/semicolon values. */
    return NULL;
}
#endif
