#include "usb_audio.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // xTaskGetTickCount / pdMS_TO_TICKS
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_compiler.h"   // likely()/unlikely() branch hints
#include <stdatomic.h>
#include <string.h>

#ifndef CONFIG_USB_AUDIO_DIAGNOSTICS
#define CONFIG_USB_AUDIO_DIAGNOSTICS 0
#endif

static const char *TAG = "usb_audio";

// ~170 ms buffer @ 48 kHz stereo 16-bit → very safe for drum prototyping
#define RING_BUFFER_SIZE  (32768)   // must be power-of-2 for fast modulo
// Allocated from PSRAM in usb_audio_init() (64 KB). It used to be a static
// int16_t[RING_BUFFER_SIZE] in internal DRAM, which alone consumed 64 KB of the
// scarce internal SRAM (~54 KB free at the time). The buffer is touched at
// USB-frame / render-block granularity, not in the per-sample DSP inner loop, so
// PSRAM latency is irrelevant here while the internal-RAM relief is significant.
//
// LOCK-FREE SPSC: there is exactly ONE producer (the AMY render task, Core 1,
// via usb_audio_write_stereo) and exactly ONE consumer (usb_mic_task, Core 0,
// via uac_input_cb). The old design guarded the indices with a mutex taken at
// portMAX_DELAY, which let the lower-priority consumer block the high-priority
// render task across PSRAM copies (priority inversion on a strict-1:1 5333 us
// budget). With single-producer/single-consumer we drop the mutex entirely:
//   - s_write_idx is WRITER-owned: only the producer stores it; the consumer
//     only loads it.
//   - s_read_idx is READER-owned: only the consumer stores it; the producer
//     only loads it.
// Each side publishes its own index with release ordering AFTER its data copy
// and loads the other side's index with acquire ordering BEFORE its copy, so a
// reader never observes an advanced s_write_idx before the samples it points to
// are committed to PSRAM (and vice-versa for free-slot accounting).
static int16_t *s_ring_buffer = NULL;
static _Atomic size_t s_write_idx = 0;   // writer-owned (producer)
static _Atomic size_t s_read_idx  = 0;   // reader-owned (consumer)

static bool s_initialized = false;

// Consumer-liveness: tick of the most recent host pull (uac_input_cb call).
// Written only by the consumer; read relaxed cross-core for advisory gating.
#define USB_AUDIO_CONSUMER_TIMEOUT_MS 200
static _Atomic uint32_t s_last_consumed_tick = 0;

bool usb_audio_consumer_active(void)
{
    if (!s_initialized) return false;
    uint32_t last = atomic_load_explicit(&s_last_consumed_tick, memory_order_relaxed);
    if (last == 0) return false;   // host never pulled yet
    return (xTaskGetTickCount() - last) < pdMS_TO_TICKS(USB_AUDIO_CONSUMER_TIMEOUT_MS);
}

#if CONFIG_USB_AUDIO_DIAGNOSTICS
// Producer-owned counters (written only by usb_audio_write_stereo).
static uint32_t s_write_calls = 0;
static uint32_t s_write_drop_events = 0;
static size_t s_peak_fill_samples = 0;
static int16_t s_peak_abs_sample = 0;
// Consumer-owned counter (written only by uac_input_cb).
static uint32_t s_underrun_events = 0;
#endif

// Advisory fill level for diagnostics. Loads both indices relaxed; the result
// is a momentary estimate, never used for correctness decisions.
static inline size_t usb_audio_fill_samples_relaxed(void)
{
    size_t w = atomic_load_explicit(&s_write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&s_read_idx, memory_order_relaxed);
    return (w - r + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx)
{
    (void)cb_ctx;

    if (!s_initialized) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    // Consumer liveness + flush-on-resume. The consumer owns s_read_idx, so it
    // may legally advance it to s_write_idx to discard stale buffered audio when
    // the host resumes after a gap (so playback starts fresh, not 200 ms late).
    uint32_t now = xTaskGetTickCount();
    uint32_t last = atomic_load_explicit(&s_last_consumed_tick, memory_order_relaxed);
    if (last != 0 && (now - last) > pdMS_TO_TICKS(USB_AUDIO_CONSUMER_TIMEOUT_MS)) {
        size_t w = atomic_load_explicit(&s_write_idx, memory_order_acquire);
        atomic_store_explicit(&s_read_idx, w, memory_order_release);
    }
    atomic_store_explicit(&s_last_consumed_tick, now, memory_order_relaxed);

    // CONSUMER side (single reader). s_read_idx is ours to advance; we acquire
    // s_write_idx so every sample below [read, write) is guaranteed committed.
    size_t read_idx = atomic_load_explicit(&s_read_idx, memory_order_relaxed);
    size_t write_idx = atomic_load_explicit(&s_write_idx, memory_order_acquire);

    size_t available_samples = (write_idx - read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    size_t samples_to_copy = (len / 2 < available_samples) ? len / 2 : available_samples;

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (unlikely(samples_to_copy < (len / 2))) {
        s_underrun_events++;   // consumer-owned counter
    }
#endif

    // Copy with wrap-around
    size_t first = RING_BUFFER_SIZE - read_idx;
    if (first > samples_to_copy) first = samples_to_copy;

    memcpy(buf, &s_ring_buffer[read_idx], first * sizeof(int16_t));
    if (samples_to_copy > first) {
        memcpy(buf + first * sizeof(int16_t),
               s_ring_buffer,
               (samples_to_copy - first) * sizeof(int16_t));
    }

    // Publish the advanced read index with release ordering so the producer
    // only sees the slots freed after the copy above has completed.
    atomic_store_explicit(&s_read_idx,
                          (read_idx + samples_to_copy) % RING_BUFFER_SIZE,
                          memory_order_release);
    *bytes_read = samples_to_copy * 2;   // bytes

    // Zero-pad on underrun (prevents pops)
    if (*bytes_read < len) {
        memset(buf + *bytes_read, 0, len - *bytes_read);
        *bytes_read = len;
    }

    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *ctx)
{
    ESP_LOGI(TAG, "Mute → %lu", mute);
}

static void uac_set_volume_cb(uint32_t volume, void *ctx)
{
    ESP_LOGI(TAG, "Volume → %lu%%", volume);
}

esp_err_t usb_audio_init(void)
{
    if (s_initialized) return ESP_OK;

    // 64 KB ring buffer in PSRAM (was a static array in internal DRAM).
    s_ring_buffer = heap_caps_malloc(RING_BUFFER_SIZE * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_buffer == NULL) {
        ESP_LOGE(TAG, "ring buffer alloc failed (%u bytes PSRAM)",
                 (unsigned)(RING_BUFFER_SIZE * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&s_write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&s_read_idx, 0, memory_order_relaxed);

    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb   = NULL,                    // we only need mic direction
        .input_cb    = uac_input_cb,
        .set_mute_cb = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx      = NULL
    };

    esp_err_t ret = uac_device_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(ret));
        heap_caps_free(s_ring_buffer);
        s_ring_buffer = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "USB Audio ready – 48 kHz 16-bit stereo mic");
    ESP_LOGI(TAG, "Plug into PC and select the new audio input device");
    return ESP_OK;
}

esp_err_t usb_audio_write_stereo(const int16_t *data, size_t num_frames)
{
    if (!s_initialized || !data || num_frames == 0) return ESP_ERR_INVALID_STATE;

    // No host consuming → do not churn the ring. Keep it empty so the host gets
    // fresh audio on open. Not a drop (nobody is listening); report success.
    if (!usb_audio_consumer_active()) {
        return ESP_OK;
    }

    const size_t samples = num_frames * 2;   // interleaved L,R

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    s_write_calls++;   // producer-owned counter
#endif

    // PRODUCER side (single writer). s_write_idx is ours to advance; we acquire
    // s_read_idx so the free-slot count reflects everything the consumer has
    // already drained.
    size_t write_idx = atomic_load_explicit(&s_write_idx, memory_order_relaxed);
    size_t read_idx  = atomic_load_explicit(&s_read_idx, memory_order_acquire);

    size_t free_slots = (read_idx - write_idx - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;

    // ALL-OR-NOTHING: if the whole block does not fit we write nothing and
    // report the drop. A partial write would splice a half-block against the
    // next full block and mangle the waveform; a clean whole-block gap is
    // absorbed by the ~170 ms buffer / host resync instead.
    if (unlikely(samples > free_slots)) {
#if CONFIG_USB_AUDIO_DIAGNOSTICS
        s_write_drop_events++;
#endif
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    int16_t local_peak = 0;
#endif

    // Copy the block into the ring with wrap-around (two memcpys, no per-sample
    // index churn). The producer is the sole writer of these slots, so this is
    // safe before publishing the new write index.
    size_t first = RING_BUFFER_SIZE - write_idx;
    if (first > samples) first = samples;
    memcpy(&s_ring_buffer[write_idx], data, first * sizeof(int16_t));
    if (samples > first) {
        memcpy(s_ring_buffer, data + first, (samples - first) * sizeof(int16_t));
    }

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    for (size_t i = 0; i < samples; i++) {
        int16_t sample = data[i];
        int16_t abs_sample = (sample < 0) ? (int16_t)(-sample) : sample;
        if (abs_sample > local_peak) {
            local_peak = abs_sample;
        }
    }
    if (local_peak > s_peak_abs_sample) {
        s_peak_abs_sample = local_peak;
    }
#endif

    // Publish the advanced write index with release ordering so the consumer
    // only observes it after all sample stores above are committed to PSRAM.
    atomic_store_explicit(&s_write_idx,
                          (write_idx + samples) % RING_BUFFER_SIZE,
                          memory_order_release);

#if CONFIG_USB_AUDIO_DIAGNOSTICS
    size_t fill_samples = usb_audio_fill_samples_relaxed();
    if (fill_samples > s_peak_fill_samples) {
        s_peak_fill_samples = fill_samples;
    }
#endif

    return ESP_OK;
}

esp_err_t usb_audio_write_mono(const int16_t *data, size_t num_samples)
{
    if (!s_initialized || !data || num_samples == 0) return ESP_ERR_INVALID_STATE;

    // Expand to interleaved stereo on the stack and hand off in one all-or-
    // nothing, lock-free write (the old version called write_stereo once PER
    // sample). Chunked so the stack frame stays small for arbitrary lengths.
    int16_t frames[128 * 2];
    const size_t chunk = 128;
    for (size_t off = 0; off < num_samples; off += chunk) {
        size_t n = (num_samples - off < chunk) ? (num_samples - off) : chunk;
        for (size_t i = 0; i < n; i++) {
            frames[i * 2]     = data[off + i];
            frames[i * 2 + 1] = data[off + i];
        }
        esp_err_t err = usb_audio_write_stereo(frames, n);
        if (err != ESP_OK) return err;   // drop the rest of this call on full buffer
    }
    return ESP_OK;
}

void usb_audio_diag_get_snapshot(usb_audio_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->initialized = s_initialized;

    if (!s_initialized) {
        return;
    }

    // Lock-free, advisory read. fill_samples is a momentary estimate; the diag
    // counters are each written by a single owner task so plain reads here are
    // race-tolerant (we only ever lose/gain one update, never corrupt state).
    snapshot->fill_samples = usb_audio_fill_samples_relaxed();
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    snapshot->peak_fill_samples = s_peak_fill_samples;
    snapshot->write_calls = s_write_calls;
    snapshot->write_drop_events = s_write_drop_events;
    snapshot->underrun_events = s_underrun_events;
    snapshot->peak_abs_sample = s_peak_abs_sample;
#endif
}

void usb_audio_diag_reset(void)
{
#if CONFIG_USB_AUDIO_DIAGNOSTICS
    if (!s_initialized) {
        return;
    }

    // Best-effort reset; producer/consumer may race a single increment, which
    // is harmless for advisory diagnostics.
    s_write_calls = 0;
    s_write_drop_events = 0;
    s_underrun_events = 0;
    s_peak_fill_samples = 0;
    s_peak_abs_sample = 0;
#else
    (void)0;
#endif
}

// from main int16_t mix[960];   // 10 ms @ 48 kHz mono
// ... fill mix with your drum samples ... usb_audio_write_mono(mix, 960);