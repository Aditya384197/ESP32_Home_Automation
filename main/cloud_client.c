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
#define NVS_SCHEDULES "schedules"

typedef struct {
    bool enabled;
    int id;
    int relay;
    int hour;
    int minute;
    int action;
    int days;
    int duration_minutes;
} schedule_t;

static cloud_client_config_t g_cfg;
static volatile bool g_online = false;
static volatile bool g_ota_busy = false;
static schedule_t schedules[SCHEDULE_MAX];
static size_t schedule_count = 0;
static volatile uint32_t schedule_generation = 0;
static volatile bool schedule_dirty = false;
static time_t timed_off_at[5] = {0,0,0,0,0};
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

static void save_schedules(void)
{
    xSemaphoreTake(cloud_mutex, portMAX_DELAY);
    nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_SCHEDULES, schedules, sizeof(schedules));
        nvs_set_u8(h, "sched_n", (uint8_t)schedule_count);
        nvs_commit(h); nvs_close(h);
    }
    xSemaphoreGive(cloud_mutex);
}

typedef struct {
    bool enabled;
    int id;
    int relay;
    int hour;
    int minute;
    int action;
    int days;
} legacy_schedule_t;

static void load_schedules(void)
{
    memset(schedules, 0, sizeof(schedules));
    schedule_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(schedules);
    esp_err_t err = nvs_get_blob(h, NVS_SCHEDULES, schedules, &sz);
    if (err != ESP_OK || sz != sizeof(schedule_t) * SCHEDULE_MAX) {
        legacy_schedule_t legacy[SCHEDULE_MAX];
        memset(legacy, 0, sizeof(legacy));
        size_t lsz = sizeof(legacy);
        if (nvs_get_blob(h, NVS_SCHEDULES, legacy, &lsz) == ESP_OK) {
            size_t legacy_count = lsz / sizeof(legacy_schedule_t);
            if (legacy_count > SCHEDULE_MAX) legacy_count = SCHEDULE_MAX;
            for (size_t i = 0; i < legacy_count; ++i) {
                schedules[i].enabled = legacy[i].enabled;
                schedules[i].id = legacy[i].id;
                schedules[i].relay = legacy[i].relay;
                schedules[i].hour = legacy[i].hour;
                schedules[i].minute = legacy[i].minute;
                schedules[i].action = legacy[i].action;
                schedules[i].days = legacy[i].days;
                schedules[i].duration_minutes = 0;
            }
        }
    }

    uint8_t n = 0;
    if (nvs_get_u8(h, "sched_n", &n) == ESP_OK && n <= SCHEDULE_MAX)
        schedule_count = n;
    nvs_close(h);
}

static void apply_schedules(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    if (now < 1700000000 || localtime_r(&now, &tmv) == NULL) return;

    static int64_t last_run[SCHEDULE_MAX];
    static uint32_t seen_generation = UINT32_MAX;
    if (seen_generation != schedule_generation) {
        for (size_t i = 0; i < SCHEDULE_MAX; ++i) last_run[i] = -1;
        for (int i = 0; i < 5; ++i) timed_off_at[i] = 0;
        seen_generation = schedule_generation;
    }

    /* Finish any active "ON for N minutes" schedule. */
    if (g_cfg.command_cb) {
        for (int r = 0; r < 5; ++r) {
            if (timed_off_at[r] > 0 && now >= timed_off_at[r]) {
                g_cfg.command_cb(r, 0, g_cfg.ctx);
                timed_off_at[r] = 0;
            }
        }
    }

    int64_t minute_key = (int64_t)tmv.tm_year * 366 * 1440 +
                         (int64_t)tmv.tm_yday * 1440 +
                         (int64_t)tmv.tm_hour * 60 + tmv.tm_min;
    int daybit = 1 << tmv.tm_wday;

    schedule_t local[SCHEDULE_MAX];
    size_t n;
    xSemaphoreTake(cloud_mutex, portMAX_DELAY);
    n = schedule_count;
    memcpy(local, schedules, sizeof(local));
    xSemaphoreGive(cloud_mutex);

    for (size_t i = 0; i < n; ++i) {
        if (!local[i].enabled ||
            local[i].relay < 1 || local[i].relay > 5 ||
            local[i].hour < 0 || local[i].hour > 23 ||
            local[i].minute < 0 || local[i].minute > 59 ||
            local[i].duration_minutes < 0 || local[i].duration_minutes > 1439 ||
            !(local[i].days & daybit)) {
            continue;
        }
        if (local[i].hour != tmv.tm_hour || local[i].minute != tmv.tm_min) continue;
        if (last_run[i] == minute_key) continue;

        last_run[i] = minute_key;
        int relay = local[i].relay - 1;
        int state = local[i].action ? 1 : 0;
        if (g_cfg.command_cb) g_cfg.command_cb(relay, state, g_cfg.ctx);

        if (state && local[i].duration_minutes > 0) {
            timed_off_at[relay] = now + (time_t)local[i].duration_minutes * 60;
        } else if (!state) {
            timed_off_at[relay] = 0;
        }
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    while (1) {
        apply_schedules();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void schedule_to_public(const schedule_t *s, cloud_schedule_t *p)
{
    p->enabled = s->enabled;
    p->id = s->id;
    p->relay = s->relay;
    p->hour = s->hour;
    p->minute = s->minute;
    p->action = s->action;
    p->days = s->days;
    p->duration_minutes = s->duration_minutes;
}

size_t cloud_client_get_schedules(cloud_schedule_t *out, size_t max_count)
{
    if (!out || max_count == 0 || !cloud_mutex) return 0;
    size_t n;
    xSemaphoreTake(cloud_mutex, portMAX_DELAY);
    n = schedule_count < max_count ? schedule_count : max_count;
    for (size_t i = 0; i < n; ++i) schedule_to_public(&schedules[i], &out[i]);
    xSemaphoreGive(cloud_mutex);
    return n;
}

bool cloud_client_replace_schedules(const cloud_schedule_t *items, size_t count)
{
    if (!cloud_mutex || !items || count > SCHEDULE_MAX) return false;
    schedule_t tmp[SCHEDULE_MAX];
    memset(tmp, 0, sizeof(tmp));
    for (size_t i = 0; i < count; ++i) {
        const cloud_schedule_t *p = &items[i];
        if (!p->relay || p->relay > 5 || p->hour < 0 || p->hour > 23 ||
            p->minute < 0 || p->minute > 59 || p->days < 1 || p->days > 127 ||
            (p->action != 0 && p->action != 1) ||
            p->duration_minutes < 0 || p->duration_minutes > 1439) return false;
        tmp[i].enabled = p->enabled;
        tmp[i].id = (int)i;
        tmp[i].relay = p->relay;
        tmp[i].hour = p->hour;
        tmp[i].minute = p->minute;
        tmp[i].action = p->action;
        tmp[i].days = p->days;
        tmp[i].duration_minutes = p->duration_minutes;
    }

    bool changed;
    xSemaphoreTake(cloud_mutex, portMAX_DELAY);
    changed = schedule_count != count || memcmp(schedules, tmp, sizeof(schedule_t) * SCHEDULE_MAX) != 0;
    if (changed) {
        memset(schedules, 0, sizeof(schedules));
        memcpy(schedules, tmp, sizeof(schedule_t) * count);
        schedule_count = count;
        schedule_generation++;
        schedule_dirty = true;
    }
    xSemaphoreGive(cloud_mutex);
    if (changed) save_schedules();
    return true;
}

static void parse_response(const char *json)
{
    cJSON *root = cJSON_Parse(json); if (!root) return;
    cJSON *cmds = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(cmds) && g_cfg.command_cb) {
        cJSON *c = NULL; cJSON_ArrayForEach(c, cmds) {
            int relay = cJSON_GetObjectItem(c,"relay") ? cJSON_GetObjectItem(c,"relay")->valueint : 0;
            int state = cJSON_GetObjectItem(c,"state") ? cJSON_GetObjectItem(c,"state")->valueint : 0;
            if (relay >= 1 && relay <= 5) g_cfg.command_cb(relay - 1, state ? 1 : 0, g_cfg.ctx);
        }
    }
    cJSON *sched = cJSON_GetObjectItem(root, "schedules");
    if (cJSON_IsArray(sched)) {
        schedule_t tmp[SCHEDULE_MAX]; size_t n = 0; cJSON *x = NULL;
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
            if (tmp[n].relay >= 1 && tmp[n].relay <= 5 && tmp[n].hour >= 0 && tmp[n].hour < 24 && tmp[n].minute >= 0 && tmp[n].minute < 60 &&
                tmp[n].days >= 1 && tmp[n].days <= 127 && tmp[n].duration_minutes >= 0 && tmp[n].duration_minutes <= 1439) n++;
        }
        bool changed = false;
        xSemaphoreTake(cloud_mutex, portMAX_DELAY);
        if (schedule_count != n || memcmp(schedules, tmp, sizeof(schedule_t) * n) != 0 ||
            (n < schedule_count && memcmp(&schedules[n], &tmp[n], sizeof(schedule_t) * (SCHEDULE_MAX - n)) != 0)) {
            changed = true;
            memset(schedules, 0, sizeof(schedules));
            memcpy(schedules, tmp, n * sizeof(schedule_t));
            schedule_count = n;
            schedule_generation++;
        }
        xSemaphoreGive(cloud_mutex);
        if (changed) save_schedules();
        schedule_dirty = false;
    }
    cJSON *ota = cJSON_GetObjectItem(root, "ota");
    if (cJSON_IsObject(ota) && g_cfg.ota_cb) {
        cJSON *url = cJSON_GetObjectItem(ota, "url");
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) g_cfg.ota_cb(url->valuestring, g_cfg.ctx);
    }
    cJSON_Delete(root);
}


static bool push_schedules_to_cloud(uint32_t generation)
{
    if (!g_cfg.base_url[0] || !g_cfg.device_id[0] || !g_cfg.device_token[0]) return false;

    cloud_schedule_t local[CLOUD_SCHEDULE_MAX];
    size_t n = cloud_client_get_schedules(local, CLOUD_SCHEDULE_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) { if (root) cJSON_Delete(root); if (arr) cJSON_Delete(arr); return false; }

    for (size_t i = 0; i < n; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", local[i].enabled);
        cJSON_AddNumberToObject(o, "relay", local[i].relay);
        cJSON_AddNumberToObject(o, "hour", local[i].hour);
        cJSON_AddNumberToObject(o, "minute", local[i].minute);
        cJSON_AddNumberToObject(o, "action", local[i].action);
        cJSON_AddNumberToObject(o, "days", local[i].days);
        cJSON_AddNumberToObject(o, "durationMinutes", local[i].duration_minutes);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddStringToObject(root, "deviceId", g_cfg.device_id);
    cJSON_AddItemToObject(root, "schedules", arr);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    char response[512];
    bool ok = cloud_post("/api/device/schedules", body, response, sizeof(response));
    free(body);

    if (ok && schedule_generation == generation) schedule_dirty = false;
    return ok;
}

static void cloud_task(void *arg)
{
    while (1) {
        if (schedule_dirty) {
            uint32_t gen = schedule_generation;
            if (!push_schedules_to_cloud(gen)) {
                vTaskDelay(pdMS_TO_TICKS(POLL_SECONDS * 1000));
                continue;
            }
        }

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

void cloud_client_init(const cloud_client_config_t *cfg)
{
    memset(&g_cfg,0,sizeof(g_cfg)); if(cfg) memcpy(&g_cfg,cfg,sizeof(g_cfg));
    cloud_mutex=xSemaphoreCreateMutex();
    if (!cloud_mutex) {
        ESP_LOGE(TAG, "Cloud/scheduler mutex allocation failed");
        return;
    }

    load_schedules();
    if (xTaskCreate(scheduler_task, "scheduler", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Local scheduler task creation failed");
    }

    if(!g_cfg.base_url[0]||!g_cfg.device_id[0]||!g_cfg.device_token[0]) {
        ESP_LOGW(TAG,"Cloud configuration incomplete; running local-only mode");
        return;
    }
    /* Cached local schedules are the device's source of truth until the first
     * successful sync. This prevents a reboot from silently replacing a
     * locally-created schedule with an older cloud copy. */
    if (schedule_count > 0) schedule_dirty = true;
    if (xTaskCreate(cloud_task,"cloud_client",6144,NULL,3,NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cloud client task creation failed");
    }
}

void cloud_client_start_ota(const char *url)
{
    if(g_ota_busy || !url || strncmp(url,"https://",8)!=0) return;
    g_ota_busy=true; size_t n=strlen(url)+1; char *copy=malloc(n); if(!copy){g_ota_busy=false;return;} memcpy(copy,url,n);
    xTaskCreate(remote_ota_task,"remote_ota",6144,copy,4,NULL);
}

bool cloud_client_is_online(void){return g_online;}
