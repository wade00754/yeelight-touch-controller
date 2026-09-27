/*
  ESP32-S3-Touch-LCD-1.28 Yeelight 區網燈控

  透過 Yeelight LAN Control 協定控制區網內的真實燈具。
  在整個螢幕短按可切換開關；整個螢幕向上／向下滑可調整亮度。
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "CST816S.h"
#include "secrets.h"

namespace {

constexpr uint16_t kScreenWidth = 240;
constexpr uint16_t kScreenHeight = 240;
constexpr uint32_t kLvglTickPeriodMs = 2;
constexpr uint8_t kTouchIrqPin = 5;
constexpr uint8_t kDisplaySleepInCommand = 0x10;
constexpr uint8_t kDisplaySleepOutCommand = 0x11;
constexpr uint8_t kDisplayOffCommand = 0x28;
constexpr uint8_t kDisplayOnCommand = 0x29;
constexpr uint32_t kWifiConnectTimeoutMs = 10000;
constexpr uint32_t kWifiReconnectIntervalMs = 5000;
constexpr uint32_t kLightReconnectIntervalMs = 1500;
constexpr int32_t kLightConnectTimeoutMs = 300;
constexpr uint32_t kBrightnessCommandIntervalMs = 100;

struct LightState {
  bool power;
  uint8_t brightness;
};

TFT_eSPI display(kScreenWidth, kScreenHeight);
CST816S touch(6, 7, 13, kTouchIrqPin);  // SDA, SCL, RST, IRQ

lv_disp_draw_buf_t drawBuffer;
lv_color_t pixelBuffer[kScreenWidth * kScreenHeight / 10];
esp_timer_handle_t lvglTickTimer = nullptr;

LightState lightState{false, 50};
bool screenAwake = true;
uint32_t lastInteractionMs = 0;
WiFiClient lightClient;
uint32_t nextCommandId = 1;
uint32_t lastWifiReconnectAttemptMs = 0;
uint32_t lastLightConnectAttemptMs = 0;
uint32_t lastBrightnessCommandMs = 0;
bool powerCommandPending = false;
bool brightnessCommandPending = false;

lv_obj_t *brightnessFill = nullptr;
lv_obj_t *brightnessLabel = nullptr;

struct TouchGestureState {
  bool active;
  bool verticalSwipe;
  int16_t startX;
  int16_t startY;
  uint8_t startBrightness;
};

TouchGestureState touchGesture{};

void setLightPower(bool power);
void setLightBrightness(uint8_t brightness);
void renderLightState();
void handleTouchEvent(int16_t x, int16_t y, uint8_t event);
void wakeScreen();

void displayFlush(lv_disp_drv_t *displayDriver, const lv_area_t *area, lv_color_t *colorPixels) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;

  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, width, height);
  display.pushColors(reinterpret_cast<uint16_t *>(&colorPixels->full), width * height, true);
  display.endWrite();

  lv_disp_flush_ready(displayDriver);
}

void touchRead(lv_indev_drv_t *inputDriver, lv_indev_data_t *data) {
  (void)inputDriver;

  if (!touch.available()) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  data->state = LV_INDEV_STATE_PR;
  data->point.x = constrain(touch.data.x, 0, static_cast<int>(kScreenWidth - 1));
  data->point.y = constrain(touch.data.y, 0, static_cast<int>(kScreenHeight - 1));

  const uint32_t now = millis();
  const bool wokeScreenThisEvent = !screenAwake;
  if (!screenAwake) {
    // ESP32 與觸控持續運作；第一次觸控會喚醒面板並繼續作為本次手勢。
    wakeScreen();
    touchGesture.active = false;
  }

  lastInteractionMs = now;

  // 若喚醒時收到的第一筆資料已是放開事件，就把它視為完整短按，
  // 避免快速點擊只喚醒畫面卻沒有切換燈光。
  if (wokeScreenThisEvent && touch.data.event == 1) {
    setLightPower(!lightState.power);
    renderLightState();
    touchGesture.active = false;
    data->state = LV_INDEV_STATE_REL;
    Serial.printf("[UI] Wake tap power: %s, brightness: %u%%\n",
                  lightState.power ? "ON" : "OFF",
                  lightState.brightness);
    return;
  }

  handleTouchEvent(data->point.x, data->point.y, touch.data.event);

  // CST816S: 0 = 按下、1 = 放開、2 = 持續接觸。
  if (touch.data.event == 1) {
    data->state = LV_INDEV_STATE_REL;
  }
}

void increaseLvglTick(void *argument) {
  (void)argument;
  lv_tick_inc(kLvglTickPeriodMs);
}

void setLightPower(bool power) {
  lightState.power = power;
  powerCommandPending = true;
}

void setLightBrightness(uint8_t brightness) {
  lightState.brightness = constrain(brightness, 1, 100);
  brightnessCommandPending = true;
}

bool ensureLightConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (lightClient.connected()) {
    return true;
  }

  const uint32_t now = millis();
  if (lastLightConnectAttemptMs != 0 &&
      static_cast<uint32_t>(now - lastLightConnectAttemptMs) < kLightReconnectIntervalMs) {
    return false;
  }

  lastLightConnectAttemptMs = now;
  lightClient.stop();
  lightClient.setTimeout(200);
  if (!lightClient.connect(kLightIp, kYeelightPort, kLightConnectTimeoutMs)) {
    Serial.printf("[LIGHT] Cannot connect to %s:%u\n", kLightIp.toString().c_str(), kYeelightPort);
    return false;
  }

  Serial.printf("[LIGHT] Connected to %s:%u\n", kLightIp.toString().c_str(), kYeelightPort);
  return true;
}

int32_t sendYeelightCommand(const char *method, const String &params) {
  if (!ensureLightConnection()) {
    return -1;
  }

  const uint32_t commandId = nextCommandId++;
  const String command = String("{\"id\":") + commandId +
                         ",\"method\":\"" + method +
                         "\",\"params\":" + params + "}\r\n";

  const size_t written = lightClient.print(command);
  if (written != command.length()) {
    Serial.printf("[LIGHT] Send failed: %s\n", method);
    lightClient.stop();
    return -1;
  }

  return static_cast<int32_t>(commandId);
}

void drainLightResponses() {
  while (lightClient.connected() && lightClient.available()) {
    const String response = lightClient.readStringUntil('\n');
    if (!response.isEmpty()) {
      Serial.printf("[LIGHT] %s\n", response.c_str());
    }
  }
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  lightClient.stop();
  const uint32_t now = millis();
  if (lastWifiReconnectAttemptMs != 0 &&
      static_cast<uint32_t>(now - lastWifiReconnectAttemptMs) < kWifiReconnectIntervalMs) {
    return;
  }

  lastWifiReconnectAttemptMs = now;
  Serial.println("[WIFI] Reconnecting...");
  WiFi.reconnect();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (kWifiSsid[0] == '\0') {
    Serial.println("[WIFI] Using saved credentials");
    WiFi.begin();
  } else {
    Serial.printf("[WIFI] Connecting to %s\n", kWifiSsid);
    WiFi.begin(kWifiSsid, kWifiPassword);
  }

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedAt) < kWifiConnectTimeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected, ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WIFI] Initial connection timed out; control will retry in the background");
  }
}

bool syncLightStateFromBulb() {
  const int32_t commandId = sendYeelightCommand("get_prop", "[\"power\",\"bright\"]");
  if (commandId < 0) {
    return false;
  }

  const String idMarker = String("\"id\":") + commandId;
  const uint32_t deadline = millis() + 600;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    if (!lightClient.available()) {
      delay(5);
      continue;
    }

    const String response = lightClient.readStringUntil('\n');
    if (response.indexOf(idMarker) < 0) {
      continue;
    }

    const int resultStart = response.indexOf("\"result\":[\"");
    if (resultStart < 0) {
      Serial.printf("[LIGHT] Unexpected state response: %s\n", response.c_str());
      return false;
    }

    const int powerStart = resultStart + 11;
    const int powerEnd = response.indexOf('"', powerStart);
    const int brightnessStart = powerEnd + 3;
    const int brightnessEnd = response.indexOf('"', brightnessStart);
    if (powerEnd < 0 || brightnessEnd < 0) {
      return false;
    }

    lightState.power = response.substring(powerStart, powerEnd) == "on";
    lightState.brightness = constrain(response.substring(brightnessStart, brightnessEnd).toInt(), 1, 100);
    Serial.printf("[LIGHT] Synced state: %s, %u%%\n",
                  lightState.power ? "ON" : "OFF", lightState.brightness);
    return true;
  }

  Serial.println("[LIGHT] State request timed out");
  lightClient.stop();
  return false;
}

void serviceLightControl() {
  serviceWiFi();
  drainLightResponses();

  if (powerCommandPending) {
    const String params = String("[\"") + (lightState.power ? "on" : "off") +
                          "\",\"smooth\",300]";
    if (sendYeelightCommand("set_power", params) >= 0) {
      powerCommandPending = false;
      if (!lightState.power) {
        brightnessCommandPending = false;
      }
      Serial.printf("[LIGHT] Power -> %s\n", lightState.power ? "ON" : "OFF");
    }
  }

  const uint32_t now = millis();
  if (brightnessCommandPending && lightState.power &&
      static_cast<uint32_t>(now - lastBrightnessCommandMs) >= kBrightnessCommandIntervalMs) {
    if (sendYeelightCommand("set_bright", String("[") + lightState.brightness + ",\"smooth\",100]") >= 0) {
      brightnessCommandPending = false;
      lastBrightnessCommandMs = now;
      Serial.printf("[LIGHT] Brightness -> %u%%\n", lightState.brightness);
    }
  }
}

void setScreenAwake(bool awake) {
  screenAwake = awake;
  digitalWrite(TFT_BL, awake ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);

  if (awake) {
    lastInteractionMs = millis();
  } else {
    touchGesture.active = false;
  }
}

void wakeScreen() {
  display.writecommand(kDisplaySleepOutCommand);
  delay(120);
  display.writecommand(kDisplayOnCommand);
  delay(20);
  setScreenAwake(true);
  lv_obj_invalidate(lv_scr_act());
  Serial.println("[POWER] Screen awake; Wi-Fi remained connected");
}

void turnOffScreen() {
  // IRQ 已為低表示正在觸控，不應在這個時刻關閉畫面。
  if (digitalRead(kTouchIrqPin) == LOW) {
    lastInteractionMs = millis();
    return;
  }

  setScreenAwake(false);
  display.writecommand(kDisplayOffCommand);
  display.writecommand(kDisplaySleepInCommand);
  delay(5);
  Serial.println("[POWER] Screen off; ESP32, Wi-Fi and Yeelight connection remain active");
}

void updateBrightnessFromSwipe(int16_t x, int16_t y) {
  const int16_t deltaX = x - touchGesture.startX;
  const int16_t deltaY = y - touchGesture.startY;
  const int16_t distanceY = abs(deltaY);

  if (!lightState.power || distanceY < kSwipeThresholdPixels || distanceY < abs(deltaX)) {
    return;
  }

  touchGesture.verticalSwipe = true;

  // 向上滑 (deltaY < 0) 變亮；向下滑 (deltaY > 0) 變暗。
  const int16_t brightnessDelta = -deltaY / kPixelsPerBrightnessPercent;
  const int16_t targetBrightness = touchGesture.startBrightness + brightnessDelta;
  const uint8_t previousBrightness = lightState.brightness;
  setLightBrightness(static_cast<uint8_t>(constrain(targetBrightness, 1, 100)));

  if (lightState.brightness != previousBrightness) {
    renderLightState();
  }
}

void handleTouchEvent(int16_t x, int16_t y, uint8_t event) {
  if (event == 0 || !touchGesture.active) {
    touchGesture.active = true;
    touchGesture.verticalSwipe = false;
    touchGesture.startX = x;
    touchGesture.startY = y;
    touchGesture.startBrightness = lightState.brightness;
    return;
  }

  if (event == 2 || event == 1) {
    updateBrightnessFromSwipe(x, y);
  }

  if (event != 1) {
    return;
  }

  const int16_t totalX = abs(x - touchGesture.startX);
  const int16_t totalY = abs(y - touchGesture.startY);

  if (touchGesture.verticalSwipe) {
    Serial.printf("[UI] Swipe brightness: %u%%\n", lightState.brightness);
  } else if (totalX < kSwipeThresholdPixels && totalY < kSwipeThresholdPixels) {
    // 任意位置的短按都切換電源，包含大型按鈕與亮度指示區域。
    setLightPower(!lightState.power);
    renderLightState();
    Serial.printf("[UI] Tap power: %s, brightness: %u%%\n",
                  lightState.power ? "ON" : "OFF",
                  lightState.brightness);
  }

  touchGesture.active = false;
}

void renderLightState() {
  char brightnessText[8];
  if (lightState.power) {
    snprintf(brightnessText, sizeof(brightnessText), "%u%%", lightState.brightness);
  } else {
    snprintf(brightnessText, sizeof(brightnessText), "OFF");
  }
  lv_label_set_text(brightnessLabel, brightnessText);

  // OFF 仍保留亮度高度，以深琥珀色表示目前燈泡已關閉。
  const int16_t fillHeight = map(lightState.brightness, 1, 100, 2, kScreenHeight);
  lv_obj_set_size(brightnessFill, kScreenWidth, fillHeight);
  lv_obj_align(brightnessFill, LV_ALIGN_BOTTOM_MID, 0, 0);

  if (lightState.power) {
    lv_obj_set_style_bg_color(brightnessFill, lv_color_hex(0xF5B800), LV_PART_MAIN);

    // 黃色越過螢幕中央時，中央數字改成深色以維持清楚對比。
    const uint32_t textColor = lightState.brightness >= 50 ? 0x17130A : 0xF2F4F7;
    lv_obj_set_style_text_color(brightnessLabel, lv_color_hex(textColor), LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(brightnessFill, lv_color_hex(0x4A3A12), LV_PART_MAIN);
    lv_obj_set_style_text_color(brightnessLabel, lv_color_hex(0xE5D9B6), LV_PART_MAIN);
  }

}

void createUserInterface() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x11151D), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  // 整個螢幕就是亮度條，黃色從底部依目前亮度向上填滿。
  brightnessFill = lv_obj_create(screen);
  lv_obj_remove_style_all(brightnessFill);
  lv_obj_set_style_bg_opa(brightnessFill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(brightnessFill, LV_OBJ_FLAG_CLICKABLE);

  brightnessLabel = lv_label_create(screen);
  // 使用字型原生尺寸，避免 transform_zoom 在部分刷新時把 Label 裁掉。
  lv_obj_set_style_text_font(brightnessLabel, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_opa(brightnessLabel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(brightnessLabel);

  renderLightState();
}

void initializeLvglDrivers() {
  lv_disp_draw_buf_init(&drawBuffer, pixelBuffer, nullptr, kScreenWidth * kScreenHeight / 10);

  static lv_disp_drv_t displayDriver;
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = kScreenWidth;
  displayDriver.ver_res = kScreenHeight;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);

  static lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = touchRead;
  lv_indev_drv_register(&inputDriver);
}

void startLvglTickTimer() {
  const esp_timer_create_args_t timerArguments = {
      .callback = &increaseLvglTick,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lvgl_tick",
      .skip_unhandled_events = false,
  };

  ESP_ERROR_CHECK(esp_timer_create(&timerArguments, &lvglTickTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvglTickTimer, kLvglTickPeriodMs * 1000));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("ESP32-S3 Yeelight LAN Controller");

  lv_init();

  display.begin();
  display.setRotation(0);
  pinMode(TFT_BL, OUTPUT);
  setScreenAwake(true);
  touch.begin();

  initializeLvglDrivers();
  createUserInterface();
  startLvglTickTimer();

  connectToWiFi();
  if (syncLightStateFromBulb()) {
    renderLightState();
  }

  Serial.printf("[LIGHT] Ready. Power: %s, brightness: %u%%\n",
                lightState.power ? "ON" : "OFF", lightState.brightness);
}

void loop() {
  lv_timer_handler();
  serviceLightControl();

  if (screenAwake && static_cast<uint32_t>(millis() - lastInteractionMs) >= kScreenTimeoutMs) {
    turnOffScreen();
  }

  delay(5);
}
