#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BLE MIDI transport: BLE-MIDI GATT service (peripheral role) on NimBLE.
 * Lifecycle is owned by radio_manager - these entry points are only valid
 * inside a radio session (between nimble_port_init and nimble_port_deinit). */

/* Host-stack configuration + GATT registration. Call after nimble_port_init()
 * and before nimble_port_freertos_init(). Returns 0 on success. */
int ble_midi_setup(void);

/* NimBLE host task body: runs nimble_port_run() until nimble_port_stop(),
 * then self-deletes via nimble_port_freertos_deinit(). Pass to
 * nimble_port_freertos_init(). */
void ble_midi_host_task(void *param);

/* True while a central is connected. */
bool ble_midi_connected(void);

/* Hook invoked (from the NimBLE host task) when a central disconnects.
 * Used for stuck-note safety: the registered hook releases held live notes.
 * `cb` must have static lifetime; NULL detaches. */
void ble_midi_set_disconnect_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
