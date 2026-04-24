#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>

esp_err_t web_server_start(uint16_t port);
void web_server_stop(void);
httpd_handle_t web_server_get_handle(void);
