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

/* Start the SoftAP + HTTP server. Non-fatal: logs and returns silently on
 * any WiFi failure so the synth still boots without a radio. */
esp_err_t wifi_importer_init(void);

/* Poll pending uploads. MUST run on the sequencer single-applier task
 * (synth_ui_task, once per frame): applying an upload rebuilds layers
 * through sequencer_core_add/delete_layer and saves the project. */
void wifi_import_service(void);

#ifdef __cplusplus
}
#endif