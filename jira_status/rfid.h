#pragma once
#include <Arduino.h>

// Badge reader (RC522 over SPI): tap your badge when you arrive and when you leave. Arriving sets
// your presence status to IN, leaving to LEFT, through the same Jira user property your bot uses.
void rfid_begin();
void rfid_loop();  // call from loop(); polls the reader a few times a second
