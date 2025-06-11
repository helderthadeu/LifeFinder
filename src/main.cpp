// #include <Arduino.h>
// #include <Adafruit_MLX90614.h>
// #include <Wire.h>
// #include <ESP32Servo.h>

// #define RXD2 16
// #define TXD2 17

// #define servoPin 15

// struct Presence_sensor
// {
//   float distance;
//   bool presence;
// };

// QueueHandle_t presence_queue;
// QueueHandle_t temperature_queue;

// SemaphoreHandle_t start_close_search;
// SemaphoreHandle_t activate_secondary_sensor;

// SemaphoreHandle_t serial_mutex = xSemaphoreCreateMutex();

// void process_data(void *parameters)
// {
//   unsigned long last_time_checked = 0;
//   bool start_search = false;
//   float temperature = 0.0;
//   Presence_sensor sensor_data;
//   for (;;)
//   {
//     xQueueReceive(presence_queue, &sensor_data, portMAX_DELAY);
//     if (!start_search)
//     {
//       start_search = (sensor_data.presence && sensor_data.distance <= 3.0);
//     }
//     if (start_search)
//     {
//       xSemaphoreGive(activate_secondary_sensor);

//       xQueueReceive(temperature_queue, &temperature, portMAX_DELAY);

//       if (temperature >= 33)
//       {
//         xSemaphoreTake(serial_mutex, portMAX_DELAY);
//         Serial.println("Probably there is a person nearby.");
//         xSemaphoreGive(serial_mutex);
//       }
//       if (millis() - last_time_checked >= 10000)
//       {
//         xSemaphoreTake(serial_mutex, portMAX_DELAY);
//         Serial.print("Presence detected: ");
//         Serial.print(sensor_data.presence);
//         Serial.print(", Distance: ");
//         Serial.print(sensor_data.distance);
//         Serial.println(" m");
//         last_time_checked = millis();
//         start_search = false; // Reset the search flag after processing
//         xSemaphoreGive(serial_mutex);
//       }
//     }
//     else
//     {
//       last_time_checked = millis();
//     }
//   }
// }

// void scan_presence(void *parameters)
// {
//   Presence_sensor sensor_data;
//   for (;;)
//   {
//     if (Serial2.available())
//     {
//       uint8_t header = Serial2.read();

//       // Verifica se é o início de um pacote (padrão: 0xAA 0x00 ...)
//       if (header == 0xAA && Serial2.peek() == 0x00)
//       {
//         vTaskDelay(100 / portTICK_PERIOD_MS);
//         uint8_t packet[20];
//         packet[0] = header;
//         for (int i = 1; i < 20 && Serial2.available(); i++)
//         {
//           packet[i] = Serial2.read();
//         }

//         // Presença detectada?
//         bool presence = packet[8] > 0;
//         uint16_t distance = packet[9]; // distância em decímetros (~10 cm)

//         float distance_m = distance / 10.0;

//         sensor_data.presence = presence;
//         sensor_data.distance = distance_m;
//         xQueueSend(presence_queue, &sensor_data, 0);
//       }
//     }
//     vTaskDelay(1000 / portTICK_PERIOD_MS); // Espera 1 segundo antes de verificar novamente
//   }
// }

// void scan_temperature(void *mlx_parameter)
// {
//   Adafruit_MLX90614 *mlx = (Adafruit_MLX90614 *)mlx_parameter;
//   for (;;)
//   {

//     if (xSemaphoreTake(activate_secondary_sensor, portMAX_DELAY) == pdTRUE)
//     {
//       float temperature = mlx->readObjectTempC();
//       xQueueSend(temperature_queue, &temperature, 0);
//     }
//     vTaskDelay(1000 / portTICK_PERIOD_MS);
//   }
// }

// void servo_controller_task(void *controller)
// {
//   Servo *servo = (Servo *)controller;

//   servo->write(0);
//   vTaskDelay(1000 / portTICK_PERIOD_MS);

//   for (int posDegrees = 0; posDegrees <= 180; posDegrees++)
//   {
//     servo->write(posDegrees);
//     vTaskDelay(50 / portTICK_PERIOD_MS);
//   }
// }

// void setup()
// {
//   Serial.begin(9600);
//   Serial2.begin(256000, SERIAL_8N1, RXD2, TXD2);

//   start_close_search = xSemaphoreCreateBinary();
//   activate_secondary_sensor = xSemaphoreCreateBinary();

//   Adafruit_MLX90614 mlx = Adafruit_MLX90614();
//   // Wire.begin(21, 22);
//   while (!mlx.begin())
//   {
//     Serial.println("Error connecting to MLX sensor. Check wiring.");
//     delay(1000);
//     // while (1)
//     //   ;
//   };

//   Servo servo_controller;
//   servo_controller.write(0);

//   servo_controller.attach(servoPin);

//   xTaskCreate(scan_presence, "ScanPresence", 2048, NULL, 2, NULL);
//   xTaskCreate(scan_temperature, "ScanTemperature", 2048, (void *)&mlx, 1, NULL);
//   xTaskCreate(process_data, "ProcessData", 2048, NULL, 1, NULL);
// }

// void loop()
// {
// }