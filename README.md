# ESP32 SoftAP byte stream

ESP-IDF SoftAP with GNSS read and mock-input endpoints:

```text
GET  http://192.168.1.1/stream
POST http://192.168.1.1/write
POST http://192.168.1.1/flash
GET  http://192.168.1.1/flash/status
POST http://192.168.1.1/flash/confirm
POST http://192.168.1.1/flash/rollback
```

The implementation is reduced from the ESP-IDF `wifi/getting_started/softAP`
and `protocols/http_server/async_handlers` examples, the FreeRTOS
`basic_freertos_smp_usage` queue example, the FreeRTOS software timer API, and
the GPIO blink example. The OTA path is reduced from ESP-IDF's
`system/ota/native_ota_example`. GNSS parsing and UART2 baud scanning are
adapted from `/home/allan/git/personal/usb-serial-mocker/main/main.c`.

The `/stream` endpoint sends selected original NMEA bytes directly as HTTP chunks
with content type `application/octet-stream`. The `/write` endpoint temporarily
replaces UART2 parser input with an uploaded GNSS mock stream.

## Task layout

ESP-IDF already runs on FreeRTOS and creates internal Wi-Fi, HTTP daemon, and
timer-service tasks. This application adds these unpinned tasks:

| Task | Initialization function | Responsibility |
| --- | --- | --- |
| `wifi_stack` | `wifi_ap_stack_init()` | Processes SoftAP station join and leave events received through a FreeRTOS queue. |
| `gnss_init` | `gnss_stack_init()` | Initializes UART2 and scans baud rates until a checksum-valid NMEA sentence is found. It then launches `gnss_uart` and deletes itself. |
| `gnss_uart` | `gnss_init_task()` | Drains locked UART2 input. Its bytes are ignored while an HTTP GNSS override is active. |
| `gnss_filter` | `gnss_stack_init()` | Selects the lowest-HDOP candidate in each timer window and overwrites a one-item publication queue. |
| `http_stack` | `http_stream_stack_init()` | Drains the latest publication queue, updates a shared snapshot, and wakes stream workers. |
| `http_worker` | `http_stream_stack_init()` | Owns one asynchronous `/stream` response. Two instances run by default. |
| `http_write` | `http_stream_stack_init()` | Owns the single asynchronous `/write` upload and forwards its bytes into the GNSS parser. |
| `ota_worker` | `ota_update_stack_init()` | Receives one asynchronous `/flash` upload, verifies it, writes the inactive OTA slot, and selects it for the next boot. |
| `generic` | `generic_stack_init()` | Event-driven LED template task. GPIO2 blinks five times at startup, remains off while idle, and toggles after each packet successfully sent to an HTTP client. |

The HTTP daemon stays available while endless responses run in worker tasks.
Two `/stream` clients are accepted by default; an additional request receives
`503 Service Unavailable`. Each worker tracks its last-sent sequence number.
A slow client skips obsolete points and receives the newest available point
instead of accumulating a backlog.

## GNSS input

UART2 reads the external GNSS antenna on the same pins as the reference bridge:

```text
UART2 TX: GPIO17
UART2 RX: GPIO16
```

The scanner probes `4800`, `9600`, `19200`, `38400`, `57600`, and `115200`
baud until the `gnss_init` task sees a checksum-valid NMEA sentence. It then
launches the steady-state UART reader and starts the filter timer. The filter
pairs each valid GGA sentence with the following valid RMC sentence and ranks
pairs by the GGA HDOP value. Every `500 ms` by default, it publishes the best
new pair seen in that window. A window without a new valid pair publishes
nothing, so a `1 Hz` antenna still produces at most `1 Hz` output.

The publication queue has length one and intentionally overwrites stale data.
The HTTP dispatcher fans that newest value out to both stream workers, so one
client cannot consume data that another client still needs.

### GNSS mock input

`POST /write` accepts one GNSS mock upload at a time. While a writer is active,
UART2 continues to be drained to avoid a hardware-buffer overflow, but its
bytes are ignored. Uploaded bytes pass through the same checksum validation,
GGA/RMC pairing, timer-based HDOP filter, and latest-value publication queue as
antenna bytes. When the upload ends, the best remaining uploaded candidate is
published before UART2 parser ownership resumes.

ESP-IDF's HTTP server does not support chunked request bodies. Upload a finite
mock file so curl sends a `Content-Length` header:

```sh
curl --data-binary @mock.nmea http://192.168.1.1/write
```

For a replay closer to a `4800`-baud antenna, limit the upload to roughly
`480` bytes per second:

```sh
curl --limit-rate 480 --data-binary @mock.nmea http://192.168.1.1/write
```

Keep one `/stream` request open in another terminal to observe the selected
output. A second simultaneous `/write` request receives `409 Conflict`.

## LED indication

GPIO2 blinks five times during startup and then remains off until an HTTP
stream client connects. Every packet successfully sent to a connected client
toggles the LED state. With two connected clients, a GNSS publication can cause
two quick toggles because each client receives its own copy.

## OTA firmware update

This board was detected as an `ESP32-D0WD-V3` with `4 MB` of flash. The project
uses ESP-IDF's standard two-slot OTA partition table: a factory application,
`ota_0`, and `ota_1`. Each application slot is `1048576` bytes. The current
build occupies `857856` bytes and leaves `190720` bytes of slot headroom.

Flash this layout over USB once before trying remote updates:

```sh
make flash-monitor
```

That command writes the bootloader, partition table, empty OTA selection data,
and factory application. Afterward, build and upload the complete application
image:

```sh
make build
sha256=$(sha256sum build/esp32_wifi_stream.bin | awk '{print $1}')
curl -H "X-Firmware-SHA256: $sha256" \
     --data-binary @build/esp32_wifi_stream.bin \
     http://192.168.1.1/flash
```

Upload only `build/esp32_wifi_stream.bin` to `/flash`. It is the bootable
application image for one OTA slot. Do not upload the `.elf`, bootloader,
partition table, `ota_data_initial.bin`, or a merged serial-flash image.

The ESP32 writes the inactive OTA slot and reboots into it. Reconnect to its
Wi-Fi network and confirm the new image within `60` seconds:

```sh
curl http://192.168.1.1/flash/status
curl -X POST http://192.168.1.1/flash/confirm
```

If confirmation does not arrive, the ESP32 reboots and ESP-IDF's bootloader
rolls back to the previous working image.

### Slot sequence and automatic rollback

The device always keeps the previously confirmed image available while testing
the new image. It does not always roll back specifically to `ota_0`:

| Situation | New image boots from | If not confirmed within 60 seconds |
| --- | --- | --- |
| First OTA update after the USB factory flash | `ota_0` | Reboot and return to `factory` |
| Next update after confirming `ota_0` | `ota_1` | Reboot and return to `ota_0` |
| Next update after confirming `ota_1` | `ota_0` | Reboot and return to `ota_1` |

The OTA slots continue alternating. A manual reset or power loss before
`POST /flash/confirm` has the same rollback effect: on the next boot, the
bootloader rejects the unconfirmed image and starts the previous confirmed
firmware.

### Manual rollback

A working OTA image can also request an immediate rollback:

```sh
curl -X POST http://192.168.1.1/flash/rollback
```

If remote recovery is unavailable, connect USB and erase only the OTA
selection state. The factory application remains intact:

```sh
make factory-reset
```

`POST /flash` requires a fixed `Content-Length` and an
`X-Firmware-SHA256` header. It rejects oversized bodies, concurrent uploads,
checksum mismatches, and malformed ESP-IDF images without changing the next
boot slot. Flash writes can briefly delay `/stream` delivery.

The SHA-256 header detects corrupted uploads but does not authenticate who
built the firmware. This is suitable for development on the controlled SoftAP.
A production device should enable ESP-IDF signed-app verification and protect
the signing key before exposing remote updates more broadly.

## Build and flash

The top-level Makefile activates this machine's ESP-IDF installation inside
each command. From the project root, use:

```sh
make build
make flash
make monitor
make factory-reset
```

`make monitor` opens the interactive console and exits with `Ctrl+]`.
`make flash-monitor` flashes and then opens the console. `/dev/ttyUSB0` is the
default serial device. `make factory-reset` clears OTA boot selection over USB
and returns to the factory image. Override the port when necessary:

```sh
make flash PORT=/dev/ttyUSB1
make monitor PORT=/dev/ttyUSB1 BAUD=115200
```

Run `make help` to list wrapper targets. The project currently builds for the
classic `esp32` target.

### ESP-IDF commands

The equivalent explicit ESP-IDF workflow is:

```sh
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Native Ninja commands

ESP-IDF uses CMake to generate Ninja targets. For direct Ninja usage, configure
the `build` directory once from an activated terminal:

```sh
source ~/.espressif/tools/activate_idf_v6.0.1.sh
cmake -S . -B build -G Ninja
```

Then use Ninja directly from the project root:

```sh
ninja -C build
ESPPORT=/dev/ttyUSB0 ninja -C build flash
ESPPORT=/dev/ttyUSB0 ninja -C build serial-monitor
```

Alternatively, enter the generated directory first:

```sh
cd build
ninja
ESPPORT=/dev/ttyUSB0 ninja flash
ESPPORT=/dev/ttyUSB0 ninja serial-monitor
```

The normal Ninja build command is just `ninja`; there is no separate
`ninja build` action. The generated `flash` target rebuilds changed sources
before programming the ESP32. `ESPPORT` can be omitted when automatic serial
port detection is sufficient.

Use the project-local `serial-monitor` target for the interactive console.
ESP-IDF v6.0.1 also generates a target named `monitor`, but its CMake wrapper
returns immediately in this local setup instead of keeping the terminal
attached. Exit `serial-monitor` with `Ctrl+]`.

The default Wi-Fi credentials are:

```text
SSID:     esp32-stream
Password: esp32stream
```

The credentials, station count, HTTP client count, and GNSS timer window can be
changed under `ESP32 stream configuration` in `menuconfig`.

After connecting to the AP:

```sh
curl --no-buffer http://192.168.1.1/stream
```

Open two terminals and run the `curl` command in both to test fan-out. Both
clients should receive the same selected NMEA pairs as they become available.
