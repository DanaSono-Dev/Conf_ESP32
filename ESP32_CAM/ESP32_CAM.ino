#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>

// Credenciales de red
const char* WIFI_SSID     = "SRAI";
const char* WIFI_PASSWORD = "SRAI_E310";

// Broker MQTT
const char* MQTT_BROKER   = "10.42.0.1";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "esp32cam_srai";

// Topicos MQTT
const char* TOPIC_INICIO  = "srai/camara/inicio";
const char* TOPIC_CHUNK   = "srai/camara/chunk";
const char* TOPIC_FIN     = "srai/camara/fin";

// Parametros de captura
const unsigned long INTERVALO_MS = 1UL * 60 * 1000;
const size_t        CHUNK_SIZE   = 4096;

// Pinout AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Objetos globales
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long ultimaCaptura = 0;

// Prototipos
void iniciarCamara();
void conectarWiFi();
void conectarMQTT();
void capturarYPublicar();

void iniciarCamara() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Error camara: 0x%x\n", err);
        ESP.restart();
    }
}

void conectarWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    delay(100);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Conectando WiFi");

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - inicio > 20000) {
            Serial.println("\nTimeout WiFi, reiniciando...");
            ESP.restart();
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
}

void conectarMQTT() {
    while (!mqtt.connected()) {
        Serial.print("Conectando MQTT...");
        if (mqtt.connect(MQTT_CLIENT)) {
            Serial.println("conectado");
        } else {
            Serial.printf("fallo rc=%d, reintentando en 3s\n", mqtt.state());
            delay(3000);
        }
    }
}

void capturarYPublicar() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Error al capturar frame");
        return;
    }

    char timestamp[12];
    snprintf(timestamp, sizeof(timestamp), "%lu", millis());

    size_t total_chunks = (fb->len + CHUNK_SIZE - 1) / CHUNK_SIZE;

    char inicio[128];
    snprintf(inicio, sizeof(inicio),
        "{\"timestamp\":\"%s\",\"total_chunks\":%zu,\"size\":%zu}",
        timestamp, total_chunks, fb->len);
    mqtt.publish(TOPIC_INICIO, inicio);
    delay(50);

    uint8_t buffer[CHUNK_SIZE + 2];
    for (size_t i = 0; i < total_chunks; i++) {
        size_t offset = i * CHUNK_SIZE;
        size_t len    = min(CHUNK_SIZE, fb->len - offset);

        buffer[0] = (i >> 8) & 0xFF;
        buffer[1] =  i       & 0xFF;
        memcpy(buffer + 2, fb->buf + offset, len);

        mqtt.publish(TOPIC_CHUNK, buffer, len + 2, false);
        mqtt.loop();
        delay(20);
    }

    mqtt.publish(TOPIC_FIN, timestamp);

    esp_camera_fb_return(fb);
    Serial.printf("Imagen publicada: %zu bytes en %zu chunks\n", fb->len, total_chunks);
}

void setup() {
    Serial.begin(115200);

    mqtt.setBufferSize(CHUNK_SIZE + 64);
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);

    conectarWiFi();
    iniciarCamara();
    conectarMQTT();
    capturarYPublicar();

    ultimaCaptura = millis();
}

void loop() {
    if (!mqtt.connected()) conectarMQTT();
    mqtt.loop();

    if (millis() - ultimaCaptura >= INTERVALO_MS) {
        capturarYPublicar();
        ultimaCaptura = millis();
    }
}