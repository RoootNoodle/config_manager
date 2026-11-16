#include <stdlib.h>
#include "unity.h"
#include "unity_fixture.h"

static void run_all_tests(void)
{
    RUN_TEST_GROUP(parse);
    RUN_TEST_GROUP(hash);
    RUN_TEST_GROUP(update);
}

void app_main(void)
{
    const char *argv[] = {"config_manager_host_test", "-v"};
    exit(UnityMain(2, argv, run_all_tests));
}
