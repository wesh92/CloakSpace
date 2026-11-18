#ifndef SP_CONFIG_FILE_H
#define SP_CONFIG_FILE_H
#include "config.h"
bool sp_config_load(SpConfig *cfg,const wchar_t *explicit_path,wchar_t *error,size_t capacity);
#endif
