/*
 * Display Hub — firmware "loader" (test WiFi via USB)
 *
 * Build minima, senza display/touch/LVGL: serve solo a scansionare le reti
 * WiFi visibili dal display e testare una connessione, pilotato dalla web UI
 * tramite Web Serial mentre il display resta collegato in USB.
 *
 * Se la connessione ha successo, le credenziali vengono salvate in NVS nello
 * stesso namespace/chiavi che il firmware definitivo (src/main.cpp) legge al
 * boot (namespace "dh_wifi", chiavi "ssid"/"password"): flashare poi il
 * firmware definitivo NON cancella questa memoria (vive in una partizione
 * flash separata da bootloader/app), quindi al primo avvio si connetterà
 * subito, senza passare dal captive portal.
 *
 * Protocollo seriale: JSON per riga, sia in ricezione che in invio (stesso
 * pattern usato altrove nel progetto). Vedi PROTOCOL.md per i dettagli.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

Preferences prefs;
String serial_buffer;

void send_json(JsonDocument& doc) {
    serializeJson(doc, Serial);
    Serial.println();
}

void send_hello() {
    JsonDocument doc;
    doc["type"] = "loader_hello";
    doc["fw"] = "wifi-setup-loader";
    send_json(doc);
}

void handle_scan() {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    doc["type"] = "scan_result";
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    send_json(doc);
    WiFi.scanDelete();
}

void handle_connect(const String& ssid, const String& password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    const unsigned long timeout_ms = 15000;
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
        delay(300);
    }

    JsonDocument doc;
    doc["type"] = "connect_result";

    if (WiFi.status() == WL_CONNECTED) {
        doc["success"] = true;
        doc["ip"] = WiFi.localIP().toString();

        // Salva le credenziali per il firmware definitivo, stesso namespace/chiavi
        // di load_wifi_credentials()/save_wifi_credentials() in src/main.cpp
        prefs.begin("dh_wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        prefs.end();
    } else {
        doc["success"] = false;
        doc["error"] = "Connessione fallita o timeout (rete non trovata, password errata?)";
    }

    send_json(doc);
    WiFi.disconnect(true); // libera la radio per un eventuale nuovo test
}

void process_line(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return; // riga non valida, ignorata

    String cmd = doc["cmd"] | "";
    if (cmd == "scan") {
        handle_scan();
    } else if (cmd == "connect") {
        String ssid = doc["ssid"] | "";
        String password = doc["password"] | "";
        if (ssid.length() > 0) handle_connect(ssid, password);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    send_hello();
}

void loop() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            process_line(serial_buffer);
            serial_buffer = "";
        } else {
            serial_buffer += c;
        }
    }
    delay(5);
}
