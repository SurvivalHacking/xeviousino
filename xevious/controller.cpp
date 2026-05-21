#include <Arduino.h>

#include "hw_config.h"

/* controller is GPIO */
#if defined(HW_CONTROLLER_GPIO)

extern "C" void controller_init()
{
#if defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK)
  pinMode(HW_CONTROLLER_GPIO_UP_DOWN, INPUT);
  pinMode(HW_CONTROLLER_GPIO_LEFT_RIGHT, INPUT);
#else  /* !defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK) */
  pinMode(HW_CONTROLLER_GPIO_UP, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_DOWN, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_LEFT, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_RIGHT, INPUT_PULLUP);
#endif /* !defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK) */
  pinMode(HW_CONTROLLER_GPIO_SELECT, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_START, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_A, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_B, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_X, INPUT_PULLUP);
  pinMode(HW_CONTROLLER_GPIO_Y, INPUT_PULLUP);
}

extern "C" uint32_t controller_read_input()
{
  uint32_t u, d, l, r, s, t, a, b, x, y;

#if defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK)

  #if defined(HW_CONTROLLER_GPIO_REVERSE_UD)
    int joyY = 4095 - analogRead(HW_CONTROLLER_GPIO_UP_DOWN);
  #else /* !defined(HW_CONTROLLER_GPIO_REVERSE_UD) */
    int joyY = analogRead(HW_CONTROLLER_GPIO_UP_DOWN);
  #endif /* !defined(HW_CONTROLLER_GPIO_REVERSE_UD) */

  #if defined(HW_CONTROLLER_GPIO_REVERSE_LF)
    int joyX = 4095 - analogRead(HW_CONTROLLER_GPIO_LEFT_RIGHT);
  #else /* !defined(HW_CONTROLLER_GPIO_REVERSE_LF) */
    int joyX = analogRead(HW_CONTROLLER_GPIO_LEFT_RIGHT);
  #endif /* !defined(HW_CONTROLLER_GPIO_REVERSE_LF) */

  log_v("joyX: %d, joyY: %d\n", joyX, joyY);
  #if defined(ARDUINO_ODROID_ESP32)

    if (joyY > 2048 + 1024)
    {
      u = 0;
      d = 1;
    }
    else if (joyY > 1024)
    {
      u = 1;
      d = 0;
    }
    else
    {
      u = 1;
      d = 1;
    }
    if (joyX > 2048 + 1024)
    {
      l = 0;
      r = 1;
    }
    else if (joyX > 1024)
    {
      l = 1;
      r = 0;
    }
    else
    {
      l = 1;
      r = 1;
    }

  #else  /* !defined(ARDUINO_ODROID_ESP32) */

    if (joyY > 2048 + 1024)
    {
      u = 1;
      d = 0;
    }
    else if (joyY < 1024)
    {
      u = 0;
      d = 1;
    }
    else
    {
      u = 1;
      d = 1;
    }

    if (joyX > 2048 + 1024)
    {
      l = 1;
      r = 0;
    }
    else if (joyX < 1024)
    {
      l = 0;
      r = 1;
    }
    else
    {
      l = 1;
      r = 1;
    }

  #endif /* !defined(ARDUINO_ODROID_ESP32) */
  #else  /* !defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK) */
    u = digitalRead(HW_CONTROLLER_GPIO_UP);
    d = digitalRead(HW_CONTROLLER_GPIO_DOWN);
    l = digitalRead(HW_CONTROLLER_GPIO_LEFT);
    r = digitalRead(HW_CONTROLLER_GPIO_RIGHT);
  #endif /* !defined(HW_CONTROLLER_GPIO_ANALOG_JOYSTICK) */

    s = digitalRead(HW_CONTROLLER_GPIO_SELECT);
    t = digitalRead(HW_CONTROLLER_GPIO_START);
    a = digitalRead(HW_CONTROLLER_GPIO_A);
    b = digitalRead(HW_CONTROLLER_GPIO_B);
    x = digitalRead(HW_CONTROLLER_GPIO_X);
    y = digitalRead(HW_CONTROLLER_GPIO_Y);

    return 0xFFFFFFFF ^ ((!u << 0) | (!d << 1) | (!l << 2) | (!r << 3) | (!s << 4) | (!t << 5) | (!a << 6) | (!b << 7) | (!x << 8) | (!y << 9));
  }

/* controller is I2C M5Stack CardKB */
#elif defined(HW_CONTROLLER_I2C_M5CARDKB)

  #include <Wire.h>

  #define I2C_M5CARDKB_ADDR 0x5f
  #define READ_BIT I2C_MASTER_READ /*!< I2C master read */
  #define ACK_CHECK_EN 0x1         /*!< I2C master will check ack from slave */
  #define NACK_VAL 0x1             /*!< I2C nack value */

  extern "C" void controller_init()
  {
    Wire.begin();
  }

  extern "C" uint32_t controller_read_input()
  {
    uint32_t value = 0xFFFFFFFF;

    Wire.requestFrom(I2C_M5CARDKB_ADDR, 1);
    while (Wire.available())
    {
      char c = Wire.read(); // receive a byte as characterif
      if (c != 0)
      {
        switch (c)
        {
        case 181: // up
          value ^= (1 << 0);
          break;
        case 182: // down
          value ^= (1 << 1);
          break;
        case 180: // left
          value ^= (1 << 2);
          break;
        case 183: // right
          value ^= (1 << 3);
          break;
        case ' ': // select
          value ^= (1 << 4);
          break;
        case 13: // enter -> start
          value ^= (1 << 5);
          break;
        case 'k': // A
          value ^= (1 << 6);
          break;
        case 'l': // B
          value ^= (1 << 7);
          break;
        case 'o': // X
          value ^= (1 << 8);
          break;
        case 'p': // Y
          value ^= (1 << 9);
          break;
        }
      }
    }

    return value;
  }

/* controller is I2C BBQ10Keyboard */
#elif defined(HW_CONTROLLER_I2C_BBQ10KB)

  #include <Wire.h>
  #include <BBQ10Keyboard.h>
  BBQ10Keyboard keyboard;
  static uint32_t value = 0xFFFFFFFF;

  extern "C" void controller_init()
  {
    Wire.begin();
    keyboard.begin();
    keyboard.setBacklight(0.2f);
  }

  extern "C" uint32_t controller_read_input()
  {

    int keyCount = keyboard.keyCount();
    while (keyCount--)
    {
      const BBQ10Keyboard::KeyEvent key = keyboard.keyEvent();
      String state = "pressed";
      if (key.state == BBQ10Keyboard::StateLongPress)
        state = "held down";
      else if (key.state == BBQ10Keyboard::StateRelease)
        state = "released";

      // Serial.printf("key: '%c' (dec %d, hex %02x) %s\r\n", key.key, key.key, key.key, state.c_str());

      uint32_t bit = 0;
      if (key.key != 0)
      {
        switch (key.key)
        {
        case 'w': // up
          bit = (1 << 0);
          break;
        case 'z': // down
          bit = (1 << 1);
          break;
        case 'a': // left
          bit = (1 << 2);
          break;
        case 'd': // right
          bit = (1 << 3);
          break;
        case ' ': // select
          bit = (1 << 4);
          break;
        case 10: // enter -> start
          bit = (1 << 5);
          break;
        case 'k': // A
          bit = (1 << 6);
          break;
        case 'l': // B
          bit = (1 << 7);
          break;
        case 'o': // X
          bit = (1 << 8);
          break;
        case 'p': // Y
          bit = (1 << 9);
          break;
        }
        if (key.state == BBQ10Keyboard::StatePress)
        {
          value ^= bit;
        }
        else if (key.state == BBQ10Keyboard::StateRelease)
        {
          value |= bit;
        }
      }
    }

    return value;
  }

#elif defined(HW_CONTROLLER_I2C_PCF8574)

  #include <Wire.h>
  #include "PCF8574.h"
  #include <EEPROM.h>
  #include <esp_system.h>
  #include "ble_gamepad.h"

  #define EEPROM_SIZE 64  /* enough for volume (0-1) + BLE address (30-49) */
  #define EEPROM_VOL_ADDR 0
  #define EEPROM_VOL_MAGIC 0xA5  /* magic byte to validate stored volume */
  #define VOL_STEP 5
  #define VOL_REPEAT_MS 150  /* repeat rate when holding START+UP/DOWN */

  /* volume variable defined in sound.c */
  extern volatile int audio_volume;

  #ifdef HW_I2C_SDA
  PCF8574 pcf8574(HW_CONTROLLER_I2C_PCF8574_ADDR, HW_I2C_SDA, HW_I2C_SCL);
  #else
  PCF8574 pcf8574(HW_CONTROLLER_I2C_PCF8574_ADDR);
  #endif

  static unsigned long vol_last_change = 0;
  static bool vol_dirty = false;
  static unsigned long vol_dirty_time = 0;
  static unsigned long start_press_time = 0;
  static bool start_was_pressed = false;

  static void volume_load()
  {
    EEPROM.begin(EEPROM_SIZE);
    uint8_t magic = EEPROM.read(EEPROM_VOL_ADDR);
    uint8_t vol = EEPROM.read(EEPROM_VOL_ADDR + 1);
    if (magic == EEPROM_VOL_MAGIC && vol <= 100)
    {
      audio_volume = vol;
      log_i("Volume loaded from EEPROM: %d%%", vol);
    }
    else
    {
      audio_volume = 40;
      log_i("No saved volume, using default: 40%%");
    }
  }

  static void volume_save()
  {
    EEPROM.write(EEPROM_VOL_ADDR, EEPROM_VOL_MAGIC);
    EEPROM.write(EEPROM_VOL_ADDR + 1, (uint8_t)audio_volume);
    EEPROM.commit();
    log_i("Volume saved to EEPROM: %d%%", audio_volume);
  }

  extern "C" void controller_init()
  {
    pcf8574.pinMode(P0, INPUT_PULLUP);
    pcf8574.pinMode(P1, INPUT_PULLUP);
    pcf8574.pinMode(P2, INPUT_PULLUP);
    pcf8574.pinMode(P3, INPUT_PULLUP);

    pcf8574.pinMode(P4, INPUT_PULLUP);
    pcf8574.pinMode(P5, INPUT_PULLUP);
    pcf8574.pinMode(P6, INPUT_PULLUP);
    pcf8574.pinMode(P7, INPUT_PULLUP);

    log_i("Init pcf8574...");
    if (pcf8574.begin()){
      log_i("OK");
    } else {
      log_e("FAILED");
    }

    volume_load();

    // BLE gamepad already initialized in show_boot_menu()
  }

  uint32_t key_mask = 0xFFFFFFFF;

  extern "C" uint32_t controller_read_input()
  {
    /* Leggi PCF8574 (0 = premuto, 1 = rilasciato) */
    uint8_t u = pcf8574.digitalRead(PCF8574_MAP_UP);
    uint8_t d = pcf8574.digitalRead(PCF8574_MAP_DOWN);
    uint8_t l = pcf8574.digitalRead(PCF8574_MAP_LEFT);
    uint8_t r = pcf8574.digitalRead(PCF8574_MAP_RIGHT);

    uint8_t t = pcf8574.digitalRead(PCF8574_MAP_START);
    uint8_t s = pcf8574.digitalRead(PCF8574_MAP_SELECT);
    uint8_t b = pcf8574.digitalRead(PCF8574_MAP_B);
    uint8_t a = pcf8574.digitalRead(PCF8574_MAP_A);

    /* Merge BLE gamepad PRIMA di tutti i controlli speciali,
     * cosi' START long-press e volume funzionano anche da gamepad.
     * BLE: bit=1 = premuto. PCF8574: 0 = premuto.
     * Forziamo a 0 i bit PCF8574 se il corrispondente BLE e' premuto. */
    uint32_t ble = ble_gamepad_buttons();
    if (ble & (1 << 0)) u = 0;  // UP
    if (ble & (1 << 1)) d = 0;  // DOWN
    if (ble & (1 << 2)) l = 0;  // LEFT
    if (ble & (1 << 3)) r = 0;  // RIGHT
    if (ble & (1 << 4)) t = 0;  // BLE SELECT → START
    if (ble & (1 << 5)) s = 0;  // BLE START  → SELECT
    if (ble & (1 << 6)) a = 0;  // A
    if (ble & (1 << 7)) b = 0;  // B

    /* Long-press START (4 sec) → ritorna al menu di boot */
    {
      bool sp = !t;
      unsigned long now_ms = millis();
      if (sp)
      {
        if (!start_was_pressed)
        {
          start_press_time  = now_ms;
          start_was_pressed = true;
        }
        else if (now_ms - start_press_time >= 4000)
        {
          esp_restart();
        }
      }
      else
      {
        start_was_pressed = false;
      }
    }

    /* Volume control: SELECT held + UP or DOWN */
    bool select_pressed = !s;
    bool up_pressed = !u;
    bool down_pressed = !d;

    if (select_pressed && (up_pressed || down_pressed))
    {
      unsigned long now = millis();
      if (now - vol_last_change >= VOL_REPEAT_MS)
      {
        vol_last_change = now;
        if (up_pressed && audio_volume < 100)
        {
          audio_volume += VOL_STEP;
          if (audio_volume > 100) audio_volume = 100;
          log_i("Volume: %d%%", audio_volume);
          vol_dirty = true;
          vol_dirty_time = now;
        }
        else if (down_pressed && audio_volume > 0)
        {
          audio_volume -= VOL_STEP;
          if (audio_volume < 0) audio_volume = 0;
          log_i("Volume: %d%%", audio_volume);
          vol_dirty = true;
          vol_dirty_time = now;
        }
      }
      /* Mask SELECT, UP, DOWN so they don't reach the NES */
      key_mask = 0xFFFFFFFF ^ ((!l << 2) | (!r << 3) | (!t << 5) | (!a << 6) | (!b << 7));
      log_v("%#04x (vol mode)", key_mask);
    }
    else
    {
      key_mask = 0xFFFFFFFF ^ ((!u << 0) | (!d << 1) | (!l << 2) | (!r << 3) | (!s << 4) | (!t << 5) | (!a << 6) | (!b << 7));
      log_v("%#04x", key_mask);
    }

    /* Save to EEPROM 1 second after last volume change (avoid excessive writes) */
    if (vol_dirty && (millis() - vol_dirty_time >= 1000))
    {
      volume_save();
      vol_dirty = false;
    }

    return key_mask;
  }

#else /* no controller defined */

extern "C" void controller_init()
{
  log_w("GPIO controller disabled in menuconfig; no input enabled.\n");
}

extern "C" uint32_t controller_read_input()
{
  return 0xFFFFFFFF;
}

#endif /* no controller defined */
