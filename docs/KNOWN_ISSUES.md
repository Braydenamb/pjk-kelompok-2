# Firmware review: known reliability limitations

## Fixed and checked locally

- ESP A previously called `trigger()` then `wait()` on ESP32DMASPI 0.8.0. `wait()` invokes `trigger()` internally and receives an empty queued list after the first dispatch. The task now consumes completion via `numTransactionsCompleted()`, `numBytesReceived()` and `error()` before reusing DMA buffers.
- READY is now set by the hardware post-setup callback, not immediately after asynchronous dispatch, and cleared by the completion callback.
- ESP A recompiles after these fixes. Two source-level regression guards check API usage and callback registration. These tests do not prove electrical timing or SPI delivery.
- Host receiver no longer ACKs a failed final whole-file integrity check and only exposes a final image filename after validation. Five real loopback TCP regression tests pass.

## Still unresolved: not demo-ready end-to-end

1. **Lost MQTT message can stall ESP B.** A QoS-0 publish success is not delivery confirmation. ESP B can remain `waitingMqtt` indefinitely after a lost MQTT packet or failed SPI CRC. Requires bounded idempotent retry, timeout and explicit upstream failure reporting.
2. **No session ID or explicit synchronized abort.** Sender restarts at sequence zero; ESP A deduplicates by sequence only. Aborting after START and restarting a new image can ACK old metadata without forwarding the new START. Resetting only one board can also leave its peer waiting on an old transfer. Proper fix is a transfer/session ID propagated through frames and ACKs, plus an abort/new-session handshake; resetting both boards is only a temporary manual workaround.
3. **LoRa retry exhaustion stalls ESP B.** After two retries, firmware prints an error but keeps the transaction pending indefinitely. Requires an explicit failed state and safe resynchronization/restart.
4. **No physical proof yet.** USB port mapping, wiring, radio readback, radio power/spectrum compliance, and UART/SPI/MQTT/LoRa end-to-end image delivery have not been verified.

Do not treat a successful build, READY heartbeat, host-only test, or local MQTT test as proof that all four protocol links work. Preserve radio pacing and do not flash or alter radio configuration until hardware/pin mapping and lab-approved settings are confirmed.
