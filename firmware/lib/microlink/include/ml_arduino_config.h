/*
 * [Display Hub / Arduino port] Sostituto delle opzioni Kconfig di MicroLink.
 *
 * Nel progetto ESP-IDF originale queste macro arrivano da `idf.py menuconfig`
 * (sdkconfig.h). Con framework = arduino il sdkconfig è quello PRECOMPILATO del
 * core e non contiene nessuna CONFIG_ML_*: le definiamo qui, incluse in ogni
 * sorgente della libreria tramite `-include ml_arduino_config.h` (vedi
 * library.json). Ogni valore è sovrascrivibile da build_flags del progetto
 * (-DCONFIG_ML_MAX_PEERS=8 ecc.), grazie agli #ifndef.
 *
 * Moduli ESCLUSI dal port (vedi srcFilter in library.json): cellulare 4G
 * (ml_cellular, ml_at_socket, ml_net_switch) e server HTTP di configurazione
 * (ml_config_httpd): le loro macro abilitanti restano quindi NON definite.
 * La VPN usa la connessione WiFi già gestita dal firmware (WiFi.begin), che sia
 * un router o l'hotspot del cellulare: per MicroLink sono indistinguibili.
 */
#pragma once

#ifndef CONFIG_ML_MAX_PEERS
#define CONFIG_ML_MAX_PEERS 16
#endif

#ifndef CONFIG_ML_NVS_MAX_PEERS
#define CONFIG_ML_NVS_MAX_PEERS 32
#endif

/* Buffer della MapResponse: allocati in PSRAM (8 MB OPI sul modulo 4848S040)
 * solo durante il polling di coordinamento. 256 KB bastano ampiamente per una
 * tailnet personale (upstream: 512 KB, tarati su tailnet da 300+ nodi). */
#ifndef CONFIG_ML_H2_BUFFER_SIZE_KB
#define CONFIG_ML_H2_BUFFER_SIZE_KB 256
#endif
#ifndef CONFIG_ML_JSON_BUFFER_SIZE_KB
#define CONFIG_ML_JSON_BUFFER_SIZE_KB 256
#endif

#ifndef CONFIG_ML_PRIORITY_PEER_IP
#define CONFIG_ML_PRIORITY_PEER_IP ""
#endif

/* Prefisso hostname usato solo se device_name è vuoto: il nostro wrapper passa
 * sempre un nome esplicito, questo è solo il fallback. */
#ifndef CONFIG_ML_DEVICE_NAME
#define CONFIG_ML_DEVICE_NAME "displayhub"
#endif

/* NON definite di proposito (disattivano codice non portato / non necessario):
 *   CONFIG_ML_ENABLE_CELLULAR, CONFIG_ML_ENABLE_NET_SWITCH,
 *   CONFIG_ML_ENABLE_CONFIG_HTTPD, CONFIG_ML_ZERO_COPY_WG */
