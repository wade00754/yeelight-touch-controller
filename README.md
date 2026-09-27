# ESP32-S3 Yeelight Touch Controller

An Arduino project that controls a Yeelight on the local network using the circular touchscreen on the Waveshare **ESP32-S3-Touch-LCD-1.28**. The sketch retains the name `Mi_Light_UI_Simulator`, but the current implementation sends commands to a real light over TCP.

## Features and gestures

- Tap anywhere to toggle the light.
- Swipe up to increase brightness or down to decrease it, from 1% to 100%. A vertical displacement of at least 24 pixels, no smaller than the horizontal displacement, starts a brightness gesture. Brightness changes by 1% per 2 pixels relative to the gesture's starting position.
- A yellow fill and percentage indicate brightness. When off, the screen shows `OFF` and preserves the brightness level as a dark amber fill. Swiping while off does not adjust brightness.
- After 5 seconds without touch input, the backlight turns off and the LCD enters sleep. A wake-up tap also toggles the light.
- The ESP32, touch input, and Wi-Fi continue running. This is display sleep, not ESP32 Light Sleep or Deep Sleep. Turning off the display does not turn off the light.

## Hardware

The [Waveshare product documentation](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.28/) identifies an ESP32-S3 board with 16 MB Flash, 2 MB PSRAM, and a 1.28-inch, 240 × 240 circular LCD. It uses a GC9A01A display controller and CST816S capacitive touch controller. The onboard QMI8658 IMU and battery features are not used by this sketch. Connect the board to a computer with a USB Type-C data cable for development.

The following assignments come from this sketch and the locally installed Waveshare `Setup207_GC9A01.h`. These are onboard connections; no external display wiring is required.

| Signal | GPIO |
| --- | --- |
| LCD MOSI / SCLK | 11 / 10 |
| LCD CS / DC / RST | 9 / 8 / 14 |
| LCD backlight | 2, active high |
| MISO defined in the TFT setup | 12 |
| Touch SDA / SCL | 6 / 7 |
| Touch RST / IRQ | 13 / 5 |

The bundled touch driver uses I2C address `0x15` and is initialized as `CST816S touch(6, 7, 13, 5)`.

## Dependencies

The versions listed in the [Waveshare Arduino guide](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.28/Arduino/) match the locally installed dependencies:

| Dependency | Version or configuration |
| --- | --- |
| esp32 by Espressif Systems | 2.0.12 |
| LVGL | 8.3.10 |
| TFT_eSPI | 2.5.34 |
| TFT_eSPI_Setups | `Setup207_GC9A01.h` from the Waveshare example package |
| CST816S | Bundled `.h` and `.cpp` files |

Obtain the matching libraries from the example package linked in the official Arduino guide. The ESP32 core supplies `WiFi.h`, `esp_timer.h`, and `Wire.h`. This project uses LVGL 8 APIs; compatibility with LVGL 9 or ESP32 core 3.x has not been verified.

### TFT_eSPI configuration

Place `TFT_eSPI` and `TFT_eSPI_Setups` in your Arduino sketchbook's `libraries` directory. In `TFT_eSPI/User_Setup_Select.h`, enable the setup for this board:

```cpp
#include <../TFT_eSPI_Setups/Setup207_GC9A01.h>
```

Disable other setup includes, including the default `User_Setup.h`. Do not select the similarly numbered `Setup207_LilyGo_T_HMI.h`. The local Waveshare setup uses `GC9A01_DRIVER`, a 240 × 240 resolution, an 80 MHz SPI clock, and the GPIO assignments above. Recheck the selection after library updates.

### LVGL configuration

Ensure the active `lv_conf.h` contains these settings:

```cpp
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_TICK_CUSTOM 0
```

The current local installation stores this file at `libraries/lvgl/src/lv_conf.h`. For a fresh installation, follow the configuration layout of the supplied library package. The sketch uses `esp_timer` to call `lv_tick_inc()` every 2 ms and calls `lv_timer_handler()` in its main loop.

## Enable Yeelight LAN Control

For compatible bulbs whose Mi Home app does not expose LAN Control, use the following setup procedure. Support depends on the bulb model and firmware.

### Retrieve the bulb's IP and token

Pair the bulb in Mi Home first. Use [Xiaomi Cloud Tokens Extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor) to retrieve its device token. Find the bulb in the results and record its IP address and token.

### Enable LAN Control with python-miio

On a computer connected to the bulb's local network, install Python and [python-miio](https://github.com/rytilahti/python-miio), then run:

```sh
python -m pip install python-miio
miiocli yeelight --ip BULB_IP --token BULB_TOKEN set_developer_mode 1
```

Replace `BULB_IP` and `BULB_TOKEN` with the retrieved values. A successful response typically looks like:

```text
Setting developer mode to True
['ok']
```

Set `kLightIp` in `secrets.h` to that bulb's current LAN address and leave `kYeelightPort` at `55443`. The token is used for this setup command only; the ESP32 firmware uses Yeelight LAN Control directly and does not need the token or Xiaomi account credentials in `secrets.h`.

## Configure, build, and upload

Arduino IDE is optional. Use either the IDE workflow or the Arduino CLI workflow below; both require the same libraries and configuration described under [Dependencies](#dependencies).

### Arduino IDE

1. Keep the folder named `Mi_Light_UI_Simulator` and open `Mi_Light_UI_Simulator.ino` in Arduino IDE.
2. For a fresh checkout, copy `secrets.example.h` to `secrets.h` and set `kWifiSsid`, `kWifiPassword`, and `kLightIp`. The migrated local copy already retains its existing credentials. Git ignores `secrets.h`.
3. Leaving the SSID empty calls `WiFi.begin()` to try credentials already stored in ESP32 NVS. The sketch uses `WiFi.persistent(false)`, so do not assume credentials supplied by this sketch will be saved to NVS.
4. Set `kLightIp` in `secrets.h` to the light's address; the template uses a placeholder address. `kYeelightPort` defaults to `55443`. Display timeout (`kScreenTimeoutMs`) and swipe sensitivity (`kSwipeThresholdPixels`, `kPixelsPerBrightnessPercent`) are also configured in `secrets.h`; keep these values positive. The light must support and have Yeelight LAN Control enabled. Place it and the ESP32 on a mutually reachable local network; a DHCP reservation helps keep its address stable.
5. Select `ESP32S3 Dev Module` and the board's COM port. Compare board options with the settings image in the official Arduino guide. Use 16 MB Flash and QSPI PSRAM for this board. For serial logging through its onboard USB-to-UART bridge, set USB CDC On Boot to Disabled.
6. Select **Verify** to compile, then **Upload**. Open Serial Monitor at **115200 baud**.
7. Check for `[WIFI] Connected` and `[LIGHT]` messages. Test power toggling, brightness gestures, and waking the display after it becomes idle.

### Arduino CLI (without Arduino IDE)

The examples below use **Windows PowerShell**. Install the standalone executable following the [Arduino CLI installation guide](https://docs.arduino.cc/arduino-cli/installation/), add its directory to `PATH`, and open a new terminal. Arduino IDE is not required.

#### 1. Install the ESP32 core

Check the CLI and create its configuration file on a fresh installation. If a configuration file already exists, skip `config init` and keep your existing settings.

```powershell
arduino-cli version
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.12
arduino-cli core list
```

Skip `config add` if that URL is already configured. The package index is the [official Espressif stable index](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html). Keep the core at **2.0.12** for this project.

#### 2. Prepare the libraries and local configuration

Run `arduino-cli config dump` and check `directories.user`, the CLI's sketchbook directory. Place the Waveshare libraries from [Dependencies](#dependencies) in its `libraries` subdirectory: `lvgl`, `TFT_eSPI`, and `TFT_eSPI_Setups`. Apply the TFT_eSPI and LVGL settings above before compiling. If reusing an IDE installation, ensure the CLI points to the same sketchbook; for example, `arduino-cli config set directories.user "C:\Users\YOUR_USER_NAME\Documents\Arduino"` (replace the example path with your actual sketchbook).

The libraries are not supplied by installing the ESP32 core. The bundled `CST816S.h` and `CST816S.cpp` stay in the project folder and need no separate installation.

Change into your checkout, keeping its folder named `Mi_Light_UI_Simulator`. Replace the example path below. Create `secrets.h` only if it does not already exist:

```powershell
Set-Location "C:\path\to\Mi_Light_UI_Simulator"
if (-not (Test-Path .\secrets.h)) {
    Copy-Item .\secrets.example.h .\secrets.h
}
```

Edit `secrets.h` with your editor and set `kWifiSsid`, `kWifiPassword`, and `kLightIp` as described in the IDE workflow. Enable Yeelight LAN Control before testing the firmware.

#### 3. Select the board and compile

Connect the board with a USB data cable, then inspect available ports and board options:

```powershell
arduino-cli board list
arduino-cli board details --fqbn esp32:esp32:esp32s3
$fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled,CDCOnBoot=default"
arduino-cli compile --fqbn $fqbn --output-dir .\build .
```

This FQBN selects **ESP32S3 Dev Module**, **16 MB Flash**, **QSPI PSRAM**, and **USB CDC On Boot: Disabled** for logging through the onboard USB-to-UART bridge. Other board options retain the core's defaults; flash size does not automatically select a 16 MB partition layout. These option names match ESP32 core 2.0.12. The build files go into the Git-ignored `build` directory.

#### 4. Upload and read serial output

Continue in the same PowerShell session so `$fqbn` remains defined. Replace `COM3` with the board's port from `board list`; an `Unknown` board name is acceptable when the port is correct and the FQBN is supplied explicitly. Close any serial monitor using that port first.

Run upload only after compilation succeeds:

```powershell
$port = "COM3"
arduino-cli upload --port $port --fqbn $fqbn --input-dir .\build .
```

Upload does not compile the sketch. After changing source code, libraries, board options, or `secrets.h`, rerun the compile command before uploading. After a successful upload, open the serial monitor:

```powershell
arduino-cli monitor --port $port --config baudrate=115200
```

Press the board's RESET button if you need to see startup messages. Check for `[WIFI] Connected` and `[LIGHT]` messages, then test the gestures and display timeout. Press **Ctrl+C** to close the monitor before the next upload. For missing ports or upload errors, see [Troubleshooting](#troubleshooting).

See the [Arduino CLI getting started guide](https://docs.arduino.cc/arduino-cli/getting-started/) for general CLI usage.

## Connection behavior and limitations

At startup, the sketch makes one `get_prop` request for `power` and `bright`. If it fails, the local UI starts at off and 50% brightness; these fallback values are not automatically sent to the light. Touch input updates the local UI first, then queues a `set_power` or `set_bright` command. Brightness commands are spaced at least 100 ms apart.

Wi-Fi reconnects after a disconnect. A TCP connection is established when a command needs to be sent, with connection attempts spaced at least 1.5 seconds apart. While disconnected, the sketch retains the latest pending state rather than a history of every gesture. Sending an off command clears pending brightness changes. State is held in RAM; rebooting triggers a new startup query.

Normal command responses and light notifications are printed to Serial Monitor but are not continuously parsed to update the UI. Reconnecting does not automatically repeat `get_prop`. Changes made through another app may therefore leave the display out of sync, and a local UI update is not confirmation that the light executed a command. There is no automatic device discovery, Wi-Fi provisioning page, or cloud integration.

## Project files

| File | Purpose |
| --- | --- |
| `Mi_Light_UI_Simulator.ino` | LVGL UI, gestures, display sleep, Wi-Fi, and light control |
| `CST816S.h` / `CST816S.cpp` | Bundled touch driver |
| `secrets.example.h` | Configuration template with example IP and default preferences |
| `secrets.h` | Local Wi-Fi credentials, light address, and preferences; excluded from Git |
| `.gitignore` | Excludes credentials, build output, and a local utility |

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Missing `secrets.h` | Create the local configuration file from the template |
| Undefined `TFT_BL`, blank screen, or incorrect display output | Confirm TFT_eSPI selects only the correct Waveshare setup |
| Undefined `lv_font_montserrat_28` | Enable the font in the active `lv_conf.h` |
| LVGL type or API compilation errors | Check that LVGL 8.3.10 and matching libraries are selected |
| Upload failure or missing COM port | Check the data cable, CH343 USB-to-UART driver, and selected port; use the documented BOOT/RESET procedure if download mode is needed |
| Repeated `[WIFI]` reconnection messages | Check credentials, 2.4 GHz Wi-Fi availability, and signal strength |
| `[LIGHT] Cannot connect` | Check the light's IP, LAN Control, TCP port 55443, and router client isolation |
| Screen becomes black after 5 seconds | This is the default timeout; change `kScreenTimeoutMs` in `secrets.h` to adjust it |

## Licensing and references

`CST816S.h` and `CST816S.cpp` retain the MIT license notice from Felix Biego (2021). No separate license has been declared for the remaining project code. Third-party libraries retain their own licenses.

- [Waveshare ESP32-S3-Touch-LCD-1.28 product documentation](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.28/)
- [Waveshare Arduino setup and example downloads](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.28/Arduino/)

This README is based on the official documentation and the project source. Changes to hardware configuration or dependency versions should be verified on the target board.
