#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Radio session lifecycle: the ONLY module that touches nimble_port_* /
 * esp_bt_* (portability seam for the P4 + esp-hosted target). Sessions are
 * on-demand: nothing is initialized at boot, and stopping a session returns
 * the BLE stack's heap allocations.
 *
 * Threading contract: request_start/request_stop only set a flag (callable
 * from the button task); every state transition - including the blocking
 * NimBLE init/teardown - runs inside radio_manager_service(), called once per
 * frame by synth_ui (same deferred-applier pattern as projects_menu_service). */

typedef enum {
    RADIO_OFF = 0,
    RADIO_STARTING,     /* transient inside one service() call */
    RADIO_ACTIVE,       /* advertising or connected */
    RADIO_STOPPING,     /* transient inside one service() call */
    RADIO_FAILED_RAM,   /* pre-flight refusal: not enough internal heap */
    RADIO_FAILED_ERR,   /* stack init failed; session unwound */
} radio_state_t;

/* Hooks into the synth application, registered once at boot from main so
 * this component stays free of synth_core dependencies. All hooks run on
 * the synth_ui task except peer_disconnect (NimBLE host task). */
typedef struct {
    void (*session_start)(void);   /* after the stack is up (prepare live slot) */
    void (*session_stop)(void);    /* before teardown (release held notes) */
    void (*peer_disconnect)(void); /* central dropped (release held notes) */
} radio_hooks_t;

void radio_manager_init(const radio_hooks_t *hooks); /* hooks: static lifetime */

void radio_manager_request_start(void);
void radio_manager_request_stop(void);

/* Drain pending requests; called once per synth_ui frame. */
void radio_manager_service(void);

radio_state_t radio_manager_state(void);
bool          radio_manager_connected(void);

#ifdef __cplusplus
}
#endif
