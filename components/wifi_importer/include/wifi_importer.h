#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Song import over WiFi. When started, the device runs a SoftAP
 * ("AMYSYNTH") with a bare HTTP server on 192.168.4.1: a GET / serves a
 * tiny page whose JS reads an uploaded/text-pasted AMYSONG file and POSTs
 * the raw text to /upload; the firmware parses it into the sequencer and
 * saves it into a project slot, all in one click. No companion app, no PC
 * install. The AP is ON DEMAND (Projects menu) and OFF at boot by default:
 * boot is therefore identical to a build without WiFi. */

/* Start the import AP. The whole WiFi bring-up (RF init, PHY calibration,
 * SoftAP) happens on a dedicated task pinned to the DSP core - never on
 * the caller or the UI core - so a radio that cannot start can never stall
 * boot or freeze the display/input (that task is not registered with the
 * task watchdog either). Runs once; the AP stays until reboot. */
esp_err_t wifi_importer_start(void);

/* True while the AP task is up and serving. */
bool wifi_import_ap_running(void);

/* Short current state for a menu value field, e.g. "Off", "AP AMYSYNTH",
 * "Start", "FAIL". */
const char *wifi_import_ap_state(void);

/* Poll pending uploads. MUST run on the sequencer single-applier task
 * (synth_ui_task, once per frame): applying an upload rebuilds layers
 * through sequencer_core_add/delete_layer and saves the project. */
void wifi_import_service(void);

/* Boot status for the UI's hint strip. Returns NULL once the AP has been up
 * for a few seconds or the opponent UI is irrelevant; non-NULL otherwise,
 * e.g. "WiFi: starting...", "WiFi: fail <reason>". */
const char *wifi_import_status_line(void);

#ifdef __cplusplus
}
#endif