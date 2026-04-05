#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "secrets.h"
#include "esp_sleep.h"

#define LED_PIN 1
#define NUM_LEDS 8
#define LED_TYPE WS2811
#define COLOR_ORDER GRB
#define VIBRATION_PIN 5
#define SLEEP_AFTER_MS (4UL * 60 * 60 * 1000)

const char *ssid     = WIFI_SSID_1;
const char *password = WIFI_PASSWORD_1;

const char *ssidKos     = WIFI_SSID_2;
const char *passwordKos = WIFI_PASSWORD_2;

const char *ssidOwn    = WIFI_SSID_3;
const char *passwordOwn = WIFI_PASSWORD_3;

const char* mdnsName = MDNS_NAME;

AsyncWebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>Lampu Hu Tao</title>
</head>
<body>
<label for="color">Pilih</label>
<input type="color" name="color" id="color">
<br>
<label for="password">Password:</label>
<input type="text" name="password" id="password">
<br>
<label for="brightness">Brightness</label>
<input type="range" id="brightness" name="brightness" min="0" max="240" />
<br>
<button type="button">Change</button>
<script>
let brightness = 0
document.addEventListener('DOMContentLoaded', async () => {
const res = await fetch("/brightness");
const data = await res.json();
brightness = data.brightness;
document.querySelector('#brightness').value = brightness;
});

document.querySelector('button').addEventListener('click', () => {
const color = document.querySelector('#color').value;
const newBrightness = document.querySelector('#brightness').value;
const password = document.querySelector('#password').value;
let url = "/update?color=" + encodeURIComponent(color) + "&password=" + password;
if (Number(newBrightness) !== brightness) {
url += "&brightness=" + newBrightness;
brightness = Number(newBrightness);
}
fetch(url);
});
</script>
</body>
</html>
)rawliteral";

Preferences preferences;

CRGB leds[NUM_LEDS];
volatile int brightness;
portMUX_TYPE brightnessMux = portMUX_INITIALIZER_UNLOCKED;

RTC_DATA_ATTR int savedBrightness = 48;
RTC_DATA_ATTR uint32_t savedColor = 0xFFFFFF;

int getBrightness() {
    int val;
    portENTER_CRITICAL(&brightnessMux);
    val = brightness;
    portEXIT_CRITICAL(&brightnessMux);
    return val;
}

void setBrightness(int val) {
    portENTER_CRITICAL(&brightnessMux);
    brightness = val;
    portEXIT_CRITICAL(&brightnessMux);
}

int snapBrightness(int value) {
    int step = 48;
    return (value / step) * step;
}

void readVibrationSensor()
{
    static bool lastState = HIGH;
    static unsigned long lastTriggerMs = 0;

    bool currentState = digitalRead(VIBRATION_PIN);
    if (currentState == LOW && lastState == HIGH) {
        if (millis() - lastTriggerMs > 300) {
            lastTriggerMs = millis();
            int snapped = snapBrightness(getBrightness());

            snapped += 48;
            if (snapped > 240) snapped = 0;
            setBrightness(snapped);
            preferences.putInt("brightness", snapped);
            FastLED.setBrightness(snapped);
            FastLED.show();
        }
    }
    lastState = currentState;
}

void goToSleep() {
    FastLED.setBrightness(0);
    FastLED.show();
    esp_deep_sleep_enable_gpio_wakeup(1ULL << VIBRATION_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

void setup()
{
    Serial.begin(115200);
    preferences.begin("led", false);
    pinMode(VIBRATION_PIN, INPUT_PULLUP);

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);

    esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();

    if (reason == ESP_SLEEP_WAKEUP_GPIO) {
        setBrightness(savedBrightness);
        FastLED.setBrightness(savedBrightness);
        fill_solid(leds, NUM_LEDS, CRGB(savedColor));
        FastLED.show();
    }

    WiFi.begin(ssid, password);
    if (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
        WiFi.begin(ssidKos, passwordKos);
        if (WiFi.waitForConnectResult() != WL_CONNECTED)
        {
            WiFi.begin(ssidOwn, passwordOwn);
            if (WiFi.waitForConnectResult() != WL_CONNECTED)
            {
                Serial.printf("WiFi Failed!\n");
                return;
            }
        }
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());
    if (!MDNS.begin(mdnsName)) {
        Serial.println("Error setting up MDNS responder!");
        while(1) {
            delay(1000);
        }
    }

    server.on("/brightness", HTTP_GET, [](AsyncWebServerRequest *request) { 
        request->send(200, "application/json", "{\"brightness\":" + String(getBrightness()) + "}");
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { 
        request->send(200, "text/html", index_html); 
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("password")) {
            String password = request->getParam("password")->value();
            if (password == APP_PASSWORD)
            {
                if (request->hasParam("color")) {
                    String color = request->getParam("color")->value();
                    
                    uint32_t number = strtoul(color.substring(1).c_str(), NULL, 16);
                    fill_solid(leds, NUM_LEDS, CRGB(number));
                    preferences.putUInt("color", number);
                }
                if (request->hasParam("brightness")) {
                    int b = request->getParam("brightness")->value().toInt();
                    setBrightness(constrain(b, 0, 240));
                    preferences.putInt("brightness", getBrightness());
                    FastLED.setBrightness(getBrightness());
                }
                FastLED.show();
            }
        }
        request->send(200, "text/plain", "OK"); 
    });
    MDNS.addService("_http", "_tcp", 80);

    server.begin();

    setBrightness(preferences.getInt("brightness", 48));
    uint32_t color = preferences.getUInt("color", 0xFFFFFF);
    FastLED.setBrightness(getBrightness());

    fill_solid(leds, NUM_LEDS, CRGB(color));
    FastLED.show();
}

void loop()
{
    readVibrationSensor();
    if (millis() >= SLEEP_AFTER_MS) {
        goToSleep();
    }
}