/*
 * Shared harness for the port-layer test binary (test_port_main.c links
 * test_service.c / test_events.c / test_discovery.c).
 */

#ifndef ONVIF_TEST_UTIL_H
#define ONVIF_TEST_UTIL_H

#include <stdio.h>
#include <string.h>
#include <time.h>

/* RFC 5737 documentation addresses only — never a real deployment IP. */
#define T_IP     "192.0.2.10"
#define T_SERIAL "aabbccddeeff"
#define T_UUID   "f472b01e-0000-1000-8000-aabbccddeeff"
#define T_EPOCH  ((time_t)1750000000)

/* defined in test_port_main.c (one instance for the whole binary) */
extern int g_checks, g_failures;

#define CHECK(cond, name)                                                                          \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("FAIL: %s (line %d)\n", name, __LINE__);                                        \
        }                                                                                          \
    } while (0)

#define CHECK_SUB(hay, needle, name) CHECK((hay) != NULL && strstr((hay), (needle)) != NULL, name)

void test_service(void);
void test_time_config_service(void);
void test_events(void);
void test_discovery(void);

#endif /* ONVIF_TEST_UTIL_H */
