// Sony headphones battery levels (left / right / case) over the proprietary
// RFCOMM protocol used by the "Sony | Headphones Connect" app.
// Protocol details follow the reverse engineering done by Gadgetbridge.

#ifndef SONY_BATTERY_H
#define SONY_BATTERY_H

#include <stdbool.h>
#include "btstack.h"

// Call once from btstack_main after l2cap_init. debug: dump every frame.
void sony_battery_init(bool debug);

// Start SDP lookup + RFCOMM connection to an already connected headset
void sony_battery_connect(bd_addr_t addr);

// Headset went away: close channel and forget state
void sony_battery_disconnect(void);

#endif
