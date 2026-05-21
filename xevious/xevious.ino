//***************************************************************************************
// MICRO XEVIOUS Project by Marco Prunca and Davide Gatti www.survivalhacking.it        *
// Board = ESP32S3 DEV MODULE                                                           *
// PARTITION = Default 4mb with spiffs (1.2M APP/1.5M spiffs)                           *
// PSRAM = Qspi Psram (Psram obbligatoria)                                              *
// USB CDCD on boot = Enabled (Per far si che si possa riprogrammare senza premere boot)* 
// USB Firmware MSC on BOOT = enabled  (per attivare monitor seriale)                   *
// Installare libreria NimBLE-Arduino da h2zero 2.4.0                                   *
//                                                                                      *
// Installazione supporto LittleFS mediante plugin                                      *
// Istruzioni: https://github.com/earlephilhower/arduino-littlefs-upload/releases       *
// path per installare il plugin:                                                       *
// MACOS: ~/Documents/Arduino/tools oppure ~/.arduinoIDE/plugins/                       *
// WIN: C:\Users\<username>\.arduinoIDE\plugins\                                        *
// USO: F1 e poi scrivi   Upload LittleFS to pico/esp8266/ESP32                         *
//                                                                                      *
// Copiare le ROM nella cartella  data con i file descritti nel file ROM.TXT            *                      
//                                                                                      * 
// COMANDI:                                                                             *
// Tasto START tenuto premuto 5 secondi torna nel menu giochi                           *
// Tasto SELECT tenuto premuto insieme a joystick SU e GIU aumenta e diminuisce volume  * 
// SELECT premuto 10 secondi = attiva/disattiva Bluetooth (salvato in EEPROM)           *
// Su questo PCB i tasti SELECT e' a destra e lo START a sinistra                       *
//                                                                                      *
// Per la modalita con BATTERIE si consiglia audio in buzzer per aumentare autonomia    *
// Per passare da audio DAC a BUZZER commentare la riga 4 in HW_CONFIG.H                *
//***************************************************************************************


#include "arduino_config.h"

//*********LIBRERIE****************
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <FFat.h>
#include <LittleFS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
//*********************************

#include "ble_gamepad.h"
#include <EEPROM.h>
#include <esp_system.h>

#include "hw_config.h"
#include "logo_galaga.h"
#include "logo_gyruss.h"
#include "logo_xevious.h"
#include "logo_terracresta.h"
#include "logo_zanac.h"
#include "logo_lifeforce.h"
#include "logo_gradius2.h"
#include "logo_1942.h"
#include "logo_1943.h"
#include "logo_crisisforce.h"

extern "C"
{
#include "src/nofrendo/nofrendo.h"
}

int16_t bg_color;
extern Arduino_TFT *gfx;
extern void display_begin();

// Colori RGB565
#ifndef BLACK
#define BLACK  0x0000
#endif
#ifndef WHITE
#define WHITE  0xFFFF
#endif

// ── BLE enable/disable in EEPROM ──────────────────────────────────────────
#define EEPROM_BLE_EN_MAGIC_ADDR 5
#define EEPROM_BLE_EN_MAGIC_VAL  0xBB
#define EEPROM_BLE_EN_STATE_ADDR 6   // 1=enabled (default), 0=disabled
#define EEPROM_TOTAL_SIZE        64

static bool ble_enabled = true;  // default: attivo

static void ble_state_load()
{
    EEPROM.begin(EEPROM_TOTAL_SIZE);
    if (EEPROM.read(EEPROM_BLE_EN_MAGIC_ADDR) == EEPROM_BLE_EN_MAGIC_VAL)
        ble_enabled = (EEPROM.read(EEPROM_BLE_EN_STATE_ADDR) != 0);
    else
        ble_enabled = true;  // prima volta → attivo di default
    log_i("BLE state: %s", ble_enabled ? "ON" : "OFF");
}

static void ble_state_toggle()
{
    ble_enabled = !ble_enabled;
    EEPROM.begin(EEPROM_TOTAL_SIZE);
    EEPROM.write(EEPROM_BLE_EN_MAGIC_ADDR, EEPROM_BLE_EN_MAGIC_VAL);
    EEPROM.write(EEPROM_BLE_EN_STATE_ADDR, ble_enabled ? 1 : 0);
    EEPROM.commit();
    log_i("BLE toggled: %s", ble_enabled ? "ON" : "OFF");
}

// ── Boot Menu ──────────────────────────────────────────────────────────────

static const char *rom_paths[] = {
    FSROOT "/Galaga (U).nes",
    FSROOT "/Gyruss (USA).nes",
    FSROOT "/Xevious (E).nes",
    FSROOT "/Terra Cresta (Japan).nes",
    FSROOT "/Zanac (U).nes",
    FSROOT "/Lifeforce (U).nes",
    FSROOT "/Gradius 2 (J) [hM04][a1].nes",
    FSROOT "/1942 (JU) [p1].nes",
    FSROOT "/1943 - The Battle of Midway (USA).nes",
    FSROOT "/Crisis Force (J) [hM04].nes"
};

static const int NUM_GAMES = 10;

// Slot layout su display 240×240:
//  Slot 0: y=4   (68px: 2 bordo + 64 logo + 2 bordo)
//  Slot 1: y=76
//  Slot 2: y=148
//  Istruzioni: y=222

#define SLOT_X      9
#define SLOT_W      222
#define LOGO_X      10
#define LOGO_Y_OFF  2
#define SLOT_H      68

static const int slot_y[3] = { 4, 76, 148 };

// Disegna un logo: full-brightness o dimezzato (50% su ogni canale RGB)
static void drawLogo(int x, int y, const uint16_t *data, int w, int h, bool dimmed)
{
    if (!dimmed)
    {
        gfx->draw16bitRGBBitmap(x, y, (uint16_t *)data, w, h);
        return;
    }
    // Processa una riga alla volta con buffer in stack (max 220px × 2B = 440B)
    uint16_t row_buf[220];
    for (int row = 0; row < h; row++)
    {
        const uint16_t *src = data + row * w;
        for (int col = 0; col < w; col++)
        {
            uint16_t px = pgm_read_word(&src[col]);
            uint16_t r  = ((px >> 11) & 0x1F) >> 1;
            uint16_t g  = ((px >>  5) & 0x3F) >> 1;
            uint16_t b  = ( px        & 0x1F) >> 1;
            row_buf[col] = (r << 11) | (g << 5) | b;
        }
        gfx->draw16bitRGBBitmap(x, y + row, row_buf, w, 1);
    }
}

// Disegna il simbolo Bluetooth (runa) centrato su (cx, cy) con altezza 2*h
static void drawBtIcon(int cx, int cy, int h, uint16_t color)
{
    int w = h / 2;  // meta' larghezza
    // Linea verticale
    gfx->drawLine(cx, cy - h, cx, cy + h, color);
    // Freccia in alto a destra
    gfx->drawLine(cx, cy - h, cx + w, cy - h / 3, color);
    // Incrocio verso basso-sinistra
    gfx->drawLine(cx + w, cy - h / 3, cx - w, cy + h / 3, color);
    // Freccia in basso a destra
    gfx->drawLine(cx, cy + h, cx + w, cy + h / 3, color);
    // Incrocio verso alto-sinistra
    gfx->drawLine(cx + w, cy + h / 3, cx - w, cy - h / 3, color);
}

static void menu_draw(int selected, int view_offset)
{
    gfx->fillScreen(BLACK);

    static const uint16_t *logos[10]   = { logo_galaga_data,  logo_gyruss_data,  logo_xevious_data,  logo_terracresta_data,  logo_zanac_data,  logo_lifeforce_data,  logo_gradius2_data,  logo_1942_data,  logo_1943_data,  logo_crisisforce_data  };
    static const int        logo_w[10] = { GALAGA_LOGO_W,     GYRUSS_LOGO_W,     XEVIOUS_LOGO_W,     TERRACRESTA_LOGO_W,     ZANAC_LOGO_W,     LIFEFORCE_LOGO_W,     GRADIUS2_LOGO_W,     G1942_LOGO_W,    G1943_LOGO_W,    CRISISFORCE_LOGO_W     };

    for (int slot = 0; slot < 3; slot++)
    {
        int  game_idx = view_offset + slot;
        if (game_idx >= NUM_GAMES) break;

        int  sy  = slot_y[slot];
        bool sel = (game_idx == selected);

        if (sel)
            gfx->drawRect(SLOT_X, sy, SLOT_W, SLOT_H, WHITE);

        drawLogo(LOGO_X, sy + LOGO_Y_OFF, logos[game_idx], logo_w[game_idx], 64, !sel);
    }

    // frecce scroll
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    if (view_offset > 0)
    {
        gfx->setCursor(228, 4);
        gfx->print("^");
    }
    if (view_offset + 3 < NUM_GAMES)
    {
        gfx->setCursor(228, 212);
        gfx->print("v");
    }

    // Icona Bluetooth in basso a destra
    if (ble_enabled)
    {
        // BT attivo: simbolo blu
        drawBtIcon(228, 232, 6, 0x041F);  // blu RGB565
    }
    else
    {
        // BT disattivato: simbolo grigio + X rossa sopra
        drawBtIcon(228, 232, 6, 0x4208);  // grigio scuro
        gfx->drawLine(224, 226, 232, 238, 0xF800);  // X rossa
        gfx->drawLine(224, 238, 232, 226, 0xF800);
    }

    gfx->setTextColor(0x8410); // grigio
    gfx->setCursor(10, 228);
    gfx->print("UP/DOWN: naviga   A: avvia");
}

static const char *show_boot_menu()
{
    Wire.begin(HW_I2C_SDA, HW_I2C_SCL);

    // Leggi stato BLE da EEPROM
    ble_state_load();

    // Init BLE solo se abilitato
    if (ble_enabled)
        ble_gamepad_init();

    int selected    = 2; // default: Xevious
    int view_offset = 0;
    menu_draw(selected, view_offset);

    bool prev_up   = false;
    bool prev_down = false;
    bool prev_fire = false;

    // Long-press SELECT per toggle BLE
    unsigned long select_press_time = 0;
    bool select_was_pressed = false;

    while (true)
    {
        Wire.requestFrom((uint8_t)HW_CONTROLLER_I2C_PCF8574_ADDR, (uint8_t)1);
        uint8_t pcf = Wire.available() ? Wire.read() : 0xFF;

        // PCF8574: bit = 0 → tasto premuto, 1 → rilasciato
        bool up     = !(pcf & (1 << 2)); // P2 = UP
        bool down   = !(pcf & (1 << 3)); // P3 = DOWN
        bool fire   = !(pcf & (1 << 7)); // P7 = A
        bool select = !(pcf & (1 << 5)); // P5 = SELECT

        // Merge BLE gamepad input (solo se attivo)
        if (ble_enabled)
        {
            uint32_t ble = ble_gamepad_buttons();
            if (ble & (1 << 0)) up   = true;  // NES_UP
            if (ble & (1 << 1)) down = true;  // NES_DOWN
            if (ble & (1 << 6)) fire = true;  // NES_A
            if (ble & (1 << 5)) select = true; // BLE START → SELECT
        }

        // Long-press SELECT (10 sec) → toggle Bluetooth ON/OFF + reboot
        {
            unsigned long now_ms = millis();
            if (select && !up && !down && !fire)
            {
                if (!select_was_pressed)
                {
                    select_press_time  = now_ms;
                    select_was_pressed = true;
                }
                else if (now_ms - select_press_time >= 10000)
                {
                    // Toggle BLE
                    ble_state_toggle();

                    // Mostra messaggio
                    gfx->fillScreen(BLACK);
                    gfx->setTextColor(WHITE);
                    gfx->setTextSize(2);
                    gfx->setCursor(20, 100);
                    gfx->print("Bluetooth: ");
                    gfx->print(ble_enabled ? "ON" : "OFF");
                    gfx->setTextSize(1);
                    gfx->setCursor(40, 140);
                    gfx->print("Riavvio in corso...");
                    delay(2000);
                    esp_restart();
                }
            }
            else
            {
                select_was_pressed = false;
            }
        }

        if (up && !prev_up)
        {
            selected = (selected - 1 + NUM_GAMES) % NUM_GAMES;
            if (selected == NUM_GAMES - 1)  view_offset = NUM_GAMES - 3; // wrap → fine lista
            else if (selected < view_offset) view_offset = selected;
            menu_draw(selected, view_offset);
        }
        if (down && !prev_down)
        {
            selected = (selected + 1) % NUM_GAMES;
            if (selected == 0)               view_offset = 0;             // wrap → inizio lista
            else if (selected >= view_offset + 3) view_offset = selected - 2;
            menu_draw(selected, view_offset);
        }
        if (fire && !prev_fire)
        {
            log_i("Selected: %s", rom_paths[selected]);
            return rom_paths[selected];
        }

        prev_up   = up;
        prev_down = down;
        prev_fire = fire;

        delay(50);
    }
}



// ── Setup ──────────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(SERIAL_BAUD);
    Serial.setDebugOutput(true);

    #if defined(ARDUINO_USB_CDC_ON_BOOT) && (CORE_DEBUG_LEVEL >= ARDUHAL_LOG_LEVEL_DEBUG)
    delay(3000);
    #endif

    // turn off WiFi
    esp_wifi_stop();
    esp_wifi_deinit();

    // disable Core 0 WDT
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = false
    };
    esp_task_wdt_reconfigure(&wdt_config);

    // start display
    display_begin();

    // filesystem defined in hw_config.h
    FILESYSTEM_BEGIN

    if (!filesystem.exists("/"))
    {
        log_e("Filesystem mount failed! Please check hw_config.h settings.");
        gfx->println("Filesystem mount failed! Please check hw_config.h settings.");
        return;
    }

    // mostra menu e ottieni la rom scelta dall'utente
    const char *selected_rom = show_boot_menu();

    char argv_buf[256];
    strncpy(argv_buf, selected_rom, sizeof(argv_buf) - 1);
    argv_buf[sizeof(argv_buf) - 1] = '\0';
    char *argv[1] = { argv_buf };

    log_i("NoFrendo start: %s", argv_buf);
    nofrendo_main(1, argv);
    log_i("NoFrendo end!");
}

void loop()
{
}
