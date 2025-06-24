// Base library:
#include <Arduino.h>
#include <vector>

// Sensors libraries
#include "DHT.h"
#include <queue>
#include <ESP32Servo.h>
#include <Ultrasonic.h>

// MQTT libraries
#include <WiFi.h>
#include <PubSubClient.h>

// Pins definition
#define trigPin 13
#define echoPin 12
Ultrasonic ultrasonic(trigPin, echoPin, 20000UL);

#define DHTPIN 14
DHT dht(DHTPIN, DHT11);

#define SERVO_PIN 27

Servo servo;
// #define OT1_PIN 32
// #define OT2_PIN 33

// MQTT configuration

#define ID_MQTT "Lifefinder"
#define TOPIC_PUBLISH_DISTANCE "topic_distance"
#define TOPIC_PUBLISH_ANGLE "topic_angle"

const char *SSID = "CAM2-4";       // SSID / name of the Wi-Fi network to connect to
const char *PASSWORD = "33754367"; // Password of the Wi-Fi network to connect to

const char *BROKER_MQTT = "test.mosquitto.org";
int BROKER_PORT = 1883; // MQTT Broker port

WiFiClient espClient;
PubSubClient MQTT(espClient);

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

SemaphoreHandle_t start_scan = xSemaphoreCreateBinary();
SemaphoreHandle_t start_distance_measurement = xSemaphoreCreateBinary();
SemaphoreHandle_t send_mqtt = xSemaphoreCreateBinary();
SemaphoreHandle_t mqtt_connecting = xSemaphoreCreateBinary();

SemaphoreHandle_t done_distance = xSemaphoreCreateBinary();
SemaphoreHandle_t done_servo = xSemaphoreCreateBinary();
SemaphoreHandle_t done_MQTT = xSemaphoreCreateBinary();

// xSemaphoreGive(send_mqtt); // Inicializa liberado

SemaphoreHandle_t serial_mutex = xSemaphoreCreateMutex();

// Function declarations
void process_data(void *parameters);
void measure_distance(void *ultrassonic_parameters);
void scan_presence(void *parameters);
void scan_humidity(void *dht_parameters);
void servo_controller_task(void *controller);
void VerificaConexoesWiFIEMQTT(void);
void mqtt_task(void *parameters);
void initWiFi(void);
void initMQTT(void);
void reconnectMQTT(void);
void reconnectWiFi(void);
void mqtt_callback(char *topic, byte *payload, unsigned int length);

/**
 * @brief Processes sensor data, checks for unexpected humidity changes, and manages queues.
 * @param parameters Pointer to task parameters (unused).
 * @return None
 */
void process_data(void *parameters)
{
    unsigned long last_time_checked = 0;

    float humidity = 0.0;
    Presence_sensor sensor_data;
    float total_humidity = 0;
    const float MAX_HUMIDITY_VALUES = 50;
    queue<float> humidity_values;
    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    Serial.println("------Process Data Task Started------");
    xSemaphoreGive(serial_mutex);
    for (;;)
    {

        if (xSemaphoreTake(start_distance_measurement, pdMS_TO_TICKS(100)))
        {
            if (xSemaphoreTake(start_scan, pdMS_TO_TICKS(100)))
            {
            }
        }

        xSemaphoreTake(mqtt_connecting, portMAX_DELAY);

        xQueueReceive(humidity_queue, &humidity, portMAX_DELAY);

        humidity_values.push(humidity);
        total_humidity = total_humidity + humidity;
        if (humidity_values.size() == MAX_HUMIDITY_VALUES)
        {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.printf("Total Humidity %.2f - Current Humidity: %.2f\n", (total_humidity / MAX_HUMIDITY_VALUES), humidity);
            xSemaphoreGive(serial_mutex);
            if (humidity > (total_humidity / MAX_HUMIDITY_VALUES) * 1.06)
            {
                total_humidity = 0;
                while (!humidity_values.empty())
                {
                    humidity_values.pop();
                }
                xSemaphoreTake(serial_mutex, portMAX_DELAY);
                Serial.println("There is a unexpected humidity change!");
                xSemaphoreGive(serial_mutex);

                xSemaphoreGive(start_scan);
                xSemaphoreGive(start_distance_measurement);

                vTaskDelay(9000 / portTICK_PERIOD_MS);
                xSemaphoreGive(send_mqtt);
                xSemaphoreTake(done_distance, portMAX_DELAY);
                xSemaphoreTake(done_servo, portMAX_DELAY);

                xSemaphoreTake(start_scan, 0);
                xSemaphoreTake(start_distance_measurement, 0);
                xSemaphoreTake(send_mqtt, 0);

                total_humidity = 0;
                // while (!humidity_values.empty())
                // {
                //     humidity_values.pop();
                // }
                // vTaskDelay(500 / portTICK_PERIOD_MS);
            }

            total_humidity -= humidity_values.front();
            humidity_values.pop();
        }
        vTaskDelay(50 / portTICK_PERIOD_MS); // Waits 1 second before checking again
        xSemaphoreGive(mqtt_connecting);
    }
}

/**
 * @brief Measures distance using an ultrasonic sensor and sends the value to a queue.
 * @param ultrassonic_parameters Pointer to the Ultrasonic sensor object.
 * @return None
 */
void measure_distance(void *ultrassonic_parameters)
{
    // Ultrasonic *ultrasonic = (Ultrasonic *)ultrassonic_parameters;
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    for (;;)
    {
        xSemaphoreTake(start_distance_measurement, portMAX_DELAY);

        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("------ Measure Distance ------");
        xSemaphoreGive(serial_mutex);
        for (int i = 0; i < 180; i++)
        {
            float distance = ultrasonic.read();
            xQueueSend(distance_queue, &distance, 0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("------  Distance measured ------");
        xSemaphoreGive(serial_mutex);

        // xSemaphoreGive(start_distance_measurement);
        xSemaphoreGive(done_distance);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Controls a servo motor, sending its angle to a queue.
 * @param controller Pointer to the Servo object.
 * @return None
 */
void servo_controller_task(void *controller)
{
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    for (;;)
    {
        xSemaphoreTake(start_scan, portMAX_DELAY);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("------Servo Control------");
        xSemaphoreGive(serial_mutex);
        for (int posDegrees = 0; posDegrees <= 180; posDegrees++)
        {
            // xSemaphoreTake(serial_mutex, portMAX_DELAY);
            // Serial.printf("Degrees: %d\n", posDegrees);
            // xSemaphoreGive(serial_mutex);
            xQueueSend(servo_angle_queue, &posDegrees, 0);
            servo.write(posDegrees);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        // xSemaphoreGive(start_scan);
        xSemaphoreGive(done_servo);

        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Scans for presence using serial data and sends results to a queue.
 * @param parameters Pointer to task parameters (unused).
 * @return None
 */
void scan_presence(void *parameters)
{
    Presence_sensor sensor_data;
    for (;;)
    {
        if (Serial2.available())
        {
            uint8_t header = Serial2.read();

            // Checks if it is the start of a packet (pattern: 0xAA 0x00 ...)
            if (header == 0xAA && Serial2.peek() == 0x00)
            {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                uint8_t packet[20];
                packet[0] = header;
                for (int i = 1; i < 20 && Serial2.available(); i++)
                {
                    packet[i] = Serial2.read();
                }

                // Presence detected?
                bool presence = packet[8] > 0;
                uint16_t distance = packet[9]; // distance in decimeters (~10 cm)

                float distance_m = distance / 20.0;

                sensor_data.presence = presence;
                sensor_data.distance = distance_m;
                xQueueSend(presence_queue, &sensor_data, 0);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Waits 1 second before checking again
    }
}

/**
 * @brief Scans humidity using a DHT sensor and sends the value to a queue.
 * @param dht_parameters Pointer to the DHT sensor object.
 * @return None
 */
void scan_humidity(void *dht_parameters)
{
    // DHT *dht = (DHT *)dht_parameters;
    float humidity = 0;
    for (;;)
    {
        humidity = dht.readHumidity();
        // xSemaphoreTake(serial_mutex, portMAX_DELAY);
        // Serial.print("Humidity: ");
        // Serial.println(humidity);
        // xSemaphoreGive(serial_mutex);
        if (isnan(humidity))
        {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.println("Failed to read humidity from DHT sensor!");
            xSemaphoreGive(serial_mutex);
        }
        else
        {
            xQueueSend(humidity_queue, &humidity, 0);
            // xSemaphoreTake(serial_mutex, portMAX_DELAY);
            // Serial.print("Humidity read successfully:");
            // Serial.println(humidity);
            // xSemaphoreGive(serial_mutex);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS); // Waits 2 seconds before checking again
    }
}

/**
 * @brief Checks the connections to WiFi and MQTT broker, reconnecting if necessary.
 * @return None
 */
void VerificaConexoesWiFIEMQTT(void)
{
    if (!MQTT.connected())
        reconnectMQTT(); // if not connected to the Broker, reconnect

    reconnectWiFi(); // if not connected to WiFi, reconnect
}

/**
 * @brief Handles MQTT communication, publishing sensor data and angles.
 * @param parameters Pointer to task parameters (unused).
 * @return None
 */
void mqtt_task(void *parameters)
{
    

    for (;;)
    {
        VerificaConexoesWiFIEMQTT();
        MQTT.loop();

        // Publish distance data (non-blocking)
        xSemaphoreTake(send_mqtt, portMAX_DELAY);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("------ Send to MQTT ------");
        xSemaphoreGive(serial_mutex);

        char json_payload[2048]; // Aumente o tamanho se necessário
        strcpy(json_payload, "{\"readings\":[");

        float distance = 0;
        int angle = 0;
        bool first = true;
        for (int i = 0; i < 180 && xQueueReceive(distance_queue, &distance, 0) == pdTRUE && xQueueReceive(servo_angle_queue, &angle, 0) == pdTRUE; i++)
        {
            char reading[50];
            if (!first)
            {
                strcat(json_payload, ",");
            }
            snprintf(reading, 50, "{\"angle\":%d,\"dist\":%.2f}", angle, distance);
            strcat(json_payload, reading);
            first = false;
        }
        strcat(json_payload, "]}");

        // Publique UMA ÚNICA VEZ com todos os dados
        MQTT.publish(TOPIC_PUBLISH_DISTANCE, json_payload);

        // MQTT.publish(TOPIC_PUBLISH_ANGLE, "start");

        // for (int i = 0; i < 180 && xQueueReceive(servo_angle_queue, &angle, 0) == pdTRUE; i++) // Timeout 0 = non-blocking
        // {
        //     char msg[50];
        //     snprintf(msg, 50, "%d", angle);
        //     MQTT.publish(TOPIC_PUBLISH_ANGLE, msg);
        //     vTaskDelay(50 / portTICK_PERIOD_MS); // Small delay to prevent flooding
        // }
        // MQTT.publish(TOPIC_PUBLISH_ANGLE, "end");

        xSemaphoreGive(done_MQTT);

        vTaskDelay(100 / portTICK_PERIOD_MS); // Main task delay
    }
}

/**
 * @brief Initializes WiFi connection parameters and connects to the network.
 * @return None
 */
void initWiFi(void)
{

    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    Serial.println("------Wi-Fi Connection------");
    Serial.print("Connecting to network: ");
    Serial.println(SSID);
    Serial.println("Please wait");
    xSemaphoreGive(serial_mutex);

    reconnectWiFi();
}

/**
 * @brief Initializes MQTT connection parameters and sets the callback function.
 * @return None
 */
void initMQTT(void)
{
    MQTT.setServer(BROKER_MQTT, BROKER_PORT); // sets which broker and port to connect to
    MQTT.setCallback(mqtt_callback);          // sets callback function (called when any information from subscribed topics arrives)
    MQTT.setKeepAlive(60);
}

/**
 * @brief Reconnects to the MQTT broker if the connection is lost.
 * @return None
 */
void reconnectMQTT(void)
{
    while (!MQTT.connected())
    {
        xQueueReset(distance_queue);
        xQueueReset(servo_angle_queue);
        xSemaphoreTake(mqtt_connecting, portMAX_DELAY);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.print("* Trying to connect to MQTT Broker: ");
        xSemaphoreGive(serial_mutex);
        Serial.println(BROKER_MQTT);
        if (MQTT.connect(ID_MQTT))
        {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.println("Successfully connected to MQTT broker!");
            xSemaphoreGive(serial_mutex);
            xSemaphoreGive(mqtt_connecting);
            MQTT.subscribe(TOPIC_PUBLISH_DISTANCE);
            MQTT.subscribe(TOPIC_PUBLISH_ANGLE);
        }
        else
        {
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.print("Failed to reconnect to broker. ERROR: ");
            Serial.println(MQTT.state());
            Serial.println("There will be a new connection attempt in 2s");
            xSemaphoreGive(serial_mutex);
            delay(2000);
        }
    }
}

/**
 * @brief Reconnects to the WiFi network if not already connected.
 * @return None
 */
void reconnectWiFi(void)
{
    // if already connected to the Wi-Fi network, do nothing.
    // Otherwise, attempts to connect
    if (WiFi.status() == WL_CONNECTED)
        return;

    WiFi.begin(SSID, PASSWORD); // Connects to the Wi-Fi network

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(100);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.print(".");
        xSemaphoreGive(serial_mutex);
    }
    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    Serial.println();
    Serial.print("Successfully connected to network ");
    Serial.print(SSID);
    Serial.println("\nIP obtained: ");
    Serial.println(WiFi.localIP());
    xSemaphoreGive(serial_mutex);
}

/**
 * @brief Callback function for MQTT messages.
 * @param topic The topic of the received message.
 * @param payload The payload data.
 * @param length The length of the payload.
 * @return None
 */
void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
    String msg;

    /* gets the string from the received payload */
    for (int i = 0; i < length; i++)
    {
        char c = (char)payload[i];
        msg += c;
    }
}

/**
 * @brief Arduino setup function. Initializes hardware, connections, and tasks.
 * @return None
 */
void setup()
{
    /* Initializes the semaphores */
    initWiFi();

    /* Initializes the connection to the MQTT broker */
    initMQTT();

    xSemaphoreGive(start_scan);                 // Inicializa liberado
    xSemaphoreGive(start_distance_measurement); // Inicializa liberado
    // xSemaphoreGive(send_mqtt);                  // Inicializa liberado
    xSemaphoreGive(serial_mutex);    // Inicializa liberado
    xSemaphoreGive(mqtt_connecting); // Inicializa liberado

    Serial.begin(115200);

    // pinMode(OT1_PIN, INPUT_PULLDOWN);
    // pinMode(OT2_PIN, INPUT_PULLDOWN);

    pinMode(DHTPIN, INPUT_PULLUP);
    dht.begin();

    servo.attach(SERVO_PIN);
    servo.write(0);

    humidity_queue = xQueueCreate(1, sizeof(float));
    presence_queue = xQueueCreate(10, sizeof(Presence_sensor));
    distance_queue = xQueueCreate(180, sizeof(float));
    servo_angle_queue = xQueueCreate(180, sizeof(int));

    if (presence_queue == NULL || humidity_queue == NULL)
    {
        Serial.println("ERRO: Falha ao criar filas!");
        while (1)
            ;
    }

    // Wait for the serial monitor to open
    xTaskCreate(mqtt_task, "MQTTLoop", 4096, NULL, 3, NULL);
    xTaskCreate(scan_humidity, "ScanHumidity", 4096, NULL, 1, NULL);
    xTaskCreate(process_data, "ProcessData", 4096, NULL, 4, NULL);
    xTaskCreate(measure_distance, "MeasureDistance", 4096, NULL, 2, NULL);
    xTaskCreate(servo_controller_task, "ServoController", 2048, NULL, 2, NULL);
    // xTaskCreate(scan_presence, "ScanPresence", 2048, NULL, 1, NULL);
}

/**
 * @brief Arduino main loop function. Not used in this application.
 * @return None
 */
void loop()
{
}