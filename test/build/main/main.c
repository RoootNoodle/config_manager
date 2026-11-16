#include "config_manager.h"

void app_main(void)
{
    config_manager_init();
    config_manager_load();
    config_get_live_value("x", 0);
}
