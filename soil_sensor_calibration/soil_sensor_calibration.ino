#define PIN_SUELO_Z1  34
#define ADC_MUESTRAS  16

int leerPromedio(int pin, int muestras) {
  analogRead(pin);
  delay(10);

  int valores[muestras];
  for (int i = 0; i < muestras; i++) {
    valores[i] = analogRead(pin);
    delayMicroseconds(500);
  }

  // Ordena y descarta el 25% inferior y superior
  for (int i = 0; i < muestras - 1; i++)
    for (int j = i + 1; j < muestras; j++)
      if (valores[i] > valores[j]) { int t = valores[i]; valores[i] = valores[j]; valores[j] = t; }

  int inicio = muestras / 4;
  int fin    = muestras - inicio;
  long suma  = 0;
  for (int i = inicio; i < fin; i++) suma += valores[i];
  return (int)(suma / (fin - inicio));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Calibracion sensor de suelo");
}

void loop() {
  int lectura = leerPromedio(PIN_SUELO_Z1, ADC_MUESTRAS);
  Serial.printf("Lectura raw: %d\n", lectura);
  delay(1000);
}