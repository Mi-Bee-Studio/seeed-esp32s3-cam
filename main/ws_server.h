#pragma once
#include "esp_http_server.h"

esp_err_t ws_server_init(httpd_handle_t server);
void ws_broadcast(const char *type, const char *data);
