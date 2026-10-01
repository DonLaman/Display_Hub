# BSP del pannello

Per il modulo **ESP32-4848S040** (quello comprato per questo progetto) la BSP
è già qui, in `esp32_4848s040_bsp/`: pin, sequenza di init del pannello RGB
ST7701 e del touch GT911 sono presi 1:1 dal progetto open source
[OraQuadraNano](https://github.com/SurvivalHacking) di SurvivalHacking/Davide
Gatti, che gira esattamente su questo hardware — non serve aggiungere altro
per compilare `display_init()`/`touch_init()`.

Include anche, come funzionalità **opzionali** (non richieste da
`src/main.cpp`, chiamale solo se ti servono):
- `sd_init()` — inizializza la microSD, che condivide il bus SPI col display
- `audio_init()` / `audio_beep()` — audio via I2S. **Richiede una modifica
  hardware**: il modulo di serie non ha un amplificatore I2S collegato, va
  cablato a mano un amplificatore esterno (es. MAX98357A) sui pin definiti in
  `esp32_4848s040_bsp.h`. Senza quella modifica, non chiamare queste funzioni.

**Prima della prima build**, leggi le note in cima a `firmware/platformio.ini`
sulla versione del core ESP32 richiesta — non verificata con una build reale
in questo ambiente. Le dipendenze PlatformIO (Arduino_GFX, TAMC_GT911, lvgl)
sono invece già confermate corrette da una build riuscita.

## Se il tuo modulo è diverso

Se in futuro usi un pannello/touch diverso, sostituisci il contenuto di
`esp32_4848s040_bsp/` con la BSP del tuo venditore, mantenendo gli stessi nomi
di funzione (`display_init()`, `touch_init()`) e la stessa integrazione con
LVGL (registrazione di un `lv_disp_drv_t` e di un `lv_indev_drv_t`) — è quel
ponte verso LVGL, non i dettagli del pannello, che `src/main.cpp` si aspetta.
