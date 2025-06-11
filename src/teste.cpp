// #include <Arduino.h>
// #include <Wire.h>

// void setup() {
//   Wire.begin();
//   Serial.begin(9600);
//   while (!Serial);

//   Serial.println("Scanning I2C bus...");
//   byte error, address;
//   int nDevices = 0;

//   for (address = 1; address < 127; address++) {
//     Wire.beginTransmission(address);
//     error = Wire.endTransmission();

//     if (error == 0) {
//       Serial.print("I2C device found at address 0x");
//       Serial.println(address, HEX);
//       nDevices++;
//     }
//   }

//   if (nDevices == 0)
//     Serial.println("No I2C devices found");
//   else
//     Serial.println("Done.");
// }

// void loop() {}
#include <Arduino.h>

#define OT1_PIN 32  // GPIO conectado ao OT1 (movimento)
#define OT2_PIN 33  // GPIO conectado ao OT2 (presença parada)

unsigned long ot1Start = 0;
unsigned long ot2Start = 0;
bool ot1Detecting = false;
bool ot2Detecting = false;

void setup() {
  Serial.begin(115200);
  pinMode(OT1_PIN, INPUT);
  pinMode(OT2_PIN, INPUT);
  Serial.println("Iniciando monitoramento do HLK-LD2420...");
}

void loop() {
  bool movimento = digitalRead(OT1_PIN);
  bool presenca = digitalRead(OT2_PIN);
  unsigned long agora = millis();

  // OT1 - Movimento
  if (movimento && !ot1Detecting) {
    ot1Start = agora;
    ot1Detecting = true;
    Serial.println("Movimento detectado (OT1) - iniciando contagem...");
  } else if (!movimento && ot1Detecting) {
    unsigned long duracao = agora - ot1Start;
    ot1Detecting = false;
    Serial.print("Movimento terminou. Duração: ");
    Serial.print(duracao);
    Serial.println(" ms");
  }

  // OT2 - Presença estática
  if (presenca && !ot2Detecting) {
    ot2Start = agora;
    ot2Detecting = true;
    Serial.println("Presença estática detectada (OT2) - iniciando contagem...");
  } else if (!presenca && ot2Detecting) {
    unsigned long duracao = agora - ot2Start;
    ot2Detecting = false;
    Serial.print("Presença estática terminou. Duração: ");
    Serial.print(duracao);
    Serial.println(" ms");
  }

  delay(50);
}
