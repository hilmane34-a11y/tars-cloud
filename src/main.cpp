#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BluetoothA2DPSource.h"

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include "config.h"

Adafruit_SSD1306 oled(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

BluetoothA2DPSource a2dp;

void oledText(const char *text)
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(text);
    oled.display();
}

int32_t get_audio_data(uint8_t *data, int32_t len)
{
    memset(data, 0, len);
    return len;
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Wire.begin(OLED_SDA, OLED_SCL);

    if (!oled.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
    ))
    {
        while (true)
        {
            delay(1000);
        }
    }

    oledText("TARS BOOT");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    oledText("WIFI...");

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(300);
    }

    oledText("WIFI OK");

    delay(1000);

    oledText("BT START");

    a2dp.set_data_callback(get_audio_data);
    a2dp.start(BT_HEADSET_NAME);

    oledText("TARS READY");

    Serial.println();
    Serial.println("==============================");
    Serial.println("TARS ESP32");
    Serial.println("==============================");
    Serial.println("OLED     : OK");
    Serial.println("WIFI     : OK");
    Serial.println("A2DP     : START");
    Serial.print("HEADSET  : ");
    Serial.println(BT_HEADSET_NAME);
    Serial.println("==============================");
}

void loop()
{
    delay(20);
}
