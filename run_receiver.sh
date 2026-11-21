#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON="$ROOT/.venv/bin/python"
if [ ! -x "$PYTHON" ]; then
    printf '%s\n' 'Project .venv missing. Run python3 -m venv .venv first.' >&2
    exit 1
fi
exec "$PYTHON" "$ROOT/Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/Laptop_B/receive_image.py" "$@"
