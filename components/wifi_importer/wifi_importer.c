#include "wifi_importer.h"
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

#if CONFIG_SYNTH_WIFI_IMPORT

static const char *TAG = "wifi_import";

#define IMP_MAX_BODY  (60 * 1024)          /* AMYSONG text cap        */
#define IMP_HEAD_CAP  (2048)               /* HTTP head cap           */
#define IMP_RESULT_WAIT_MS (12000)

/* ── pending-upload handoff ────────────────────────────────────────────────
 * The socket task receives the body and parks it; wifi_import_service()
 * on the synth_ui task applies it (only that task may rebuild layers) and
 * posts the outcome back. The HTTP handler blocks on s_done_sem meanwhile,
 * so the browser gets either OK or the parser's line-numbered reason. */

typedef struct {
    char            *body;
    uint8_t          slot;
    volatile bool    pending;     /* release-published by the socket task     */
    SemaphoreHandle_t done_sem;
    char             result[256];
} imp_state_t;

static imp_state_t s_imp;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── embedded page ── */

static const char s_page[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>AMYSYNTH import</title></head><body>"
    "<h2>AMYSYNTH song import</h2>"
    "<form id=f>"
    "Slot <input type=number name=slot min=1 max=64 value=1 style=width:4em>"
    "&nbsp;&nbsp;<b id=st>idle</b><br><br>"
    "File: <input type=file id=file accept=.txt,.amysong><br><br>"
    "<i>or paste a song below:</i><br>"
    "<textarea id=txt rows=14 cols=56 placeholder=\"amysong 1\nname &quot;Demo&quot;\nbpm 120\npattern 32\nlayer melodic 256\nnotes 0 . +4 . . . +7 . . . . . . . . .\nlayer drum\nhit 0 x . . . x . . . x . . . x . . .\n\"></textarea><br>"
    "<button type=button onclick=go()>Import</button>"
    "</form>"
    "<script>"
    "function go(){"
    "var f=document.getElementById('file').files[0],txt=document.getElementById('txt');"
    "if(f){var rd=new FileReader();"
    "rd.onload=function(){send(rd.result)};rd.readAsText(f);return;}"
    "send(txt.value);"
    "}"
    "function send(text){"
    "var s=document.getElementById('slot').value||'1';"
    "fetch('/upload?slot='+s,{method:'POST',body:text}).then(function(r){return r.text()}).then(function(t){"
    "document.getElementById('st').textContent=t;"
    "}).catch(function(){document.getElementById('st').textContent='NET ERR';});"
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
        /* Drain request headers, find slot + Content-Length. */
        uint8_t slot = 1;
        char    name[PROJECT_NAME_LEN];
        memset(name, 0, sizeof name);
        const char *q = strchr(path, '?');
        if (q && sscanf(q + 1, "slot=%hhu", &slot) == 1 && slot < 1) slot = 1;
        if (slot >= CONFIG_SYNTH_PROJECT_MAX_SLOTS) slot = (uint8_t)(CONFIG_SYNTH_PROJECT_MAX_SLOTS - 1);

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
        s_imp.slot    = slot;
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

static void ap_server_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
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
        ESP_LOGE(TAG, "bind/listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
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
    uint8_t slot;

    portENTER_CRITICAL(&s_mux);
    if (!s_imp.pending) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    body     = s_imp.body;
    slot     = s_imp.slot;
    s_imp.pending = false;
    portEXIT_CRITICAL(&s_mux);

    char out[sizeof s_imp.result];
    out[0] = '\0';
    bool ok = song_import_apply(slot, body, NULL, out, sizeof out);

    free(body);
    if (ok) {
        snprintf(s_imp.result, sizeof s_imp.result,
                 "OK:saved to slot %u", (unsigned)(slot + 1));
    } else {
        snprintf(s_imp.result, sizeof s_imp.result, "ERR:%s",
                 out[0] ? out : "parse failed");
    }
    if (s_imp.done_sem) xSemaphoreGive(s_imp.done_sem);
}

esp_err_t wifi_importer_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase = nvs_flash_erase();
        if (erase != ESP_OK) ESP_LOGW(TAG, "nvs erase: %s", esp_err_to_name(erase));
        nvs_flash_init();
    }

    s_imp.done_sem = xSemaphoreCreateBinary();
    if (!s_imp.done_sem) return ESP_ERR_NO_MEM;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
    }
    if (esp_netif_init() != ESP_OK) {
        ESP_LOGW(TAG, "netif init failed");
    }

    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) {
        ESP_LOGW(TAG, "wifi ap netif failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init: %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, CONFIG_SYNTH_WIFI_AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.channel         = CONFIG_SYNTH_WIFI_AP_CHANNEL;
    wc.ap.max_connection  = 2;
    wc.ap.authmode        = WIFI_AUTH_OPEN;
    wc.ap.ssid_hidden     = 0;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi mode: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi config: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(err));
        return err;
    }

    /* Give the AP a moment to come up before binding 192.168.4.1. */
    vTaskDelay(pdMS_TO_TICKS(400));

    xTaskCreatePinnedToCore(ap_server_task, "wifi_import", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "SoftAP '%s' up - browse http://192.168.4.1/",
             CONFIG_SYNTH_WIFI_AP_SSID);
    return ESP_OK;
}

#endif /* CONFIG_SYNTH_WIFI_IMPORT */