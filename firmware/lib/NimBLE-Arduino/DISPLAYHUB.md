# NimBLE-Arduino 1.4.2 (vendorizzato per Display Hub)

Origine: https://github.com/h2zero/NimBLE-Arduino tag 1.4.2 (Apache 2.0), solo
src/, LICENSE, README. Sostituisce lo stack Bluedroid del core (libreria BLE
di arduino-esp32): meno RAM interna e archivio degli accoppiamenti sotto il
nostro controllo.

Modifiche (tutte in src/nimconfig.h, cercare "[Display Hub]"):
- CONFIG_BT_NIMBLE_NVS_PERSIST 1 -> 0: nessuna scrittura nella NVS. Gli
  accoppiamenti vivono nell'archivio in RAM di NimBLE e lib/dh_ble li copia
  nella configurazione ([bt] bond), da cui li ricarica all'avvio.
- CONFIG_BT_NIMBLE_MAX_BONDS 3 -> 8, CONFIG_BT_NIMBLE_MAX_CCCDS 8 -> 16.

Non usare insieme alla libreria BLE del core (#include <BLEDevice.h>): i due
stack non possono convivere.
