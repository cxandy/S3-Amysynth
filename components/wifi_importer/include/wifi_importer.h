#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Song import over WiFi. The device runs a SoftAP ("AMYSYNTH") with a bare
 * HTTP server on 192.168.4.1: a GET / serves a tiny page whose JS reads an
 * uploaded/text-pasted AMYSONG file and POSTs the raw text to /upload;
 * the firmware parses it into the sequencer and saves it into a project
 * slot, all in one click. No companion app, no PC install. */

/* Start the import AP. The whole WiFi bring-up (RF init, PHY calibration,
 * SoftAP) happens on a dedicated task, never on the caller, so a radio that
 * cannot start can never stall boot or trip the task watchdog (that task is
 * not registered with it). Boot continues regardless. */
esp_err_t wifi_importer_init(void);

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