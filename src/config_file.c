#include "config_file.h"
#include "win32.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PATH_CAP (SP_CONFIG_PATH_CHARS+1u)
static bool file_error(wchar_t *error,size_t cap,const wchar_t *format,...) {
    if (error && cap) {
        va_list args;va_start(args,format);error[0]=0;
        vswprintf(error,cap,format,args);va_end(args);error[cap-1]=0;
    }
    return false;
}
static bool separator(wchar_t ch) { return ch==L'\\' || ch==L'/'; }
static bool drive_letter(wchar_t ch) { return (ch>=L'A'&&ch<=L'Z') || (ch>=L'a'&&ch<=L'z'); }
/* Validate before normalization/opening: never reinterpret device namespaces,
   drive-relative names, wildcard patterns or alternate data streams as files. */
static bool valid_config_path(const wchar_t *path,wchar_t *error,size_t cap) {
    size_t length=wcslen(path);
    if (!length || length>SP_CONFIG_PATH_CHARS)
        return file_error(error,cap,L"Configuration path is empty or too long.");
    if (length>=4 && separator(path[0]) && separator(path[1]) &&
            (path[2]==L'?' || path[2]==L'.') && separator(path[3]))
        return file_error(error,cap,L"Device/namespace configuration paths are not allowed.");
    if (length>=2 && path[1]==L':' && (!drive_letter(path[0]) || length<3 || !separator(path[2])))
        return file_error(error,cap,L"Drive-relative configuration paths are not allowed; use an absolute drive path.");
    for (size_t i=0;i<length;++i) {
        if (path[i]==L'*' || path[i]==L'?') return file_error(error,cap,L"Configuration paths cannot contain globs/wildcards (* or ?).");
        if (path[i]<32 || path[i]==127 || wcschr(L"\"<>|",path[i]) || (path[i]==L':' && i!=1))
            return file_error(error,cap,L"Invalid configuration file path (no control characters or alternate streams).");
    }
    if (length>=2 && separator(path[0]) && separator(path[1])) {
        const wchar_t *server=path+2,*share=server;
        while (*share && !separator(*share)) ++share;
        if (share==server || !*share) return file_error(error,cap,L"UNC configuration paths require a server and share.");
        const wchar_t *end=++share;
        while (*end && !separator(*end)) ++end;
        if (end==share) return file_error(error,cap,L"UNC configuration paths require a nonempty share.");
        if (end-share==4 && CompareStringOrdinal(share,4,L"pipe",4,TRUE)==CSTR_EQUAL)
            return file_error(error,cap,L"Configuration target must be a regular file, not a named pipe.");
    }
    return true;
}
static wchar_t *resolve_config_path(const wchar_t *from,const wchar_t *path,wchar_t *error,size_t cap) {
    if (!valid_config_path(path,error,cap)) return NULL;
    wchar_t *joined=calloc(PATH_CAP,sizeof(*joined)),*full=calloc(PATH_CAP,sizeof(*full));
    if (!joined || !full) { free(joined);free(full);file_error(error,cap,L"Out of memory resolving configuration path.");return NULL; }
    size_t prefix=0,length=wcslen(path);
    bool absolute=(length>=3 && path[1]==L':' && separator(path[2])) ||
        (length>=2 && separator(path[0]) && separator(path[1]));
    if (from && !absolute) {
        const wchar_t *slash=wcsrchr(from,L'\\');
        if (!slash) {file_error(error,cap,L"Import source must have an absolute canonical directory.");goto fail;}
        prefix=(size_t)(slash-from)+1;
        if (separator(path[0])) {
            /* Root-relative paths use the containing file's drive/UNC share,
               never the process's current drive. */
            if (drive_letter(from[0]) && from[1]==L':') prefix=2;
            else {
                const wchar_t *root=from+2;
                for (unsigned i=0;i<2;++i) {
                    while (*root && !separator(*root)) ++root;
                    if (i==0 && *root) ++root;
                }
                prefix=(size_t)(root-from);
            }
        }
        if (prefix+length>SP_CONFIG_PATH_CHARS) {file_error(error,cap,L"Resolved configuration path is too long.");goto fail;}
        wmemcpy(joined,from,prefix);
    }
    if (prefix+length>SP_CONFIG_PATH_CHARS) {file_error(error,cap,L"Configuration path is too long.");goto fail;}
    wmemcpy(joined+prefix,path,length+1);
    for (wchar_t *q=joined;*q;++q) if (*q==L'/') *q=L'\\';
    DWORD n=GetFullPathNameW(joined,PATH_CAP,full,NULL);
    if (!n || n>=PATH_CAP || !valid_config_path(full,error,cap)) {
        file_error(error,cap,L"Configuration path is invalid or too long.");goto fail;
    }
    free(joined);return full;
fail:
    free(joined);free(full);return NULL;
}
/* Open, identify and read through the same handle. A writer/deleter cannot
   replace the source while it is read. Extended syntax is generated internally
   only after validating and normalizing an ordinary absolute drive/UNC path. */
static bool read_config_source(const wchar_t *path,SpConfigSource *source,wchar_t *error,size_t cap) {
    size_t length=wcslen(path);bool unc=separator(path[0]) && separator(path[1]);
    size_t extra=unc?6:4;
    if (length+extra>SP_CONFIG_PATH_CHARS) return file_error(error,cap,L"%ls:1: Configuration open path is too long.",path);
    wchar_t *open_path=malloc((length+extra+1)*sizeof(*open_path));
    if (!open_path) return file_error(error,cap,L"%ls:1: Out of memory opening configuration.",path);
    if (unc) {wcscpy(open_path,L"\\\\?\\UNC\\");wcscat(open_path,path+2);}
    else {wcscpy(open_path,L"\\\\?\\");wcscat(open_path,path);}
    HANDLE file=CreateFileW(open_path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    DWORD code=GetLastError();free(open_path);
    if (file==INVALID_HANDLE_VALUE)
        return file_error(error,cap,L"%ls:1: Cannot open configuration (Windows error %lu).",path,(unsigned long)code);
    wchar_t *name=NULL,*text=NULL;char *bytes=NULL;bool ok=false;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(file)!=FILE_TYPE_DISK || !GetFileInformationByHandle(file,&info) ||
            (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_DEVICE))) {
        file_error(error,cap,L"%ls:1: Configuration target must be an existing regular file (not a directory/device/pipe).",path);goto done;
    }
    DWORD need=GetFinalPathNameByHandleW(file,NULL,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!need || need>PATH_CAP) {file_error(error,cap,L"%ls:1: Cannot obtain final canonical configuration path.",path);goto done;}
    name=calloc((size_t)need+1,sizeof(*name));
    if (!name) {file_error(error,cap,L"%ls:1: Out of memory storing canonical path.",path);goto done;}
    DWORD n=GetFinalPathNameByHandleW(file,name,need+1,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!n || n>need) {file_error(error,cap,L"%ls:1: Cannot obtain final canonical configuration path.",path);goto done;}
    /* Keep the final path identity, but remove Win32's generated namespace
       prefix so diagnostics and subsequent relative imports use DOS/UNC form. */
    if (!wcsncmp(name,L"\\\\?\\UNC\\",8)) {
        memmove(name+2,name+8,(wcslen(name+8)+1)*sizeof(*name));name[0]=name[1]=L'\\';
    } else if (n>=7 && !wcsncmp(name,L"\\\\?\\",4) && drive_letter(name[4]) && name[5]==L':' && separator(name[6])) {
        memmove(name,name+4,(wcslen(name+4)+1)*sizeof(*name));
    } else {file_error(error,cap,L"%ls:1: Final configuration path is not an absolute drive/UNC path.",path);goto done;}
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file,&size) || size.QuadPart<0 || size.QuadPart>SP_CONFIG_FILE_BYTES) {
        file_error(error,cap,L"%ls:1: Configuration exceeds 65536 bytes per file or its size cannot be read.",name);goto done;
    }
    DWORD count=(DWORD)size.QuadPart,read=0;
    bytes=malloc((size_t)count+1);
    if (!bytes) {file_error(error,cap,L"%ls:1: Out of memory reading configuration.",name);goto done;}
    if ((count && (!ReadFile(file,bytes,count,&read,NULL) || read!=count)) || memchr(bytes,0,count)) {
        file_error(error,cap,L"%ls:1: Cannot read configuration or embedded NUL in UTF-8 text.",name);goto done;
    }
    size_t skip=count>=3 && (unsigned char)bytes[0]==0xef && (unsigned char)bytes[1]==0xbb && (unsigned char)bytes[2]==0xbf?3:0;
    int payload=(int)(count-skip);
    int chars=payload?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes+skip,payload,NULL,0):0;
    if (payload && !chars) {file_error(error,cap,L"%ls:1: Configuration requires valid UTF-8 text.",name);goto done;}
    text=calloc((size_t)chars+1,sizeof(*text));
    if (!text) {file_error(error,cap,L"%ls:1: Out of memory decoding configuration.",name);goto done;}
    if (chars && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes+skip,payload,text,chars)!=chars) {
        file_error(error,cap,L"%ls:1: Cannot decode UTF-8 configuration.",name);goto done;
    }
    source->text=text;source->name=name;text=NULL;name=NULL;ok=true;
done:
    CloseHandle(file);free(bytes);free(text);free(name);return ok;
}
static bool filesystem_loader(void *context,const wchar_t *from,const wchar_t *path,
        SpConfigSource *source,wchar_t *error,size_t cap) {
    (void)context;*source=(SpConfigSource){0};
    wchar_t *full=resolve_config_path(from,path,error,cap);
    if (!full) return false;
    bool ok=read_config_source(full,source,error,cap);free(full);return ok;
}
/* Absence is the only reason to try a different keymap. In particular, access
   and network failures must not silently enable the built-in shortcuts. */
static bool config_candidate(wchar_t *path,const wchar_t *location,wchar_t *error,size_t cap) {
    if (GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) return true;
    DWORD code=GetLastError();
    if (code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND) { path[0]=0; return true; }
    return file_error(error,cap,L"Cannot inspect %ls configuration (Windows error %lu).",location,(unsigned long)code);
}
static bool discover_config(wchar_t *path,wchar_t *error,size_t cap) {
    wchar_t root[32000];SetLastError(ERROR_SUCCESS);
    DWORD n=GetEnvironmentVariableW(L"APPDATA",root,32000),environment_error=GetLastError();
    if (n>=32000) return file_error(error,cap,L"APPDATA configuration root is too long.");
    if (!n && environment_error!=ERROR_ENVVAR_NOT_FOUND && environment_error!=ERROR_SUCCESS)
        return file_error(error,cap,L"Cannot read APPDATA configuration root (Windows error %lu).",(unsigned long)environment_error);
    if (n) {
        swprintf(path,PATH_CAP,L"%ls\\win-scratchpad\\scratchpad.conf",root);
        if (!config_candidate(path,L"APPDATA",error,cap)) return false;
    }
    if (!path[0]) {
        n=GetModuleFileNameW(NULL,path,PATH_CAP);
        if (!n || n>=PATH_CAP) return file_error(error,cap,L"Cannot locate executable directory.");
        wchar_t *slash=wcsrchr(path,L'\\');
        if (!slash) return file_error(error,cap,L"Executable has no absolute directory.");
        slash[1]=0;
        if (wcslen(path)+16>=PATH_CAP) return file_error(error,cap,L"Executable directory path is too long.");
        wcscat(path,L"scratchpad.conf");
        if (!config_candidate(path,L"executable-adjacent",error,cap)) return false;
    }
    return true;
}
bool sp_config_load(SpConfig *cfg,const wchar_t *explicit_path,wchar_t *error,size_t cap) {
    if (error && cap) error[0]=0;
    if (!cfg) return file_error(error,cap,L"Missing configuration destination.");
    wchar_t *discovered=NULL;
    if (!explicit_path) {
        discovered=calloc(PATH_CAP,sizeof(*discovered));
        if (!discovered) return file_error(error,cap,L"Out of memory discovering configuration.");
        if (!discover_config(discovered,error,cap)) {free(discovered);return false;}
        if (!*discovered) {free(discovered);sp_config_defaults(cfg);return true;}
        explicit_path=discovered;
    }
    SpConfigSource source={0};bool ok=filesystem_loader(NULL,NULL,explicit_path,&source,error,cap);
    if (ok) ok=sp_config_parse_ex(cfg,source.text,source.name,filesystem_loader,NULL,error,cap);
    free(source.text);free(source.name);free(discovered);return ok;
}
