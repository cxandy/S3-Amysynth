/* Serial test harness command task. See include/harness.h for the protocol
 * contract and docs (overlay repo) for the full design. Everything below
 * compiles out when CONFIG_DEV_SERIAL_HARNESS is off. */

#include "sdkconfig.h"

#if CONFIG_DEV_SERIAL_HARNESS

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "harness.h"
#include "sequencer_core.h"
#include "dropout_stats.h"
#include "render_stats.h"

static const char *TAG = "harness";

#define HARNESS_UART        UART_NUM_0
#define HARNESS_RX_BUF      1024
#define HARNESS_LINE_MAX    160
#define HARNESS_TASK_STACK  6144
#define HARNESS_TASK_PRIO   3

static const harness_hooks_t *s_hooks = NULL;

/* Responses go through printf like the seq dump does: raw lines on the
 * console UART, no ESP_LOG prefix, so the host parser keys on "H<" columns
 * regardless of interleaved log output. */
#define REPLY(fmt, ...) printf("H< " fmt "\n", ##__VA_ARGS__)

/* Return the value of "key=" in args, or NULL. Args is the mutable tail of
 * the command line; values are space-delimited. */
static char *arg_value(char *args, const char *key)
{
    size_t klen = strlen(key);
    for (char *tok = strtok(args, " "); tok; tok = strtok(NULL, " ")) {
        if (strncmp(tok, key, klen) == 0 && tok[klen] == '=') {
            return tok + klen + 1;
        }
    }
    return NULL;
}

static void cmd_sys_build(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    REPLY("ok fw=%s proj=%s idf=%s", app->version, app->project_name,
          esp_get_idf_version());
}

static void cmd_st_heap(void)
{
    REPLY("ok internal=%u largest=%u min=%u psram=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void cmd_st_drop(void)
{
    dropout_stats_t d;
    dropout_stats_get(&d);
    REPLY("ok wire_zlp=%" PRIu32 " ring_underrun=%" PRIu32
          " ring_overrun=%" PRIu32 " render_overrun=%" PRIu32
          " chunk_drop=%" PRIu32,
          d.wire_zlp, d.ring_underrun, d.ring_overrun, d.render_overrun,
          d.chunk_drop);
}

#if CONFIG_AMYSYNTH_DROPOUT_TS
static void cmd_st_dropts(void)
{
    /* Harness task only, so a static bounce buffer beats 1 KB of stack. */
    static int64_t ts[DROPOUT_TS_RING];
    uint32_t seq;
    uint32_t n = dropout_ts_snapshot(ts, &seq);
    for (uint32_t i = 0; i < n; i++) {
        printf("DROPTS %" PRIu32 " %lld\n", seq - n + i, (long long)ts[i]);
    }
    REPLY("ok seq=%" PRIu32 " n=%" PRIu32 " ring=%d", seq, n, DROPOUT_TS_RING);
}
#endif

static void cmd_in_btn(char *args)
{
    if (s_hooks == NULL || s_hooks->inject_button == NULL) {
        REPLY("err no button hook");
        return;
    }
    /* arg_value consumes args via strtok; grab both keys in one pass by
     * scanning the copies. */
    char copy[HARNESS_LINE_MAX];
    strlcpy(copy, args, sizeof(copy));
    char *id_s = arg_value(copy, "id");
    char copy2[HARNESS_LINE_MAX];
    strlcpy(copy2, args, sizeof(copy2));
    char *act_s = arg_value(copy2, "act");
    if (id_s == NULL || act_s == NULL) {
        REPLY("err usage: in.btn id=<n> act=down|up|click|long");
        return;
    }
    harness_btn_act_t act;
    if      (strcmp(act_s, "down")  == 0) act = HARNESS_BTN_DOWN;
    else if (strcmp(act_s, "up")    == 0) act = HARNESS_BTN_UP;
    else if (strcmp(act_s, "click") == 0) act = HARNESS_BTN_CLICK;
    else if (strcmp(act_s, "long")  == 0) act = HARNESS_BTN_LONG;
    else { REPLY("err bad act '%s'", act_s); return; }

    s_hooks->inject_button(atoi(id_s), act);
    REPLY("ok");
}

static void cmd_in_enc(char *args)
{
    if (s_hooks == NULL || s_hooks->inject_encoder_steps == NULL) {
        REPLY("err no encoder hook");
        return;
    }
    char *d_s = arg_value(args, "delta");
    if (d_s == NULL) { REPLY("err usage: in.enc delta=<n>"); return; }
    long steps = strtol(d_s, NULL, 10);
    if (steps == 0) { REPLY("err zero delta"); return; }
    s_hooks->inject_encoder_steps(steps);
    REPLY("ok");
}

static void cmd_tr_bpm(char *args)
{
    char *tok = strtok(args, " ");
    if (tok != NULL) {
        long bpm = strtol(tok, NULL, 10);
        if (bpm <= 0 || bpm > UINT16_MAX) { REPLY("err bad bpm"); return; }
        sequencer_core_set_bpm((uint16_t)bpm);
    }
    REPLY("ok bpm=%u", (unsigned)sequencer_core_get_bpm());
}

static void handle_line(char *line)
{
    /* Strip the command sentinel; tolerate its absence so hand-typed
     * commands in a terminal work too. */
    if (strncmp(line, "H>", 2) == 0) line += 2;
    while (*line == ' ') line++;
    if (*line == '\0') return;          /* blank/log echo - ignore silently */

    char *args = strchr(line, ' ');
    if (args != NULL) { *args++ = '\0'; } else { args = line + strlen(line); }

    if      (strcmp(line, "sys.echo") == 0)  REPLY("ok %s", args);
    else if (strcmp(line, "sys.build") == 0) cmd_sys_build();
    else if (strcmp(line, "st.heap") == 0)   cmd_st_heap();
    else if (strcmp(line, "st.drop") == 0)   cmd_st_drop();
    else if (strcmp(line, "st.dropts") == 0) {
#if CONFIG_AMYSYNTH_DROPOUT_TS
        cmd_st_dropts();
#else
        REPLY("err dropout ts not compiled in");
#endif
    }
    else if (strcmp(line, "st.render") == 0) {
#if CONFIG_AMYSYNTH_RENDER_STATS
        render_stats_report();              /* prints its own block */
        REPLY("ok");
#else
        REPLY("err render_stats not compiled in");
#endif
    }
    else if (strcmp(line, "st.seqdump") == 0) {
#if CONFIG_SYNTH_DEV_MENU
        sequencer_core_dump_state();        /* prints SEQDUMP BEGIN/END */
        REPLY("ok");
#else
        REPLY("err dev menu not compiled in");
#endif
    }
    else if (strcmp(line, "tr.play") == 0) { sequencer_core_set_playing(true);  REPLY("ok"); }
    else if (strcmp(line, "tr.stop") == 0) { sequencer_core_set_playing(false); REPLY("ok"); }
    else if (strcmp(line, "tr.bpm") == 0)  cmd_tr_bpm(args);
    else if (strcmp(line, "in.btn") == 0)  cmd_in_btn(args);
    else if (strcmp(line, "in.enc") == 0)  cmd_in_enc(args);
    else REPLY("err unknown cmd '%s'", line);
}

static void harness_task(void *arg)
{
    (void)arg;
    static char line[HARNESS_LINE_MAX];
    size_t len = 0;

    REPLY("ok harness ready");
    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(HARNESS_UART, &ch, 1, pdMS_TO_TICKS(100));
        if (n != 1) continue;
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = (char)ch;
            } else {
                len = 0;                    /* oversize: drop the whole line */
                REPLY("err line too long");
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        handle_line(line);
    }
}

esp_err_t harness_init(const harness_hooks_t *hooks)
{
    if (hooks == NULL) return ESP_ERR_INVALID_ARG;
    if (s_hooks != NULL) return ESP_ERR_INVALID_STATE;

    /* RX driver only: console TX (logs, replies via printf) keeps using the
     * default non-driver path; both coexist on UART0. */
    esp_err_t err = uart_driver_install(HARNESS_UART, HARNESS_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_hooks = hooks;
    if (xTaskCreatePinnedToCore(harness_task, "harness_cmd",
                                HARNESS_TASK_STACK, NULL,
                                HARNESS_TASK_PRIO, NULL, 0) != pdPASS) {
        uart_driver_delete(HARNESS_UART);
        s_hooks = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial harness listening on UART0");
    return ESP_OK;
}

#endif /* CONFIG_DEV_SERIAL_HARNESS */
