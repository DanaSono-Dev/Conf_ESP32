# Conf_ESP32

Configuraciones de firmware para ESP32 orientadas al monitoreo de invernadero de jitomate. El sistema captura imágenes, lee sensores ambientales y de suelo, y publica todo vía MQTT hacia un broker central.

---

## Estructura del repositorio

```
Conf_ESP32/
├── ESP32_CAM/                   # Captura y transmisión de imágenes por MQTT
│   └── ESP32_CAM.ino
├── ESP32_sensors/               # Monitor ambiental con FreeRTOS y control de riego
│   └── ESP32_sensors.ino
└── soil_sensor_calibration/     # Lectura raw para calibrar sensores capacitivos de suelo
    └── soil_sensor_calibration.ino
```

---

## Dependencias

Instalar desde el Library Manager de Arduino IDE o como ZIP desde los enlaces:

| Librería | Uso | Repositorio |
|---|---|---|
| Adafruit Unified Sensor | Capa base para sensores Adafruit | [adafruit/Adafruit_Sensor](https://github.com/adafruit/Adafruit_Sensor) |
| DHT sensor library | Lectura del DHT11 (temperatura y humedad) | [adafruit/DHT-sensor-library](https://github.com/adafruit/DHT-sensor-library) |
| MQSensorsLib | Lectura y calibración del MQ-135 | [miguel5612/MQSensorsLib](https://github.com/miguel5612/MQSensorsLib) |
| PubSubClient | Cliente MQTT | [knolleary/pubsubclient](https://github.com/knolleary/pubsubclient) |

---

## ESP32-CAM

**Hardware:** AI-Thinker ESP32-CAM

Captura una foto JPEG en formato SVGA cada minuto y la transmite al broker MQTT dividida en chunks de 4 096 bytes. Permite reconstruir la imagen en el servidor a partir de tres tópicos secuenciales.

### Tópicos MQTT

| Tópico | Contenido |
|---|---|
| `srai/camara/inicio` | JSON con timestamp, total de chunks y tamaño total en bytes |
| `srai/camara/chunk` | Chunk binario: 2 bytes de índice + datos de imagen |
| `srai/camara/fin` | Timestamp que indica el fin de la transmisión |

### Parámetros configurables

| Parámetro | Descripción |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Red WiFi |
| `MQTT_BROKER` | IP del broker |
| `MQTT_PORT` | Puerto MQTT |
| `MQTT_CLIENT` | ID de cliente |
| `INTERVALO_MS` | Frecuencia de captura |
| `CHUNK_SIZE` | Tamaño de cada chunk |

### Pinout AI-Thinker ESP32-CAM

| Señal | GPIO |
|---|---|
| PWDN | 32 |
| XCLK | 0 |
| SIOD / SIOC | 26 / 27 |
| Y9–Y2 | 35, 34, 39, 36, 21, 19, 18, 5 |
| VSYNC / HREF / PCLK | 25, 23, 22 |

---

## ESP32_sensors

**Hardware:** ESP32 (doble núcleo)

Monitor de invernadero con FreeRTOS. Ejecuta dos tareas paralelas: una de lectura de sensores y control de válvulas (núcleo 0) y otra de publicación MQTT (núcleo 1). Incluye sistema de alertas con tres niveles de severidad.

### Sensores y actuadores

| Elemento | Pin | Descripción |
|---|---|---|
| DHT11/DTH22 | GPIO 4 | Temperatura y humedad ambiente |
| Sensor suelo zona 1 | GPIO 34 | Humedad capacitiva (ADC) |
| Sensor suelo zona 2 | GPIO 35 | Humedad capacitiva (ADC) |
| MQ-135 | GPIO 32 | Calidad de aire (CO₂, CO, NH₃, etc.) |
| Válvula zona 1 | GPIO 25 | Relé de riego |
| Válvula zona 2 | GPIO 26 | Relé de riego |

### Tópicos MQTT

Base: `invernadero/jitomate/ESP32_zona_1`

| Tópico | Contenido |
|---|---|
| `.../estado` | JSON completo con todas las lecturas y estados |
| `.../alertas` | Alertas de texto con formato `NIVEL\|variable\|descripción` |

### Umbrales de cultivo

**Temperatura (DHT11/DTH22)**

| Condición | Rango | Acción sugerida |
|---|---|---|
| Óptima | 22 – 28 °C | — |
| Advertencia | 28 – 35 °C | Revisar ventilación |
| Crítica | > 35 °C | Activar extractor |

**Humedad ambiente (DHT11/DTH22)**

| Condición | Rango | Acción sugerida |
|---|---|---|
| Óptima | 60 – 80 % | — |
| Advertencia | 80 – 90 % | Aumentar ventilación |
| Crítica | > 90 % | Riesgo de hongos, activar extractor |

**Humedad de suelo (por zona)**

| Condición | Rango | Acción |
|---|---|---|
| Exceso | > 80 % | Válvula cerrada |
| Óptima | 60 – 80 % | — |
| Riego | 40 – 60 % | Válvula abierta |
| Crítica | < 40 % | Alerta urgente |

> Los valores raw de calibración del sensor capacitivo son `2389` (seco) y `352` (húmedo). Ajustar con el sketch `soil_sensor_calibration`.

**Calidad de aire — CO₂ (MQ-135)**

| Condición | Rango |
|---|---|
| Óptima | < 1 200 ppm |
| Aviso | 1 200 – 2 000 ppm |
| Advertencia | 2 000 – 5 000 ppm |
| Crítica | > 5 000 ppm |

El MQ-135 también reporta CO, NH₃, alcohol, humo, tolueno y acetona en el JSON de estado.

### Arquitectura FreeRTOS

```
Núcleo 0 — TareaSensores  (prioridad 2)
  └── Lee DHT11/DTH22, sensores de suelo y MQ-135 cada 60 s
  └── Controla válvulas automáticamente según umbrales
  └── Escribe datos en estructura compartida (mutex)

Núcleo 1 — TareaMQTT  (prioridad 1)
  └── Mantiene conexión con el broker
  └── Lee la estructura compartida y publica estado cada 60 s
  └── Evalúa condiciones y publica alertas si aplica
```

---

## soil_sensor_calibration

Sketch de calibración para los sensores capacitivos de humedad de suelo. Lee el valor ADC raw con media recortada (descarta el 25 % inferior y superior de 16 muestras) e imprime el resultado por Serial cada segundo.

**Uso:**

1. Conectar el sensor al pin `GPIO 34`.
2. Cargar el sketch y abrir el Monitor Serial a 115 200 baud.
3. Registrar el valor con el sensor **completamente seco** → `SUELO_VALOR_SECO`.
4. Sumergir el sensor en agua y registrar el valor → `SUELO_VALOR_HUMEDO`.
5. Actualizar ambas constantes en `ESP32_sensors.ino`.

---

## Configuración de red

Todos los sketches apuntan a la misma red y broker. Actualizar estas constantes antes de flashear:

```cpp
// WiFi
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// Broker MQTT
const char* MQTT_BROKER   = "";
const int   MQTT_PORT     = 1883;
```

---

## Requisitos de Arduino IDE

- Board: **ESP32** (Espressif Systems) — versión 2.x o superior
- Para ESP32-CAM: seleccionar la placa **AI Thinker ESP32-CAM**
- Para ESP32_sensors: cualquier placa ESP32 con ADC en GPIO 32, 34 y 35
