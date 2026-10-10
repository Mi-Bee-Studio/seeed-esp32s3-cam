/*
 * Port-layer tests — main: shared counters + suite runner.
 */

#include "../include/onvif_c.h"
#include "test_util.h"
#include "onvif_fake.h"

int g_checks, g_failures;

int main(void)
{
    /* Producer contract before any start: silent no-op, no crash. */
    onvif_c_motion(true, 90);
    CHECK(!onvif_c_events_subscribed(), "no subscription before start");

    test_service();
    test_time_config_service();
    test_events();
    test_discovery();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
