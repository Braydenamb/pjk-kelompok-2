# Local workflow — two ESP32, protocol v2

**Software workflow exercised locally; hardware end-to-end NOT verified.** This extends the team's reference in place. Git history retains the imported original. There is no automatic upload, radio configuration, firewall change, or Wi-Fi change.

## 1. Prepare laptops

From the repository root on Linux:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
./run_sender.sh --help
./run_sender.sh preflight
```

Install Mosquitto using your OS package manager if absent. For firmware builds install Arduino CLI, ESP32 core **3.3.12**, ESP32DMASPI **0.8.0**, PubSubClient **2.8**. These are the versions exercised here, not a claim about every core/library version. `g++` is required for native protocol/retry tests, not normal transfer. Receiver uses the standard library only; the receiver launcher still expects a project `.venv`.

Preflight shows current interface addresses, candidate serial ports and local configuration. It exits **1** when blocked, **0** when its host checks pass. Generic unbacked Linux `/dev/ttyS*` (`hwid=n/a`) entries are excluded from USB-board candidates. Even a single detected USB serial adapter is **not** identified as ESP A: explicitly select it after mapping the physical board. Preflight does not open serial, start a broker, upload, or transmit radio.

Optional `./run_sender.sh preflight --port /dev/ttyUSB0 --probe` performs an actual MQTT CONNECT/SUBSCRIBE/PUBLISH on a unique diagnostic topic and connects to the receiver TCP listener. Only use with the broker/receiver intentionally running. TCP reachability does not validate ESP firmware or prove the receiver is free of an existing connection.

## 2. Private local configuration (Laptop A)

```sh
./run_sender.sh configure
```

Enter SSID, hidden Wi-Fi password, the **current LAN IP of Laptop A** (broker), the **LAN IP of Laptop B** (receiver), MQTT port (1883), TCP port (5001). Choose the interface on the LAN reachable by both ESPs and the receiver; do not choose a Docker bridge, VPN or loopback for physical operation. IPs in previous logs are observations, not defaults.

This writes:

- `.../ESP32_A/local_config.h`
- `.../ESP32_B/local_config.h`
- `.../config.local.json` (endpoints only, **no password**)

All are ignored by git and written with mode `0600`. Header strings are escaped; there are no password command-line arguments or logs. Enter real credentials privately on your machine, never in chat. `config.example.h` is sanitized and compilable with blank credentials/zero IP; building without local headers is **compile-only**, not a runnable network configuration. Reconfigure and recompile/reupload both boards when credentials/IPs/ports change. Do not publish generated firmware binaries either: they contain Wi-Fi credentials.

## 3. Hardware gate, compile and manual upload

Confirm physical ESP32/module variants, common grounds, verified supplies, short wiring and safe antenna/power/radio configuration **with the lab first**. Disconnect power before wiring. Never feed 5 V into ESP32 GPIO/3V3; do not tie board VIN/5V rails without verified power-path schematics. Avoid driving an unpowered peer during sequential flashing; disconnect signals or use a verified simultaneous power arrangement.

Preserved pins on both ESPs:

| Link | Wiring / GPIO |
|---|---|
| UART A→B | A TX17 → B RX16 |
| UART ACK B→A | B TX17 → A RX16 |
| SPI B master / A slave | SCK18, MISO19, MOSI23, CS5, common ground |
| SPI READY | A GPIO4 → B GPIO4 |
| E220 on each node | module TXD → ESP RX26; module RXD ← ESP TX27; AUX34, M0=32, M1=33; module ground common with its ESP |

Numbers are GPIO numbers, not ordinal physical pin positions or invented board `Dxx` labels. USB to A carries host image data. USB/debug/power to B is not the final image path; Laptop B receives Wi-Fi TCP. No extra adapter or MCU is assumed. Both ESPs remain tethered by UART/SPI. Confirm the assistant approves the two-node alternating loop topology.

The main firmware expects E220 transparent mode / UART 115200 8N1 and the pair's already verified compatible radio settings. **Do not run/upload `E220_Config` as an automatic setup step.** It writes radio parameters; its existing frequency/power bytes were deliberately not changed. Exact model, legal channel, antenna gain, EIRP, supply/logic limits, air rate and duty-cycle still require module readback/manual/lab approval. The existing **10-second minimum radio transmit gap is preserved**, not certified regulatory compliance. Do not reduce it to make a demo faster.

Compile only:

```sh
./run_sender.sh build
arduino-cli board list
```

After physically mapping **each** board/port and checking safety, upload A and B manually using Arduino IDE or Arduino CLI. Example syntax (replace the port; do not execute blindly):

```sh
PROJECT=Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32
arduino-cli upload --fqbn esp32:esp32:esp32 --port YOUR_CONFIRMED_A_PORT "$PROJECT/ESP32_A"
arduino-cli upload --fqbn esp32:esp32:esp32 --port YOUR_CONFIRMED_B_PORT "$PROJECT/ESP32_B"
```

`esp32:esp32:esp32` is the compiled target, not automatic identification of your hardware. Close the serial monitor for A before sending. B debug logs use 115200; A host serial uses 460800.

## 4. Receive on Laptop B

From that laptop's repository root:

```sh
./run_receiver.sh --bind 0.0.0.0 --port 5001 --output ./received_images
```

Use `--bind LAPTOP_B_LAN_IP` to limit to one interface. `0.0.0.0` is a listen address, **not** the endpoint entered in configure. Default bind is all interfaces; only start on a trusted lab network. Run foreground, stop with Ctrl-C. Ensure the port matches firmware configuration. The launcher never changes firewall/router settings; arrange required LAN access through the lab administrator. AP/client isolation can block ESP traffic even on the same SSID. The receiver is single-connection and should have only ESP A as active sender.

Receiver validates frame CRC, session, sequence, final length and whole-file CRC. It writes `.part` during transfer and renames only after successful END. Incomplete old-session `.part` files are retained for diagnosis; they are not completed images. A new session closes the old file. Valid duplicate frames are ACKed without duplicate writes; a changed payload at the same sequence is not a duplicate. Do not run multiple receiver processes against the same output directory. Existing `received_<name>` is replaced by a successfully verified later transfer with the same filename.

## 5. Send on Laptop A

```sh
./run_sender.sh                         # guided port and image selection
# or explicit:
./run_sender.sh send --port /dev/ttyUSB0 --file ./small.png --lab-approved
```

`--lab-approved` records the operator's safety confirmation; it does not verify hardware. Without it, interactive sending asks for confirmation. Noninteractive sending without confirmation fails closed. A missing board/file/config fails **before** starting a broker or opening serial.

By default, `send` starts its **own foreground-lifetime Mosquitto child**, bound only to the configured broker IP/port, anonymous, no persistence. It verifies an MQTT round-trip and TCP listener reachability, then runs the serial sender. The child is stopped on normal finish, error or Ctrl-C. It never kills/reconfigures an existing system broker. An occupied port or nonlocal broker bind IP produces a diagnostic, not takeover. Anonymous MQTT is lab-only: never expose it to the internet or an untrusted LAN. This project does not currently implement broker authentication/TLS in firmware.

If you intentionally run an existing compatible lab broker:

```sh
./run_sender.sh send --broker external --port /dev/ttyUSB0 --file ./small.png --lab-approved
```

External brokers are probed, **not** stopped. Their listener/auth must match this lab firmware. Do not launch the old broad-listener `mosquitto.conf` by accident; the managed launcher generates its scoped ephemeral config instead.

The sender waits for READY (up to 180 seconds), then uses one stop-and-wait frame at a time (up to 90 seconds per end-to-end ACK). READY only means A has broker/B heartbeat/TCP prerequisites; it is not SPI/radio proof. On firmware stage error or timeout it stops and sends a best-effort ABORT. Ctrl-C during transfer also attempts ABORT. An ABORT cannot be guaranteed if USB/network is unplugged; the next START with a fresh random nonzero session authoritatively replaces pending work. Retry the **whole file** after diagnosis, not the next chunk. No automatic serial replay or radio reconfiguration is hidden in the launcher.

## Wire v2 and recovery

The topology remains **USB → A UART → B SPI → A MQTT via broker → B E220 → A TCP → receiver**, with reverse completion ACKs. Control ABORT/error messages are recovery, not a bypass of image links.

Fixed 200-byte frame; SPI transfer still 256 bytes with zero padding:

| Byte offset | Field |
|---|---|
| 0..3 | `K2I2` (breaks compatibility with old `K2IM`) |
| 4 | START1 / DATA2 / END3 / ACK4 / ABORT5 / ERROR6 |
| 5..8 | sequence, uint32 little-endian |
| 9..10 | payload length, uint16 little-endian |
| 11..14 | nonzero random session, uint32 little-endian |
| 15..18 | CRC32 of bytes 4..14 + unpadded payload |
| 19..199 | payload (maximum 181 bytes), then padding |

START sequence0 payload remains `<IBB` (file bytes, extension code, name length) + UTF-8 filename. DATA sequence1+ carries bytes. END carries `<I` whole-file CRC. TCP/host text ACKs are `ACK:<session>:<seq>`. Binary ACKs carry the session in the header. Firmware ERROR is sent B→A over UART and displayed as `ERR:<session>:<seq>:<stage>`; no fake success is emitted. **Update both boards and both Python scripts together; v1 and v2 cannot mix.** Session randomness prevents ordinary restart collisions but is not cryptographic authentication or a persistent resume identifier.

- B retains the UART frame until an identity-matching MQTT receipt. QoS0 publish alone is never completion. After 5 seconds without MQTT receipt B retries the real SPI→MQTT path, at most twice, then clears pending state and reports `MQTT_TIMEOUT` upstream.
- A publishes only the active session; B rejects unrelated MQTT sessions/sequences and ignores duplicate MQTT after receipt, so retries do not schedule extra radio transmissions.
- B radio ACK wait is 15 seconds with two retries, preserving the 10-second transmit pacing and AUX check. Retry exhaustion clears all transaction pending flags and reports `LORA_ACK_TIMEOUT` upstream. A missing AUX can also consume this bounded window. Slow/long-airtime hardware may require a lab-reviewed **longer timeout**, never silently faster pacing.
- New-session START/ABORT is accepted even while the previous B transfer is pending. A clears old SPI/MQTT/TCP/ACK work; session-tagged late data/ACKs cannot complete the new transfer. ABORT is local UART cancellation plus best-effort TCP receiver cancellation, not a LoRa image bypass. A closes stale TCP streams on reset. Receiver retires superseded/aborted sessions for its process lifetime.
- ESP32DMASPI asynchronous DMA ownership/completion and hardware post-setup/post-transaction READY callbacks are preserved.

## Diagnostics and evidence

| Symptom | Check |
|---|---|
| No candidate serial port | USB **data** cable, board powered, `arduino-cli board list`, OS enumeration; do not guess port |
| Serial permission denied/busy | OS serial permissions and close A monitor/other sender; no automatic sudo |
| MQTT bind error | Selected current local IP, occupied port/system broker; use `--broker external` only intentionally |
| MQTT CONNECT/probe failure | Broker listener, authentication mismatch, network reachability; firmware currently anonymous |
| TCP refused / READY timeout | Receiver bind/port/IP; same reachable LAN, isolation/firewall via lab admin; B heartbeat |
| `MQTT_TIMEOUT` | B UART receive, SPI CS/READY/mode/clock/common ground; A SPI CRC and broker subscription |
| `LORA_ACK_TIMEOUT` | E220 AUX/antenna/supply/compatible settings, A TCP receiver/integrity logs; inspect B serial logs |
| END rejected / sender stops | Whole-file size/CRC, stale firmware version or session; keep `.part`, repeat whole file |

Reproducible software verification:

```sh
.venv/bin/python -m unittest discover -s tests -v
./run_sender.sh build
./run_sender.sh preflight
```

Tests cover real loopback TCP PNG integrity/duplicate handling/failed validation/session restart/ABORT/stale-session rejection, native C++ retry and cross-language protocol CRC, SPI source guards, local config/port fixtures, and actual **loopback-only** Mosquitto round-trip with child cleanup on error. Port fixtures are fake **launcher unit tests only**. Native/source checks do not exercise MCU scheduling or electrical timing.

Still required: real board detection and identity, lab-approved wiring/power/radio readback, private credentials and endpoint configuration, explicit uploads, each real link, then a real small image byte-for-byte + visual check on Laptop B. No hardware transfer is claimed from successful builds or host tests.
