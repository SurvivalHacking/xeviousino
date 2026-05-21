#ifndef BLE_GAMEPAD_H
#define BLE_GAMEPAD_H

#include <Arduino.h>

// Initialize BLE HID Host — scans and connects to gamepad automatically
void ble_gamepad_init(void);

// Get current NES-format button bitmask
// Returns bitmask where bit=1 means PRESSED:
//   bit 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT, 4=SELECT, 5=START, 6=A, 7=B, 8=X, 9=Y
uint32_t ble_gamepad_buttons(void);

// Check if a gamepad is connected
bool ble_gamepad_connected(void);

#endif // BLE_GAMEPAD_H
