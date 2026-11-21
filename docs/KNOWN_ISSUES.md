# Reliability status and evidence boundary

## Fixed in the working implementation

- ESP32DMASPI0.8.0 completion ownership remains `trigger()` then completion/result consumption, never `trigger()` then `wait()`. READY remains driven by hardware post-setup/post-transaction callbacks. Two source guards protect this, not electrical timing.
- B now retains the original UART frame while waiting for MQTT receipt. A successful QoS0 publish is not considered receipt. B times out at 5 seconds and retries the real SPI→MQTT path twice, then clears pending state and reports `MQTT_TIMEOUT` over UART to A/host.
- LoRa exhaustion after two ACK retries now clears transaction state and reports `LORA_ACK_TIMEOUT`, instead of permanently holding pending. AUX and the 10-second minimum transmit gap remain intact; radio frequency/power defaults are unchanged.
- Wire v2 includes a CRC-protected nonzero session ID in every frame and ACK, including UART/LoRa binary ACK and TCP/host text ACK. A/B reject unrelated sessions. A fresh START replaces pending work even while B is busy; ABORT is accepted while busy and cancellation propagates over UART plus best-effort TCP. Recovery is a fresh whole-file transfer, not persistent resume.
- Receiver deduplicates the exact last frame within the active session (not sequence alone), retires replaced/aborted sessions, and only ACKs END/publishes the final image after size and whole-file CRC validation. `.part` survives failure for diagnosis.
- Managed local broker lifecycle has a real loopback CONNECT/SUBSCRIBE/PUBLISH and error-cleanup test. It never takes over existing brokers.

See [reproducible workflow and wire specification](LOCAL_WORKFLOW.md).

## Still unverified / remaining limits

1. **No USB boards detected, no physical proof.** ESP identities, wiring/power, SPI timing, E220 model/readback, regulatory/lab approval and end-to-end transfer remain unverified. Builds, READY and host tests do not prove the four real links.
2. **Local setup required.** Operator must privately supply credentials/current broker+receiver IPs, build and explicitly upload matching v2 A/B firmware, start receiver and confirm radio safety. Sanitized fallback config compiles but cannot establish the physical network. Launcher never auto-flashes/configures radio.
3. **Cancellation is best effort.** A severed USB/network link may prevent ABORT delivery. Next fresh-session START is authoritative. Resetting one MCU mid-transfer aborts progress; the host must stop/restart the whole transfer. Receiver process restart loses its in-memory retirement/deduplication set.
4. **Timing remains a lab starting point.** LoRa ACK wait15s/retries2 and conservative transmit gap10s may need longer waits for real airtime/network latency. Do not shorten pacing or change frequency/power without verified hardware and lab approval. Retries are bounded and may fail rather than conceal poor links.
5. **Trusted lab only.** MQTT/TCP are anonymous/unencrypted; sessions/CRC are reliability identifiers, not authentication. No concurrent senders, resumable transfer, persistent session history or rate-limited hostile-client protection. Receiver retired-session memory grows during a process lifetime; restart between extended lab runs when idle. One active TCP client/one image frame in flight.
6. **Host ACK transport has no automatic replay.** An ACK lost on host UART causes sender timeout and best-effort ABORT; user retries a fresh whole-file session. A legitimate completed receiver image may exist even if a final host ACK was lost. Check receiver output rather than assuming missing host ACK means no file.

No hardware was uploaded, no E220 parameters changed, no persistent LAN service started and no firewall/Wi-Fi configuration changed during software verification.
