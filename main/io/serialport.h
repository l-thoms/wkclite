#pragma once

// Serial debug key input.
//
// Lets a PC terminal that is connected to the console UART simulate the device
// keys, so the UI can be driven even when no BLE connection is available.
//
//   W / A / S / D  -> up / left / down / right
//   Space          -> OK (confirm)
//   Q              -> back
//   E              -> menu
//   Z              -> GPIO0 (power) key press, without entering light sleep
//
// Holding a key on the PC repeats the character. The repeats are coalesced
// (one key per SERIALPORT_KEY_INTERVAL_MS, the newest one wins) so the key
// queue cannot grow: the first press is delivered immediately, the key still
// held is delivered at the limited rate and the last one is delivered when the
// key is released.
void serialport_init();
