#include <Arduino.h>
#include "DHT.h"
#include <queue>
#include <Servo.h>
#include <Ultrasonic.h>

#define trigPin 11
#define echoPin 10

#define OT1_PIN 32
#define OT2_PIN 33

#define DHTPIN 13

#define SERVO_PIN 12

using namespace std;

struct Presence_sensor
{
    float distance;
    bool presence;
};

QueueHandle_t presence_queue;
QueueHandle_t humidity_queue;
QueueHandle_t distance_queue;
QueueHandle_t servo_angle_queue;

SemaphoreHandle_t start_scan;
SemaphoreHandle_t start_distance_measurement;

SemaphoreHandle_t serial_mutex = xSemaphoreCreateMutex();

void process_data(void *parameters)
{
    unsigned long last_time_checked = 0;

    float humidity = 0.0;
    Presence_sensor sensor_data;
    int count_average = 10;
    int total_humidity = 0;
    queue<int> humidity_values;

    for (;;)
    {
        xSemaphoreTake(start_scan, portMAX_DELAY);
        xSemaphoreGive(start_scan);
        xSemaphoreTake(start_distance_measurement, portMAX_DELAY);
        xSemaphoreGive(start_distance_measurement);
        xQueueReceive(presence_queue, &sensor_data, portMAX_DELAY);
        xQueueReceive(humidity_queue, &humidity, portMAX_DELAY);

        humidity_values.push(humidity);
        total_humidity += humidity;

        if (count_average > 1)
        {
            if (humidity * 1.1 > (total_humidity / 10) && humidity * 0.9 < (total_humidity / 10))
            {
                xSemaphoreTake(serial_mutex, portMAX_DELAY);
                Serial.println("There is a unexpected humidity change!");
                xSemaphoreGive(serial_mutex);
                
                xSemaphoreGive(start_scan);
                xSemaphoreTake(start_scan, portMAX_DELAY);
                xSemaphoreGive(start_distance_measurement);
                xSemaphoreTake(start_distance_measurement, portMAX_DELAY);
            }

            total_humidity -= humidity_values.front();
            humidity_values.pop();
        }
        else
        {
            count_average--;
        }
    }
}

void measure_distance(void *ultrassonic_parameters)
{
    Ultrasonic *ultrasonic = (Ultrasonic *)ultrassonic_parameters;
    for (;;)
    {
        xSemaphoreTake(start_distance_measurement, portMAX_DELAY);
        float distance = ultrasonic->read();
        xQueueSend(distance_queue, &distance, 0);
        
        vTaskDelay(50 / portTICK_PERIOD_MS);
        xSemaphoreGive(start_distance_measurement);
    }
}

void scan_presence(void *parameters)
{
    Presence_sensor sensor_data;
    for (;;)
    {
        if (Serial2.available())
        {
            uint8_t header = Serial2.read();

            // Verifica se é o início de um pacote (padrão: 0xAA 0x00 ...)
            if (header == 0xAA && Serial2.peek() == 0x00)
            {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                uint8_t packet[20];
                packet[0] = header;
                for (int i = 1; i < 20 && Serial2.available(); i++)
                {
                    packet[i] = Serial2.read();
                }

                // Presença detectada?
                bool presence = packet[8] > 0;
                uint16_t distance = packet[9]; // distância em decímetros (~10 cm)

                float distance_m = distance / 10.0;

                sensor_data.presence = presence;
                sensor_data.distance = distance_m;
                xQueueSend(presence_queue, &sensor_data, 0);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Espera 1 segundo antes de verificar novamente
    }
}

void scan_humidity(void *dht_parameters)
{
    DHT *dht = (DHT *)dht_parameters;
    float humidity = 0;
    for (;;)
    {
        humidity = dht->readHumidity();
        if (isnan(humidity))
        {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.println("Failed to read humidity from DHT sensor!");
            xSemaphoreGive(serial_mutex);
        }
        else
        {
            xQueueSend(humidity_queue, &humidity, 0);
        }
    }
}

void servo_controller_task(void *controller)
{
    Servo *servo = (Servo *)controller;

    xSemaphoreTake(start_scan, portMAX_DELAY);

    for (int posDegrees = 0; posDegrees <= 180; posDegrees++)
    {
        xQueueSend(servo_angle_queue, &posDegrees, 0);
        servo->write(posDegrees);
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
    xSemaphoreGive(start_scan);
}

void setup()
{
    Serial.begin(115200);

    pinMode(OT1_PIN, INPUT_PULLDOWN);
    pinMode(OT2_PIN, INPUT_PULLDOWN);

    DHT dht(DHTPIN, DHT11);
    Ultrasonic ultrasonic(trigPin, echoPin);
    Servo servo;
    servo.attach(SERVO_PIN);
    servo.write(0);

    start_scan = xSemaphoreCreateBinary();
    activate_secondary_sensor = xSemaphoreCreateBinary();

    xTaskCreate(measure_distance, "MeasureDistance", 2048, (void *)&ultrasonic, 1, NULL);
    xTaskCreate(scan_presence, "ScanPresence", 2048, NULL, 2, NULL);
    xTaskCreate(scan_humidity, "ScanHumidity", 2048, (void *)&dht, 1, NULL);
    xTaskCreate(process_data, "ProcessData", 2048, NULL, 1, NULL);
    xTaskCreate(servo_controller_task, "ServoController", 2048, (void *)&servo, 1, NULL);
}

void loop()
{
}