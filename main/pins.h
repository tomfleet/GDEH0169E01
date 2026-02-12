/*
Adapter board Pinout

    1. +3v3
    2. GND
    3. SDA
    4. SCL
    5. CS
    6. DC
    7. RES#
    8. BUSY
    9. MS
    10. CSB2
*/

#ifndef PINS_H

#define PINS_H  
// Pin definitions
#define PIN_SDA 13
#define PIN_SCL 12
#define PIN_CS 2
#define PIN_DC 3
#define PIN_RES 4
#define PIN_BUSY 5
#define PIN_MS 42
#define PIN_CSB2 1

#define NEOPIXEL_PWR_PIN 17
#define NEOPIXEL_DATA_PIN 18
#define NEOPIXEL_NUM 1  

#define SCD30_SDA_PIN 8
#define SCD30_SCL_PIN 9

#endif // PINS_H