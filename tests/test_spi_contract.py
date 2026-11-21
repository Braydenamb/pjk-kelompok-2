"""Source-level regression guards for the ESP32DMASPI 0.8.0 API contract.
These are not hardware timing tests; actual SPI transfer still needs boards.
"""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).resolve().parents[1] /
          'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/ESP32_A/ESP32_A.ino')

class SpiContractTests(unittest.TestCase):
    def test_ready_is_driven_by_hardware_setup_callback(self):
        source = SOURCE.read_text()
        task = source.split('void spiSlaveTask(', 1)[1].split('void processHostSerial(', 1)[0]
        self.assertIn('spiSlave.setPostSetupCb(spiTransactionArmed)', source)
        self.assertIn('spiSlave.setPostTransCb(spiTransactionFinished)', source)
        self.assertNotIn('digitalWrite(PIN_SPI_READY, HIGH)', task)

    def test_async_trigger_is_not_followed_by_wait_which_triggers_again(self):
        source = SOURCE.read_text()
        task = source.split('void spiSlaveTask(', 1)[1].split('void processHostSerial(', 1)[0]
        self.assertNotIn('spiSlave.wait(', task,
                         'wait() calls trigger() internally; do not use after trigger()')
        self.assertIn('spiSlave.numTransactionsCompleted()', task)
        self.assertIn('spiSlave.numBytesReceived()', task)

if __name__ == '__main__':
    unittest.main()
