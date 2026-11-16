#include <stdlib.h>
#include "config_manager.h"

void app_main(void)
{
    config_manager_init();
    config_manager_load();
    // REVIEW: references every new v2 entry point so the device compile check links them.
    int idx = config_manager_find_unit(1);
    config_get_live_value("x", idx < 0 ? 0 : (uint8_t)idx);
    config_manager_apply_patch("{\"v\":1}");
    cJSON_Delete(config_manager_build_state(false));
    cJSON_Delete(config_manager_build_limits());
    cJSON_Delete(config_manager_get_unit_json(1));
    free(config_manager_get_payload("{}"));
    config_manager_handle_sync("{\"v\":0,\"h\":\"00000000\"}");
}
