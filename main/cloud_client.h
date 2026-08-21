#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_SCHEDULE_MAX 64

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
    /* Optional shared lock for NVS operations performed by the application. */
    void *storage_lock;
} cloud_client_config_t;

/*
 * Compact schedule representation.  A previous implementation used seven
 * native int fields, making 64 entries large enough to stress the NVS blob
 * limit.  Keep the persistent representation small and explicit.
 */
typedef struct {
    uint16_t duration_minutes;
    uint8_t enabled;
    uint8_t id;
    uint8_t relay;
    uint8_t hour;
    uint8_t minute;
    uint8_t action;
    uint8_t days;
} cloud_schedule_t;

void cloud_client_init(const cloud_client_config_t *cfg);
void cloud_client_start_ota(const char *url);
bool cloud_client_is_online(void);
size_t cloud_client_get_schedules(cloud_schedule_t *out, size_t max_count);
bool cloud_client_replace_schedules(const cloud_schedule_t *items, size_t count);
