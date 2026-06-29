// Monitor de cultivo de jitomate — ESP32
// FreeRTOS + WiFi + MQTT + MQ-135 ppm + 2 zonas de riego

// Importaciones
#include <Arduino.h>
#include "DHT.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <MQUnifiedsensor.h>

// WiFi y MQTT
#define WIFI_SSID          "SRAI"
#define WIFI_PASSWORD      "SRAI_E310"

#define MQTT_BROKER        "10.42.0.1"
#define MQTT_PORT          1883
#define MQTT_CLIENT_ID     "ESP32_zona_1"
#define MQTT_USER          ""
#define MQTT_PASSWORD      ""

// Tópicos MQTT
String TOPIC_ALERTAS;
String TOPIC_ESTADO;

// Pines — sensores
#define DHTPIN             4
#define DHTTYPE            DHT11
#define PIN_SUELO_Z1       34
#define PIN_SUELO_Z2       35
#define PIN_MQ135          32

// Pines — relés
#define PIN_VALVULA_1      25
#define PIN_VALVULA_2      26

// Parámetros de cultivo — DHT11
const float TEMP_OPT_MIN    = 22.0;
const float TEMP_OPT_MAX    = 28.0;
const float TEMP_CRITICA     = 35.0;

const float HUM_AMB_OPT_MIN = 60.0;
const float HUM_AMB_OPT_MAX = 80.0;
const float HUM_AMB_CRITICA  = 90.0;

// Parámetros de cultivo — suelo
const int SUELO_RIEGO_MIN    = 60;
const int SUELO_RIEGO_MAX    = 80;
const int SUELO_CRITICO      = 40;

const int SUELO_VALOR_SECO   = 2389;
const int SUELO_VALOR_HUMEDO = 352;

// Parámetros de cultivo — MQ-135
const float CO2_OPTIMO      = 1200.0;
const float CO2_ADVERTENCIA = 2000.0;
const float CO2_CRITICO     = 5000.0;

// MQ-135
#define MQ135_BOARD        "ESP32"
#define MQ135_VOLTAJE      3.3
#define MQ135_RL           10.0
#define MQ135_MUESTRAS_CAL 100
#define MQ135_RATIO_AIRE   3.6

// Muestras ADC suelo
#define ADC_MUESTRAS       16

// Intervalos de tareas
#define INTERVALO_SENSORES_MS  60000
#define INTERVALO_MQTT_MS      60000

// Objetos globales
DHT          dht(DHTPIN, DHTTYPE);
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

MQUnifiedsensor mq135(MQ135_BOARD, MQ135_VOLTAJE, 12, PIN_MQ135, "MQ-135");

// Estructura compartida entre tareas
struct DatosSensores {
  float temperatura;
  float humedadAmbiente;
  int   humedadSueloZ1;
  int   humedadSueloZ2;
  bool  valvulaZ1;
  bool  valvulaZ2;
  float ppmCO2;
  float ppmCO;
  float ppmNH3;
  float ppmAlcohol;
  float ppmHumo;
  float ppmTolueno;
  float ppmAcetona;
  bool  valido;
};

DatosSensores     datosSensores;
SemaphoreHandle_t xMutexDatos;

// Handles FreeRTOS
TaskHandle_t hTareaSensores = NULL;
TaskHandle_t hTareaMQTT     = NULL;

// Prototipos
void   iniciarTopicos();
void   tareaLecturaSensores(void *pvParameters);
void   tareaMQTT(void *pvParameters);
void   conectarWiFi();
void   conectarMQTT();
void   calibrarMQ135();
int    leerSueloPromedio(int pin, int muestras);
String evaluarTemperatura(float t);
String evaluarHumedadAmbiente(float h);
String evaluarHumedadSuelo(int s, int zona);
String evaluarCalidadAire(float co2ppm);
String construirJSON(const DatosSensores &d);

// Construcción de tópicos
void iniciarTopicos() {
  String base   = String("invernadero/jitomate/") + MQTT_CLIENT_ID;
  TOPIC_ALERTAS = base + "/alertas";
  TOPIC_ESTADO  = base + "/estado";
}

// Calibración R0 del MQ-135
void calibrarMQ135() {
  Serial.println("Calibrando MQ-135 en aire limpio...");

  mq135.setRegressionMethod(1);
  mq135.init();

  float r0 = 0;
  for (int i = 0; i < MQ135_MUESTRAS_CAL; i++) {
    mq135.update();
    r0 += mq135.calibrate(MQ135_RATIO_AIRE);
    delay(10);
  }
  r0 /= MQ135_MUESTRAS_CAL;

  mq135.setR0(r0);
  Serial.printf("R0 calculado: %.2f kOhm\n", r0);

  if (r0 < 1.0 || r0 > 100.0) {
    Serial.println("ADVERTENCIA: R0 fuera de rango - verifica conexion del sensor");
  }
}

// Lectura promediada sensor de suelo
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
  Serial.println("=== Monitoreo zona 1 ===");

  iniciarTopicos();

  dht.begin();
  delay(2000);

  pinMode(PIN_VALVULA_1, OUTPUT);
  pinMode(PIN_VALVULA_2, OUTPUT);
  digitalWrite(PIN_VALVULA_1, LOW);
  digitalWrite(PIN_VALVULA_2, LOW);

  calibrarMQ135();
  conectarWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(30);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);   // ← agregar esta línea
  mqttClient.setKeepAlive(30);


  xMutexDatos   = xSemaphoreCreateMutex();
  datosSensores = { 0, 0, 0, 0, false, false, 0, 0, 0, 0, 0, 0, 0, false };

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
    // DHT11
    float temperatura     = dht.readTemperature();
    float humedadAmbiente = dht.readHumidity();

    // Sensor de suelo — zona 1
    int rawSueloZ1     = leerSueloPromedio(PIN_SUELO_Z1, ADC_MUESTRAS);
    int humedadSueloZ1 = map(rawSueloZ1, SUELO_VALOR_SECO, SUELO_VALOR_HUMEDO, 0, 100);
    humedadSueloZ1     = constrain(humedadSueloZ1, 0, 100);

    // Sensor de suelo — zona 2
    int rawSueloZ2     = leerSueloPromedio(PIN_SUELO_Z2, ADC_MUESTRAS);
    int humedadSueloZ2 = map(rawSueloZ2, SUELO_VALOR_SECO, SUELO_VALOR_HUMEDO, 0, 100);
    humedadSueloZ2     = constrain(humedadSueloZ2, 0, 100);

    // Control válvula zona 1
    if (humedadSueloZ1 <= SUELO_RIEGO_MIN) {
      digitalWrite(PIN_VALVULA_1, HIGH);
    } else if (humedadSueloZ1 >= SUELO_RIEGO_MAX) {
      digitalWrite(PIN_VALVULA_1, LOW);
    }

    // Control válvula zona 2
    if (humedadSueloZ2 <= SUELO_RIEGO_MIN) {
      digitalWrite(PIN_VALVULA_2, HIGH);
    } else if (humedadSueloZ2 >= SUELO_RIEGO_MAX) {
      digitalWrite(PIN_VALVULA_2, LOW);
    }

    bool valvulaZ1 = digitalRead(PIN_VALVULA_1);
    bool valvulaZ2 = digitalRead(PIN_VALVULA_2);

    // MQ-135
    mq135.update();

    mq135.setA(110.47); mq135.setB(-2.862);
    float ppmCO2     = mq135.readSensor();

    mq135.setA(605.18); mq135.setB(-3.937);
    float ppmCO      = mq135.readSensor();

    mq135.setA(102.2);  mq135.setB(-2.473);
    float ppmNH3     = mq135.readSensor();

    mq135.setA(77.255); mq135.setB(-3.18);
    float ppmAlcohol = mq135.readSensor();

    mq135.setA(3616.1); mq135.setB(-2.675);
    float ppmHumo    = mq135.readSensor();

    mq135.setA(44.947); mq135.setB(-3.445);
    float ppmTolueno = mq135.readSensor();
    mq135.setA(34.668); mq135.setB(-3.369);
    float ppmAcetona = mq135.readSensor();

    bool lecturaValida = !isnan(temperatura) && !isnan(humedadAmbiente) && ppmCO2 > 0;

    if (xSemaphoreTake(xMutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {
      datosSensores.temperatura     = temperatura;
      datosSensores.humedadAmbiente = humedadAmbiente;
      datosSensores.humedadSueloZ1  = humedadSueloZ1;
      datosSensores.humedadSueloZ2  = humedadSueloZ2;
      datosSensores.valvulaZ1       = valvulaZ1;
      datosSensores.valvulaZ2       = valvulaZ2;
      datosSensores.ppmCO2          = ppmCO2;
      datosSensores.ppmCO           = ppmCO;
      datosSensores.ppmNH3          = ppmNH3;
      datosSensores.ppmAlcohol      = ppmAlcohol;
      datosSensores.ppmHumo         = ppmHumo;
      datosSensores.ppmTolueno      = ppmTolueno;
      datosSensores.ppmAcetona      = ppmAcetona;
      datosSensores.valido          = lecturaValida;
      xSemaphoreGive(xMutexDatos);
    }

    Serial.println("--- Lectura sensores ---");
    Serial.printf("  Temperatura:      %.1f C\n",             temperatura);
    Serial.printf("  Humedad ambiente: %.1f %%\n",            humedadAmbiente);
    Serial.printf("  Suelo zona 1:     %d %%  Valvula: %s\n", humedadSueloZ1, valvulaZ1 ? "ABIERTA" : "CERRADA");
    Serial.printf("  Suelo zona 2:     %d %%  Valvula: %s\n", humedadSueloZ2, valvulaZ2 ? "ABIERTA" : "CERRADA");
    Serial.printf("  CO2:              %.1f ppm\n",           ppmCO2);
    Serial.printf("  CO:               %.1f ppm\n",           ppmCO);
    Serial.printf("  NH3:              %.1f ppm\n",           ppmNH3);
    Serial.printf("  Alcohol:          %.1f ppm\n",           ppmAlcohol);
    Serial.printf("  Humo:             %.1f ppm\n",           ppmHumo);
    Serial.printf("  Tolueno:          %.1f ppm\n",           ppmTolueno);
    Serial.printf("  Acetona:          %.1f ppm\n",           ppmAcetona);

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

    DatosSensores datos;
    if (xSemaphoreTake(xMutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {
      datos = datosSensores;
      xSemaphoreGive(xMutexDatos);
    } else {
      vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_MQTT_MS));
      continue;
    }

    if (!datos.valido) {
      mqttClient.publish(TOPIC_ALERTAS.c_str(), "ERROR: Fallo en lectura de sensores");
      vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(INTERVALO_MQTT_MS));
      continue;
    }

    String json = construirJSON(datos);
    mqttClient.publish(TOPIC_ESTADO.c_str(), json.c_str());

    String alerta = "";
    alerta += evaluarTemperatura(datos.temperatura);
    alerta += evaluarHumedadAmbiente(datos.humedadAmbiente);
    alerta += evaluarHumedadSuelo(datos.humedadSueloZ1, 1);
    alerta += evaluarHumedadSuelo(datos.humedadSueloZ2, 2);
    alerta += evaluarCalidadAire(datos.ppmCO2);

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

// Evaluaciones — DHT11
String evaluarTemperatura(float t) {
  if (isnan(t))         return "CRITICA|temperatura|Error de lectura DHT11\n";
  if (t > TEMP_CRITICA) return "CRITICA|temperatura|Mayor a 35C - activar extractor\n";
  if (t > TEMP_OPT_MAX) return "ADVERTENCIA|temperatura|Alta (28-35C) - revisar ventilacion\n";
  if (t < TEMP_OPT_MIN) return "AVISO|temperatura|Baja (<22C) - considerar calefaccion\n";
  return "";
}

String evaluarHumedadAmbiente(float h) {
  if (isnan(h))            return "CRITICA|hum_ambiente|Error de lectura DHT11\n";
  if (h > HUM_AMB_CRITICA) return "CRITICA|hum_ambiente|Mayor a 90% - riesgo de hongos, activar extractor\n";
  if (h > HUM_AMB_OPT_MAX) return "ADVERTENCIA|hum_ambiente|Alta (80-90%) - aumentar ventilacion\n";
  if (h < HUM_AMB_OPT_MIN) return "AVISO|hum_ambiente|Baja (<60%) - revisar nebulizacion\n";
  return "";
}

// Evaluaciones — suelo por zona
String evaluarHumedadSuelo(int s, int zona) {
  String z = "zona" + String(zona);
  if (s < SUELO_CRITICO)   return "CRITICA|hum_suelo_"     + z + "|Menor a 40% - riego urgente\n";
  if (s < SUELO_RIEGO_MIN) return "ADVERTENCIA|hum_suelo_" + z + "|Baja (40-60%) - valvula abierta\n";
  if (s > SUELO_RIEGO_MAX) return "ADVERTENCIA|hum_suelo_" + z + "|Exceso (>80%) - valvula cerrada\n";
  return "";
}

// Evaluaciones — MQ-135
String evaluarCalidadAire(float co2ppm) {
  if (co2ppm > CO2_CRITICO)     return "CRITICA|co2|Mayor a 5000 ppm - activar ventilacion urgente\n";
  if (co2ppm > CO2_ADVERTENCIA) return "ADVERTENCIA|co2|Alta (2000-5000 ppm) - aumentar ventilacion\n";
  if (co2ppm > CO2_OPTIMO)      return "AVISO|co2|Sobre nivel optimo (>1200 ppm)\n";
  return "";
}

// JSON de estado completo
String construirJSON(const DatosSensores &d) {
  String estadoTemp    = (d.temperatura > TEMP_CRITICA)  ? "critica"     :
                         (d.temperatura > TEMP_OPT_MAX)  ? "advertencia" :
                         (d.temperatura < TEMP_OPT_MIN)  ? "baja"        : "optima";

  String estadoHumAmb  = (d.humedadAmbiente > HUM_AMB_CRITICA) ? "critica"     :
                         (d.humedadAmbiente > HUM_AMB_OPT_MAX) ? "advertencia" :
                         (d.humedadAmbiente < HUM_AMB_OPT_MIN) ? "baja"        : "optima";

  String estadoSueloZ1 = (d.humedadSueloZ1 < SUELO_CRITICO)  ? "critica" :
                         (d.humedadSueloZ1 < SUELO_RIEGO_MIN) ? "riego"   :
                         (d.humedadSueloZ1 > SUELO_RIEGO_MAX) ? "exceso"  : "optima";

  String estadoSueloZ2 = (d.humedadSueloZ2 < SUELO_CRITICO)  ? "critica" :
                         (d.humedadSueloZ2 < SUELO_RIEGO_MIN) ? "riego"   :
                         (d.humedadSueloZ2 > SUELO_RIEGO_MAX) ? "exceso"  : "optima";

  String estadoCO2     = (d.ppmCO2 > CO2_CRITICO)    ? "critica"     :
                         (d.ppmCO2 > CO2_ADVERTENCIA) ? "advertencia" :
                         (d.ppmCO2 > CO2_OPTIMO)      ? "aviso"       : "optima";

  char json[1024];
  snprintf(json, sizeof(json),
    "{"
      "\"temperatura\":%.1f,"
      "\"temp_estado\":\"%s\","
      "\"hum_ambiente\":%.1f,"
      "\"hum_amb_estado\":\"%s\","
      "\"zona1\":{"
        "\"hum_suelo\":%d,"
        "\"suelo_estado\":\"%s\","
        "\"valvula\":\"%s\""
      "},"
      "\"zona2\":{"
        "\"hum_suelo\":%d,"
        "\"suelo_estado\":\"%s\","
        "\"valvula\":\"%s\""
      "},"
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
    d.humedadSueloZ1,  estadoSueloZ1.c_str(), d.valvulaZ1 ? "ABIERTA" : "CERRADA",
    d.humedadSueloZ2,  estadoSueloZ2.c_str(), d.valvulaZ2 ? "ABIERTA" : "CERRADA",
    d.ppmCO2,          estadoCO2.c_str(),
    d.ppmCO,
    d.ppmNH3,
    d.ppmAlcohol,
    d.ppmHumo,
    d.ppmTolueno,
    d.ppmAcetona
  );

  return String(json);
}