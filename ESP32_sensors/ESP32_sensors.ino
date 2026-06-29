// Monitor de cultivo de jitomate — ESP32
// FreeRTOS + WiFi + MQTT + 2 zonas independientes (DHT11, MQ-135, suelo, válvula)

#include <Arduino.h>
#include "DHT.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <MQUnifiedsensor.h>

// WiFi y MQTT
#define WIFI_SSID       "SRAI"
#define WIFI_PASSWORD   "SRAI_E310"

#define MQTT_BROKER     "10.42.0.1"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "ESP32_1"
#define MQTT_USER       ""
#define MQTT_PASSWORD   ""

// Tópicos MQTT
String TOPIC_ALERTAS;
String TOPIC_ESTADO;

// Pines — zona 1
#define DHTPIN_Z1       4
#define PIN_MQ135_Z1    32
#define PIN_SUELO_Z1    34
#define PIN_VALVULA_Z1  26
#define DHTTYPE_Z1         DHT11

// Pines — zona 2
#define DHTPIN_Z2       16
#define PIN_MQ135_Z2    33
#define PIN_SUELO_Z2    35
#define PIN_VALVULA_Z2  27
#define DHTTYPE_Z2         DHT22

// Parámetros de cultivo — DHT11
const float TEMP_OPT_MIN     = 22.0;
const float TEMP_OPT_MAX     = 28.0;
const float TEMP_CRITICA      = 35.0;

const float HUM_AMB_OPT_MIN  = 60.0;
const float HUM_AMB_OPT_MAX  = 80.0;
const float HUM_AMB_CRITICA   = 90.0;

// Parámetros de cultivo — suelo
const int SUELO_RIEGO_MIN    = 60;
const int SUELO_RIEGO_MAX    = 80;
const int SUELO_CRITICO      = 40;

const int SUELO_VALOR_SECO   = 2389;
const int SUELO_VALOR_HUMEDO = 352;

// Parámetros de cultivo — MQ-135
const float CO2_OPTIMO       = 1200.0;
const float CO2_ADVERTENCIA  = 2000.0;
const float CO2_CRITICO      = 5000.0;

// MQ-135
#define MQ135_BOARD         "ESP32"
#define MQ135_VOLTAJE       3.3
#define MQ135_RL            10.0
#define MQ135_MUESTRAS_CAL  100
#define MQ135_RATIO_AIRE    3.6

// ADC
#define ADC_MUESTRAS        16

// Intervalos de tareas
#define INTERVALO_SENSORES_MS  60000
#define INTERVALO_MQTT_MS      60000

// Objetos globales
DHT dht1(DHTPIN_Z1, DHTTYPE_Z1);
DHT dht2(DHTPIN_Z2, DHTTYPE_Z2);

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

MQUnifiedsensor mq135_z1(MQ135_BOARD, MQ135_VOLTAJE, 12, PIN_MQ135_Z1, "MQ-135");
MQUnifiedsensor mq135_z2(MQ135_BOARD, MQ135_VOLTAJE, 12, PIN_MQ135_Z2, "MQ-135");

// Datos por zona
struct DatosZona {
  float temperatura;
  float humedadAmbiente;
  int   humedadSuelo;
  bool  valvula;
  float ppmCO2;
  float ppmCO;
  float ppmNH3;
  float ppmAlcohol;
  float ppmHumo;
  float ppmTolueno;
  float ppmAcetona;
  bool  valido;
};

DatosZona datoZ1;
DatosZona datoZ2;

SemaphoreHandle_t xMutexDatos;

// Handles FreeRTOS
TaskHandle_t hTareaSensores = NULL;
TaskHandle_t hTareaMQTT     = NULL;

// Prototipos
void   iniciarTopicos();
void   calibrarMQ135(MQUnifiedsensor &sensor, const char *etiqueta);
void   leerMQ135(MQUnifiedsensor &sensor, DatosZona &zona);
int    leerSueloPromedio(int pin, int muestras);
void   tareaLecturaSensores(void *pvParameters);
void   tareaMQTT(void *pvParameters);
void   conectarWiFi();
void   conectarMQTT();
String evaluarTemperatura(float t, int zona);
String evaluarHumedadAmbiente(float h, int zona);
String evaluarHumedadSuelo(int s, int zona);
String evaluarCalidadAire(float co2ppm, int zona);
String construirJSONZona(const DatosZona &d, int zona);
String construirJSONCompleto(const DatosZona &z1, const DatosZona &z2);

// Tópicos
void iniciarTopicos() {
  String base   = String("invernadero/jitomate/") + MQTT_CLIENT_ID;
  TOPIC_ALERTAS = base + "/alertas";
  TOPIC_ESTADO  = base + "/estado";
}

// Calibración R0 — recibe el sensor por referencia
void calibrarMQ135(MQUnifiedsensor &sensor, const char *etiqueta) {
  Serial.printf("Calibrando %s en aire limpio...\n", etiqueta);

  sensor.setRegressionMethod(1);
  sensor.init();

  float r0 = 0;
  for (int i = 0; i < MQ135_MUESTRAS_CAL; i++) {
    sensor.update();
    r0 += sensor.calibrate(MQ135_RATIO_AIRE);
    delay(10);
  }
  r0 /= MQ135_MUESTRAS_CAL;

  sensor.setR0(r0);
  Serial.printf("R0 %s: %.2f kOhm\n", etiqueta, r0);

  if (r0 < 1.0 || r0 > 100.0) {
    Serial.printf("ADVERTENCIA: R0 fuera de rango en %s - verifica conexion\n", etiqueta);
  }
}

// Lectura de gases MQ-135 — escribe directo en la estructura de zona
void leerMQ135(MQUnifiedsensor &sensor, DatosZona &zona) {
  sensor.update();

  sensor.setA(110.47); sensor.setB(-2.862);
  zona.ppmCO2     = sensor.readSensor();

  sensor.setA(605.18); sensor.setB(-3.937);
  zona.ppmCO      = sensor.readSensor();

  sensor.setA(102.2);  sensor.setB(-2.473);
  zona.ppmNH3     = sensor.readSensor();

  sensor.setA(77.255); sensor.setB(-3.18);
  zona.ppmAlcohol = sensor.readSensor();

  sensor.setA(3616.1); sensor.setB(-2.675);
  zona.ppmHumo    = sensor.readSensor();

  sensor.setA(44.947); sensor.setB(-3.445);
  zona.ppmTolueno = sensor.readSensor();

  sensor.setA(34.668); sensor.setB(-3.369);
  zona.ppmAcetona = sensor.readSensor();
}

// Lectura promediada sensor capacitivo de suelo
int leerSueloPromedio(int pin, int muestras) {
  long suma = 0;
  for (int i = 0; i < muestras; i++) {
    suma += analogRead(pin);
    delayMicroseconds(100);
  }
  return (int)(suma / muestras);
}

// Setup
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Monitoreo ESP32 1 ===");

  iniciarTopicos();

  dht1.begin();
  dht2.begin();
  delay(2000);

  pinMode(PIN_VALVULA_Z1, OUTPUT);
  pinMode(PIN_VALVULA_Z2, OUTPUT);
  digitalWrite(PIN_VALVULA_Z1, LOW);
  digitalWrite(PIN_VALVULA_Z2, LOW);

  // Calibración independiente por sensor
  calibrarMQ135(mq135_z1, "MQ135-Z1");
  calibrarMQ135(mq135_z2, "MQ135-Z2");

  conectarWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(30);

  xMutexDatos = xSemaphoreCreateMutex();
  datoZ1 = { 0, 0, 0, false, 0, 0, 0, 0, 0, 0, 0, false };
  datoZ2 = { 0, 0, 0, false, 0, 0, 0, 0, 0, 0, 0, false };

  xTaskCreatePinnedToCore(tareaLecturaSensores, "TareaSensores", 8192, NULL, 2, &hTareaSensores, 0);
  xTaskCreatePinnedToCore(tareaMQTT,            "TareaMQTT",     8192, NULL, 1, &hTareaMQTT,     1);

  Serial.println("Tareas FreeRTOS iniciadas.");
  Serial.println("  [Nucleo 0] TareaSensores");
  Serial.println("  [Nucleo 1] TareaMQTT");
}

void loop() {
  vTaskDelete(NULL);
}

// Tarea 1 — Sensores y control de válvulas (núcleo 0)
void tareaLecturaSensores(void *pvParameters) {
  TickType_t xLastWake = xTaskGetTickCount();

  for (;;) {
    DatosZona z1, z2;

    // DHT11
    z1.temperatura     = dht1.readTemperature();
    z1.humedadAmbiente = dht1.readHumidity();

    // DHT22
    z2.temperatura     = dht2.readTemperature();
    z2.humedadAmbiente = dht2.readHumidity();

    // Humedad de suelo
    int rawZ1       = leerSueloPromedio(PIN_SUELO_Z1, ADC_MUESTRAS);
    z1.humedadSuelo = constrain(map(rawZ1, SUELO_VALOR_SECO, SUELO_VALOR_HUMEDO, 0, 100), 0, 100);

    int rawZ2       = leerSueloPromedio(PIN_SUELO_Z2, ADC_MUESTRAS);
    z2.humedadSuelo = constrain(map(rawZ2, SUELO_VALOR_SECO, SUELO_VALOR_HUMEDO, 0, 100), 0, 100);

    // Control de válvulas
    if (z1.humedadSuelo <= SUELO_RIEGO_MIN)      digitalWrite(PIN_VALVULA_Z1, HIGH);
    else if (z1.humedadSuelo >= SUELO_RIEGO_MAX) digitalWrite(PIN_VALVULA_Z1, LOW);

    if (z2.humedadSuelo <= SUELO_RIEGO_MIN)      digitalWrite(PIN_VALVULA_Z2, HIGH);
    else if (z2.humedadSuelo >= SUELO_RIEGO_MAX) digitalWrite(PIN_VALVULA_Z2, LOW);

    z1.valvula = digitalRead(PIN_VALVULA_Z1);
    z2.valvula = digitalRead(PIN_VALVULA_Z2);

    // MQ-135
    leerMQ135(mq135_z1, z1);
    leerMQ135(mq135_z2, z2);

    z1.valido = !isnan(z1.temperatura) && !isnan(z1.humedadAmbiente) && z1.ppmCO2 > 0;
    z2.valido = !isnan(z2.temperatura) && !isnan(z2.humedadAmbiente) && z2.ppmCO2 > 0;

    if (xSemaphoreTake(xMutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {
      datoZ1 = z1;
      datoZ2 = z2;
      xSemaphoreGive(xMutexDatos);
    }

    // Serial — zona 1
    Serial.println("--- Zona 1 ---");
    Serial.printf("  Temperatura:      %.1f C\n",  z1.temperatura);
    Serial.printf("  Humedad ambiente: %.1f %%\n", z1.humedadAmbiente);
    Serial.printf("  Suelo:            %d %%  Valvula: %s\n", z1.humedadSuelo, z1.valvula ? "ABIERTA" : "CERRADA");
    Serial.printf("  CO2: %.1f  CO: %.1f  NH3: %.1f  Alcohol: %.1f  Humo: %.1f  Tolueno: %.1f  Acetona: %.1f (ppm)\n",
      z1.ppmCO2, z1.ppmCO, z1.ppmNH3, z1.ppmAlcohol, z1.ppmHumo, z1.ppmTolueno, z1.ppmAcetona);

    // Serial — zona 2
    Serial.println("--- Zona 2 ---");
    Serial.printf("  Temperatura:      %.1f C\n",  z2.temperatura);
    Serial.printf("  Humedad ambiente: %.1f %%\n", z2.humedadAmbiente);
    Serial.printf("  Suelo:            %d %%  Valvula: %s\n", z2.humedadSuelo, z2.valvula ? "ABIERTA" : "CERRADA");
    Serial.printf("  CO2: %.1f  CO: %.1f  NH3: %.1f  Alcohol: %.1f  Humo: %.1f  Tolueno: %.1f  Acetona: %.1f (ppm)\n",
      z2.ppmCO2, z2.ppmCO, z2.ppmNH3, z2.ppmAlcohol, z2.ppmHumo, z2.ppmTolueno, z2.ppmAcetona);

    vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_SENSORES_MS));
  }
}

// Tarea 2 — MQTT (núcleo 1)
void tareaMQTT(void *pvParameters) {
  TickType_t xLastWake = xTaskGetTickCount();

  conectarMQTT();

  for (;;) {
    if (!mqttClient.connected()) conectarMQTT();
    mqttClient.loop();

    DatosZona z1, z2;
    if (xSemaphoreTake(xMutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {
      z1 = datoZ1;
      z2 = datoZ2;
      xSemaphoreGive(xMutexDatos);
    } else {
      vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_MQTT_MS));
      continue;
    }

    if (!z1.valido && !z2.valido) {
      mqttClient.publish(TOPIC_ALERTAS.c_str(), "ERROR: Fallo en lectura de ambas zonas");
      vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_MQTT_MS));
      continue;
    }

    // JSON único con ambas zonas
    String json = construirJSONCompleto(z1, z2);
    mqttClient.publish(TOPIC_ESTADO.c_str(), json.c_str());

    // Alertas por zona
    String alerta = "";
    if (z1.valido) {
      alerta += evaluarTemperatura(z1.temperatura, 1);
      alerta += evaluarHumedadAmbiente(z1.humedadAmbiente, 1);
      alerta += evaluarHumedadSuelo(z1.humedadSuelo, 1);
      alerta += evaluarCalidadAire(z1.ppmCO2, 1);
    } else {
      alerta += "CRITICA|zona1|Fallo en lectura de sensores\n";
    }

    if (z2.valido) {
      alerta += evaluarTemperatura(z2.temperatura, 2);
      alerta += evaluarHumedadAmbiente(z2.humedadAmbiente, 2);
      alerta += evaluarHumedadSuelo(z2.humedadSuelo, 2);
      alerta += evaluarCalidadAire(z2.ppmCO2, 2);
    } else {
      alerta += "CRITICA|zona2|Fallo en lectura de sensores\n";
    }

    if (alerta.length() > 0) {
      mqttClient.publish(TOPIC_ALERTAS.c_str(), alerta.c_str());
      Serial.println("Alertas publicadas:");
      Serial.println(alerta);
    }

    vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_MQTT_MS));
  }
}

// WiFi
void conectarWiFi() {
  Serial.printf("Conectando a WiFi: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nError: No se pudo conectar al WiFi. Reiniciando...");
    esp_restart();
  }
}

// MQTT
void conectarMQTT() {
  int intentos = 0;
  while (!mqttClient.connected() && intentos < 5) {
    Serial.printf("Conectando al broker MQTT %s:%d...", MQTT_BROKER, MQTT_PORT);

    bool conectado = (strlen(MQTT_USER) > 0)
      ? mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqttClient.connect(MQTT_CLIENT_ID);

    if (conectado) {
      Serial.println(" OK");
      mqttClient.publish(TOPIC_ESTADO.c_str(), "{\"evento\":\"online\",\"dispositivo\":\"ESP32_zona_1\"}");
    } else {
      Serial.printf(" Error: estado=%d. Reintentando en 3 s...\n", mqttClient.state());
      vTaskDelay(pdMS_TO_TICKS(3000));
      intentos++;
    }
  }
}

// Evaluaciones
String evaluarTemperatura(float t, int zona) {
  String z = "|zona" + String(zona) + "_temp|";
  if (isnan(t))         return "CRITICA"     + z + "Error de lectura DHT11\n";
  if (t > TEMP_CRITICA) return "CRITICA"     + z + "Mayor a 35C - activar extractor\n";
  if (t > TEMP_OPT_MAX) return "ADVERTENCIA" + z + "Alta (28-35C) - revisar ventilacion\n";
  if (t < TEMP_OPT_MIN) return "AVISO"       + z + "Baja (<22C) - considerar calefaccion\n";
  return "";
}

String evaluarHumedadAmbiente(float h, int zona) {
  String z = "|zona" + String(zona) + "_hum_amb|";
  if (isnan(h))             return "CRITICA"     + z + "Error de lectura DHT11\n";
  if (h > HUM_AMB_CRITICA)  return "CRITICA"     + z + "Mayor a 90% - riesgo de hongos, activar extractor\n";
  if (h > HUM_AMB_OPT_MAX)  return "ADVERTENCIA" + z + "Alta (80-90%) - aumentar ventilacion\n";
  if (h < HUM_AMB_OPT_MIN)  return "AVISO"       + z + "Baja (<60%) - revisar nebulizacion\n";
  return "";
}

String evaluarHumedadSuelo(int s, int zona) {
  String z = "|zona" + String(zona) + "_hum_suelo|";
  if (s < SUELO_CRITICO)   return "CRITICA"     + z + "Menor a 40% - riego urgente\n";
  if (s < SUELO_RIEGO_MIN) return "ADVERTENCIA" + z + "Baja (40-60%) - valvula abierta\n";
  if (s > SUELO_RIEGO_MAX) return "ADVERTENCIA" + z + "Exceso (>80%) - valvula cerrada\n";
  return "";
}

String evaluarCalidadAire(float co2ppm, int zona) {
  String z = "|zona" + String(zona) + "_co2|";
  if (co2ppm > CO2_CRITICO)     return "CRITICA"     + z + "Mayor a 5000 ppm - activar ventilacion urgente\n";
  if (co2ppm > CO2_ADVERTENCIA) return "ADVERTENCIA" + z + "Alta (2000-5000 ppm) - aumentar ventilacion\n";
  if (co2ppm > CO2_OPTIMO)      return "AVISO"       + z + "Sobre nivel optimo (>1200 ppm)\n";
  return "";
}

// JSON por zona
String construirJSONZona(const DatosZona &d, int zona) {
  String estadoTemp   = (d.temperatura > TEMP_CRITICA)        ? "critica"     :
                        (d.temperatura > TEMP_OPT_MAX)        ? "advertencia" :
                        (d.temperatura < TEMP_OPT_MIN)        ? "baja"        : "optima";

  String estadoHumAmb = (d.humedadAmbiente > HUM_AMB_CRITICA) ? "critica"     :
                        (d.humedadAmbiente > HUM_AMB_OPT_MAX) ? "advertencia" :
                        (d.humedadAmbiente < HUM_AMB_OPT_MIN) ? "baja"        : "optima";

  String estadoSuelo  = (d.humedadSuelo < SUELO_CRITICO)      ? "critica" :
                        (d.humedadSuelo < SUELO_RIEGO_MIN)     ? "riego"   :
                        (d.humedadSuelo > SUELO_RIEGO_MAX)     ? "exceso"  : "optima";

  String estadoCO2    = (d.ppmCO2 > CO2_CRITICO)              ? "critica"     :
                        (d.ppmCO2 > CO2_ADVERTENCIA)           ? "advertencia" :
                        (d.ppmCO2 > CO2_OPTIMO)                ? "aviso"       : "optima";

  char buf[768];
  snprintf(buf, sizeof(buf),
    "{"
      "\"temperatura\":%.1f,"
      "\"temp_estado\":\"%s\","
      "\"hum_ambiente\":%.1f,"
      "\"hum_amb_estado\":\"%s\","
      "\"hum_suelo\":%d,"
      "\"suelo_estado\":\"%s\","
      "\"valvula\":\"%s\","
      "\"co2_ppm\":%.1f,"
      "\"co2_estado\":\"%s\","
      "\"co_ppm\":%.1f,"
      "\"nh3_ppm\":%.1f,"
      "\"alcohol_ppm\":%.1f,"
      "\"humo_ppm\":%.1f,"
      "\"tolueno_ppm\":%.1f,"
      "\"acetona_ppm\":%.1f"
    "}",
    d.temperatura,     estadoTemp.c_str(),
    d.humedadAmbiente, estadoHumAmb.c_str(),
    d.humedadSuelo,    estadoSuelo.c_str(),
    d.valvula ? "ABIERTA" : "CERRADA",
    d.ppmCO2,          estadoCO2.c_str(),
    d.ppmCO,
    d.ppmNH3,
    d.ppmAlcohol,
    d.ppmHumo,
    d.ppmTolueno,
    d.ppmAcetona
  );

  return String(buf);
}

// JSON raíz con ambas zonas
String construirJSONCompleto(const DatosZona &z1, const DatosZona &z2) {
  String j1 = construirJSONZona(z1, 1);
  String j2 = construirJSONZona(z2, 2);
  return "{\"zona1\":" + j1 + ",\"zona2\":" + j2 + "}";
}