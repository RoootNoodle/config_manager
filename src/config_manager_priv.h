#pragma once

#include "sdkconfig.h"

#ifndef CONFIG_MANAGER_FILE_PATH
#define CONFIG_MANAGER_FILE_PATH CONFIG_CONFIG_MANAGER_FILE_PATH
#endif

// REVIEW: overridable like the v2 path so the host tests can point it at a temp folder.
#ifndef CONFIG_MANAGER_V1_FILE_PATH
#define CONFIG_MANAGER_V1_FILE_PATH CONFIG_CONFIG_MANAGER_V1_FILE_PATH
#endif

_Static_assert(sizeof(CONFIG_MANAGER_FILE_PATH) > 1 && sizeof(CONFIG_MANAGER_FILE_PATH) <= 64,
               "CONFIG_CONFIG_MANAGER_FILE_PATH must be 1..63 characters");
// REVIEW: empty is allowed and disables the v1 file removal.
_Static_assert(sizeof(CONFIG_MANAGER_V1_FILE_PATH) <= 64,
               "CONFIG_CONFIG_MANAGER_V1_FILE_PATH must be at most 63 characters");
