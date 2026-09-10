#include "wifi_importer.h"
#include "midi_import.h"
#include "song_import.h"
#include "project_store.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#if CONFIG_SYNTH_WIFI_IMPORT

static const char *TAG = "wifi_import";

#define IMP_MAX_BODY  (128 * 1024)         /* AMYSONG / MIDI cap      */
#define IMP_HEAD_CAP  (2048)               /* HTTP head cap           */
#define IMP_RESULT_WAIT_MS (12000)
#define IMP_STATUS_LINGER_MS (6000)        /* show "ap up" line this long */

/* ── boot-status pipeline ───────────────────────────────────────────────────
 * The AP brings WiFi up on its own task so a dead radio can never stall boot.
 * Each step stamps s_state_text; the UI hint strip renders it until the AP
 * has been up for a while (or the run failed). */

typedef enum {
    IMP_ST_IDLE = 0,
    IMP_ST_NETIF,
    IMP_ST_WIFI_INIT,
    IMP_ST_MODE,
    IMP_ST_START,
    IMP_ST_READY,
    IMP_ST_FAIL,
} imp_dir_state_t;

static imp_dir_state_t s_dir_state;
static char            s_state_text[64];
static bool            s_driver_up = false;
static TickType_t      s_ready_tick = 0;

static void imp_set_state(imp_dir_state_t st, const char *fmt, ...)
{
    char msg[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    s_dir_state = st;
    if (st == IMP_ST_READY) s_ready_tick = xTaskGetTickCount();
    snprintf(s_state_text, sizeof s_state_text, "%s", msg);
    ESP_LOGI(TAG, "imp AP: %s", msg);
}

/* ── pending-upload handoff ────────────────────────────────────────────────
 * The socket task receives the body and parks it; wifi_import_service()
 * on the synth_ui task applies it (only that task may rebuild layers) and
 * posts the outcome back. The HTTP handler blocks on s_done_sem meanwhile,
 * so the browser gets either OK or the parser's line-numbered reason. */

typedef struct {
    char            *body;
    size_t           len;         /* bytes in body (SMF is binary)          */
    uint8_t          slot;
    uint8_t          is_midi;     /* page gave us a .mid instead of text    */
    uint8_t          bars;        /* loop bars requested (page select)      */
    char             song_name[PROJECT_NAME_LEN];
    volatile bool    pending;     /* release-published by the socket task     */
    SemaphoreHandle_t done_sem;
    bool             task_created;
    char             result[256];
} imp_state_t;

static imp_state_t s_imp;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* The task tears itself down on every failure path; clear task_created first
 * so an on-demand start can retry. */
static void imp_self_delete(void)
{
    s_imp.task_created = false;
    vTaskDelete(NULL);
}

/* ── embedded page ── */

static const char s_page[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>AMYSYNTH import</title></head><body>"
    "<h2>AMYSYNTH song import</h2>"
    "<form id=f>"
    "Slot <input type=number name=slot min=1 max=64 value=1 style=width:4em>"
    "&nbsp;Loop <select id=bars><option value=1>1 bar</option>"
    "<option value=2 selected>2 bars</option></select>"
    "&nbsp;&nbsp;<b id=st>idle</b><br><br>"
    "File: <input type=file id=file accept=.mid,.midi,.txt,.amysong><br><br>"
    "<i>.mid gets converted on the device (first loop bars become the song);"
    "<br>or paste AMYSONG text below:</i><br>"
    "<textarea id=txt rows=10 cols=56 placeholder=\"amysong 1\nname &quot;Demo&quot;\nbpm 120\npattern 32\nlayer melodic 256\nnotes 0 . +4 . . . +7 . . . . . . . . .\nlayer drum\nhit 0 x . . . x . . . x . . . x . . .\n\"></textarea><br>"
    "<button type=button onclick=go()>Import</button>"
    "</form>"
    "<script>"
    "function basename(n){n=n.replace(/\\.[^.]*$/,'');return n;}"
    "function send(body,fmt,name,bars){"
    "var s=document.getElementById('slot').value||'1';"
    "var u='/upload?slot='+s+'&fmt='+fmt+'&bars='+bars"
    "+(name?'&name='+encodeURIComponent(basename(name)):'');"
    "fetch(u,{method:'POST',body:body}).then(function(r){return r.text()}).then(function(t){"
    "document.getElementById('st').textContent=t;"
    "}).catch(function(){document.getElementById('st').textContent='NET ERR';});"
    "}"
    "function go(){"
    "var f=document.getElementById('file').files[0],txt=document.getElementById('txt'),"
    "bars=document.getElementById('bars').value;"
    "if(f){"
    "if(/\\.[mM][iI][dD][iI]?$/.test(f.name)){var rd=new FileReader();"
    "rd.onload=function(){send(rd.result,'mid',f.name,bars)};rd.readAsArrayBuffer(f);return;}"
    "var r=new FileReader();"
    "r.onload=function(){send(r.result,'txt',f.name,bars)};r.readAsText(f);return;"
    "}"
    "send(txt.value,'txt','',bars);"
    "}"
    "</script></body></html>";

/* ── HTTP ── */

static int sock_read_line(int fd, char *buf, size_t cap, int *count)
{
    size_t got = 0;
    while (got + 1 < cap) {
        int n = recv(fd, &buf[got], 1, 0);
        if (n <= 0) return -1;
        got++;
        if (buf[got - 1] == '\n') break;
    }
    buf[got] = '\0';
    *count = (int)got;
    return 0;
}

static void http_reply(int fd, const char *status, const char *body)
{
    char hdr[128];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n",
                     status, (unsigned)strlen(body));
    send(fd, hdr, (size_t)n, 0);
    send(fd, body, strlen(body), 0);
}

static void http_reply_page(int fd)
{
    char hdr[128];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nContent-Type: text/html; charset=utf-8\r\n\r\n",
                     (unsigned)(sizeof(s_page) - 1));
    send(fd, hdr, (size_t)n, 0);
    send(fd, s_page, sizeof(s_page) - 1, 0);
}

static int http_read_body(int fd, size_t content_len, char **out)
{
    size_t cap = content_len + 1;
    if (cap > IMP_MAX_BODY) return -1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return -2;
    size_t got = 0;
    while (got < content_len) {
        int n = recv(fd, buf + got, content_len - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    *out = buf;
    return (got == content_len) ? 0 : -3;
}

static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '+') {
            *d++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            int hi = (s[1] >= 'A' && s[1] <= 'F') ? (s[1] - 'A' + 10)
                   : (s[1] >= 'a' && s[1] <= 'f') ? (s[1] - 'a' + 10)
                   : (s[1] - '0');
            int lo = (s[2] >= 'A' && s[2] <= 'F') ? (s[2] - 'A' + 10)
                   : (s[2] >= 'a' && s[2] <= 'f') ? (s[2] - 'a' + 10)
                   : (s[2] - '0');
            *d++ = (char)((hi << 4) | lo);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

/* Parse /upload?key=val&key=val... into an upload request struct. */
static void upload_query_parse(const char *q, uint8_t *slot, uint8_t *is_midi,
                               uint8_t *bars, char *name, size_t name_cap)
{
    char pair[128];
    const char *p = q;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);
        if (plen >= sizeof pair) plen = sizeof pair - 1;
        memcpy(pair, p, plen);
        pair[plen] = '\0';
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char *val = eq + 1;
            url_decode(val);
            if (strcmp(pair, "slot") == 0) {
                int v = atoi(val);
                if (v >= 1 && v <= 64) *slot = (uint8_t)v;
            } else if (strcmp(pair, "fmt") == 0) {
                *is_midi = (strcmp(val, "mid") == 0);
            } else if (strcmp(pair, "bars") == 0) {
                int v = atoi(val);
                if (v == 1 || v == 2) *bars = (uint8_t)v;
            } else if (strcmp(pair, "name") == 0) {
                strncpy(name, val, name_cap - 1);
                name[name_cap - 1] = '\0';
            }
        }
        if (!amp) break;
        p = amp + 1;
    }
}

static void handle_conn(int fd)
{
    char head[IMP_HEAD_CAP];
    int  nread = 0;
    if (sock_read_line(fd, head, sizeof head, &nread) < 0) return;

    char method[8], path[64];
    if (sscanf(head, "%7s %63s", method, path) != 2) return;
    path[sizeof path - 1] = '\0';

    if (strcmp(method, "GET") == 0) {
        http_reply_page(fd);
        return;
    }

    if (strcmp(method, "POST") == 0 && strncmp(path, "/upload", 7) == 0) {
        /* Drain request headers, find Content-Length. */
        uint8_t slot = 1, is_midi = 0, bars = 2;
        char    name[PROJECT_NAME_LEN];
        memset(name, 0, sizeof name);
        const char *q = strchr(path, '?');
        if (q) upload_query_parse(q + 1, &slot, &is_midi, &bars, name, sizeof name);
        if (slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS)
            slot = (uint8_t)(CONFIG_SYNTH_PROJECT_MAX_SLOTS - 1);

        size_t content_len = 0;
        char lhead[256];
        int lc = 0;
        while (lc >= 0 && sock_read_line(fd, lhead, sizeof lhead, &lc) >= 0
               && lhead[0] != '\r' && lhead[0] != '\n') {
            if (strncasecmp(lhead, "content-length:", 15) == 0) {
                content_len = (size_t)strtoul(lhead + 15, NULL, 10);
            }
        }

        if (content_len == 0 || content_len > IMP_MAX_BODY) {
            http_reply(fd, "400 Bad Request", "ERR:empty or too large");
            return;
        }

        char *body = NULL;
        if (http_read_body(fd, content_len, &body) != 0) {
            free(body);
            http_reply(fd, "400 Bad Request", "ERR:body read");
            return;
        }

        /* Park the upload, wait for the ui task to apply it. */
        portENTER_CRITICAL(&s_mux);
        s_imp.body    = body;
        s_imp.len     = content_len;
        s_imp.slot    = slot;
        s_imp.is_midi = is_midi;
        s_imp.bars    = bars;
        memcpy(s_imp.song_name, name, sizeof s_imp.song_name);
        s_imp.pending = true;
        portEXIT_CRITICAL(&s_mux);

        if (s_imp.done_sem && xSemaphoreTake(s_imp.done_sem,
                                             pdMS_TO_TICKS(IMP_RESULT_WAIT_MS)) == pdTRUE) {
            http_reply(fd, "200 OK", s_imp.result);
        } else {
            http_reply(fd, "504 Gateway Timeout", "ERR:no response");
        }
        return;
    }

    http_reply(fd, "404 Not Found", "ERR:not found");
}

/* Bring up every WiFi/netif allocation. Deliberately NEVER starts the radio:
 * esp_wifi_init() only takes the driver's control blocks and static buffers,
 * the RF is switched on later by esp_wifi_start(). main() calls this once at
 * boot, right after the heap baseline, when the internal heap is still whole -
 * allocating at that point is what every pre-fw53 release did implicitly. The
 * task re-calls it on demand; the second call is a no-op. */
esp_err_t wifi_importer_driver_init(void)
{
    if (s_driver_up) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase = nvs_flash_erase();
        if (erase != ESP_OK) ESP_LOGW(TAG, "nvs erase: %s", esp_err_to_name(erase));
        nvs_flash_init();
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
    }
    if (esp_netif_init() != ESP_OK) {
        ESP_LOGW(TAG, "netif init failed");
    }

    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) {
        ESP_LOGW(TAG, "wifi ap netif create failed");
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Trim the static (DMA-internal) buffer demand to one small contiguous
     * chunk: an import AP only moves a single HTTP POST. The static pool in
     * the INIT_CONFIG_DEFAULT tx_buf_type stays untouched. */
    cfg.static_rx_buf_num    = 4;
    cfg.dynamic_rx_buf_num   = 8;
    cfg.static_tx_buf_num    = 4;
    cfg.cache_tx_buf_num     = 4;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init %s: internal free=%u largest=%u min_free=%u "
                      "psram free=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_driver_up = true;
    return ESP_OK;
}

static void wifi_import_task(void *arg)
{
    (void)arg;
    esp_err_t err;

    s_imp.done_sem = xSemaphoreCreateBinary();
    if (!s_imp.done_sem) {
        imp_set_state(IMP_ST_FAIL, "WiFi: no mem");
        imp_self_delete();
        return;
    }

    imp_set_state(IMP_ST_WIFI_INIT, "WiFi: driver");
    err = wifi_importer_driver_init();
    if (err != ESP_OK) {
        unsigned ifree  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        unsigned ilarge = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        imp_set_state(IMP_ST_FAIL, "WiFi:init no mem i=%uKB lg=%uKB",
                      ifree / 1024, ilarge / 1024);
        imp_self_delete();
        return;
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, CONFIG_SYNTH_WIFI_AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.channel         = CONFIG_SYNTH_WIFI_AP_CHANNEL;
    wc.ap.max_connection  = 2;
    wc.ap.authmode        = WIFI_AUTH_OPEN;
    wc.ap.ssid_hidden     = 0;

    imp_set_state(IMP_ST_MODE, "WiFi: boot AP");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        imp_set_state(IMP_ST_FAIL, "WiFi: fail mode %s", esp_err_to_name(err));
        imp_self_delete();
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        imp_set_state(IMP_ST_FAIL, "WiFi: fail cfg %s", esp_err_to_name(err));
        imp_self_delete();
        return;
    }
    imp_set_state(IMP_ST_START, "WiFi: start...");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        imp_set_state(IMP_ST_FAIL, "WiFi: fail start %s", esp_err_to_name(err));
        imp_self_delete();
        return;
    }

    /* Give the AP a moment to assign 192.168.4.1 before the socket binds. */
    vTaskDelay(pdMS_TO_TICKS(400));
    imp_set_state(IMP_ST_READY, "WiFi: AP %s", CONFIG_SYNTH_WIFI_AP_SSID);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        imp_set_state(IMP_ST_FAIL, "WiFi: socket fail");
        imp_self_delete();
        return;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(listen_fd, 2) < 0) {
        imp_set_state(IMP_ST_FAIL, "WiFi: bind fail");
        close(listen_fd);
        imp_self_delete();
        return;
    }
    ESP_LOGI(TAG, "listening on 192.168.4.1:80");

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) continue;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle_conn(cfd);
        close(cfd);
    }
}

/* ── public ── */

void wifi_import_service(void)
{
    char *body;
    uint8_t slot, is_midi, bars;
    char song_name[PROJECT_NAME_LEN];
    size_t len;

    portENTER_CRITICAL(&s_mux);
    if (!s_imp.pending) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    body     = s_imp.body;
    len      = s_imp.len;
    slot     = s_imp.slot;
    is_midi  = s_imp.is_midi;
    bars     = s_imp.bars;
    memcpy(song_name, s_imp.song_name, sizeof song_name);
    s_imp.pending = false;
    portEXIT_CRITICAL(&s_mux);

    char out[sizeof s_imp.result];
    out[0] = '\0';
    bool ok = false;

    if (is_midi) {
        /* Convert the raw SMF stream to an AMYSONG text on the fly (mirror of
         * tools/midi2amysong.py), then save/apply it like a pasted song. */
        char *text = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
        if (!text) {
            snprintf(s_imp.result, sizeof s_imp.result, "ERR:no mem");
            free(body);
            if (s_imp.done_sem) xSemaphoreGive(s_imp.done_sem);
            return;
        }
        char cvt_err[96];
        if (midi_amysong_convert((const uint8_t *)body, len,
                                 (int)bars, 256, song_name,
                                 text, 2048, cvt_err, sizeof cvt_err) != 0) {
            snprintf(s_imp.result, sizeof s_imp.result, "ERR:%s",
                     cvt_err[0] ? cvt_err : "midi parse failed");
        } else {
            ok = song_import_apply(slot, text, NULL, out, sizeof out);
        }
        free(text);
    } else {
        ok = song_import_apply(slot, body, NULL, out, sizeof out);
    }

    free(body);
    if (ok) {
        snprintf(s_imp.result, sizeof s_imp.result,
                 "OK:saved to slot %u", (unsigned)(slot + 1));
    } else {
        snprintf(s_imp.result, sizeof s_imp.result, "ERR:%s",
                 out[0] ? out : "import failed");
    }
    if (s_imp.done_sem) xSemaphoreGive(s_imp.done_sem);
}

const char *wifi_import_status_line(void)
{
    if (s_dir_state == IMP_ST_FAIL) {
        return s_state_text;                  /* keep showing the failure   */
    }
    if (s_dir_state == IMP_ST_READY) {
        if (s_ready_tick &&
            (xTaskGetTickCount() - s_ready_tick) < pdMS_TO_TICKS(IMP_STATUS_LINGER_MS)) {
            return s_state_text;              /* "WiFi: AP AMYSYNTH" briefly */
        }
        return NULL;                          /* done: restore normal hints  */
    }
    if (s_dir_state == IMP_ST_IDLE) return NULL;
    return s_state_text;                      /* mid bring-up               */
}

esp_err_t wifi_importer_start(void)
{
    /* Never block the caller (app_main or the Projects-menu click): all
     * WiFi/radio bring-up runs on the task below, which - being unregistered -
     * cannot trip the task WDT even if the driver stalls. The task is pinned
     * to core 1 (the AMY DSP core): RF/PHY bring-up may disable scheduling on
     * its own core for bursts, which must never touch the core 0 UI/input. The
     * boot screen therefore always proceeds to the normal UI; the hint strip
     * reports the AP state via status_line. */
    if (s_imp.task_created) return ESP_OK;
    BaseType_t ok = xTaskCreatePinnedToCore(wifi_import_task, "wifi_import",
                                            8192, NULL, 5, NULL, 1);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    s_imp.task_created = true;
    return ESP_OK;
}

bool wifi_import_ap_running(void)
{
    return s_imp.task_created && s_dir_state == IMP_ST_READY;
}

const char *wifi_import_ap_state(void)
{
    static char short_text[16];
    switch (s_dir_state) {
        case IMP_ST_READY:
            snprintf(short_text, sizeof short_text, "AP %s",
                     CONFIG_SYNTH_WIFI_AP_SSID);
            return short_text;
        case IMP_ST_FAIL:  return "FAIL";
        case IMP_ST_IDLE:  return "Off";
        default:           return "Start";
    }
}

#endif /* CONFIG_SYNTH_WIFI_IMPORT */