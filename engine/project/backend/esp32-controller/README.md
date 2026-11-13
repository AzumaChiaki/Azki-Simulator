# ESP32 single-USB controller

This firmware exposes the original ESP32 Bluetooth controller through UART0 and its existing USB-to-serial adapter. The emulator runs the Bluetooth host. Supported installation target: classic ESP32 with 4 MB flash; ESP32-D0WDQ6 has been exercised. ESP32-S2/S3/C3 and native USB HCI are not supported by these images.

Build with ESP-IDF v5.5.1, commit `fcae32885b0296b32044cb99ecbdc50d98dddb83`:

```sh
. "$IDF_PATH/export.sh"
cd firmware
idf.py set-target esp32
idf.py build
```

Included images use DIO, 40 MHz, 4 MB: bootloader at 0x1000, partition table at 0x8000, application at 0x10000. UART0 is 115200 baud, 8N1, without hardware flow control. DTR stays low; RTS pulses EN to start the bridge. Application logging is disabled on this UART.

The application uses controller-only VHCI, preserves HCI event and ACL bytes, and stops on framing, queue or UART errors. START enables the controller; STOP disables and deinitializes it before the ordered CLOSED response. Loss of host traffic for ten seconds stops the controller. No NVS writes or radio calibration persistence are enabled.

The native app uses the official esptool RAM loader at 115200 baud. It backs up the complete flash and checks its device-side MD5 before installation. Backups and their receipt remain private in the selected library. Restore requires the matching board identity and a complete verified backup. Failed writes are not retried or automatically restored.

ESP-IDF and its Bluetooth controller libraries are supplied by Espressif. Sources and license terms: https://github.com/espressif/esp-idf/tree/v5.5.1 and https://github.com/espressif/esp32-bt-lib .
