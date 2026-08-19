#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_INA219.h>
#include <Adafruit_ADXL345_U.h>

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RST -1
#define PIN_SOIL_1 36
#define PIN_SOIL_2 34
#define PIN_RELAY  13
#define PIN_BUZZER 25

const float BATAS_POMPA_ON  = 86.0;
const float BATAS_POMPA_OFF = 81.6;
const float BATERAI_KRITIS  = 11.0;
const float THRESHOLD_KEMIRINGAN = 2.0;
const float VWC_MAX_FISIS = 0.6739;

const float EMA_ALPHA = 0.1;
float vwc1_terfilter = -1.0;
float vwc2_terfilter = -1.0;

enum SystemMode {
    MODE_MANDIRI,
    MODE_SERVER
};
SystemMode currentMode = MODE_MANDIRI;

unsigned long lastHeartbeatTime = 0;
uint8_t lastStatusSistem = 0;

bool buzzerWantsOn = false;
bool currentBuzzerPinState = false;
unsigned long lastBuzzerToggle = 0;
const unsigned int BUZZER_INTERVAL = 500;

Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
Adafruit_INA219 inaSolar(0x40);
Adafruit_INA219 inaAki(0x41);
Adafruit_ADXL345_Unified axdl = Adafruit_ADXL345_Unified(12345);

float baseX = 0.0, baseY = 0.0;
int tiltTimer = 0;
bool relayState = false;
uint8_t statusSistem = 0;

float sDangkalCache = 0.0, sDalamCache = 0.0;
float magCache = 0.0, vSolCache = 0.0, iSolCache = 0.0, vAkiCache = 0.0, iAkiCache = 0.0;

uint8_t txBuffer[14];
unsigned long lastSampleMillis = 0;
volatile int perintahDownlink = -1;
bool kirimBagianGeoteknik = true;

uint8_t NWKSKEY[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint8_t APPSKEY[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
uint32_t DEVADDR = 0x0118ECD5;

void os_getArtEui (u1_t* buf) { }
void os_getDevEui (u1_t* buf) { }
void os_getDevKey (u1_t* buf) { }

static osjob_t sendjob;

const lmic_pinmap lmic_pins = {
    .nss = 18,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = 14,
    .dio = {26, 33, 32},
};

void matikanWiFiBluetooth() {
    WiFi.mode(WIFI_OFF);
    btStop();
    esp_wifi_stop();
    esp_bt_controller_disable();
}

unsigned int getAdaptiveTxInterval() {
    return 10;
}

void handleBuzzer() {
    if (buzzerWantsOn) {
        if (millis() - lastBuzzerToggle >= BUZZER_INTERVAL) {
            lastBuzzerToggle = millis();
            currentBuzzerPinState = !currentBuzzerPinState;
            digitalWrite(PIN_BUZZER, currentBuzzerPinState ? HIGH : LOW);
        }
    } else {
        if (currentBuzzerPinState) {
            currentBuzzerPinState = false;
            digitalWrite(PIN_BUZZER, LOW);
        }
    }
}

int bacaSensorStabil(int pin) {
    long totalNilai = 0;
    int jumlahSampel = 100;
    for(int i = 0; i < jumlahSampel; i++) {
        totalNilai += analogRead(pin);
        delay(2);
    }
    return totalNilai / jumlahSampel;
}

float hitungVWCSensor1(int rawNilai) {
    float raw = (float)rawNilai;
    float vwc = 0.0;
    if (raw >= 3288.20) vwc = 0.0066;
    else if (raw >= 2846.03) vwc = (-0.000485453 * raw) + 1.602853002;
    else if (raw >= 1848.49) vwc = (-0.0000235895 * raw) + 0.288378959;
    else if (raw >= 1130.93) vwc = (-0.000129036 * raw) + 0.53846148;
    else if (raw >= 1097.12) vwc = (-0.002434864 * raw) + 3.34524179;
    else vwc = VWC_MAX_FISIS;

    if (vwc < 0.0) vwc = 0.000;
    if (vwc > VWC_MAX_FISIS) vwc = VWC_MAX_FISIS;
    return vwc;
}

float hitungVWCSensor2(int rawNilai) {
    float raw = (float)rawNilai;
    float vwc = 0.0;
    if (raw >= 3314.42) vwc = 0.0066;
    else if (raw >= 2931.75) vwc = (-0.000560947 * raw) + 1.865801754;
    else if (raw >= 1858.62) vwc = (-0.0000125188 * raw) + 0.257944676;
    else if (raw >= 1168.50) vwc = (-0.000117406 * raw) + 0.518151322;
    else if (raw >= 1139.00) vwc = (-0.002909906 * raw) + 3.988298417;
    else vwc = VWC_MAX_FISIS;

    if (vwc < 0.0) vwc = 0.000;
    if (vwc > VWC_MAX_FISIS) vwc = VWC_MAX_FISIS;
    return vwc;
}

void kalibrasiTilt() {
    float sumX = 0, sumY = 0;
    for(int i = 0; i < 20; i++) {
        sensors_event_t event; axdl.getEvent(&event);
        sumX += atan2(-event.acceleration.x, sqrt(event.acceleration.y*event.acceleration.y + event.acceleration.z*event.acceleration.z)) * 57.3;
        sumY += atan2(event.acceleration.y, event.acceleration.z) * 57.3;
        delay(50);
    }
    baseX = sumX / 20.0;
    baseY = sumY / 20.0;
}

void eksekusiLogikaLokal() {
    statusSistem = 0;
    if (magCache >= THRESHOLD_KEMIRINGAN) tiltTimer++;
    else tiltTimer = 0;

    if (tiltTimer >= 2) statusSistem = 2;
    else if (sDalamCache >= BATAS_POMPA_ON) statusSistem = 1;

    if (vAkiCache < BATERAI_KRITIS) {
        relayState = false;
    } else {
        if (sDalamCache >= BATAS_POMPA_ON) relayState = true;
        else if (sDalamCache <= BATAS_POMPA_OFF) relayState = false;
    }
    
    digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
    buzzerWantsOn = (statusSistem == 2);
}

void bacaSensor() {
    int raw1 = bacaSensorStabil(PIN_SOIL_1);
    int raw2 = bacaSensorStabil(PIN_SOIL_2);

    float vwcMentah1 = hitungVWCSensor1(raw1);
    float vwcMentah2 = hitungVWCSensor2(raw2);

    if (vwc1_terfilter < 0.0) {
        vwc1_terfilter = vwcMentah1; vwc2_terfilter = vwcMentah2;
    } else {
        vwc1_terfilter = (EMA_ALPHA * vwcMentah1) + ((1.0 - EMA_ALPHA) * vwc1_terfilter);
        vwc2_terfilter = (EMA_ALPHA * vwcMentah2) + ((1.0 - EMA_ALPHA) * vwc2_terfilter);
    }

    sDangkalCache = (vwc1_terfilter / VWC_MAX_FISIS) * 100.0;
    sDalamCache   = (vwc2_terfilter / VWC_MAX_FISIS) * 100.0;
    if (sDangkalCache > 100.0) sDangkalCache = 100.0;
    if (sDalamCache > 100.0) sDalamCache = 100.0;
    
    vSolCache = isnan(inaSolar.getBusVoltage_V()) ? 0.0 : inaSolar.getBusVoltage_V();
    iSolCache = isnan(inaSolar.getCurrent_mA()) ? 0.0 : inaSolar.getCurrent_mA();
    vAkiCache = isnan(inaAki.getBusVoltage_V()) ? 0.0 : inaAki.getBusVoltage_V();
    iAkiCache = isnan(inaAki.getCurrent_mA()) ? 0.0 : inaAki.getCurrent_mA();

    sensors_event_t event; axdl.getEvent(&event);
    float curX = atan2(-event.acceleration.x, sqrt(event.acceleration.y*event.acceleration.y + event.acceleration.z*event.acceleration.z)) * 57.3;
    float curY = atan2(event.acceleration.y, event.acceleration.z) * 57.3;
    magCache = sqrt(pow(curX - baseX, 2) + pow(curY - baseY, 2));

    if (currentMode == MODE_MANDIRI) {
        eksekusiLogikaLokal();
    } else {
        if (vAkiCache < BATERAI_KRITIS) relayState = false;
        digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
        buzzerWantsOn = (statusSistem == 2);
    }

    txBuffer[0] = (uint8_t)sDangkalCache;
    txBuffer[1] = (uint8_t)sDalamCache;
    uint16_t mag_int = magCache * 100;
    txBuffer[2] = (mag_int >> 8) & 0xFF; txBuffer[3] = mag_int & 0xFF;
    uint16_t vSol_int = vSolCache * 100;
    txBuffer[4] = (vSol_int >> 8) & 0xFF; txBuffer[5] = vSol_int & 0xFF;
    int16_t iSol_int = iSolCache;
    txBuffer[6] = (iSol_int >> 8) & 0xFF; txBuffer[7] = iSol_int & 0xFF;
    uint16_t vAki_int = vAkiCache * 100;
    txBuffer[8] = (vAki_int >> 8) & 0xFF; txBuffer[9] = vAki_int & 0xFF;
    int16_t iAki_int = iAkiCache;
    txBuffer[10] = (iAki_int >> 8) & 0xFF; txBuffer[11] = iAki_int & 0xFF;
    
    txBuffer[12] = (currentMode == MODE_SERVER) ? (statusSistem | 0x80) : statusSistem;
    txBuffer[13] = (relayState ? 1 : 0) | (buzzerWantsOn ? 2 : 0);
}

void updateOLED() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    
    display.setCursor(0, 0);
    display.printf("[%s] RLY:%s\n", (currentMode == MODE_SERVER ? "REMOTE" : "LOCAL"), (relayState ? "ON" : "OFF"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    
    display.setCursor(0, 15);
    display.printf("S1:%d%% | S2:%d%%\n", txBuffer[0], txBuffer[1]);
    
    display.setCursor(0, 27);
    display.printf("Tilt: %.1f deg | ST:%d\n", magCache, statusSistem & 0x0F);
    display.setCursor(0, 39);
    display.printf("Aki : %.1fV %dmA\n", vAkiCache, (int)iAkiCache);
    display.setCursor(0, 51);
    display.printf("Sol : %.1fV %dmA\n", vSolCache, (int)iSolCache);
    
    display.display();
}

void do_send(osjob_t* j){
    if (LMIC.opmode & OP_TXRXPEND) return;
    
    bacaSensor();
    
    if (kirimBagianGeoteknik) {
        uint8_t payloadA[6];
        payloadA[0] = txBuffer[0];
        payloadA[1] = txBuffer[1];
        payloadA[2] = txBuffer[2];
        payloadA[3] = txBuffer[3];
        payloadA[4] = txBuffer[12];
        payloadA[5] = txBuffer[13];
        LMIC_setTxData2(1, payloadA, sizeof(payloadA), 0);
        kirimBagianGeoteknik = false;
    } else {
        uint8_t payloadB[8];
        payloadB[0] = txBuffer[4];
        payloadB[1] = txBuffer[5];
        payloadB[2] = txBuffer[6];
        payloadB[3] = txBuffer[7];
        payloadB[4] = txBuffer[8];
        payloadB[5] = txBuffer[9];
        payloadB[6] = txBuffer[10];
        payloadB[7] = txBuffer[11];
        LMIC_setTxData2(2, payloadB, sizeof(payloadB), 0);
        kirimBagianGeoteknik = true;
    }
}

void onEvent (ev_t ev) {
    if (ev == EV_TXCOMPLETE) {
        bool validHeartbeat = false;
        if (LMIC.dataLen) {
            u1_t port = 0;
            if (LMIC.txrxFlags & TXRX_PORT) port = LMIC.frame[LMIC.dataBeg - 1];
            if (port == 10) {
                perintahDownlink = LMIC.frame[LMIC.dataBeg + 0];
                validHeartbeat = true;
            }
        }
        if (validHeartbeat) {
            lastHeartbeatTime = millis();
            if (currentMode == MODE_MANDIRI) {
                currentMode = MODE_SERVER;
            }
        }
        unsigned int nextTxInterval = getAdaptiveTxInterval();
        os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(nextTxInterval), do_send);
    }
}

void setup() {
    Serial.begin(115200);
    matikanWiFiBluetooth();

    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_RELAY, LOW);
    digitalWrite(PIN_BUZZER, LOW);

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(50);
    
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setCursor(15, 25);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.println(F("SLOPEX STARTING..."));
    display.display();
    delay(1000);

    inaSolar.begin();
    inaAki.begin();
    if(axdl.begin()) { kalibrasiTilt(); }

    bacaSensor();
    updateOLED();

    os_init();
    LMIC_reset();
    LMIC_setSession(0x00, DEVADDR, NWKSKEY, APPSKEY);

    LMIC.rxDelay = 3;
    LMIC_setClockError(MAX_CLOCK_ERROR * 10 / 100);

    LMIC_setupChannel(0, 923200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    for (int i = 1; i < 16; i++) { LMIC_disableChannel(i); }
    
    LMIC_setLinkCheckMode(0);
    LMIC_setAdrMode(0);
    LMIC_setDrTxpow(DR_SF10, 20);
    
    lastHeartbeatTime = millis();
    do_send(&sendjob);
}

void loop() {
    os_runloop_once();
    handleBuzzer();

    unsigned long currentMillis = millis();
    unsigned long timeoutDinamis = (getAdaptiveTxInterval() * 3 * 1000) + 15000;

    if (currentMode == MODE_SERVER && (currentMillis - lastHeartbeatTime > timeoutDinamis)) {
        currentMode = MODE_MANDIRI;
        eksekusiLogikaLokal();
        updateOLED();
    }

    if (perintahDownlink != -1) {
        int cmd = perintahDownlink;
        perintahDownlink = -1;
        
        if (currentMode == MODE_SERVER) {
            if (cmd == 0x01) {
                statusSistem = 1; relayState = true; buzzerWantsOn = false;
            } else if (cmd == 0x02) {
                statusSistem = 2; relayState = false; buzzerWantsOn = true;
            } else if (cmd == 0x00) {
                statusSistem = 0; relayState = false; buzzerWantsOn = false;
            }
            digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
            updateOLED();
        }
    }

    if (currentMillis - lastSampleMillis >= 2000) {
        lastSampleMillis = currentMillis;
        
        if ((LMIC.opmode & OP_TXRXPEND) == 0) {
            bacaSensor();
            updateOLED();

            if (statusSistem == 2 && lastStatusSistem < 2) {
                os_clearCallback(&sendjob);
                do_send(&sendjob);
            }
            lastStatusSistem = statusSistem;
        }
    }
}
