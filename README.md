| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- |

# BLE Heart Rate & Temperature Measurement Example

This example creates a GATT server demonstrating standard Heart Rate measurement service and a custom Temperature service. It simulates Heart rate measurement and temperature, notifying clients when notifications are enabled.

It uses ESP32's Bluetooth controller and NimBLE stack based BLE host.

## Features

- **Heart Rate Service** (0x180D)
  - Heart Rate Measurement (0x2A37) - Notifications every 1 second
  - Body Sensor Location (0x2A38) - Read-only

- **Temperature Service** (0x1809)
  - Temperature Measurement (0x2A1C) - Read + Notifications every 200ms
  - Uses IEEE 11073-20601 format (Health Thermometer Service)

- **Device Information Service** (0x180A)
  - Manufacturer Name - Read-only
  - Model Number - Read-only

## How to Use Example

Before project configuration and build, be sure to set the correct chip target using:

```bash
idf.py set-target <chip_name>
```

### Hardware Required

* A development board with ESP32/ESP32-C3 SoC (e.g., ESP32-DevKitC, ESP-WROVER-KIT, etc.)
* A USB cable for Power supply and programming

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.

(To exit the serial monitor, type ``Ctrl-]``.)

## Example Output

This console output can be observed when blehr is connected to client and client enables notifications:

```
I (91) BTDM_INIT: BT controller compile version [fe7ced0]
I (91) system_api: Base MAC address is not set, read default base MAC address from BLK0 of EFUSE
I (421) NimBLE_BLE_HeartRate: BLE Host Task Started
GAP procedure initiated: stop advertising.
Device Address: xx:xx:xx:xx:xx:xx
GAP procedure initiated: advertise; disc_mode=2
connection established; status=0
subscribe event; cur_notify=1
  Heart Rate subscription; val_handle=16
I (21611) BLE_GAP_SUBSCRIBE_EVENT: conn_handle from subscribe=0
GATT procedure initiated: notify; att_handle=16
  Temperature subscription; val_handle=27
GATT procedure initiated: notify; att_handle=27
```

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub.
