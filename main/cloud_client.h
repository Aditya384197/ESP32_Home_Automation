#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef void (*cloud_command_cb_t)(int relay, int state, void *ctx);
typedef void (*cloud_snapshot_cb_t)(int *states, bool *enabled, void *ctx);
typedef void (*cloud_ota_cb_t)(const char *url, void *ctx);

typedef struct {
    char base_url[192];
    char device_id[64];
    char device_token[128];
    cloud_command_cb_t command_cb;
    cloud_snapshot_cb_t snapshot_cb;
    cloud_ota_cb_t ota_cb;
    void *ctx;
} cloud_client_config_t;

void cloud_client_init(const cloud_client_config_t *cfg);
void cloud_client_start_ota(const char *url);
bool cloud_client_is_online(void);
