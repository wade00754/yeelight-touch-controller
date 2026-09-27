#pragma once

#include <IPAddress.h>
#include <stdint.h>

// Copy to secrets.h and enter your local settings. Keep secrets.h out of Git.
// Empty SSID uses credentials already stored in ESP32 NVS, if available.
constexpr char kWifiSsid[] = "";
constexpr char kWifiPassword[] = "";

// Replace this example address with your light's LAN IP.
const IPAddress kLightIp(192, 168, 1, 100);
constexpr uint16_t kYeelightPort = 55443;

// Personal preferences: display idle timeout and swipe sensitivity.
constexpr uint32_t kScreenTimeoutMs = 5UL * 1000UL;
constexpr int16_t kSwipeThresholdPixels = 24;
constexpr int16_t kPixelsPerBrightnessPercent = 2;
