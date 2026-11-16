#pragma once

#include "sdkconfig.h"

#ifndef CONFIG_MANAGER_FILE_PATH
#define CONFIG_MANAGER_FILE_PATH CONFIG_CONFIG_MANAGER_FILE_PATH
#endif

_Static_assert(sizeof(CONFIG_MANAGER_FILE_PATH) > 1 && sizeof(CONFIG_MANAGER_FILE_PATH) <= 64,
               "CONFIG_CONFIG_MANAGER_FILE_PATH must be 1..63 characters");
