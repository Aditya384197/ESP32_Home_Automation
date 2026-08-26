#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "cloud_client.h"

#define TAG "SMART_HOME"

#define RELAY1_GPIO             16
#define RELAY2_GPIO             17
#define RELAY3_GPIO             18
#define RELAY4_GPIO             19
#define RELAY5_GPIO             21

#define RELAY_COUNT             5

#define SWITCH1_GPIO            32
#define SWITCH2_GPIO            33
#define SWITCH3_GPIO            25
#define SWITCH4_GPIO            26
#define SWITCH5_GPIO            27
#define SWITCH_COUNT            5
#define SWITCH_ACTIVE_LEVEL     0
#define SWITCH_DEBOUNCE_SAMPLES 3
#define SWITCH_POLL_MS          20

#define RELAY_ACTIVE_LEVEL      1

#define DEFAULT_AP_SSID         "ESP32-SMART-HOME"
#define DEFAULT_AP_PASSWORD     "ChangeMe123"
#define DEFAULT_AP_CHANNEL      6
#define AP_MAX_CONNECTIONS      4

#define AP_IP_ADDR              "192.168.4.1"
#define AP_GW_ADDR              "192.168.4.1"
#define AP_NETMASK              "255.255.255.0"

#define NVS_NAMESPACE           "home_cfg"
#define NVS_KEY_RELAY_STATES    "relay"
#define NVS_KEY_RELAY_ENABLED   "renable"
#define NVS_KEY_RELAY_NAMES     "rnames"
#define NVS_KEY_AP_SSID         "ap_ssid"
#define NVS_KEY_AP_PASS         "ap_pass"
#define NVS_KEY_STA_SSID        "sta_ssid"
#define NVS_KEY_STA_PASS        "sta_pass"
#define NVS_KEY_CLOUD_URL       "cloud_url"
#define NVS_KEY_DEVICE_ID       "device_id"
#define NVS_KEY_DEVICE_TOKEN    "device_token"
#define NVS_KEY_BRAND_NAME      "brand_name"

#define WATCHDOG_TIMEOUT_MS     10000
#define DNS_PORT                53
#define DNS_STACK_SIZE          3072
#define DNS_RX_SIZE             512

#define MAX_AP_SSID_LEN         32
#define MAX_AP_PASS_LEN         63
#define MAX_RELAY_NAME_LEN      31
#define MAX_CLOUD_URL_LEN       191
#define MAX_DEVICE_ID_LEN       63
#define MAX_DEVICE_TOKEN_LEN    127
#define MAX_BRAND_LEN            40

static int relay_state[RELAY_COUNT] = {0, 0, 0, 0, 0};
static bool relay_enabled[RELAY_COUNT] = {true, true, true, false, false};
static char relay_name[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1] = {
    "Living Room Light",
    "Ceiling Fan",
    "Charging Socket",
    "Relay 4",
    "Relay 5"
};

static SemaphoreHandle_t relay_mutex;
static SemaphoreHandle_t storage_mutex;

static char ap_ssid[MAX_AP_SSID_LEN + 1] = DEFAULT_AP_SSID;
static char ap_password[MAX_AP_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;
static char sta_ssid[MAX_AP_SSID_LEN + 1] = "";
static char sta_password[MAX_AP_PASS_LEN + 1] = "";
static char cloud_url[MAX_CLOUD_URL_LEN + 1] = "";
static char device_id[MAX_DEVICE_ID_LEN + 1] = "";
static char device_token[MAX_DEVICE_TOKEN_LEN + 1] = "";
static char brand_name[MAX_BRAND_LEN + 1] = "Smart Home";
static volatile bool sta_connected = false;
static volatile uint8_t sta_retry_count = 0;
static volatile bool user_offline_mode = false;
static char sta_ip[16] = {0};

static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t switch_task_handle = NULL;
static TaskHandle_t relay_save_task_handle = NULL;
static httpd_handle_t http_server = NULL;
static TaskHandle_t schedule_task_handle = NULL;
static bool schedule_was_active[RELAY_COUNT] = {false, false, false, false, false};
static bool schedule_override[RELAY_COUNT] = {false, false, false, false, false};
static int schedule_revert_state[RELAY_COUNT] = {0, 0, 0, 0, 0};
static volatile bool g_time_synced = false;

static void schedule_note_manual_change(int index)
{
    if (index < 0 || index >= RELAY_COUNT) return;
    if (schedule_was_active[index]) schedule_override[index] = true;
}

static const char *HTML_PAGE =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#111\">\n"
"<title>ESP32 Smart Home</title>\n"
"<style>\n"
":root{color-scheme:light;--bg:#f7f7f7;--card:#fff;--text:#111;--muted:#68686c;--line:#e2e2e4;--accent:#111;--on:#111;--danger:#8a2f2f;--header:#eeeeef;--press:rgba(0,0,0,.045)}\n"
"@media(prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0c0c0d;--card:#1a1a1b;--text:#f4f4f5;--muted:#96969a;--line:#2b2b2d;--accent:#f4f4f5;--on:#f4f4f5;--danger:#d98f8f;--header:#161617;--press:rgba(255,255,255,.055)}}\n"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}\n"
"html,body{margin:0;height:100%;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;background:var(--bg);color:var(--text)}\n"
"body{overflow:hidden;overscroll-behavior-y:none}\n"
".app{display:flex;flex-direction:column;height:100dvh;width:100%}\n"
".top{flex:none;background:var(--header);padding:calc(env(safe-area-inset-top,0px) + 16px) 18px 15px;position:relative;z-index:1;transition:filter .32s ease}\n"
".top:after{content:'';position:absolute;left:0;right:0;bottom:-1px;height:1px;background:var(--line)}\n"
".topbar{display:grid;grid-template-columns:44px 1fr 44px;align-items:center;gap:12px;max-width:680px;margin:0 auto;width:100%}\n"
".brand{grid-column:2;text-align:center;overflow:hidden;min-width:0}\n"
".brand h1{font-size:22px;margin:0;font-weight:700;letter-spacing:.15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;transition:font-size .15s ease}\n"
".settings-btn{grid-column:3;width:42px;height:42px;border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--text);display:flex;align-items:center;justify-content:center;font-size:19px;cursor:pointer}\n"
".settings-btn:active{transform:scale(.94)}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0;box-shadow:0 2px 10px rgba(0,0,0,.04)}\n"
"#controls{flex:1;display:grid;grid-template-rows:repeat(5,1fr);width:100%;overflow:hidden;padding:10px 12px;gap:9px;transition:filter .32s ease}\n"
".relay-row{display:flex;align-items:center;justify-content:space-between;gap:15px;padding:0 20px;background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:0 1px 3px rgba(0,0,0,.05);cursor:pointer;user-select:none;-webkit-user-select:none;transition:background-color .18s cubic-bezier(.25,.8,.25,1),transform .16s cubic-bezier(.25,.8,.25,1),box-shadow .18s ease;max-width:680px;margin:0 auto;width:100%}\n"
".relay-row:active{background:var(--press);transform:scale(.982);box-shadow:0 1px 2px rgba(0,0,0,.06)}\n"
".relay-row .name{font-weight:650;font-size:17px}\n"
".relay-row .state{font-size:12.5px;color:var(--muted);margin-top:3px;letter-spacing:.3px;text-transform:uppercase}\n"
".switch{position:relative;width:54px;height:31px;flex:none;pointer-events:none}\n"
".switch input{opacity:0;width:0;height:0}\n"
".slider{position:absolute;inset:0;background:var(--line);border-radius:40px;transition:background .18s ease}\n"
".slider:before{content:'';position:absolute;width:25px;height:25px;left:3px;top:3px;background:var(--card);border-radius:50%;box-shadow:0 1px 4px rgba(0,0,0,.25);transition:transform .18s cubic-bezier(.34,1.2,.64,1)}\n"
"input:checked+.slider{background:var(--on)}input:checked+.slider:before{background:var(--bg);transform:translateX(23px)}\n"
"button{border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:10px;padding:11px 14px;font:inherit;cursor:pointer;transition:transform .16s cubic-bezier(.25,.8,.25,1),opacity .14s ease,box-shadow .16s ease}\n"
"button:active{transform:scale(.965)}\n"
"button.primary{background:var(--on);border-color:var(--on);color:var(--bg);box-shadow:0 3px 10px rgba(0,0,0,.14)}button.primary:active{box-shadow:0 1px 4px rgba(0,0,0,.1)}button:disabled{opacity:.5;cursor:not-allowed}\n"
".msg{font-size:13px;margin-top:10px;color:var(--muted)}.small{font-size:12px;color:var(--muted);line-height:1.45}\n"
"input[type=text],input[type=password],input[type=file],input[type=number],select{width:100%;padding:11px;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--text);font:inherit}\n"
"label.field{display:block;font-size:13px;color:var(--muted);margin:13px 0 6px}.hidden{display:none!important}\n"
".status{display:inline-flex;align-items:center;gap:7px;font-size:12px;color:var(--muted);flex-wrap:wrap}.dot{width:9px;height:9px;border-radius:50%;background:#8b8b8b}.dot.online{background:var(--text);box-shadow:0 0 0 4px var(--press)}.dot.forced-offline{background:var(--danger)}.online-text{color:var(--text);font-weight:650}\n"
".conn-toggle{margin-left:6px;padding:5px 10px;font-size:11px;border-radius:20px}.conn-toggle.active{background:var(--danger);border-color:var(--danger);color:#fff}\n"
".list-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 2px;border-top:1px solid var(--line);cursor:pointer;transition:background-color .12s ease}\n"
".list-row:first-child{border-top:0}.list-row:active{background:var(--press)}\n"
".list-row-text{display:flex;flex-direction:column;gap:2px;min-width:0}\n"
".list-row-text strong{font-size:15px}\n"
".list-row-sub{font-size:12px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".list-row.row-disabled{opacity:.5}\n"
".time-row{display:flex;align-items:center;gap:8px}.time-row select{flex:2}.time-row select.ampm{flex:1}.time-sep{font-weight:700;color:var(--muted)}\n"
".time-readout{padding:11px;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--muted)}\n"
".schedule-delete-row{margin-top:16px}.schedule-delete{width:100%;color:var(--danger);border-color:var(--danger)}\n"
".bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}.relay-config{margin-top:10px}.relay-config-item{padding:14px 0;border-top:1px solid var(--line)}\n"
".relay-config-item:first-child{border-top:0}.relay-config-head{display:flex;align-items:center;justify-content:space-between;gap:12px}\n"
".small-switch{position:relative;width:48px;height:27px;flex:none}.small-switch input{opacity:0;width:0;height:0}.small-slider{position:absolute;inset:0;background:var(--line);border-radius:40px;transition:.14s;cursor:pointer}\n"
".small-slider:before{content:'';position:absolute;width:21px;height:21px;left:3px;top:3px;background:var(--card);border-radius:50%;box-shadow:0 1px 4px rgba(0,0,0,.25);transition:.14s}\n"
".small-switch input:checked+.small-slider{background:var(--on)}.small-switch input:checked+.small-slider:before{background:var(--bg);transform:translateX(21px)}\n"
".relay-number{font-weight:650;font-size:15px}.relay-gpio,.relay-switch-gpio{font-size:12px;color:var(--muted);margin-top:3px}\n"
".setting-list{margin-top:14px}.setting-item{display:flex;align-items:center;gap:14px;padding:15px 2px;border-top:1px solid var(--line);cursor:pointer;transition:background-color .12s ease}\n"
".setting-item:first-child{border-top:0}.setting-item:active{background:var(--press)}.setting-icon{width:40px;height:40px;border-radius:12px;background:var(--bg);border:1px solid var(--line);display:flex;align-items:center;justify-content:center;font-size:19px;flex:none}\n"
".setting-title{font-weight:650;font-size:15px}.setting-desc{font-size:12px;color:var(--muted);margin-top:3px;line-height:1.4}.chevron{margin-left:auto;color:var(--muted);font-size:22px}\n"
".back-row{margin-top:20px;text-align:center}.back-btn{min-width:180px}.drawer-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.34);opacity:0;pointer-events:none;transition:opacity .28s ease;z-index:90}\n"
".settings-drawer{position:fixed;z-index:100;top:0;right:0;width:min(680px,100%);height:100dvh;background:var(--bg);box-shadow:-12px 0 35px rgba(0,0,0,.18);transform:translate3d(105%,0,0);transition:transform .34s cubic-bezier(.22,.8,.2,1);overflow-y:auto;overscroll-behavior:contain;will-change:transform}\n"
".settings-drawer.open{transform:translate3d(0,0,0)}.drawer-backdrop.open{opacity:1;pointer-events:auto}body.settings-open .app{filter:none}\n"
".drawer-inner{min-height:100%;padding:18px 14px 34px}.drawer-top{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 4px 18px}\n"
".drawer-title{font-size:23px;font-weight:700}.drawer-sub{font-size:14px;color:var(--muted);margin-top:4px}.drawer-status{margin-top:8px}\n"
".icon-btn{width:42px;height:42px;border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--text);display:flex;align-items:center;justify-content:center;font-size:20px;cursor:pointer;flex:none}\n"
".subpage{display:none}.subpage.active{display:block;animation:pageIn .28s cubic-bezier(.22,.8,.2,1) both}@keyframes pageIn{from{opacity:0;transform:translate3d(16px,0,0)}to{opacity:1;transform:none}}\n"
".page-title{font-size:21px;font-weight:700;margin:0}.page-sub{font-size:13px;color:var(--muted);margin-top:4px}.page-head{display:flex;align-items:center;gap:10px;margin-bottom:18px}.page-head .icon-btn{flex:none}\n"
".info-card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0}\n"
".schedule-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}.days{display:flex;gap:5px;flex-wrap:wrap;margin-top:8px}.days label{font-size:11px;display:flex;align-items:center;gap:3px}\n"
"@media(max-width:650px){.schedule-grid{grid-template-columns:1fr}.brand h1{font-size:20px}}\n"
"@media(prefers-reduced-motion:reduce){.settings-drawer,.drawer-backdrop,.app{transition:none}.subpage.active{animation:none}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"app\">\n"
"<header class=\"top\"><div class=\"topbar\">\n"
"<div class=\"brand\"><h1 id=\"brandTitle\">Smart Home</h1></div>\n"
"<button class=\"settings-btn\" onclick=\"openSettings()\" aria-label=\"Settings\" title=\"Settings\">⚙</button>\n"
"</div></header>\n"
"<section id=\"controls\"></section>\n"
"</div>\n"
"\n"
"<div id=\"drawerBackdrop\" class=\"drawer-backdrop\" onclick=\"closeSettings()\"></div>\n"
"<aside id=\"settingsDrawer\" class=\"settings-drawer\" aria-hidden=\"true\">\n"
"<div class=\"drawer-inner\">\n"
"<section id=\"settingsHome\" class=\"subpage active\">\n"
"<header class=\"drawer-top\">\n"
"<div><div class=\"drawer-title\">Settings</div><div class=\"drawer-sub\">Device configuration</div>\n"
"<div class=\"drawer-status status\"><span id=\"onlineDot\" class=\"dot\"></span><span id=\"onlineText\">Checking connection…</span><button id=\"connToggle\" class=\"conn-toggle\" onclick=\"toggleConnectivity()\" title=\"Toggle online/offline\">⏻</button></div></div>\n"
"<button class=\"icon-btn\" onclick=\"closeSettings()\" aria-label=\"Back\">←</button>\n"
"</header>\n"
"<div class=\"card setting-list\">\n"
"<div class=\"setting-item\" onclick=\"openSubPage('schedulePage')\"><div class=\"setting-icon\">◷</div><div><div class=\"setting-title\">Schedules</div><div class=\"setting-desc\">Independent weekly schedules for every relay</div></div><div class=\"chevron\">›</div></div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('brandPage')\"><div class=\"setting-icon\">✎</div><div><div class=\"setting-title\">Custom Logo / Name</div><div class=\"setting-desc\">Choose the text shown on the main control page</div></div><div class=\"chevron\">›</div></div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('relayPage')\"><div class=\"setting-icon\">▣</div><div><div class=\"setting-title\">Relay Configuration</div><div class=\"setting-desc\">Enable Relay 4/5 and rename any relay</div></div><div class=\"chevron\">›</div></div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('internetPage')\"><div class=\"setting-icon\">◎</div><div><div class=\"setting-title\">Internet Connection</div><div class=\"setting-desc\">Connect the ESP32 to your home Wi-Fi</div></div><div class=\"chevron\">›</div></div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('remotePage')\"><div class=\"setting-icon\">☁</div><div><div class=\"setting-title\">Remote Access</div><div class=\"setting-desc\">Optional cloud control from outside your home network</div></div><div class=\"chevron\">›</div></div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('apPage')\"><div class=\"setting-icon\">≋</div><div><div class=\"setting-title\">AP Configuration</div><div class=\"setting-desc\">Change the ESP32 local Wi-Fi SSID and password</div></div><div class=\"chevron\">›</div></div>\n"
"</div>\n"
"</section>\n"
"\n"
"<section id=\"schedulePage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Schedules</div><div class=\"page-sub\">Stored locally on the ESP32; Internet is not required for execution.</div></div></div>\n"
"<div class=\"card\" style=\"padding:4px 14px\"><div id=\"scheduleList\"></div></div>\n"
"<div class=\"bar\"><button onclick=\"addSchedule()\">＋ Add schedule</button></div>\n"
"<div id=\"scheduleMsg\" class=\"msg\"></div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"scheduleDetailPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backFromScheduleDetail()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Schedule</div><div class=\"page-sub\">Runs locally from the ESP32 clock, independent of Internet.</div></div></div>\n"
"<div class=\"info-card\">\n"
"<div class=\"schedule-grid\">\n"
"<div><label class=\"field\">Relay</label><select id=\"sdRelay\"></select></div>\n"
"<div><label class=\"field\">Action</label><select id=\"sdAction\" onchange=\"updateScheduleDetailEnd()\"></select></div>\n"
"</div>\n"
"<label class=\"field\">Start time</label>\n"
"<div class=\"time-row\"><select id=\"sdHour\" onchange=\"updateScheduleDetailEnd()\"></select><span class=\"time-sep\">:</span><select id=\"sdMinute\" onchange=\"updateScheduleDetailEnd()\"></select><select id=\"sdAmPm\" class=\"ampm\" onchange=\"updateScheduleDetailEnd()\"></select></div>\n"
"<div class=\"schedule-grid\">\n"
"<div><label class=\"field\">Duration hours</label><input id=\"sdDurH\" type=\"number\" min=\"0\" max=\"23\" value=\"0\" oninput=\"updateScheduleDetailEnd()\"></div>\n"
"<div><label class=\"field\">Duration minutes</label><input id=\"sdDurM\" type=\"number\" min=\"0\" max=\"59\" value=\"0\" oninput=\"updateScheduleDetailEnd()\"></div>\n"
"</div>\n"
"<label class=\"field\">After the duration</label>\n"
"<div class=\"time-readout\" id=\"sdEndReadout\">—</div>\n"
"<label class=\"field\">Repeat on</label>\n"
"<div class=\"days\" id=\"scheduleDays\"></div>\n"
"<label class=\"small\" style=\"display:block;margin-top:16px\"><input id=\"sdEnabled\" type=\"checkbox\"> Enabled</label>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveScheduleDetail()\">Save schedule</button></div>\n"
"<div id=\"scheduleDetailMsg\" class=\"msg\"></div>\n"
"<div class=\"schedule-delete-row\"><button class=\"schedule-delete\" onclick=\"deleteScheduleDetail()\">Delete schedule</button></div>\n"
"</div>\n"
"</section>\n"
"\n"
"<section id=\"brandPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Custom Logo / Name</div><div class=\"page-sub\">Personalize the title shown on the main page</div></div></div>\n"
"<div class=\"info-card\">\n"
"<label class=\"field\">Main page text</label><input id=\"brandInput\" type=\"text\" maxlength=\"40\" placeholder=\"Smart Home\">\n"
"<div class=\"small\">This changes the text only; it does not affect relay names or firmware identity.</div>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveBrand()\">Save</button></div>\n"
"<div id=\"brandMsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"relayPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Relay Configuration</div><div class=\"page-sub\">Relay 1-3 are fixed; Relay 4-5 are optional</div></div></div>\n"
"<div class=\"info-card relay-config\"><div id=\"relayConfigList\"></div><div class=\"bar\"><button class=\"primary\" onclick=\"saveRelayConfig()\">Save Relay Configuration</button></div><div id=\"relaymsg\" class=\"msg\"></div></div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"internetPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Internet Connection</div><div class=\"page-sub\">Connect the ESP32 to your home Wi-Fi.</div></div></div>\n"
"<div class=\"info-card\" id=\"wifiCard\">\n"
"<div class=\"small\">Connects the ESP32 to your home Wi-Fi. Applies instantly, no restart needed.</div>\n"
"<label class=\"field\">Home Wi-Fi SSID</label><input id=\"staSsid\" maxlength=\"32\">\n"
"<label class=\"field\">Home Wi-Fi Password</label><input id=\"staPass\" type=\"password\" maxlength=\"63\">\n"
"<div id=\"wifiStatus\" class=\"msg\">Not configured</div>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveWifiSta()\">Connect Wi-Fi</button></div><div id=\"wifiMsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"<section id=\"remotePage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Remote Access</div><div class=\"page-sub\">Optional. Control this device securely from outside your home network.</div></div></div>\n"
"<div class=\"info-card\" id=\"cloudCard\">\n"
"<div class=\"small\">Optional. Enables secure remote control from outside your home network. Applies instantly, no restart needed. Leave all three blank to disable.</div>\n"
"<label class=\"field\">Cloud API URL</label><input id=\"cloudUrl\" type=\"text\" maxlength=\"191\" placeholder=\"https://your-domain.example\">\n"
"<label class=\"field\">Device ID</label><input id=\"deviceId\" type=\"text\" maxlength=\"63\">\n"
"<label class=\"field\">Device Token</label><input id=\"deviceToken\" type=\"password\" maxlength=\"127\">\n"
"<div id=\"cloudStatus\" class=\"msg\">Not configured</div>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveCloud()\">Save Remote Access</button></div><div id=\"cloudMsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"apPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">AP Configuration</div><div class=\"page-sub\">Change the local ESP32 Wi-Fi settings</div></div></div>\n"
"<div class=\"info-card\"><label class=\"field\">SSID</label><input id=\"ssid\" maxlength=\"32\"><label class=\"field\">Password (8-63 characters)</label><input id=\"pass\" type=\"password\" maxlength=\"63\">\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveSettings()\">Save & Restart</button></div><div id=\"setmsg\" class=\"msg\"></div></div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"</div>\n"
"</aside>\n"
"\n"
"<script>\n"
"let relayCfg=[], states=[], relayRequests=Array(5).fill(null), relayTimers=Array(5).fill(null), relaySeq=Array(5).fill(0), relayPending=Array(5).fill(false), schedules=[], userOffline=false, controlsBuilt=false;\n"
"const days=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];\n"
"const $=id=>document.getElementById(id);\n"
"function esc(s){return String(s == null ? '' : s).replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]))}\n"
"\n"
"function fitBrand(){\n"
"  const el=$('brandTitle'); if(!el) return; const box=el.parentElement;\n"
"  let size=25; el.style.fontSize=size+'px';\n"
"  while(el.scrollWidth>box.clientWidth && size>13){size-=1;el.style.fontSize=size+'px';}\n"
"}\n"
"\n"
"function updateOnline(wifi,cloud,offline,timeSynced){\n"
"  userOffline=!!offline;\n"
"  $('onlineDot').classList.toggle('online',!!wifi&&!offline);\n"
"  $('onlineDot').classList.toggle('forced-offline',!!offline);\n"
"  let msg=offline?'Offline mode • Wi-Fi reconnect paused':(wifi?(cloud?'System online • Cloud remote access active':'System online • Local Wi-Fi connected'):'System offline • Waiting for Wi-Fi');\n"
"  if(!offline&&wifi&&!timeSynced)msg+=' • Syncing time (schedules paused)';\n"
"  $('onlineText').textContent=msg;\n"
"  $('onlineText').classList.toggle('online-text',!!wifi&&!offline);\n"
"  const btn=$('connToggle');\n"
"  if(btn){btn.textContent=offline?'▶ Go online':'⏻ Go offline';btn.classList.toggle('active',offline)}\n"
"}\n"
"async function toggleConnectivity(){\n"
"  const btn=$('connToggle');if(btn)btn.disabled=true;\n"
"  try{\n"
"    const r=await fetch('/api/connectivity',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({offline:!userOffline})});\n"
"    if(!r.ok)throw 0;\n"
"    await load();\n"
"  }catch(e){}\n"
"  finally{if(btn)btn.disabled=false}\n"
"}\n"
"function render(a){\n"
"  states=a||states;\n"
"  if(!controlsBuilt){\n"
"    let h='';\n"
"    for(let i=0;i<relayCfg.length;i++){\n"
"      if(!relayCfg[i]?.enabled)continue;\n"
"      const on=!!states[i];\n"
"      h+=`<div class=\"relay-row\" data-i=\"${i}\" onclick=\"rowToggle(${i})\"><div><div class=\"name\">${esc(relayCfg[i].name)}</div><div class=\"state\" id=\"st${i}\">${on?'ON':'OFF'}</div></div><span class=\"switch\"><input type=\"checkbox\" id=\"r${i}\" ${on?'checked':''} tabindex=\"-1\"><span class=\"slider\"></span></span></div>`;\n"
"    }\n"
"    $('controls').innerHTML=h;\n"
"    controlsBuilt=true;\n"
"    return;\n"
"  }\n"
"  for(let i=0;i<relayCfg.length;i++){\n"
"    if(relayPending[i])continue;\n"
"    if(!relayCfg[i]?.enabled)continue;\n"
"    const on=!!states[i];\n"
"    const el=$('r'+i),st=$('st'+i);\n"
"    if(el)el.checked=on;\n"
"    if(st)st.textContent=on?'ON':'OFF';\n"
"  }\n"
"}\n"
"function rowToggle(i){setRelay(i,!states[i])}\n"
"function rebuildControls(){controlsBuilt=false;render(states);}\n"
"async function load(){\n"
" try{\n"
"  const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;const d=await r.json();\n"
"  const cfgChanged=JSON.stringify(d.config)!==JSON.stringify(relayCfg);\n"
"  relayCfg=d.config||relayCfg;\n"
"  if(cfgChanged)controlsBuilt=false;\n"
"  render(d.states||states);\n"
"  const brandText=d.brandName||'Smart Home';\n"
"  if($('brandTitle').textContent!==brandText){$('brandTitle').textContent=brandText;fitBrand();}\n"
"  document.title=brandText;\n"
"  updateOnline(d.wifiConnected,d.cloudOnline,d.userOffline,d.timeSynced);\n"
" }catch(e){updateOnline(false,false,userOffline)}\n"
"}\n"
"function setRelay(i,on){\n"
" const seq=++relaySeq[i],target=on?1:0,previous=states[i]?1:0;states[i]=target;relayPending[i]=true;\n"
" const el=$('r'+i),st=$('st'+i);if(el)el.checked=!!target;if(st)st.textContent=target?'ON':'OFF';\n"
" if(relayTimers[i])clearTimeout(relayTimers[i]);if(relayRequests[i]){try{relayRequests[i].abort()}catch(e){}}\n"
" relayTimers[i]=setTimeout(()=>{const ctl=new AbortController();relayRequests[i]=ctl;fetch(`/api/relay?relay=${i+1}&state=${target}`,{cache:'no-store',signal:ctl.signal})\n"
"  .then(r=>{if(!r.ok)throw new Error('Relay command failed');if(seq===relaySeq[i])relayPending[i]=false;})\n"
"  .catch(e=>{if(e.name==='AbortError')return;if(seq===relaySeq[i]){relayPending[i]=false;states[i]=previous;const x=$('r'+i),s=$('st'+i);if(x)x.checked=!!previous;if(s)s.textContent=previous?'ON':'OFF';}});},35);\n"
"}\n"
"function openSettings(){\n"
" document.body.classList.add('settings-open');$('drawerBackdrop').classList.add('open');$('settingsDrawer').classList.add('open');$('settingsDrawer').setAttribute('aria-hidden','false');\n"
" showSettingsHome();load();\n"
"}\n"
"function closeSettings(){\n"
" const d=$('settingsDrawer');d.classList.remove('open');$('drawerBackdrop').classList.remove('open');document.body.classList.remove('settings-open');d.setAttribute('aria-hidden','true');\n"
" setTimeout(showSettingsHome,340);\n"
"}\n"
"function showSettingsHome(){document.querySelectorAll('.subpage').forEach(p=>p.classList.remove('active'));$('settingsHome').classList.add('active')}\n"
"function openSubPage(id){\n"
" document.querySelectorAll('.subpage').forEach(p=>p.classList.remove('active'));$(id).classList.add('active');\n"
" if(id==='relayPage')renderRelayConfig();if(id==='apPage')loadSettings();if(id==='internetPage')loadWifiStatus();if(id==='remotePage')loadCloudStatus();if(id==='schedulePage')loadSchedules();if(id==='brandPage')loadBrand();\n"
"}\n"
"function backToSettings(){showSettingsHome()}\n"
"\n"
"function renderRelayConfig(){\n"
" let h='';relayCfg.forEach((r,i)=>{const optional=i>=3;\n"
"  h+=`<div class=\"relay-config-item\"><div class=\"relay-config-head\"><div><div class=\"relay-number\">Relay ${i+1}</div><div class=\"relay-gpio\">Relay GPIO ${r.gpio}${optional?' · Optional':''}</div><div class=\"relay-switch-gpio\">Physical Switch GPIO ${r.switchGpio}</div></div>${optional?`<label class=\"small-switch\"><input type=\"checkbox\" id=\"en${i}\" ${r.enabled?'checked':''} onchange=\"relayEnableChanged(${i})\"><span class=\"small-slider\"></span></label>`:''}</div><label class=\"field\">Name</label><input type=\"text\" id=\"rn${i}\" maxlength=\"31\" value=\"${esc(r.name)}\" ${optional&&!r.enabled?'disabled':''}></div>`;\n"
" });$('relayConfigList').innerHTML=h;\n"
"}\n"
"function relayEnableChanged(i){const en=$('en'+i).checked;$('rn'+i).disabled=!en}\n"
"async function saveRelayConfig(){\n"
" let body={};for(let i=0;i<5;i++){const enabled=i<3?true:$('en'+i).checked;let name=$('rn'+i).value.trim()||('Relay '+(i+1));if(name.length>31)return $('relaymsg').textContent='Relay name is too long.';body['r'+(i+1)+'_enabled']=enabled;body['r'+(i+1)+'_name']=name}\n"
" $('relaymsg').textContent='Saving...';try{const r=await fetch('/api/relays',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await r.json().catch(()=>({}));if(!r.ok)throw Error(d.error||'save failed');relayCfg=d.config||relayCfg;$('relaymsg').textContent='Saved successfully.';renderRelayConfig();render(states)}catch(e){$('relaymsg').textContent=e.message||'Save failed.'}\n"
"}\n"
"\n"
"function timeFromMinutes(m){m=((m%1440)+1440)%1440;return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0')}\n"
"function buildTimeOptions(sel,count){let o='';for(let i=0;i<count;i++)o+=`<option value=\"${i}\" ${i===sel?'selected':''}>${String(i).padStart(2,'0')}</option>`;return o}\n"
"function buildHour12Options(sel){let o='';for(let i=1;i<=12;i++)o+=`<option value=\"${i}\" ${i===sel?'selected':''}>${i}</option>`;return o}\n"
"let currentScheduleIndex=-1,scheduleIsNew=false;\n"
"function scheduleListRow(s,idx){\n"
" const relay=Number(s.relay||1),h=Number(s.hour==null?0:s.hour),mi=Number(s.minute==null?0:s.minute),act=Number(s.action==null?1:s.action),en=s.enabled!==false&&s.enabled!==0;\n"
" const h12=h%12===0?12:h%12,ap=h<12?'am':'pm';\n"
" return `<div class=\"list-row${en?'':' row-disabled'}\" onclick=\"openScheduleDetail(${idx})\"><div class=\"list-row-text\"><strong>Relay ${relay} • ${act===1?'ON':'OFF'}</strong><span class=\"list-row-sub\">${h12}:${String(mi).padStart(2,'0')} ${ap}${en?'':' • disabled'}</span></div><span class=\"chevron\">›</span></div>`;\n"
"}\n"
"function renderSchedules(){\n"
" $('scheduleList').innerHTML=schedules.map((s,i)=>scheduleListRow(s,i)).join('')||'<div class=\"small\" style=\"padding:14px 2px\">No schedules yet. Tap Add schedule.</div>';\n"
"}\n"
"async function loadSchedules(){\n"
" $('scheduleMsg').textContent='Loading\\u2026';\n"
" try{\n"
"  const r=await fetch('/api/schedules',{cache:'no-store'});const d=await r.json();\n"
"  if(!r.ok)throw Error(d.error||'Could not load schedules');\n"
"  schedules=d.schedules||[];renderSchedules();\n"
"  $('scheduleMsg').textContent=`${schedules.length} schedule(s) stored on the ESP32.`;\n"
" }catch(e){$('scheduleMsg').textContent=e.message||'Could not load schedules.'}\n"
"}\n"
"function addSchedule(){\n"
" if(schedules.length>=64)return $('scheduleMsg').textContent='Maximum 64 schedules reached.';\n"
" schedules.push({relay:1,hour:0,minute:0,action:1,durationMinutes:0,days:127,enabled:true});\n"
" openScheduleDetail(schedules.length-1,true);\n"
"}\n"
"function openScheduleDetail(idx,isNew){\n"
" currentScheduleIndex=idx;scheduleIsNew=!!isNew;\n"
" const s=schedules[idx]||{};\n"
" const relay=Number(s.relay||1),h=Number(s.hour==null?0:s.hour),mi=Number(s.minute==null?0:s.minute),\n"
" duration=Math.max(0,Math.min(1439,Number(s.durationMinutes||0))),act=Number(s.action==null?1:s.action),\n"
" en=s.enabled!==false&&s.enabled!==0,bits=Number(s.days==null?127:s.days);\n"
" const h12v=h%12===0?12:h%12,ap=h<12?0:1;\n"
" $('sdRelay').innerHTML=[1,2,3,4,5].map(n=>`<option value=\"${n}\" ${relay===n?'selected':''}>Relay ${n}</option>`).join('');\n"
" $('sdAction').innerHTML=`<option value=\"1\" ${act===1?'selected':''}>Turn ON</option><option value=\"0\" ${act===0?'selected':''}>Turn OFF</option>`;\n"
" $('sdHour').innerHTML=buildHour12Options(h12v);\n"
" $('sdMinute').innerHTML=buildTimeOptions(mi,60);\n"
" $('sdAmPm').innerHTML=`<option value=\"0\" ${ap===0?'selected':''}>AM</option><option value=\"1\" ${ap===1?'selected':''}>PM</option>`;\n"
" $('sdDurH').value=Math.floor(duration/60);\n"
" $('sdDurM').value=duration%60;\n"
" $('sdEnabled').checked=en;\n"
" $('scheduleDays').innerHTML=days.map((d,i)=>`<label><input class=\"day\" type=\"checkbox\" data-day=\"${i}\" ${(bits&(1<<i))?'checked':''}>${d}</label>`).join('');\n"
" $('scheduleDetailMsg').textContent='';\n"
" updateScheduleDetailEnd();\n"
" openSubPage('scheduleDetailPage');\n"
"}\n"
"function readScheduleDetail(){\n"
" const relay=+$('sdRelay').value,action=+$('sdAction').value;\n"
" const h12=+$('sdHour').value,ap=+$('sdAmPm').value,minute=+$('sdMinute').value;\n"
" let hour=h12%12;if(ap===1)hour+=12;\n"
" let daysMask=0;document.querySelectorAll('#scheduleDays input.day').forEach(x=>{if(x.checked)daysMask|=1<<Number(x.dataset.day)});\n"
" const durH=Math.max(0,Math.min(23,Number($('sdDurH').value)||0)),durM=Math.max(0,Math.min(59,Number($('sdDurM').value)||0));\n"
" const duration=Math.min(1439,durH*60+durM);\n"
" return {relay,hour,minute,action,durationMinutes:duration,days:daysMask,enabled:$('sdEnabled').checked};\n"
"}\n"
"function updateScheduleDetailEnd(){\n"
" const s=readScheduleDetail();\n"
" const startMin=s.hour*60+s.minute;\n"
" const revertLabel=s.action===1?'Automatically turns OFF at ':'Automatically turns ON at ';\n"
" $('sdEndReadout').textContent=s.durationMinutes>0?(revertLabel+timeFromMinutes(startMin+s.durationMinutes)):'No automatic revert (stays until the next matching event).';\n"
"}\n"
"async function saveScheduleDetail(){\n"
" const s=readScheduleDetail();\n"
" if(s.days<1)return $('scheduleDetailMsg').textContent='Select at least one day.';\n"
" const btn=document.querySelector('#scheduleDetailPage button.primary');if(btn)btn.disabled=true;\n"
" schedules[currentScheduleIndex]=s;\n"
" $('scheduleDetailMsg').textContent='Saving\\u2026';\n"
" try{\n"
"  const r=await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({schedules})});\n"
"  const d=await r.json().catch(()=>({}));\n"
"  if(!r.ok)throw Error(d.error||'Save failed');\n"
"  scheduleIsNew=false;\n"
"  renderSchedules();\n"
"  $('scheduleMsg').textContent='Saved on ESP32. Schedules continue during Wi-Fi or Internet outages.';\n"
"  openSubPage('schedulePage');\n"
" }catch(e){$('scheduleDetailMsg').textContent=e.message||'Could not save schedule.'}\n"
" finally{if(btn)btn.disabled=false}\n"
"}\n"
"async function deleteScheduleDetail(){\n"
" schedules.splice(currentScheduleIndex,1);\n"
" try{\n"
"  await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({schedules})});\n"
" }catch(e){}\n"
" renderSchedules();\n"
" $('scheduleMsg').textContent=`${schedules.length} schedule(s) stored on the ESP32.`;\n"
" openSubPage('schedulePage');\n"
"}\n"
"function backFromScheduleDetail(){\n"
" if(scheduleIsNew)schedules.splice(currentScheduleIndex,1);\n"
" scheduleIsNew=false;\n"
" renderSchedules();\n"
" openSubPage('schedulePage');\n"
"}\n"
"async function saveBrand(){const name=$('brandInput').value.trim(),m=$('brandMsg');if(!name||name.length>40)return m.textContent='Enter 1-40 characters.';m.textContent='Saving…';try{const r=await fetch('/api/brand',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brandName:name})});const d=await r.json().catch(()=>({}));if(!r.ok)throw Error(d.error||'Save failed');$('brandTitle').textContent=name;m.textContent='Saved successfully.'}catch(e){m.textContent=e.message||'Save failed.'}}\n"
"\n"
"async function loadWifiStatus(){try{const r=await fetch('/api/internet',{cache:'no-store'});const d=await r.json();$('staSsid').value=d.staSsid||'';$('staPass').value='';\n"
" $('wifiStatus').textContent=!d.wifiConfigured?'Wi-Fi not configured yet.':d.connected?('Connected to home Wi-Fi. Open this address on any device on the same Wi-Fi: http://'+(d.staIp||'')):'Configured; waiting for connection. Automatic reconnect is active.';\n"
"}catch(e){$('wifiStatus').textContent='Could not read Wi-Fi configuration.'}}\n"
"async function loadCloudStatus(){try{const r=await fetch('/api/internet',{cache:'no-store'});const d=await r.json();$('cloudUrl').value=d.cloudUrl||'';$('deviceId').value=d.deviceId||'';$('deviceToken').value='';\n"
" $('cloudStatus').textContent=!d.cloudConfigured?'Remote access is off.':d.connected?'Remote access is on.':'Remote access is configured; waiting for Wi-Fi.';\n"
"}catch(e){$('cloudStatus').textContent='Could not read remote access configuration.'}}\n"
"async function saveWifiSta(){\n"
" const ssid=$('staSsid').value.trim(),pass=$('staPass').value,m=$('wifiMsg'),btn=document.querySelector('#wifiCard button.primary');\n"
" if(ssid.length<1||ssid.length>32||pass.length<8||pass.length>63)return m.textContent='Enter a valid Wi-Fi SSID and password (8-63 characters).';\n"
" if(btn)btn.disabled=true;m.textContent='Connecting\\u2026';\n"
" try{const r=await fetch('/api/wifi-sta',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:pass})});const d=await r.json().catch(()=>({}));if(!r.ok)throw Error(d.error||'Save failed');m.textContent='Saved. Connecting to Wi-Fi\\u2026';setTimeout(loadWifiStatus,3000)}catch(e){m.textContent=e.message||'Could not save Wi-Fi.'}\n"
" finally{if(btn)btn.disabled=false}\n"
"}\n"
"async function saveCloud(){\n"
" const url=$('cloudUrl').value.trim(),id=$('deviceId').value.trim(),token=$('deviceToken').value.trim(),m=$('cloudMsg'),btn=document.querySelector('#cloudCard button.primary');\n"
" const any=url||id||token;\n"
" if(any&&(!url||!id||!token||!url.startsWith('https://')))return m.textContent='Provide URL + Device ID + Device Token together, with an HTTPS URL, or leave all three blank to disable.';\n"
" if(btn)btn.disabled=true;m.textContent='Saving\\u2026';\n"
" try{const r=await fetch('/api/internet',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cloudUrl:url,deviceId:id,deviceToken:token})});const d=await r.json().catch(()=>({}));if(!r.ok)throw Error(d.error||'Save failed');m.textContent=any?'Remote access saved and active.':'Remote access disabled.';loadCloudStatus()}catch(e){m.textContent=e.message||'Could not save remote access settings.'}\n"
" finally{if(btn)btn.disabled=false}\n"
"}\n"
"async function saveSettings(){const ssid=$('ssid').value,pass=$('pass').value,m=$('setmsg');if(ssid.length<1||ssid.length>32||pass.length<8||pass.length>63)return m.textContent='Invalid SSID or password.';m.textContent='Saving and restarting…';try{const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:pass})});if(!r.ok)throw 0}catch(e){m.textContent='Connection lost. The AP may be restarting.'}}\n"
"async function loadSettings(){try{const r=await fetch('/api/settings',{cache:'no-store'}),d=await r.json();$('ssid').value=d.ssid||''}catch(e){}}\n"
"\n"
"load();setInterval(load,1200);\n"
"</script>\n"
"</body></html>\n"
;

static bool valid_ssid(const char *s)
{
    size_t n = strnlen(s, MAX_AP_SSID_LEN + 1);
    return n >= 1 && n <= MAX_AP_SSID_LEN;
}

static bool valid_password(const char *s)
{
    size_t n = strnlen(s, MAX_AP_PASS_LEN + 1);
    return n >= 8 && n <= MAX_AP_PASS_LEN;
}

static bool valid_relay_name(const char *s)
{
    size_t n = strnlen(s, MAX_RELAY_NAME_LEN + 1);
    if (n < 1 || n > MAX_RELAY_NAME_LEN) return false;

    
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') return false;
    }
    return true;
}

static bool valid_brand_name(const char *s)
{
    size_t n = strnlen(s, MAX_BRAND_LEN + 1);
    if (n < 1 || n > MAX_BRAND_LEN) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') return false;
    }
    return true;
}

static esp_err_t save_brand_name(const char *name)
{
    if (!valid_brand_name(name)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_BRAND_NAME, name);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    if (err == ESP_OK) strlcpy(brand_name, name, sizeof(brand_name));
    return err;
}

static void load_defaults(void)
{
    strlcpy(brand_name, "Smart Home", sizeof(brand_name));
    strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
    strlcpy(ap_password, DEFAULT_AP_PASSWORD, sizeof(ap_password));

    for (int i = 0; i < RELAY_COUNT; ++i) {
        relay_state[i] = 0;
        relay_enabled[i] = (i < 3);
    }

    strlcpy(relay_name[0], "Living Room Light", sizeof(relay_name[0]));
    strlcpy(relay_name[1], "Ceiling Fan", sizeof(relay_name[1]));
    strlcpy(relay_name[2], "Charging Socket", sizeof(relay_name[2]));
    strlcpy(relay_name[3], "Relay 4", sizeof(relay_name[3]));
    strlcpy(relay_name[4], "Relay 5", sizeof(relay_name[4]));
}

static void load_nvs(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No existing config; using defaults");
        return;
    }

    uint8_t states[RELAY_COUNT] = {0};
    size_t sz = sizeof(states);
    if (nvs_get_blob(h, NVS_KEY_RELAY_STATES, states, &sz) == ESP_OK && sz == sizeof(states)) {
        for (int i = 0; i < RELAY_COUNT; ++i) relay_state[i] = states[i] ? 1 : 0;
    }

    uint8_t enabled[RELAY_COUNT] = {1, 1, 1, 0, 0};
    sz = sizeof(enabled);
    if (nvs_get_blob(h, NVS_KEY_RELAY_ENABLED, enabled, &sz) == ESP_OK && sz == sizeof(enabled)) {
        for (int i = 0; i < RELAY_COUNT; ++i) {
            relay_enabled[i] = (i < 3) ? true : (enabled[i] != 0);
        }
    }

    size_t names_sz = sizeof(relay_name);
    if (nvs_get_blob(h, NVS_KEY_RELAY_NAMES, relay_name, &names_sz) == ESP_OK &&
        names_sz == sizeof(relay_name)) {
        for (int i = 0; i < RELAY_COUNT; ++i) {
            relay_name[i][MAX_RELAY_NAME_LEN] = '\0';
            if (!valid_relay_name(relay_name[i])) {
                if (i == 0) strlcpy(relay_name[i], "Living Room Light", sizeof(relay_name[i]));
                else if (i == 1) strlcpy(relay_name[i], "Ceiling Fan", sizeof(relay_name[i]));
                else if (i == 2) strlcpy(relay_name[i], "Charging Socket", sizeof(relay_name[i]));
                else {
                    snprintf(relay_name[i], sizeof(relay_name[i]), "Relay %d", i + 1);
                }
            }
        }
    }

    char tmp_ssid[MAX_AP_SSID_LEN + 1] = {0};
    sz = sizeof(tmp_ssid);
    if (nvs_get_str(h, NVS_KEY_AP_SSID, tmp_ssid, &sz) == ESP_OK && valid_ssid(tmp_ssid)) {
        strlcpy(ap_ssid, tmp_ssid, sizeof(ap_ssid));
    }

    char tmp_pass[MAX_AP_PASS_LEN + 1] = {0};
    sz = sizeof(tmp_pass);
    if (nvs_get_str(h, NVS_KEY_AP_PASS, tmp_pass, &sz) == ESP_OK && valid_password(tmp_pass)) {
        strlcpy(ap_password, tmp_pass, sizeof(ap_password));
    }

    char tmp_brand[MAX_BRAND_LEN + 1] = {0};
    sz = sizeof(tmp_brand);
    if (nvs_get_str(h, NVS_KEY_BRAND_NAME, tmp_brand, &sz) == ESP_OK &&
        strlen(tmp_brand) >= 1 && strlen(tmp_brand) <= MAX_BRAND_LEN) {
        strlcpy(brand_name, tmp_brand, sizeof(brand_name));
    }

    char tmp_sta_ssid[MAX_AP_SSID_LEN + 1] = {0};
    sz = sizeof(tmp_sta_ssid);
    if (nvs_get_str(h, NVS_KEY_STA_SSID, tmp_sta_ssid, &sz) == ESP_OK && valid_ssid(tmp_sta_ssid)) strlcpy(sta_ssid, tmp_sta_ssid, sizeof(sta_ssid));
    char tmp_sta_pass[MAX_AP_PASS_LEN + 1] = {0};
    sz = sizeof(tmp_sta_pass);
    if (nvs_get_str(h, NVS_KEY_STA_PASS, tmp_sta_pass, &sz) == ESP_OK && valid_password(tmp_sta_pass)) strlcpy(sta_password, tmp_sta_pass, sizeof(sta_password));
    sz = sizeof(cloud_url);
    if (nvs_get_str(h, NVS_KEY_CLOUD_URL, cloud_url, &sz) != ESP_OK) cloud_url[0] = '\0';
    sz = sizeof(device_id);
    if (nvs_get_str(h, NVS_KEY_DEVICE_ID, device_id, &sz) != ESP_OK) device_id[0] = '\0';
    sz = sizeof(device_token);
    if (nvs_get_str(h, NVS_KEY_DEVICE_TOKEN, device_token, &sz) != ESP_OK) device_token[0] = '\0';

    nvs_close(h);
    ESP_LOGI(TAG, "Internet config: STA=%s cloud=%s device=%s", sta_ssid[0] ? "configured" : "not configured", cloud_url[0] ? cloud_url : "none", device_id[0] ? device_id : "none");
    ESP_LOGI(TAG, "Restored relay states: %d %d %d %d %d",
             relay_state[0], relay_state[1], relay_state[2], relay_state[3], relay_state[4]);
    ESP_LOGI(TAG, "Relay enabled: %d %d %d %d %d",
             relay_enabled[0], relay_enabled[1], relay_enabled[2], relay_enabled[3], relay_enabled[4]);
}

static esp_err_t save_relay_states(void)
{
    uint8_t states[RELAY_COUNT];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) states[i] = relay_state[i] ? 1 : 0;
    xSemaphoreGive(relay_mutex);

    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_RELAY_STATES, states, sizeof(states));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);

    if (err != ESP_OK) ESP_LOGE(TAG, "Relay NVS save failed: %s", esp_err_to_name(err));
    return err;
}

static void relay_save_task(void *arg)
{
    (void)arg;
    while (1) {
        
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(250));
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {}
        save_relay_states();
    }
}

static esp_err_t save_relay_config(void)
{
    uint8_t enabled[RELAY_COUNT];
    uint8_t states[RELAY_COUNT];
    char names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) {
        enabled[i] = relay_enabled[i] ? 1 : 0;
        states[i] = relay_state[i] ? 1 : 0;
    }
    memcpy(names, relay_name, sizeof(names));

    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_RELAY_ENABLED, enabled, sizeof(enabled));
        if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_RELAY_NAMES, names, sizeof(names));
        if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_RELAY_STATES, states, sizeof(states));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    xSemaphoreGive(relay_mutex);

    if (err != ESP_OK) ESP_LOGE(TAG, "Relay config NVS save failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t save_ap_settings(const char *ssid, const char *password)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_AP_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_AP_PASS, password);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);

    if (err == ESP_OK) {
        strlcpy(ap_ssid, ssid, sizeof(ap_ssid));
        strlcpy(ap_password, password, sizeof(ap_password));
    }
    return err;
}


static esp_err_t save_wifi_sta_settings(const char *ssid, const char *pass)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STA_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_STA_PASS, pass);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    if (err == ESP_OK) {
        strlcpy(sta_ssid, ssid, sizeof(sta_ssid));
        strlcpy(sta_password, pass, sizeof(sta_password));
    }
    return err;
}

static esp_err_t save_cloud_settings(const char *url, const char *id, const char *token)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_CLOUD_URL, url);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DEVICE_ID, id);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DEVICE_TOKEN, token);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    if (err == ESP_OK) {
        strlcpy(cloud_url, url, sizeof(cloud_url));
        strlcpy(device_id, id, sizeof(device_id));
        strlcpy(device_token, token, sizeof(device_token));
    }
    return err;
}

static gpio_num_t relay_gpio(int index);
static int relay_output_level(int logical_state);

static void apply_remote_relay_state(int index, int state)
{
    if (index < 0 || index >= RELAY_COUNT) return;
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relay_enabled[index]) {
        relay_state[index] = state ? 1 : 0;
        gpio_set_level(relay_gpio(index), relay_output_level(relay_state[index]));
    }
    xSemaphoreGive(relay_mutex);
    xTaskNotifyGive(relay_save_task_handle);
}

static void get_relay_snapshot(int *states, bool *enabled)
{
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(states, relay_state, sizeof(int) * RELAY_COUNT);
    memcpy(enabled, relay_enabled, sizeof(bool) * RELAY_COUNT);
    xSemaphoreGive(relay_mutex);
}

static void cloud_command_cb(int relay, int state, void *ctx)
{
    apply_remote_relay_state(relay, state);
}

static void cloud_snapshot_cb(int *states, bool *enabled, void *ctx)
{
    get_relay_snapshot(states, enabled);
}

static int relay_output_level(int logical_state)
{
    return logical_state ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL;
}

static gpio_num_t relay_gpio(int index)
{
    static const gpio_num_t pins[RELAY_COUNT] = {
        RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO, RELAY4_GPIO, RELAY5_GPIO
    };
    return pins[index];
}

static void apply_all_relays(void)
{
    int s[RELAY_COUNT];
    bool enabled[RELAY_COUNT];

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    memcpy(enabled, relay_enabled, sizeof(enabled));
    xSemaphoreGive(relay_mutex);

    for (int i = 0; i < RELAY_COUNT; ++i) {
        gpio_set_level(relay_gpio(i),
                       (enabled[i] && s[i]) ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
    }
}

static void init_relays(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < RELAY_COUNT; ++i) mask |= (1ULL << relay_gpio(i));

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    
    for (int i = 0; i < RELAY_COUNT; ++i) {
        gpio_set_level(relay_gpio(i), relay_output_level(0));
    }
}

static gpio_num_t switch_gpio(int index)
{
    static const gpio_num_t pins[SWITCH_COUNT] = {
        SWITCH1_GPIO, SWITCH2_GPIO, SWITCH3_GPIO, SWITCH4_GPIO, SWITCH5_GPIO
    };
    return pins[index];
}

static bool read_switch_state(int index)
{
    return gpio_get_level(switch_gpio(index)) == SWITCH_ACTIVE_LEVEL;
}

static void init_switches(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < SWITCH_COUNT; ++i) mask |= (1ULL << switch_gpio(i));

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void apply_switch_command(int index, bool on)
{
    bool changed = false;

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relay_enabled[index] && relay_state[index] != (int)on) {
        relay_state[index] = on ? 1 : 0;
        gpio_set_level(relay_gpio(index), relay_output_level(on ? 1 : 0));
        changed = true;
    }
    xSemaphoreGive(relay_mutex);
    if (changed) schedule_note_manual_change(index);

    if (changed) {
        
        
        xTaskNotifyGive(relay_save_task_handle);
    }
}

static void physical_switch_task(void *arg)
{
    int last_raw[SWITCH_COUNT];
    int stable[SWITCH_COUNT];
    uint8_t samples[SWITCH_COUNT] = {0};

    esp_task_wdt_add(NULL);

    for (int i = 0; i < SWITCH_COUNT; ++i) {
        last_raw[i] = gpio_get_level(switch_gpio(i));
        
        stable[i] = last_raw[i];
        samples[i] = SWITCH_DEBOUNCE_SAMPLES;
    }

    while (1) {
        for (int i = 0; i < SWITCH_COUNT; ++i) {
            int raw = gpio_get_level(switch_gpio(i));

            if (raw == last_raw[i]) {
                if (samples[i] < SWITCH_DEBOUNCE_SAMPLES) samples[i]++;
            } else {
                last_raw[i] = raw;
                samples[i] = 0;
            }

            if (samples[i] >= SWITCH_DEBOUNCE_SAMPLES && stable[i] != raw) {
                stable[i] = raw;
                apply_switch_command(i, raw == SWITCH_ACTIVE_LEVEL);
                ESP_LOGI(TAG, "Physical switch %d -> %s", i + 1,
                         (raw == SWITCH_ACTIVE_LEVEL) ? "ON" : "OFF");
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SWITCH_POLL_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started");
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Local client connected");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Local client disconnected");
    } else if (id == WIFI_EVENT_STA_START) {
        sta_retry_count = 0;
        if (sta_ssid[0]) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "Initial STA connect request failed: %s", esp_err_to_name(err));
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_connected = false;
        sta_ip[0] = '\0';
        if (!sta_ssid[0] || user_offline_mode) return;

        if (sta_retry_count < 10) sta_retry_count++;
        esp_err_t err = esp_wifi_connect();
        ESP_LOGW(TAG, "STA disconnected (retry %u): %s",
                 (unsigned)sta_retry_count, esp_err_to_name(err));
    }
}

static bool g_sntp_started = false;

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        sta_retry_count = 0;
        sta_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA connected; internet features enabled; local IP: %s", sta_ip);
        if (!g_sntp_started) {
            g_sntp_started = true;
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }
}

static void wifi_init_ap_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!ap_netif || !sta_netif) ESP_ERROR_CHECK(ESP_FAIL);

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_IP_ADDR, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_GW_ADDR, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_NETMASK, &ip_info.netmask));
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t w_any, ip_any;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &w_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, &ip_any));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, ap_password, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ap_ssid); ap.ap.channel = DEFAULT_AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONNECTIONS; ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false; ap.ap.pmf_cfg.capable = true;

    wifi_config_t sta = {0};
    if (sta_ssid[0]) {
        strlcpy((char *)sta.sta.ssid, sta_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, sta_password, sizeof(sta.sta.password));
        
        sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta.sta.pmf_cfg.capable = true;
        sta.sta.pmf_cfg.required = false;
        sta.sta.failure_retry_cnt = 7;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (sta_ssid[0]) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(60);
    ESP_LOGI(TAG, "AP SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "AP IP: %s", AP_IP_ADDR);
}

static int find_question_end(const uint8_t *buf, int len)
{
    if (len < 17) return -1;
    int p = 12;
    int jumps = 0;
    while (p < len && jumps++ < 64) {
        uint8_t l = buf[p++];
        if (l == 0) {
            if (p + 4 > len) return -1;
            return p + 4;
        }
        if ((l & 0xC0) != 0 || l > 63 || p + l > len) return -1;
        p += l;
    }
    return -1;
}

static int build_dns_answer(uint8_t *out, int out_cap, const uint8_t *query, int qlen)
{
    int qend = find_question_end(query, qlen);
    if (qend < 0 || qend + 16 > out_cap || qend > qlen) return -1;

    memcpy(out, query, qend);
    out[2] = 0x81; out[3] = 0x80;
    out[4] = 0x00; out[5] = 0x01;
    out[6] = 0x00; out[7] = 0x01;
    out[8] = out[9] = out[10] = out[11] = 0;

    int p = qend;
    out[p++] = 0xC0; out[p++] = 0x0C;
    out[p++] = 0x00; out[p++] = 0x01;
    out[p++] = 0x00; out[p++] = 0x01;
    out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x3C;
    out[p++] = 0x00; out[p++] = 0x04;
    out[p++] = 192; out[p++] = 168; out[p++] = 4; out[p++] = 1;
    return p;
}

static void dns_task(void *arg)
{
    uint8_t rx[DNS_RX_SIZE];
    uint8_t tx[DNS_RX_SIZE + 32];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = inet_addr(AP_IP_ADDR);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Local DNS started on UDP/53");
    esp_task_wdt_add(NULL);

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            esp_task_wdt_reset();
            continue;
        }

        int out_len = build_dns_answer(tx, sizeof(tx), rx, n);
        if (out_len > 0) {
            sendto(sock, tx, out_len, 0, (struct sockaddr *)&from, from_len);
        }
        esp_task_wdt_reset();
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *json, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t redirect_to_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    int s[RELAY_COUNT];
    bool enabled[RELAY_COUNT];
    char names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    memcpy(enabled, relay_enabled, sizeof(enabled));
    memcpy(names, relay_name, sizeof(names));
    xSemaphoreGive(relay_mutex);

    char json[1400];
    int pos = snprintf(json, sizeof(json),
                       "{\"states\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos, "%d%s", s[i], i == RELAY_COUNT - 1 ? "" : ",");
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "],\"config\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"enabled\":%s,\"name\":\"%s\",\"gpio\":%d,\"switchGpio\":%d}%s",
                        enabled[i] ? "true" : "false",
                        names[i],
                        (int)relay_gpio(i),
                        (int)switch_gpio(i),
                        i == RELAY_COUNT - 1 ? "" : ",");
    }
    pos += snprintf(json + pos, sizeof(json) - pos,
                    "],\"brandName\":\"%s\",\"wifiConnected\":%s,\"cloudOnline\":%s,\"userOffline\":%s,\"timeSynced\":%s,\"staIp\":\"%s\"}",
                    brand_name,
                    sta_connected ? "true" : "false",
                    cloud_client_is_online() ? "true" : "false",
                    user_offline_mode ? "true" : "false",
                    g_time_synced ? "true" : "false",
                    sta_ip);

    return send_json(req, json, "200 OK");
}

static esp_err_t relay_handler(httpd_req_t *req)
{

    char query[128];
    char value[20];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return send_json(req, "{\"error\":\"missing query\"}", "400 Bad Request");

    if (httpd_query_key_value(query, "relay", value, sizeof(value)) != ESP_OK)
        return send_json(req, "{\"error\":\"relay\"}", "400 Bad Request");

    char *end = NULL;
    long relay = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || relay < 1 || relay > RELAY_COUNT)
        return send_json(req, "{\"error\":\"relay\"}", "400 Bad Request");

    if (httpd_query_key_value(query, "state", value, sizeof(value)) != ESP_OK)
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");

    end = NULL;
    long state = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || (state != 0 && state != 1))
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");

    int idx = (int)relay - 1;

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (!relay_enabled[idx]) {
        xSemaphoreGive(relay_mutex);
        return send_json(req, "{\"error\":\"relay disabled\"}", "409 Conflict");
    }

    relay_state[idx] = (int)state;
    gpio_set_level(relay_gpio(idx), relay_output_level((int)state));
    xSemaphoreGive(relay_mutex);
    schedule_note_manual_change(idx);

    xTaskNotifyGive(relay_save_task_handle);
    return send_json(req, "{\"ok\":true}", "200 OK");
}

static bool json_extract_string(const char *body, const char *key, char *out, size_t out_sz);

static esp_err_t connectivity_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 256)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[257];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) return send_json(req, "{\"error\":\"invalid JSON\"}", "400 Bad Request");
    cJSON *v = cJSON_GetObjectItem(root, "offline");
    bool offline = cJSON_IsBool(v) ? cJSON_IsTrue(v) : false;
    cJSON_Delete(root);

    user_offline_mode = offline;
    cloud_client_set_offline(offline);

    if (offline) {
        if (sta_ssid[0]) esp_wifi_disconnect();
    } else {
        if (sta_ssid[0]) esp_wifi_connect();
    }

    return send_json(req, offline ? "{\"ok\":true,\"userOffline\":true}" : "{\"ok\":true,\"userOffline\":false}", "200 OK");
}

static esp_err_t internet_get_handler(httpd_req_t *req)
{
    char json[768];
    bool wifi_configured = sta_ssid[0] != '\0';
    bool cloud_configured = cloud_url[0] != '\0' &&
                            device_id[0] != '\0' &&
                            device_token[0] != '\0';

    
    snprintf(json, sizeof(json),
             "{\"staSsid\":\"%s\",\"cloudUrl\":\"%s\",\"deviceId\":\"%s\",\"staIp\":\"%s\","
             "\"wifiConfigured\":%s,\"cloudConfigured\":%s,\"connected\":%s}",
             sta_ssid, cloud_url, device_id, sta_ip,
             wifi_configured ? "true" : "false",
             cloud_configured ? "true" : "false",
             sta_connected ? "true" : "false");
    return send_json(req, json, "200 OK");
}

static esp_err_t wifi_sta_post_handler(httpd_req_t *req)
{

    if (req->content_len <= 0 || req->content_len > 512)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[513];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    char ssid[MAX_AP_SSID_LEN + 1] = {0};
    char pass[MAX_AP_PASS_LEN + 1] = {0};

    if (!json_extract_string(body, "ssid", ssid, sizeof(ssid)) ||
        !json_extract_string(body, "password", pass, sizeof(pass)) ||
        !valid_ssid(ssid) || !valid_password(pass)) {
        return send_json(req, "{\"error\":\"valid Wi-Fi SSID/password are required\"}",
                         "400 Bad Request");
    }

    if (save_wifi_sta_settings(ssid, pass) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    sta_retry_count = 0;
    if (!user_offline_mode) esp_wifi_connect();

    return send_json(req, "{\"ok\":true,\"restarting\":false}", "200 OK");
}

static esp_err_t internet_post_handler(httpd_req_t *req)
{

    if (req->content_len <= 0 || req->content_len > 1024)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[1025];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    char url[MAX_CLOUD_URL_LEN + 1] = {0};
    char id[MAX_DEVICE_ID_LEN + 1] = {0};
    char token[MAX_DEVICE_TOKEN_LEN + 1] = {0};

    bool have_url = json_extract_string(body, "cloudUrl", url, sizeof(url)) && url[0];
    bool have_id = json_extract_string(body, "deviceId", id, sizeof(id)) && id[0];
    bool have_token = json_extract_string(body, "deviceToken", token, sizeof(token)) && token[0];

    if (!have_url && !have_id && !have_token) {
        url[0] = 0; id[0] = 0; token[0] = 0;
    } else if (!have_url || !have_id || !have_token ||
        strncmp(url, "https://", 8) != 0 ||
        strlen(id) < 3 || strlen(id) > MAX_DEVICE_ID_LEN ||
        strlen(token) < 16) {
        return send_json(req,
                         "{\"error\":\"provide Cloud URL, Device ID and Device Token together; HTTPS is required\"}",
                         "400 Bad Request");
    }

    if (save_cloud_settings(url, id, token) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");

    cloud_client_set_credentials(url, id, token);

    return send_json(req, "{\"ok\":true,\"restarting\":false}", "200 OK");
}
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ssid\":\"%s\",\"brandName\":\"%s\"}", ap_ssid, brand_name);
    return send_json(req, json, "200 OK");
}

static bool json_extract_string(const char *body, const char *key, char *out, size_t out_sz)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) return false;
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

static bool json_extract_bool(const char *body, const char *key, bool *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[1025];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    char ssid[MAX_AP_SSID_LEN + 1];
    char pass[MAX_AP_PASS_LEN + 1];
    if (!json_extract_string(body, "ssid", ssid, sizeof(ssid)) ||
        !json_extract_string(body, "password", pass, sizeof(pass)) ||
        !valid_ssid(ssid) || !valid_password(pass)) {
        return send_json(req, "{\"error\":\"invalid SSID/password\"}", "400 Bad Request");
    }

    if (save_ap_settings(ssid, pass) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

static esp_err_t brand_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[513], name[MAX_BRAND_LEN + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';
    if (!json_extract_string(body, "brandName", name, sizeof(name)) || !valid_brand_name(name))
        return send_json(req, "{\"error\":\"invalid brand name\"}", "400 Bad Request");
    if (save_brand_name(name) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"brandName\":\"%s\"}", brand_name);
    return send_json(req, json, "200 OK");
}

static bool schedule_time_is_active(const cloud_schedule_t *s, const struct tm *tmv, int now_minute)
{
    if (!s || !s->enabled || s->duration_minutes <= 0) return false;
    int start = s->hour * 60 + s->minute;
    int elapsed;
    int day = tmv->tm_wday;
    if (now_minute >= start) {
        if (!(s->days & (1 << day))) return false;
        elapsed = now_minute - start;
    } else {
        day = (day + 6) % 7;
        if (!(s->days & (1 << day))) return false;
        elapsed = now_minute + 1440 - start;
    }
    return elapsed >= 0 && elapsed < s->duration_minutes;
}

static void schedule_set_relay(int index, int state)
{
    if (index < 0 || index >= RELAY_COUNT) return;
    bool changed = false;
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relay_enabled[index] && relay_state[index] != state) {
        relay_state[index] = state ? 1 : 0;
        gpio_set_level(relay_gpio(index), relay_output_level(relay_state[index]));
        changed = true;
    }
    xSemaphoreGive(relay_mutex);
    if (changed) xTaskNotifyGive(relay_save_task_handle);
}

static void schedule_task(void *arg)
{
    cloud_schedule_t local[CLOUD_SCHEDULE_MAX];
    for (;;) {
        time_t now = time(NULL);
        struct tm tmv;
        g_time_synced = (now >= 1700000000);
        if (!g_time_synced) {
            static uint32_t warn_count = 0;
            if ((warn_count++ % 30) == 0) {
                ESP_LOGW(TAG, "Clock not yet synced via NTP; schedules are paused until time sync completes");
            }
        }
        if (g_time_synced && localtime_r(&now, &tmv) != NULL) {
            size_t n = cloud_client_get_schedules(local, CLOUD_SCHEDULE_MAX);
            int now_minute = tmv.tm_hour * 60 + tmv.tm_min;
            bool explicit_event[RELAY_COUNT] = {false,false,false,false,false};
            int explicit_state[RELAY_COUNT] = {0,0,0,0,0};
            bool active[RELAY_COUNT] = {false,false,false,false,false};
            int active_state[RELAY_COUNT] = {0,0,0,0,0};

            for (size_t i = 0; i < n; ++i) {
                cloud_schedule_t *s = &local[i];
                if (!s->enabled || s->relay < 1 || s->relay > RELAY_COUNT) continue;
                int r = s->relay - 1;
                if (schedule_time_is_active(s, &tmv, now_minute)) {
                    active[r] = true;
                    active_state[r] = s->action ? 1 : 0;
                }
                if ((s->days & (1 << tmv.tm_wday)) &&
                    s->hour == tmv.tm_hour && s->minute == tmv.tm_min) {
                    explicit_event[r] = true;
                    explicit_state[r] = s->action ? 1 : 0;
                }
            }

            for (int r = 0; r < RELAY_COUNT; ++r) {
                if (explicit_event[r]) {
                    schedule_set_relay(r, explicit_state[r]);
                    schedule_was_active[r] = active[r];
                    schedule_revert_state[r] = active[r] ? (1 - active_state[r]) : explicit_state[r];
                    schedule_override[r] = false;
                } else if (active[r]) {
                    schedule_revert_state[r] = 1 - active_state[r];
                    if (!schedule_override[r]) schedule_set_relay(r, active_state[r]);
                    schedule_was_active[r] = true;
                } else if (schedule_was_active[r]) {
                    if (!schedule_override[r]) schedule_set_relay(r, schedule_revert_state[r]);
                    schedule_was_active[r] = false;
                    schedule_override[r] = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t schedules_handler(httpd_req_t *req)
{

    if (req->method == HTTP_GET) {
        cloud_schedule_t items[CLOUD_SCHEDULE_MAX];
        size_t n = cloud_client_get_schedules(items, CLOUD_SCHEDULE_MAX);
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        if (!root || !arr) {
            if (root) cJSON_Delete(root);
            if (arr) cJSON_Delete(arr);
            return send_json(req, "{\"error\":\"out of memory\"}", "500 Internal Server Error");
        }
        for (size_t i = 0; i < n; ++i) {
            cJSON *o = cJSON_CreateObject();
            if (!o) { cJSON_Delete(root); return send_json(req, "{\"error\":\"out of memory\"}", "500 Internal Server Error"); }
            cJSON_AddNumberToObject(o, "id", (double)i);
            cJSON_AddBoolToObject(o, "enabled", items[i].enabled);
            cJSON_AddNumberToObject(o, "relay", items[i].relay);
            cJSON_AddNumberToObject(o, "hour", items[i].hour);
            cJSON_AddNumberToObject(o, "minute", items[i].minute);
            cJSON_AddNumberToObject(o, "action", items[i].action);
            cJSON_AddNumberToObject(o, "days", items[i].days);
            cJSON_AddNumberToObject(o, "durationMinutes", items[i].duration_minutes);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(root, "schedules", arr);
        char *out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!out) return send_json(req, "{\"error\":\"out of memory\"}", "500 Internal Server Error");
        esp_err_t err = send_json(req, out, "200 OK");
        free(out);
        return err;
    }

    if (req->method != HTTP_POST || req->content_len <= 0 || req->content_len > 20000)
        return send_json(req, "{\"error\":\"invalid request\"}", "400 Bad Request");

    char *body = calloc(1, req->content_len + 1);
    if (!body) return send_json(req, "{\"error\":\"out of memory\"}", "500 Internal Server Error");
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { free(body); return ESP_FAIL; }
        received += (size_t)n;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json(req, "{\"error\":\"invalid JSON\"}", "400 Bad Request");

    cJSON *arr = cJSON_GetObjectItem(root, "schedules");
    int arr_count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    if (arr_count < 0 || arr_count > CLOUD_SCHEDULE_MAX) {
        cJSON_Delete(root);
        return send_json(req, "{\"error\":\"maximum 64 schedules\"}", "400 Bad Request");
    }

    cloud_schedule_t *items = NULL;
    if (arr_count > 0) {
        items = calloc((size_t)arr_count, sizeof(cloud_schedule_t));
        if (!items) {
            cJSON_Delete(root);
            return send_json(req, "{\"error\":\"out of memory\"}", "500 Internal Server Error");
        }
    }
    bool valid = true;
    for (int i = 0; i < arr_count; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *v;
        if (!cJSON_IsObject(o)) { valid = false; break; }
        items[i].id = i;
        items[i].enabled = (v=cJSON_GetObjectItem(o,"enabled")) ? cJSON_IsTrue(v) : false;
        items[i].relay = (v=cJSON_GetObjectItem(o,"relay")) ? v->valueint : 0;
        items[i].hour = (v=cJSON_GetObjectItem(o,"hour")) ? v->valueint : -1;
        items[i].minute = (v=cJSON_GetObjectItem(o,"minute")) ? v->valueint : -1;
        items[i].action = (v=cJSON_GetObjectItem(o,"action")) ? v->valueint : -1;
        items[i].days = (v=cJSON_GetObjectItem(o,"days")) ? v->valueint : 0;
        items[i].duration_minutes = (v=cJSON_GetObjectItem(o,"durationMinutes")) ? v->valueint : 0;
        if (items[i].relay < 1 || items[i].relay > RELAY_COUNT ||
            items[i].hour > 23 ||
            items[i].minute > 59 ||
            (items[i].action != 0 && items[i].action != 1) ||
            items[i].days < 1 || items[i].days > 127 ||
            items[i].duration_minutes > 1439) {
            valid = false;
            break;
        }
    }
    cJSON_Delete(root);

    if (!valid) {
        free(items);
        return send_json(req, "{\"error\":\"invalid schedule entry\"}", "400 Bad Request");
    }

    bool saved = cloud_client_replace_schedules(items, (size_t)arr_count);
    free(items);
    if (!saved)
        return send_json(req, "{\"error\":\"could not save schedules\"}", "500 Internal Server Error");

    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"count\":%d}", arr_count);
    return send_json(req, out, "200 OK");
}

static esp_err_t relay_config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[2049];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    bool new_enabled[RELAY_COUNT];
    char new_names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    for (int i = 0; i < RELAY_COUNT; ++i) {
        char key[16];

        if (i < 3) {
            new_enabled[i] = true;
        } else {
            snprintf(key, sizeof(key), "r%d_enabled", i + 1);
            if (!json_extract_bool(body, key, &new_enabled[i])) {
                return send_json(req, "{\"error\":\"invalid relay enable state\"}", "400 Bad Request");
            }
        }

        snprintf(key, sizeof(key), "r%d_name", i + 1);
        if (!json_extract_string(body, key, new_names[i], sizeof(new_names[i])) ||
            !valid_relay_name(new_names[i])) {
            return send_json(req, "{\"error\":\"invalid relay name\"}", "400 Bad Request");
        }
    }

    bool old_enabled[RELAY_COUNT];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(old_enabled, relay_enabled, sizeof(old_enabled));
    for (int i = 0; i < RELAY_COUNT; ++i) {
        relay_enabled[i] = new_enabled[i];
        strlcpy(relay_name[i], new_names[i], sizeof(relay_name[i]));

        if (!relay_enabled[i]) {
            relay_state[i] = 0;
            gpio_set_level(relay_gpio(i), relay_output_level(0));
        } else if (i >= 3 && !old_enabled[i]) {
            
            bool on = read_switch_state(i);
            relay_state[i] = on ? 1 : 0;
            gpio_set_level(relay_gpio(i), relay_output_level(on ? 1 : 0));
        }
    }
    xSemaphoreGive(relay_mutex);

    esp_err_t err = save_relay_config();
    if (err != ESP_OK)
        return send_json(req, "{\"error\":\"configuration save failed\"}", "500 Internal Server Error");

    
    char json[1024];
    int pos = snprintf(json, sizeof(json), "{\"config\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"enabled\":%s,\"name\":\"%s\",\"gpio\":%d,\"switchGpio\":%d}%s",
                        relay_enabled[i] ? "true" : "false",
                        relay_name[i],
                        (int)relay_gpio(i),
                        (int)switch_gpio(i),
                        i == RELAY_COUNT - 1 ? "" : ",");
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

    return send_json(req, json, "200 OK");
}

static esp_err_t captive_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 24;
    config.stack_size = 6144;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;

    if (httpd_start(&http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_handler};
    httpd_uri_t status = {.uri="/api/status", .method=HTTP_GET, .handler=status_handler};
    httpd_uri_t relay = {.uri="/api/relay", .method=HTTP_GET, .handler=relay_handler};
    httpd_uri_t internet_get = {.uri="/api/internet", .method=HTTP_GET, .handler=internet_get_handler};
    httpd_uri_t internet_post = {.uri="/api/internet", .method=HTTP_POST, .handler=internet_post_handler};
    httpd_uri_t wifi_sta_post = {.uri="/api/wifi-sta", .method=HTTP_POST, .handler=wifi_sta_post_handler};
    httpd_uri_t settings_get = {.uri="/api/settings", .method=HTTP_GET, .handler=settings_get_handler};
    httpd_uri_t settings_post = {.uri="/api/settings", .method=HTTP_POST, .handler=settings_post_handler};
    httpd_uri_t brand_post = {.uri="/api/brand", .method=HTTP_POST, .handler=brand_post_handler};
    httpd_uri_t schedules_get = {.uri="/api/schedules", .method=HTTP_GET, .handler=schedules_handler};
    httpd_uri_t schedules_post = {.uri="/api/schedules", .method=HTTP_POST, .handler=schedules_handler};
    httpd_uri_t relay_config_post = {.uri="/api/relays", .method=HTTP_POST, .handler=relay_config_post_handler};
    httpd_uri_t connectivity_post = {.uri="/api/connectivity", .method=HTTP_POST, .handler=connectivity_post_handler};

    httpd_uri_t c1 = {.uri="/generate_204", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c2 = {.uri="/hotspot-detect.html", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c3 = {.uri="/connecttest.txt", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c4 = {.uri="/ncsi.txt", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c5 = {.uri="/connectivitycheck.gstatic.com/generate_204", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c6 = {.uri="/success.txt", .method=HTTP_GET, .handler=captive_handler};

    httpd_register_uri_handler(http_server, &root);
    httpd_register_uri_handler(http_server, &status);
    httpd_register_uri_handler(http_server, &relay);
    httpd_register_uri_handler(http_server, &internet_get);
    httpd_register_uri_handler(http_server, &internet_post);
    httpd_register_uri_handler(http_server, &wifi_sta_post);
    httpd_register_uri_handler(http_server, &settings_get);
    httpd_register_uri_handler(http_server, &settings_post);
    httpd_register_uri_handler(http_server, &brand_post);
    httpd_register_uri_handler(http_server, &schedules_get);
    httpd_register_uri_handler(http_server, &schedules_post);
    httpd_register_uri_handler(http_server, &relay_config_post);
    httpd_register_uri_handler(http_server, &connectivity_post);
    httpd_register_uri_handler(http_server, &c1);
    httpd_register_uri_handler(http_server, &c2);
    httpd_register_uri_handler(http_server, &c3);
    httpd_register_uri_handler(http_server, &c4);
    httpd_register_uri_handler(http_server, &c5);
    httpd_register_uri_handler(http_server, &c6);

    ESP_LOGI(TAG, "HTTP server ready");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    relay_mutex = xSemaphoreCreateMutex();
    storage_mutex = xSemaphoreCreateMutex();
    if (!relay_mutex || !storage_mutex) {
        ESP_LOGE(TAG, "Mutex allocation failed");
        abort();
    }

    load_nvs();
    init_relays();
    init_switches();
    apply_all_relays();

    
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true
    };
    ret = esp_task_wdt_init(&wdt_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    BaseType_t save_ok = xTaskCreate(relay_save_task, "relay_save", 3072, NULL, 2, &relay_save_task_handle);
    if (save_ok != pdPASS) {
        ESP_LOGE(TAG, "Relay save task creation failed");
        abort();
    }

    BaseType_t switch_ok = xTaskCreate(physical_switch_task, "physical_switches", 3072, NULL, 4, &switch_task_handle);
    if (switch_ok != pdPASS) {
        ESP_LOGE(TAG, "Physical switch task creation failed");
    }

    wifi_init_ap_sta();

    BaseType_t ok = xTaskCreate(dns_task, "local_dns", DNS_STACK_SIZE, NULL, 3, &dns_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "DNS task creation failed");
    }

    start_http_server();

    cloud_client_config_t ccfg = {0};
    strlcpy(ccfg.base_url, cloud_url, sizeof(ccfg.base_url));
    strlcpy(ccfg.device_id, device_id, sizeof(ccfg.device_id));
    strlcpy(ccfg.device_token, device_token, sizeof(ccfg.device_token));
    ccfg.command_cb = cloud_command_cb; ccfg.snapshot_cb = cloud_snapshot_cb; ccfg.storage_lock = storage_mutex;
    
    setenv("TZ", "IST-5:30", 1);
    tzset();

    cloud_client_init(&ccfg);
    BaseType_t sched_ok = xTaskCreate(schedule_task, "scheduler", 4096, NULL, 3, &schedule_task_handle);
    if (sched_ok != pdPASS) ESP_LOGE(TAG, "Schedule task creation failed");

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Offline Smart Home ready");
    ESP_LOGI(TAG, "Control:  http://%s/", AP_IP_ADDR);
    ESP_LOGI(TAG, "AP only: no STA, no Internet");
    ESP_LOGI(TAG, "Relays: 3 fixed + 2 optional");
    ESP_LOGI(TAG, "========================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
