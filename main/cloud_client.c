#include "cloud_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define TAG "CLOUD_V2"
#define POLL_SECONDS 3
#define HTTP_TIMEOUT_MS 8000
#define RESPONSE_MAX 16384
#define SCHEDULE_MAX CLOUD_SCHEDULE_MAX
#define NVS_NS "home_cfg"
#define NVS_SCHEDULES "sched22"\n#define NVS_SCHEDULE_COUNT "sched_n2"\n#define NVS_SCHEDULE_VERSION "sched_v2"\n#define NVS_LEGACY_SCHEDULES "sched21"\n#define NVS_LEGACY_COUNT "sched_n"

typedef cloud_schedule_t schedule_t;

typedef struct {
    bool enabled;
    int id;
    int relay;
    int hour;
    int minute;
    int action;
    int days;
    int duration_minutes;
} legacy_schedule_t;

static cloud_client_config_t g_cfg;
static SemaphoreHandle_t storage_lock_handle(void)
{
    return (SemaphoreHandle_t)g_cfg.storage_lock;
}
static volatile bool g_online = false;
static volatile bool g_ota_busy = false;
static schedule_t schedules[SCHEDULE_MAX];
static size_t schedule_count = 0;
static SemaphoreHandle_t cloud_mutex;

static esp_err_t http_write_cb(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) return 0;
    char *buf = (char *)evt->user_data;
    size_t *used = (size_t *)(buf + RESPONSE_MAX);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t room = RESPONSE_MAX - 1 - *used;
        size_t n = evt->data_len < room ? evt->data_len : room;
        memcpy(buf + *used, evt->data, n);
        *used += n;
        buf[*used] = '\0';
    }
    return ESP_OK;
}

static bool valid_url(const char *u)
{
    return u && (strncmp(u, "https://", 8) == 0 || strncmp(u, "http://", 7) == 0);
}

static bool cloud_post(const char *path, const char *body, char *response, size_t response_sz)
{
    if (!g_cfg.base_url[0] || !g_cfg.device_id[0] || !g_cfg.device_token[0] || !valid_url(g_cfg.base_url)) return false;
    char url[320];
    snprintf(url, sizeof(url), "%s%s", g_cfg.base_url, path);
    if (strstr(url, "//api/") != NULL) { char fixed[320]; snprintf(fixed, sizeof(fixed), "%.*s%s", (int)(strlen(g_cfg.base_url)-1), g_cfg.base_url, path); strlcpy(url, fixed, sizeof(url)); }
    char *capture = calloc(1, RESPONSE_MAX + sizeof(size_t));
    if (!capture) return false;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_data = capture,
        .event_handler = http_write_cb,
        .keep_alive_enable = true
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { free(capture); return false; }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    char auth[160]; snprintf(auth, sizeof(auth), "Bearer %s", g_cfg.device_token);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_post_field(h, body, strlen(body));
    esp_err_t err = esp_http_client_perform(h);
    int code = err == ESP_OK ? esp_http_client_get_status_code(h) : 0;
    if (err == ESP_OK && code >= 200 && code < 300 && response && response_sz) {
        strlcpy(response, capture, response_sz);
    }
    esp_http_client_cleanup(h);
    free(capture);
    return err == ESP_OK && code >= 200 && code < 300;
}

static bool save_schedules_locked(void)
{
    SemaphoreHandle_t storage = storage_lock_handle();
    if (storage) xSemaphoreTake(storage, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for schedules failed: %s", esp_err_to_name(err));
        if (storage) xSemaphoreGive(storage);
        return false;
    }

    const size_t blob_size = schedule_count * sizeof(schedule_t);

    if (blob_size == 0) {
        err = nvs_erase_key(h, NVS_SCHEDULES);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_blob(h, NVS_SCHEDULES, schedules, blob_size);
    }

    if (err == ESP_OK)
        err = nvs_set_u8(h, NVS_SCHEDULE_COUNT, (uint8_t)schedule_count);

    if (err == ESP_OK)
        err = nvs_set_u8(h, NVS_SCHEDULE_VERSION, 2);

    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);

    if (storage) xSemaphoreGive(storage);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS schedule save failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}
static void load_schedules(void)
{
    memset(schedules, 0, sizeof(schedules));
    schedule_count = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;

    uint8_t version = 0;
    uint8_t n = 0;

    esp_err_t verr = nvs_get_u8(h, NVS_SCHEDULE_VERSION, &version);
    esp_err_t nerr = nvs_get_u8(h, NVS_SCHEDULE_COUNT, &n);

    if (verr == ESP_OK && version == 2 && nerr == ESP_OK && n <= SCHEDULE_MAX && n > 0) {
        size_t sz = (size_t)n * sizeof(schedule_t);
        esp_err_t err = nvs_get_blob(h, NVS_SCHEDULES, schedules, &sz);
        if (err == ESP_OK && sz == (size_t)n * sizeof(schedule_t)) {
            schedule_count = n;
            nvs_close(h);
            return;
        }

        ESP_LOGW(TAG, "Schedule cache is invalid; ignoring corrupted v2 cache");
        memset(schedules, 0, sizeof(schedules));
        schedule_count = 0;
    } else if (verr == ESP_OK && version == 2 && n == 0) {
        /* A deliberately saved empty schedule list. */
        nvs_close(h);
        return;
    }

    /*
     * One-time migration from the older V2.1 layout.  The old structure used
     * native ints and therefore is not reused as the new persistent format.
     */
    uint8_t old_n = 0;
    if (nvs_get_u8(h, NVS_LEGACY_COUNT, &old_n) == ESP_OK &&
        old_n > 0 && old_n <= SCHEDULE_MAX) {
        legacy_schedule_t legacy[SCHEDULE_MAX];
        memset(legacy, 0, sizeof(legacy));
        size_t sz = (size_t)old_n * sizeof(legacy_schedule_t);

        if (nvs_get_blob(h, NVS_LEGACY_SCHEDULES, legacy, &sz) == ESP_OK &&
            sz == (size_t)old_n * sizeof(legacy_schedule_t)) {

            size_t converted = 0;
            for (size_t i = 0; i < old_n; ++i) {
                if (legacy[i].relay < 1 || legacy[i].relay > 5 ||
                    legacy[i].hour < 0 || legacy[i].hour > 23 ||
                    legacy[i].minute < 0 || legacy[i].minute > 59 ||
                    (legacy[i].action != 0 && legacy[i].action != 1) ||
                    legacy[i].days < 1 || legacy[i].days > 127 ||
                    legacy[i].duration_minutes < 0 ||
                    legacy[i].duration_minutes > 1439) {
                    continue;
                }

                schedules[converted].enabled = legacy[i].enabled ? 1 : 0;
                schedules[converted].id = (uint8_t)converted;
                schedules[converted].relay = (uint8_t)legacy[i].relay;
                schedules[converted].hour = (uint8_t)legacy[i].hour;
                schedules[converted].minute = (uint8_t)legacy[i].minute;
                schedules[converted].action = (uint8_t)legacy[i].action;
                schedules[converted].days = (uint8_t)legacy[i].days;
                schedules[converted].duration_minutes =
                    (uint16_t)legacy[i].duration_minutes;
                converted++;
            }

            schedule_count = converted;
            ESP_LOGI(TAG, "Migrated %u legacy schedules to compact format",
                     (unsigned)converted);
            nvs_close(h);

            /* Re-open through the normal writer so migration is atomic. */
            if (cloud_mutex) {
                xSemaphoreTake(cloud_mutex, portMAX_DELAY);
                bool ok = save_schedules_locked();
                xSemaphoreGive(cloud_mutex);
                if (!ok) {
                    ESP_LOGE(TAG, "Legacy schedule migration could not be saved");
                }
            }
            return;
        }
    }

    nvs_close(h);
}
static void parse_response(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *cmds = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(cmds) && g_cfg.command_cb) {
        cJSON *c = NULL;
        cJSON_ArrayForEach(c, cmds) {
            cJSON *rv = cJSON_GetObjectItem(c, "relay");
            cJSON *sv = cJSON_GetObjectItem(c, "state");
            int relay = rv ? rv->valueint : 0;
            int state = sv ? sv->valueint : 0;
            if (relay >= 1 && relay <= 5)
                g_cfg.command_cb(relay - 1, state ? 1 : 0, g_cfg.ctx);
        }
    }

    cJSON *sched = cJSON_GetObjectItem(root, "schedules");
    if (cJSON_IsArray(sched)) {
        schedule_t tmp[SCHEDULE_MAX];
        memset(tmp, 0, sizeof(tmp));
        size_t n = 0;
        cJSON *x = NULL;
        cJSON_ArrayForEach(x, sched) {
            if (n >= SCHEDULE_MAX) break;
            cJSON *v;
            tmp[n].enabled = (v=cJSON_GetObjectItem(x,"enabled")) ? cJSON_IsTrue(v) : false;
            tmp[n].id = (v=cJSON_GetObjectItem(x,"id")) ? v->valueint : (int)n;
            tmp[n].relay = (v=cJSON_GetObjectItem(x,"relay")) ? v->valueint : 0;
            tmp[n].hour = (v=cJSON_GetObjectItem(x,"hour")) ? v->valueint : -1;
            tmp[n].minute = (v=cJSON_GetObjectItem(x,"minute")) ? v->valueint : -1;
            tmp[n].action = (v=cJSON_GetObjectItem(x,"action")) ? v->valueint : 0;
            tmp[n].days = (v=cJSON_GetObjectItem(x,"days")) ? v->valueint : 127;
            tmp[n].duration_minutes = (v=cJSON_GetObjectItem(x,"durationMinutes")) ? v->valueint : 0;
            if (tmp[n].relay >= 1 && tmp[n].relay <= 5 &&
                tmp[n].hour >= 0 && tmp[n].hour < 24 &&
                tmp[n].minute >= 0 && tmp[n].minute < 60 &&
                (tmp[n].action == 0 || tmp[n].action == 1) &&
                tmp[n].days >= 1 && tmp[n].days <= 127 &&
                tmp[n].duration_minutes >= 0 && tmp[n].duration_minutes <= 1439) {
                n++;
            }
        }
        xSemaphoreTake(cloud_mutex, portMAX_DELAY);
        memset(schedules, 0, sizeof(schedules));
        memcpy(schedules, tmp, n * sizeof(schedule_t));
        schedule_count = n;
        if (!save_schedules_locked()) {
            ESP_LOGE(TAG, "Cloud schedule cache could not be persisted");
        }
        xSemaphoreGive(cloud_mutex);
    }

    cJSON *ota = cJSON_GetObjectItem(root, "ota");
    if (cJSON_IsObject(ota) && g_cfg.ota_cb) {
        cJSON *url = cJSON_GetObjectItem(ota, "url");
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0])
            g_cfg.ota_cb(url->valuestring, g_cfg.ctx);
    }
    cJSON_Delete(root);
}

static void cloud_task(void *arg)
{
    load_schedules();
    while (1) {
        int states[5] = {0}; bool enabled[5] = {0};
        if (g_cfg.snapshot_cb) g_cfg.snapshot_cb(states, enabled, g_cfg.ctx);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root,"deviceId",g_cfg.device_id);
        cJSON *sa=cJSON_CreateArray(),*ea=cJSON_CreateArray();
        for(int i=0;i<5;i++){cJSON_AddItemToArray(sa,cJSON_CreateNumber(states[i]));cJSON_AddItemToArray(ea,cJSON_CreateBool(enabled[i]));}
        cJSON_AddItemToObject(root,"states",sa); cJSON_AddItemToObject(root,"enabled",ea);
        char *body=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        char response[RESPONSE_MAX]; bool ok = body && cloud_post("/api/device/poll",body,response,sizeof(response));
        free(body);
        g_online = ok;
        if (ok) parse_response(response);
        vTaskDelay(pdMS_TO_TICKS(POLL_SECONDS*1000));
    }
}

static void remote_ota_task(void *arg)
{
    esp_task_wdt_add(NULL);
    char *url = (char *)arg;
    esp_http_client_config_t cfg = {.url=url,.timeout_ms=15000,.crt_bundle_attach=esp_crt_bundle_attach,.keep_alive_enable=true};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) goto done;
    if (esp_http_client_open(h, 0) != ESP_OK) goto cleanup;
    int64_t len = esp_http_client_fetch_headers(h);
    if (len <= 0 || len > 0x1A0000) goto cleanup;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL); if (!part) goto cleanup;
    esp_ota_handle_t oh=0; if (esp_ota_begin(part, len, &oh) != ESP_OK) goto cleanup;
    uint8_t *buf=malloc(4096); if(!buf){esp_ota_abort(oh);goto cleanup;}
    int n; bool fail=false;
    while((n=esp_http_client_read(h,(char*)buf,4096))>0){if(esp_ota_write(oh,buf,n)!=ESP_OK){fail=true;break;} esp_task_wdt_reset();}
    free(buf);
    if(fail || n<0 || esp_ota_end(oh)!=ESP_OK){if(!fail)esp_ota_abort(oh);goto cleanup;}
    if(esp_ota_set_boot_partition(part)==ESP_OK){ESP_LOGI(TAG,"Remote OTA complete; restarting");vTaskDelay(pdMS_TO_TICKS(800));esp_restart();}
cleanup: esp_http_client_close(h); esp_http_client_cleanup(h);
done: free(url); g_ota_busy=false; vTaskDelete(NULL);
}

size_t cloud_client_get_schedules(cloud_schedule_t *out, size_t max_count)
{
    if (!out || max_count == 0 || !cloud_mutex) return 0;
    xSemaphoreTake(cloud_mutex, portMAX_DELAY);
    size_t n = schedule_count < max_count ? schedule_count : max_count;
    for (size_t i = 0; i < n; ++i) { out[i].enabled=schedules[i].enabled; out[i].id=schedules[i].id; out[i].relay=schedules[i].relay; out[i].hour=schedules[i].hour; out[i].minute=schedules[i].minute; out[i].action=schedules[i].action; out[i].days=schedules[i].days; out[i].duration_minutes=schedules[i].duration_minutes; }
    xSemaphoreGive(cloud_mutex);
    return n;
}

bool cloud_client_replace_schedules(const cloud_schedule_t *items, size_t count)
{
    if (count > CLOUD_SCHEDULE_MAX || (count > 0 && !items) || !cloud_mutex)
        return false;

    schedule_t tmp[CLOUD_SCHEDULE_MAX];
    memset(tmp, 0, sizeof(tmp));

    for (size_t i = 0; i < count; ++i) {
        if (items[i].relay < 1 || items[i].relay > 5 ||
            items[i].hour > 23 ||
            items[i].minute > 59 ||
            (items[i].action != 0 && items[i].action != 1) ||
            items[i].days < 1 || items[i].days > 127 ||
            items[i].duration_minutes > 1439) {
            return false;
        }

        tmp[i].enabled = items[i].enabled ? 1 : 0;
        tmp[i].id = (uint8_t)i;
        tmp[i].relay = items[i].relay;
        tmp[i].hour = items[i].hour;
        tmp[i].minute = items[i].minute;
        tmp[i].action = items[i].action;
        tmp[i].days = items[i].days;
        tmp[i].duration_minutes = items[i].duration_minutes;
    }

    xSemaphoreTake(cloud_mutex, portMAX_DELAY);

    /*
     * Do not replace the live cache until NVS has successfully committed.
     * This prevents a failed save from leaving RAM and flash disagreeing.
     */
    schedule_t old[CLOUD_SCHEDULE_MAX];
    size_t old_count = schedule_count;
    memcpy(old, schedules, sizeof(old));

    memset(schedules, 0, sizeof(schedules));
    memcpy(schedules, tmp, count * sizeof(schedule_t));
    schedule_count = count;

    bool saved = save_schedules_locked();
    if (!saved) {
        memset(schedules, 0, sizeof(schedules));
        memcpy(schedules, old, sizeof(old));
        schedule_count = old_count;
    }

    xSemaphoreGive(cloud_mutex);
    return saved;
}
void cloud_client_init(const cloud_client_config_t *cfg)
{
    memset(&g_cfg,0,sizeof(g_cfg)); if(cfg) memcpy(&g_cfg,cfg,sizeof(g_cfg));
    cloud_mutex=xSemaphoreCreateMutex();
    if (!cloud_mutex) { ESP_LOGE(TAG, "Schedule mutex allocation failed"); return; }
    load_schedules();
    if(!g_cfg.base_url[0]||!g_cfg.device_id[0]||!g_cfg.device_token[0]) {
        ESP_LOGW(TAG,"Internet cloud configuration incomplete; local-only mode");
        return;
    }
    xTaskCreate(cloud_task,"cloud_client",6144,NULL,3,NULL);
}

void cloud_client_start_ota(const char *url)
{
    if(g_ota_busy || !url || strncmp(url,"https://",8)!=0) return;
    g_ota_busy=true; size_t n=strlen(url)+1; char *copy=malloc(n); if(!copy){g_ota_busy=false;return;} memcpy(copy,url,n);
    xTaskCreate(remote_ota_task,"remote_ota",6144,copy,4,NULL);
}

bool cloud_client_is_online(void){return g_online;}
