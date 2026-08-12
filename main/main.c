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
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "cloud_client.h"

#define TAG "SMART_HOME_V2"

/* ---------------- User hardware/configuration ---------------- */

#define RELAY1_GPIO             16
#define RELAY2_GPIO             17
#define RELAY3_GPIO             18
#define RELAY4_GPIO             19
#define RELAY5_GPIO             21

#define RELAY_COUNT             5

/* Physical wall-switch inputs: connect one switch terminal to GND and the
 * other terminal to the corresponding GPIO. Internal pull-ups are used, so
 * an open switch reads HIGH and a closed switch reads LOW. */
#define SWITCH1_GPIO            32
#define SWITCH2_GPIO            33
#define SWITCH3_GPIO            25
#define SWITCH4_GPIO            26
#define SWITCH5_GPIO            27
#define SWITCH_COUNT            5
#define SWITCH_ACTIVE_LEVEL     0
#define SWITCH_DEBOUNCE_SAMPLES 3
#define SWITCH_POLL_MS          20

/* Change to 0 if your relay board is active-low. */
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
#define NVS_KEY_OTA_PASS        "ota_pass"
#define NVS_KEY_STA_SSID        "sta_ssid"
#define NVS_KEY_STA_PASS        "sta_pass"
#define NVS_KEY_CLOUD_URL       "cloud_url"
#define NVS_KEY_DEVICE_ID       "device_id"
#define NVS_KEY_DEVICE_TOKEN    "device_token"

#define WATCHDOG_TIMEOUT_MS     10000
#define DNS_PORT                53
#define DNS_STACK_SIZE          3072
#define DNS_RX_SIZE             512
#define OTA_BUFFER_SIZE         4096

#define MAX_AP_SSID_LEN         32
#define MAX_AP_PASS_LEN         63
#define MAX_RELAY_NAME_LEN      31
#define OTA_UPDATE_PASSWORD     "OTA@ESP32#2026"
#define MAX_OTA_PASS_LEN        63
#define MAX_CLOUD_URL_LEN       191
#define MAX_DEVICE_ID_LEN       63
#define MAX_DEVICE_TOKEN_LEN    127

/* ------------------------------------------------------------- */

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
static SemaphoreHandle_t ota_mutex;

static char ap_ssid[MAX_AP_SSID_LEN + 1] = DEFAULT_AP_SSID;
static char ap_password[MAX_AP_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;
static char ota_password[MAX_OTA_PASS_LEN + 1] = OTA_UPDATE_PASSWORD;
static char sta_ssid[MAX_AP_SSID_LEN + 1] = "";
static char sta_password[MAX_AP_PASS_LEN + 1] = "";
static char cloud_url[MAX_CLOUD_URL_LEN + 1] = "";
static char device_id[MAX_DEVICE_ID_LEN + 1] = "";
static char device_token[MAX_DEVICE_TOKEN_LEN + 1] = "";
static volatile bool sta_connected = false;
static volatile uint8_t sta_retry_count = 0;

static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t switch_task_handle = NULL;
static volatile bool ota_in_progress = false;
static httpd_handle_t http_server = NULL;

/* -------------------- Local Web UI -------------------- */

static const char *HTML_PAGE =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#111827\">\n"
"<title>ESP32 Smart Home</title>\n"
"<style>\n"
":root{--bg:#f3f5f7;--card:#fff;--text:#17202a;--muted:#697586;--line:#e5e7eb;--on:#168a4b;--accent:#2563eb;--danger:#b42318}\n"
"*{box-sizing:border-box}\n"
"html,body{margin:0;min-height:100%;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;background:var(--bg);color:var(--text)}\n"
"body{overflow-x:hidden}\n"
".wrap{width:min(680px,100%);margin:auto;padding:18px 14px 34px;transition:filter .34s ease,transform .34s ease}\n"
".top{padding:8px 4px 18px}\n"
".topbar{display:flex;align-items:center;justify-content:space-between;gap:12px}\n"".brand{position:absolute;left:50%;transform:translateX(-50%);text-align:center;white-space:nowrap}\n"".footer{text-align:center;color:var(--muted);font-size:11px;margin:18px 0 2px;opacity:.72}\n"
"h1{font-size:25px;margin:0 0 5px}.sub{color:var(--muted);font-size:14px}\n"
".settings-btn{width:42px;height:42px;border:1px solid var(--line);border-radius:12px;background:#fff;display:flex;align-items:center;justify-content:center;font-size:21px;cursor:pointer;box-shadow:0 2px 8px rgba(15,23,42,.06)}\n"
".settings-btn:active{transform:scale(.96)}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0;box-shadow:0 2px 10px rgba(15,23,42,.04)}\n"
".row{display:flex;align-items:center;justify-content:space-between;gap:15px}\n"
".name{font-weight:650;font-size:17px}.state{font-size:13px;color:var(--muted);margin-top:4px}\n"
".switch{position:relative;width:58px;height:32px;flex:none}.switch input{opacity:0;width:0;height:0}\n"
".slider{position:absolute;inset:0;background:#c8ced5;border-radius:40px;transition:.18s;cursor:pointer}\n"
".slider:before{content:'';position:absolute;width:26px;height:26px;left:3px;top:3px;background:white;border-radius:50%;box-shadow:0 1px 4px #0003;transition:.18s}\n"
"input:checked+.slider{background:var(--on)}input:checked+.slider:before{transform:translateX(26px)}\n"
"button{border:1px solid var(--line);background:#fff;border-radius:10px;padding:10px 13px;font:inherit;cursor:pointer}\n"
"button.primary{background:var(--accent);border-color:var(--accent);color:#fff}\n"
"button:disabled{opacity:.55;cursor:not-allowed}\n"
".msg{font-size:13px;margin-top:10px;color:var(--muted)}\n"
"input[type=text],input[type=password],input[type=file]{width:100%;padding:11px;border:1px solid #d5dae0;border-radius:10px;background:#fff;font:inherit}\n"
"label.field{display:block;font-size:13px;color:var(--muted);margin:13px 0 6px}\n"
".hidden{display:none!important}\n"
".status{display:inline-flex;align-items:center;gap:7px;font-size:12px;color:var(--muted)}\n"
".dot{width:8px;height:8px;border-radius:50%;background:var(--on)}\n"
".progress-wrap{margin-top:14px}.progress-head{display:flex;justify-content:space-between;gap:10px;font-size:12px;color:var(--muted);margin-bottom:6px}\n"
".progress{height:8px;background:#e8ebef;border-radius:20px;overflow:hidden}.progress-fill{height:100%;width:0;background:var(--accent);transition:width .12s ease}\n"
".relay-config{margin-top:10px}.relay-config-item{padding:14px 0;border-top:1px solid var(--line)}\n"
".relay-config-item:first-child{border-top:0}.relay-config-head{display:flex;align-items:center;justify-content:space-between;gap:12px}\n"
".small-switch{position:relative;width:48px;height:27px;flex:none}.small-switch input{opacity:0;width:0;height:0}\n"
".small-slider{position:absolute;inset:0;background:#c8ced5;border-radius:40px;transition:.18s;cursor:pointer}\n"
".small-slider:before{content:'';position:absolute;width:21px;height:21px;left:3px;top:3px;background:#fff;border-radius:50%;box-shadow:0 1px 4px #0003;transition:.18s}\n"
".small-switch input:checked+.small-slider{background:var(--on)}.small-switch input:checked+.small-slider:before{transform:translateX(21px)}\n"
".relay-number{font-weight:650;font-size:15px}.relay-gpio,.relay-switch-gpio{font-size:12px;color:var(--muted);margin-top:3px}\n"
".bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}\n"
".setting-list{margin-top:14px}\n"
".setting-item{display:flex;align-items:center;gap:14px;padding:15px 2px;border-top:1px solid var(--line);cursor:pointer}\n"
".setting-item:first-child{border-top:0}\n"
".setting-item:active{opacity:.72}\n"
".setting-icon{width:40px;height:40px;border-radius:12px;background:#f1f4f8;display:flex;align-items:center;justify-content:center;font-size:19px;flex:none}\n"
".setting-title{font-weight:650;font-size:15px}.setting-desc{font-size:12px;color:var(--muted);margin-top:3px;line-height:1.4}\n"
".chevron{margin-left:auto;color:#8993a1;font-size:22px}\n"
".back-row{margin-top:20px;text-align:center}.back-btn{min-width:180px}\n"
".drawer-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.34);opacity:0;pointer-events:none;transition:opacity .34s ease;z-index:90}\n"
".settings-drawer{position:fixed;z-index:100;top:0;right:0;width:min(680px,100%);height:100dvh;background:var(--bg);box-shadow:-12px 0 35px rgba(15,23,42,.18);transform:translateX(105%);transition:transform .38s cubic-bezier(.22,.8,.2,1);overflow-y:auto;overscroll-behavior:contain}\n"
".settings-drawer.open{transform:translateX(0)}\n"
".drawer-backdrop.open{opacity:1;pointer-events:auto}\n"
"body.settings-open .wrap{filter:brightness(.86)}\n"
".drawer-inner{min-height:100%;padding:18px 14px 34px}\n"
".drawer-top{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 4px 18px}\n"
".drawer-title{font-size:25px;font-weight:700}.drawer-sub{font-size:14px;color:var(--muted);margin-top:4px}\n"
".icon-btn{width:42px;height:42px;border:1px solid var(--line);border-radius:12px;background:#fff;display:flex;align-items:center;justify-content:center;font-size:22px;cursor:pointer}\n"
".subpage{display:none;animation:pageIn .25s ease both}.subpage.active{display:block}\n"
"@keyframes pageIn{from{opacity:0;transform:translateX(18px)}to{opacity:1;transform:translateX(0)}}\n"
".page-title{font-size:22px;font-weight:700;margin:0}.page-sub{font-size:13px;color:var(--muted);margin-top:4px}\n"
".page-head{display:flex;align-items:center;gap:10px;margin-bottom:18px}\n"
".page-head .icon-btn{flex:none}\n"
".info-card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0}\n"
"@media (prefers-reduced-motion:reduce){.settings-drawer,.drawer-backdrop,.wrap{transition:none}.subpage{animation:none}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<main class=\"wrap\">\n"
"<header class=\"top\"><div class=\"topbar\">\n"
"<div class=\"brand\"><h1>Smart Home</h1></div>\n"
"<button class=\"settings-btn\" onclick=\"openSettings()\" aria-label=\"Settings\" title=\"Settings\">⚙</button>\n"
"</div></header>\n"
"<section id=\"controls\"></section>\n"
"<footer class=\"footer\">Created with ChatGPT</footer>\n"
"</main>\n"
"\n"
"<div id=\"drawerBackdrop\" class=\"drawer-backdrop\" onclick=\"closeSettings()\"></div>\n"
"<aside id=\"settingsDrawer\" class=\"settings-drawer\" aria-hidden=\"true\">\n"
"<div class=\"drawer-inner\">\n"
"\n"
"<section id=\"settingsHome\" class=\"subpage active\">\n"
"<header class=\"drawer-top\">\n"
"<div><div class=\"drawer-title\">Settings</div><div class=\"drawer-sub\">Device configuration</div></div>\n"
"<button class=\"icon-btn\" onclick=\"closeSettings()\" aria-label=\"Close settings\">✕</button>\n"
"</header>\n"
"<div class=\"card setting-list\">\n"
"<div class=\"setting-item\" onclick=\"openSubPage('otaPage')\">\n"
"<div class=\"setting-icon\">↻</div><div><div class=\"setting-title\">OTA Update</div><div class=\"setting-desc\">Update firmware locally from a .bin file</div></div><div class=\"chevron\">›</div>\n"
"</div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('otaPasswordPage')\">\n"
"<div class=\"setting-icon\">□</div><div><div class=\"setting-title\">OTA Password</div><div class=\"setting-desc\">Change the password required for firmware updates</div></div><div class=\"chevron\">›</div>\n"
"</div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('relayPage')\">\n"
"<div class=\"setting-icon\">▣</div><div><div class=\"setting-title\">Relay Configuration</div><div class=\"setting-desc\">Enable Relay 4/5 and rename any relay</div></div><div class=\"chevron\">›</div>\n"
"</div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('internetPage')\">\n"
"<div class=\"setting-icon\">◎</div><div><div class=\"setting-title\">Internet Connection</div><div class=\"setting-desc\">Connect the ESP32 to home Wi-Fi and secure remote control</div></div><div class=\"chevron\">›</div>\n"
"</div>\n"
"<div class=\"setting-item\" onclick=\"openSubPage('apPage')\">\n"
"<div class=\"setting-icon\">≋</div><div><div class=\"setting-title\">AP Configuration</div><div class=\"setting-desc\">Change the ESP32 local Wi-Fi SSID and password</div></div><div class=\"chevron\">›</div>\n"
"</div>\n"
"</div>\n"
"</section>\n"
"\n"
"<section id=\"otaPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">OTA Update</div><div class=\"page-sub\">Local firmware update</div></div></div>\n"
"<div class=\"info-card\">\n"
"<label class=\"field\">Firmware .bin</label><input id=\"fw\" type=\"file\" accept=\".bin,application/octet-stream\">\n"
"<div class=\"bar\"><button id=\"uploadBtn\" class=\"primary\" onclick=\"uploadFirmware()\">Upload & Restart</button></div>\n"
"<div id=\"otaProgress\" class=\"progress-wrap hidden\">\n"
"<div class=\"progress-head\"><span id=\"otaProgressText\">Uploading...</span><span id=\"otaPercent\">0%</span></div>\n"
"<div class=\"progress\"><div id=\"otaFill\" class=\"progress-fill\"></div></div></div>\n"
"<div id=\"otamsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"otaPasswordPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">OTA Password</div><div class=\"page-sub\">Change the password used for OTA updates</div></div></div>\n"
"<div class=\"info-card\">\n"
"<label class=\"field\">Old password</label><input id=\"oldOtaPass\" type=\"password\" maxlength=\"63\" autocomplete=\"current-password\">\n"
"<label class=\"field\">New password</label><input id=\"newOtaPass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">\n"
"<label class=\"field\">Confirm new password</label><input id=\"confirmOtaPass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">\n"
"<div class=\"bar\"><button class=\"primary\" id=\"otaPassBtn\" onclick=\"saveOtaPassword()\">Save OTA Password</button></div>\n"
"<div id=\"otaPassMsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"relayPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Relay Configuration</div><div class=\"page-sub\">Relay 1-3 are fixed; Relay 4-5 are optional</div></div></div>\n"
"<div class=\"info-card relay-config\"><div id=\"relayConfigList\"></div>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveRelayConfig()\">Save Relay Configuration</button></div>\n"
"<div id=\"relaymsg\" class=\"msg\"></div></div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"internetPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">Internet Connection</div><div class=\"page-sub\">Home Wi-Fi first; cloud remote access is optional</div></div></div>\n"
"<div class=\"info-card\">\n"
"<div class=\"small\">Enter only your home Wi-Fi SSID and password to connect the ESP32. Cloud URL, Device ID and Device Token are optional.</div>\n"
"<label class=\"field\">Home Wi-Fi SSID</label><input id=\"staSsid\" maxlength=\"32\">\n"
"<label class=\"field\">Home Wi-Fi Password</label><input id=\"staPass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">\n"
"<label class=\"field\">Cloud API URL <span class=\"muted\">(optional)</span></label><input id=\"cloudUrl\" type=\"text\" maxlength=\"191\" placeholder=\"https://your-domain.example\">\n"
"<label class=\"field\">Device ID <span class=\"muted\">(optional)</span></label><input id=\"deviceId\" type=\"text\" maxlength=\"63\">\n"
"<label class=\"field\">Device Token <span class=\"muted\">(optional)</span></label><input id=\"deviceToken\" type=\"password\" maxlength=\"127\" autocomplete=\"off\">\n"
"<div id=\"internetStatus\" class=\"msg\">Not configured</div>\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveInternet()\">Save &amp; Restart</button></div>\n"
"<div id=\"internetMsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"<section id=\"apPage\" class=\"subpage\">\n"
"<div class=\"page-head\"><button class=\"icon-btn\" onclick=\"backToSettings()\" aria-label=\"Back\">←</button><div><div class=\"page-title\">AP Configuration</div><div class=\"page-sub\">Change the local ESP32 Wi-Fi settings</div></div></div>\n"
"<div class=\"info-card\">\n"
"<label class=\"field\">SSID</label><input id=\"ssid\" maxlength=\"32\">\n"
"<label class=\"field\">Password (8-63 characters)</label><input id=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">\n"
"<div class=\"bar\"><button class=\"primary\" onclick=\"saveSettings()\">Save & Restart</button></div>\n"
"<div id=\"setmsg\" class=\"msg\"></div>\n"
"</div>\n"
"<div class=\"back-row\"><button class=\"back-btn\" onclick=\"backToSettings()\">← Back to Settings</button></div>\n"
"</section>\n"
"\n"
"</div>\n"
"</aside>\n"
"\n"
"<script>\n"
"let relayCfg=[\n"
"{enabled:true,name:'Living Room Light',gpio:16,switchGpio:32},\n"
"{enabled:true,name:'Ceiling Fan',gpio:17,switchGpio:33},\n"
"{enabled:true,name:'Charging Socket',gpio:18,switchGpio:25},\n"
"{enabled:false,name:'Relay 4',gpio:19,switchGpio:26},\n"
"{enabled:false,name:'Relay 5',gpio:21,switchGpio:27}\n"
"];\n"
"\n"
"function esc(s){return String(s).replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]))}\n"
"\n"
"function render(a){\n"
" let h='';\n"
" a.forEach((v,i)=>{\n"
"  if(!relayCfg[i]||!relayCfg[i].enabled)return;\n"
"  h+=`<section class=\"card\"><div class=\"row\"><div><div class=\"name\">${esc(relayCfg[i].name)}</div><div class=\"state\" id=\"st${i}\">${v?'ON':'OFF'}</div></div><label class=\"switch\"><input type=\"checkbox\" id=\"r${i}\" ${v?'checked':''} onchange=\"setRelay(${i},this.checked)\"><span class=\"slider\"></span></label></div></section>`;\n"
" });\n"
" document.getElementById('controls').innerHTML=h;\n"
"}\n"
"\n"
"async function load(){\n"
" try{\n"
"  let r=await fetch('/api/status',{cache:'no-store'});\n"
"  if(!r.ok)throw 0;\n"
"  let d=await r.json();\n"
"  relayCfg=d.config||relayCfg;\n"
"  render(d.states||[]);\n"
" }catch(e){}\n"
"}\n"
"\n"
"async function setRelay(i,on){\n"
" let el=document.getElementById('r'+i);\n"
" if(!el)return;\n"
" el.disabled=true;\n"
" try{\n"
"  let r=await fetch(`/api/relay?relay=${i+1}&state=${on?1:0}`,{cache:'no-store'});\n"
"  if(!r.ok)throw 0;\n"
"  await load();\n"
" }catch(e){\n"
"  el.checked=!on;\n"
"  alert('Relay command failed.');\n"
" }finally{el.disabled=false}\n"
"}\n"
"\n"
"function openSettings(){\n"
" document.body.classList.add('settings-open');\n"
" document.getElementById('drawerBackdrop').classList.add('open');\n"
" document.getElementById('settingsDrawer').classList.add('open');\n"
" document.getElementById('settingsDrawer').setAttribute('aria-hidden','false');\n"
" showSettingsHome();\n"
"}\n"
"\n"
"function closeSettings(){\n"
" let d=document.getElementById('settingsDrawer');\n"
" d.classList.remove('open');\n"
" document.getElementById('drawerBackdrop').classList.remove('open');\n"
" document.body.classList.remove('settings-open');\n"
" d.setAttribute('aria-hidden','true');\n"
" setTimeout(showSettingsHome,380);\n"
"}\n"
"\n"
"function showSettingsHome(){\n"
" document.querySelectorAll('.subpage').forEach(p=>p.classList.remove('active'));\n"
" document.getElementById('settingsHome').classList.add('active');\n"
"}\n"
"\n"
"function openSubPage(id){\n"
" document.querySelectorAll('.subpage').forEach(p=>p.classList.remove('active'));\n"
" document.getElementById(id).classList.add('active');\n"
" if(id==='relayPage')renderRelayConfig();\n"
" if(id==='apPage')loadSettings();\n"
" if(id==='internetPage')loadInternet();\n"
"}\n"
"\n"
"function backToSettings(){\n"
" showSettingsHome();\n"
"}\n"
"\n"
"function renderRelayConfig(){\n"
" let h='';\n"
" relayCfg.forEach((r,i)=>{\n"
"  let optional=i>=3;\n"
"  h+=`<div class=\"relay-config-item\"><div class=\"relay-config-head\"><div><div class=\"relay-number\">Relay ${i+1}</div><div class=\"relay-gpio\">Relay GPIO ${r.gpio}${optional?' · Optional':''}</div><div class=\"relay-switch-gpio\">Physical Switch GPIO ${r.switchGpio}</div></div>${optional?`<label class=\"small-switch\"><input type=\"checkbox\" id=\"en${i}\" ${r.enabled?'checked':''} onchange=\"relayEnableChanged(${i})\"><span class=\"small-slider\"></span></label>`:''}</div><label class=\"field\">Name</label><input type=\"text\" id=\"rn${i}\" maxlength=\"31\" value=\"${esc(r.name)}\" ${optional&&!r.enabled?'disabled':''}></div>`;\n"
" });\n"
" document.getElementById('relayConfigList').innerHTML=h;\n"
"}\n"
"\n"
"function relayEnableChanged(i){\n"
" let en=document.getElementById('en'+i).checked;\n"
" document.getElementById('rn'+i).disabled=!en;\n"
"}\n"
"\n"
"async function saveRelayConfig(){\n"
" let m=document.getElementById('relaymsg'),body={};\n"
" for(let i=0;i<5;i++){\n"
"  let enabled=i<3?true:document.getElementById('en'+i).checked;\n"
"  let name=document.getElementById('rn'+i).value.trim();\n"
"  if(!name)name='Relay '+(i+1);\n"
"  if(name.length>31){m.textContent='Relay name is too long.';return}\n"
"  body['r'+(i+1)+'_enabled']=enabled;\n"
"  body['r'+(i+1)+'_name']=name;\n"
" }\n"
" m.textContent='Saving...';\n"
" try{\n"
"  let r=await fetch('/api/relays',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});\n"
"  let d=await r.json().catch(()=>({}));\n"
"  if(!r.ok)throw new Error(d.error||'save failed');\n"
"  relayCfg=d.config||relayCfg;\n"
"  m.textContent='Saved successfully.';\n"
"  renderRelayConfig();\n"
"  await load();\n"
" }catch(e){m.textContent='Could not save relay configuration: '+(e.message||'request failed')}\n"
"}\n"
"\n"
"async function loadInternet(){\n"
" try{\n"
"  let r=await fetch('/api/internet',{cache:'no-store'});\n"
"  if(!r.ok)throw new Error('request failed');\n"
"  let d=await r.json();\n"
"  document.getElementById('staSsid').value=d.staSsid||'';\n"
"  document.getElementById('staPass').value='';\n"
"  document.getElementById('cloudUrl').value=d.cloudUrl||'';\n"
"  document.getElementById('deviceId').value=d.deviceId||'';\n"
"  document.getElementById('deviceToken').value='';\n"
"  if(!d.wifiConfigured) document.getElementById('internetStatus').textContent='Wi-Fi not configured. Cloud settings are optional.';\n"
"  else if(!d.cloudConfigured) document.getElementById('internetStatus').textContent=d.connected?'Wi-Fi connected. Local Internet mode is active; remote cloud access is not configured.':'Wi-Fi configured; waiting for connection. Remote cloud access is optional.';\n"
"  else document.getElementById('internetStatus').textContent=d.connected?'Wi-Fi connected and cloud remote access is enabled.':'Wi-Fi/cloud configured; waiting for connection. Remote access will recover automatically.';\n"
" }catch(e){\n"
"  document.getElementById('internetStatus').textContent='Could not read Internet configuration.';\n"
" }\n"
"}\n"
"\n"
"async function saveInternet(){\n"
" let ssid=document.getElementById('staSsid').value.trim();\n"
" let pass=document.getElementById('staPass').value;\n"
" let url=document.getElementById('cloudUrl').value.trim();\n"
" let id=document.getElementById('deviceId').value.trim();\n"
" let token=document.getElementById('deviceToken').value.trim();\n"
" let m=document.getElementById('internetMsg');\n"
" if(ssid.length<1||ssid.length>32||pass.length<8||pass.length>63){\n"
"  m.textContent='Enter a valid Wi-Fi SSID and password (8-63 characters).';\n"
"  return;\n"
" }\n"
" let anyCloud=url||id||token;\n"
" if(anyCloud && (!url||id.length<3||token.length<16||!url.startsWith('https://'))){\n"
"  m.textContent='Cloud fields are optional, but if used, URL + Device ID + Device Token must all be provided and the URL must use HTTPS.';\n"
"  return;\n"
" }\n"
" m.textContent='Saving and restarting...';\n"
" try{\n"
"  let r=await fetch('/api/internet',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pass,cloudUrl:url,deviceId:id,deviceToken:token})});\n"
"  let d=await r.json().catch(()=>({}));\n"
"  if(!r.ok)throw new Error(d.error||'Save failed');\n"
" }catch(e){\n"
"  m.textContent=e.message||'Could not save Internet configuration.';\n"
" }\n"
"}\n"
"\n"
"async function saveSettings(){\n"
" let ssid=document.getElementById('ssid').value,pass=document.getElementById('pass').value,m=document.getElementById('setmsg');\n"
" if(ssid.length<1||ssid.length>32||pass.length<8||pass.length>63){m.textContent='Invalid SSID or password.';return}\n"
" m.textContent='Saving and restarting...';\n"
" try{\n"
"  let r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,password:pass})});\n"
"  if(!r.ok)throw 0;\n"
" }catch(e){m.textContent='Connection lost. The AP may be restarting.'}\n"
"}\n"
"\n"
"async function loadSettings(){\n"
" try{\n"
"  let r=await fetch('/api/settings',{cache:'no-store'}),d=await r.json();\n"
"  document.getElementById('ssid').value=d.ssid||'';\n"
" }catch(e){}\n"
"}\n"
"\n"
"async function saveOtaPassword(){\n"
" let oldPass=document.getElementById('oldOtaPass').value;\n"
" let newPass=document.getElementById('newOtaPass').value;\n"
" let confirmPass=document.getElementById('confirmOtaPass').value;\n"
" let msg=document.getElementById('otaPassMsg');\n"
" let btn=document.getElementById('otaPassBtn');\n"
" if(oldPass.length<8||newPass.length<8||confirmPass.length<8||oldPass.length>63||newPass.length>63||confirmPass.length>63){\n"
"  msg.textContent='All passwords must be 8-63 characters.';return;\n"
" }\n"
" if(newPass!==confirmPass){msg.textContent='New passwords do not match.';return}\n"
" if(oldPass===newPass){msg.textContent='New password must be different from the old password.';return}\n"
" btn.disabled=true;msg.textContent='Saving...';\n"
" try{\n"
"  let r=await fetch('/api/ota-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({oldPassword:oldPass,newPassword:newPass,confirmPassword:confirmPass})});\n"
"  let d=await r.json().catch(()=>({}));\n"
"  if(!r.ok)throw new Error(d.error||'Could not change OTA password.');\n"
"  msg.textContent='OTA password changed successfully.';\n"
"  document.getElementById('oldOtaPass').value='';\n"
"  document.getElementById('newOtaPass').value='';\n"
"  document.getElementById('confirmOtaPass').value='';\n"
" }catch(e){msg.textContent=e.message||'Could not change OTA password.'}\n"
" finally{btn.disabled=false}\n"
"}\n"
"\n"
"function setOtaProgress(p){\n"
" p=Math.max(0,Math.min(100,p));\n"
" document.getElementById('otaProgress').classList.remove('hidden');\n"
" document.getElementById('otaFill').style.width=p+'%';\n"
" document.getElementById('otaPercent').textContent=Math.round(p)+'%';\n"
"}\n"
"\n"
"function uploadFirmware(){\n"
" let f=document.getElementById('fw').files[0],m=document.getElementById('otamsg'),btn=document.getElementById('uploadBtn');\n"
" if(!f){m.textContent='Select a .bin file first.';return}\n"
" if(f.size<1024){m.textContent='Firmware file is too small.';return}\n"
" if(!confirm('Start OTA update? The device will restart after a successful update.'))return;\n"
" let otaPassword=prompt('Enter OTA update password:');\n"
" if(otaPassword===null)return;\n"
" if(!otaPassword){m.textContent='OTA password is required.';return}\n"
" btn.disabled=true;m.textContent='Uploading... Do not disconnect.';setOtaProgress(0);\n"
" let xhr=new XMLHttpRequest();\n"
" xhr.open('POST','/api/ota',true);\n"
" xhr.setRequestHeader('Content-Type','application/octet-stream');\n"
" xhr.setRequestHeader('X-OTA-Password',otaPassword);\n"
" xhr.upload.onprogress=function(e){if(e.lengthComputable){setOtaProgress((e.loaded/e.total)*100);m.textContent='Uploading firmware...'}};\n"
" xhr.onload=function(){\n"
"  if(xhr.status>=200&&xhr.status<300){setOtaProgress(100);m.textContent=xhr.responseText||'OTA successful. Restarting...';setTimeout(()=>location.reload(),8000)}\n"
"  else{btn.disabled=false;m.textContent='OTA failed. Current firmware should remain active.'}\n"
" };\n"
" xhr.onerror=function(){btn.disabled=false;m.textContent='Upload interrupted. Current firmware should remain active.'};\n"
" xhr.ontimeout=function(){btn.disabled=false;m.textContent='OTA request timed out.'};\n"
" xhr.send(f);\n"
"}\n"
"\n"
"load();\n"
"loadSettings();\n"
"setInterval(load,500);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* -------------------- NVS / persistence -------------------- */

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

    /* Reject ASCII control characters while still allowing UTF-8 names. */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

static void load_defaults(void)
{
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

    char tmp_ota_pass[MAX_OTA_PASS_LEN + 1] = {0};
    sz = sizeof(tmp_ota_pass);
    if (nvs_get_str(h, NVS_KEY_OTA_PASS, tmp_ota_pass, &sz) == ESP_OK && valid_password(tmp_ota_pass)) {
        strlcpy(ota_password, tmp_ota_pass, sizeof(ota_password));
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

static esp_err_t save_relay_config(void)
{
    uint8_t enabled[RELAY_COUNT];
    uint8_t states[RELAY_COUNT];
    char names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    /* Serialize the complete relay configuration/state snapshot. The relay
     * mutex stays held until the NVS transaction has committed, so a physical
     * switch or web command cannot change relay_state between the snapshot and
     * the configuration write. No other path takes storage_mutex and then
     * relay_mutex, so this lock order is safe. */
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

static esp_err_t save_ota_password(const char *password)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_OTA_PASS, password);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);

    if (err == ESP_OK) {
        strlcpy(ota_password, password, sizeof(ota_password));
    }
    return err;
}

static esp_err_t save_internet_settings(const char *ssid, const char *pass, const char *url, const char *id, const char *token)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h; esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_STA_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_STA_PASS, pass);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_CLOUD_URL, url);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DEVICE_ID, id);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_DEVICE_TOKEN, token);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    if (err == ESP_OK) {
        strlcpy(sta_ssid, ssid, sizeof(sta_ssid)); strlcpy(sta_password, pass, sizeof(sta_password));
        strlcpy(cloud_url, url, sizeof(cloud_url)); strlcpy(device_id, id, sizeof(device_id)); strlcpy(device_token, token, sizeof(device_token));
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
    save_relay_states();
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

static void cloud_ota_cb(const char *url, void *ctx)
{
    ESP_LOGI(TAG, "Remote OTA requested: %s", url);
    cloud_client_start_ota(url);
}

/* -------------------- GPIO / relay -------------------- */

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

    /* Safe physical OFF before restoring persistent state. */
    for (int i = 0; i < RELAY_COUNT; ++i) {
        gpio_set_level(relay_gpio(i), relay_output_level(0));
    }
}

/* -------------------- Physical wall switches -------------------- */

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

    if (changed) {
        /* Persist physical-switch changes so the last known state survives a
         * power cycle. NVS handles wear-leveling internally. */
        if (save_relay_states() != ESP_OK) {
            ESP_LOGE(TAG, "Physical switch state save failed for relay %d", index + 1);
        }
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
        /* Establish a boot baseline without generating a relay command.
         * This preserves the NVS-restored relay state across power cycles.
         * A later physical transition is what changes the relay. */
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

/* -------------------- Wi-Fi AP + STA -------------------- */
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
        if (!sta_ssid[0]) return;

        /*
         * Do not pin the station to a BSSID or channel. The configured SSID
         * is scanned again by the Wi-Fi driver, so router channel changes and
         * hidden SSIDs remain recoverable.
         */
        if (sta_retry_count < 10) sta_retry_count++;
        esp_err_t err = esp_wifi_connect();
        ESP_LOGW(TAG, "STA disconnected (retry %u): %s",
                 (unsigned)sta_retry_count, esp_err_to_name(err));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        sta_retry_count = 0;
        sta_connected = true;
        ESP_LOGI(TAG, "STA connected; internet features enabled");
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
        /*
         * No channel or BSSID is stored. The ESP32 scans by SSID, which is
         * important for routers that automatically change channels and for
         * hidden SSIDs supplied manually by the user.
         */
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
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "AP IP: %s", AP_IP_ADDR);
}

/* -------------------- Local DNS captive portal -------------------- */

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

/* -------------------- HTTP helpers -------------------- */

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

    char json[1024];
    int pos = snprintf(json, sizeof(json), "{\"states\":[");
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
    snprintf(json + pos, sizeof(json) - pos, "]}");

    return send_json(req, json, "200 OK");
}

static esp_err_t relay_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");

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

    esp_err_t err = save_relay_states();
    if (err != ESP_OK) {
        return send_json(req, "{\"error\":\"state applied but not saved\"}", "500 Internal Server Error");
    }

    return send_json(req, "{\"ok\":true}", "200 OK");
}

static bool json_extract_string(const char *body, const char *key, char *out, size_t out_sz);

static esp_err_t internet_get_handler(httpd_req_t *req)
{
    char json[768];
    bool wifi_configured = sta_ssid[0] != '\0';
    bool cloud_configured = cloud_url[0] != '\0' &&
                            device_id[0] != '\0' &&
                            device_token[0] != '\0';

    /*
     * Never return the stored Wi-Fi password or device token to the browser.
     * The token is write-only from the local UI.
     */
    snprintf(json, sizeof(json),
             "{\"staSsid\":\"%s\",\"cloudUrl\":\"%s\",\"deviceId\":\"%s\","
             "\"wifiConfigured\":%s,\"cloudConfigured\":%s,\"connected\":%s}",
             sta_ssid, cloud_url, device_id,
             wifi_configured ? "true" : "false",
             cloud_configured ? "true" : "false",
             sta_connected ? "true" : "false");
    return send_json(req, json, "200 OK");
}

static esp_err_t internet_post_handler(httpd_req_t *req)
{
    if (ota_in_progress)
        return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");

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

    char ssid[MAX_AP_SSID_LEN + 1] = {0};
    char pass[MAX_AP_PASS_LEN + 1] = {0};
    char url[MAX_CLOUD_URL_LEN + 1] = {0};
    char id[MAX_DEVICE_ID_LEN + 1] = {0};
    char token[MAX_DEVICE_TOKEN_LEN + 1] = {0};

    if (!json_extract_string(body, "ssid", ssid, sizeof(ssid)) ||
        !json_extract_string(body, "password", pass, sizeof(pass)) ||
        !valid_ssid(ssid) || !valid_password(pass)) {
        return send_json(req, "{\"error\":\"valid Wi-Fi SSID/password are required\"}",
                         "400 Bad Request");
    }

    /*
     * Cloud is optional. Empty cloud fields mean "keep the existing cloud
     * credentials" when they already exist, or local-only mode for a new
     * installation. Partial cloud configuration is rejected so a typo cannot
     * silently disable remote access.
     */
    bool have_url = json_extract_string(body, "cloudUrl", url, sizeof(url)) && url[0];
    bool have_id = json_extract_string(body, "deviceId", id, sizeof(id)) && id[0];
    bool have_token = json_extract_string(body, "deviceToken", token, sizeof(token)) && token[0];

    if (have_url || have_id || have_token) {
        if (!have_url || !have_id || !have_token ||
            strncmp(url, "https://", 8) != 0 ||
            strlen(id) < 3 || strlen(id) > MAX_DEVICE_ID_LEN ||
            strlen(token) < 16) {
            return send_json(req,
                             "{\"error\":\"provide Cloud URL, Device ID and Device Token together; HTTPS is required\"}",
                             "400 Bad Request");
        }
    } else {
        /* Preserve a previously configured remote-access identity. */
        strlcpy(url, cloud_url, sizeof(url));
        strlcpy(id, device_id, sizeof(id));
        strlcpy(token, device_token, sizeof(token));
    }

    if (save_internet_settings(ssid, pass, url, id, token) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json), "{\"ssid\":\"%s\"}", ap_ssid);
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
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");
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

static esp_err_t relay_config_post_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");
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
            /* When optional Relay 4/5 is enabled, immediately adopt the
             * current corresponding physical switch position. */
            bool on = read_switch_state(i);
            relay_state[i] = on ? 1 : 0;
            gpio_set_level(relay_gpio(i), relay_output_level(on ? 1 : 0));
        }
    }
    xSemaphoreGive(relay_mutex);

    esp_err_t err = save_relay_config();
    if (err != ESP_OK)
        return send_json(req, "{\"error\":\"configuration save failed\"}", "500 Internal Server Error");

    /* Return the same compact configuration format the page already uses. */
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

static bool constant_time_equal(const char *a, const char *b);

static esp_err_t ota_password_post_handler(httpd_req_t *req)
{
    if (ota_in_progress)
        return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");

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

    char old_pass[MAX_OTA_PASS_LEN + 1];
    char new_pass[MAX_OTA_PASS_LEN + 1];
    char confirm_pass[MAX_OTA_PASS_LEN + 1];

    if (!json_extract_string(body, "oldPassword", old_pass, sizeof(old_pass)) ||
        !json_extract_string(body, "newPassword", new_pass, sizeof(new_pass)) ||
        !json_extract_string(body, "confirmPassword", confirm_pass, sizeof(confirm_pass))) {
        return send_json(req, "{\"error\":\"all password fields are required\"}", "400 Bad Request");
    }

    if (!valid_password(old_pass) || !valid_password(new_pass) || !valid_password(confirm_pass))
        return send_json(req, "{\"error\":\"password must be 8-63 characters\"}", "400 Bad Request");

    if (!constant_time_equal(old_pass, ota_password))
        return send_json(req, "{\"error\":\"old OTA password is incorrect\"}", "403 Forbidden");

    if (!constant_time_equal(new_pass, confirm_pass))
        return send_json(req, "{\"error\":\"new passwords do not match\"}", "400 Bad Request");

    if (constant_time_equal(new_pass, ota_password))
        return send_json(req, "{\"error\":\"new password must be different\"}", "400 Bad Request");

    if (save_ota_password(new_pass) != ESP_OK)
        return send_json(req, "{\"error\":\"could not save OTA password\"}", "500 Internal Server Error");

    return send_json(req, "{\"ok\":true}", "200 OK");
}

/* -------------------- OTA -------------------- */

static bool constant_time_equal(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la ^ lb);

    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    size_t pass_len = httpd_req_get_hdr_value_len(req, "X-OTA-Password");
    if (pass_len == 0 || pass_len > MAX_AP_PASS_LEN) {
        return send_json(req, "{\"error\":\"OTA password required\"}", "401 Unauthorized");
    }
    char supplied_ota_password[64];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", supplied_ota_password, sizeof(supplied_ota_password)) != ESP_OK ||
        !constant_time_equal(supplied_ota_password, ota_password)) {
        return send_json(req, "{\"error\":\"invalid OTA password\"}", "403 Forbidden");
    }

    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA busy\"}", "409 Conflict");

    if (req->content_len < 1024)
        return send_json(req, "{\"error\":\"firmware too small\"}", "400 Bad Request");

    ota_in_progress = true;
    xSemaphoreTake(ota_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        err = ESP_FAIL;
        goto ota_fail;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) goto ota_fail;

    uint8_t *buf = malloc(OTA_BUFFER_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        err = ESP_ERR_NO_MEM;
        goto ota_fail;
    }

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining > OTA_BUFFER_SIZE ? OTA_BUFFER_SIZE : remaining;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            err = ESP_FAIL;
            goto ota_fail;
        }

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            goto ota_fail;
        }
        remaining -= (size_t)n;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) goto ota_fail;

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) goto ota_fail;

    xSemaphoreGive(ota_mutex);
    ota_in_progress = false;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "OTA successful. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;

ota_fail:
    xSemaphoreGive(ota_mutex);
    ota_in_progress = false;

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"error\":\"OTA failed\",\"code\":%d}", (int)err);
    return send_json(req, msg, "500 Internal Server Error");
}

/* Common captive portal probe paths. */
static esp_err_t captive_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

/* -------------------- HTTP server -------------------- */

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 18;
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
    httpd_uri_t settings_get = {.uri="/api/settings", .method=HTTP_GET, .handler=settings_get_handler};
    httpd_uri_t settings_post = {.uri="/api/settings", .method=HTTP_POST, .handler=settings_post_handler};
    httpd_uri_t relay_config_post = {.uri="/api/relays", .method=HTTP_POST, .handler=relay_config_post_handler};
    httpd_uri_t ota = {.uri="/api/ota", .method=HTTP_POST, .handler=ota_handler};
    httpd_uri_t ota_password = {.uri="/api/ota-password", .method=HTTP_POST, .handler=ota_password_post_handler};

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
    httpd_register_uri_handler(http_server, &settings_get);
    httpd_register_uri_handler(http_server, &settings_post);
    httpd_register_uri_handler(http_server, &relay_config_post);
    httpd_register_uri_handler(http_server, &ota);
    httpd_register_uri_handler(http_server, &ota_password);
    httpd_register_uri_handler(http_server, &c1);
    httpd_register_uri_handler(http_server, &c2);
    httpd_register_uri_handler(http_server, &c3);
    httpd_register_uri_handler(http_server, &c4);
    httpd_register_uri_handler(http_server, &c5);
    httpd_register_uri_handler(http_server, &c6);

    ESP_LOGI(TAG, "HTTP server ready");
}

/* -------------------- app_main -------------------- */

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
    ota_mutex = xSemaphoreCreateMutex();
    if (!relay_mutex || !storage_mutex || !ota_mutex) {
        ESP_LOGE(TAG, "Mutex allocation failed");
        abort();
    }

    load_nvs();
    init_relays();
    init_switches();
    apply_all_relays();

    /* Configure TWDT before creating tasks that register themselves with it. */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true
    };
    ret = esp_task_wdt_init(&wdt_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
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
    ccfg.command_cb = cloud_command_cb; ccfg.snapshot_cb = cloud_snapshot_cb; ccfg.ota_cb = cloud_ota_cb;
    /*
     * NTP is tied only to STA configuration, not to cloud credentials.
     * This lets locally cached schedules keep correct time even when the
     * cloud service is not configured or temporarily unreachable.
     */
    if (sta_ssid[0]) {
        setenv("TZ", "IST-5:30", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }
    cloud_client_init(&ccfg);

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
