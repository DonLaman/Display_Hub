#pragma once
// Valori scelti alla COMPILAZIONE (web UI > Firmware > "Nome del gateway"): il builder riscrive
// questo file prima di ogni build del gateway. Qui sotto i predefiniti.
//   GW_NAME      nome mostrato nelle Impostazioni del display e usato come nome Bluetooth/mDNS
//   GW_DEVICE_ID id con cui il server riconosce questo gateway (file data/BT_setting/<id>.ini)
// Se sul gateway sono stati impostati nome/id a runtime (comando gateway_config / server_config),
// quelli salvati hanno la precedenza su questi.
#define GW_NAME "DH-Gateway"
#define GW_DEVICE_ID "gateway-01"
