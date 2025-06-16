// Base library:
#include <Arduino.h>
#include <vector>

// Sensors libraries
#include "DHT.h"
#include <queue>
#include <Servo.h>
#include <Ultrasonic.h>

// MQTT libraries
#include <WiFi.h>
#include <PubSubClient.h>

// Pins definition
#define trigPin 11
#define echoPin 10

#define OT1_PIN 32
#define OT2_PIN 33

#define DHTPIN 13

#define SERVO_PIN 12

// MQTT configuration

#define ID_MQTT "SessaoMorta"
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

SemaphoreHandle_t start_scan;
SemaphoreHandle_t start_distance_measurement;
SemaphoreHandle_t send_mqtt;

SemaphoreHandle_t serial_mutex = xSemaphoreCreateMutex();

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
    int total_humidity = 0;
    queue<int> humidity_values;

    for (;;)
    {
        if (xSemaphoreTake(start_scan, pdMS_TO_TICKS(100)))
        {
            if (xSemaphoreTake(start_distance_measurement, pdMS_TO_TICKS(100)))
            {
                xSemaphoreGive(start_distance_measurement);
            }
            xSemaphoreGive(start_scan);
        }
        xQueueReceive(presence_queue, &sensor_data, portMAX_DELAY);
        xQueueReceive(humidity_queue, &humidity, portMAX_DELAY);

        humidity_values.push(humidity);
        total_humidity += humidity;

        if (humidity_values.size() == 10)
        {
            if (humidity * 1.1 > (total_humidity / 10))
            {
                xSemaphoreTake(serial_mutex, portMAX_DELAY);
                Serial.println("There is a unexpected humidity change!");
                xSemaphoreGive(serial_mutex);

                xSemaphoreGive(start_scan);
                xSemaphoreGive(start_distance_measurement);
                xSemaphoreTake(start_scan, portMAX_DELAY);
                xSemaphoreTake(start_distance_measurement, portMAX_DELAY);
            }

            total_humidity -= humidity_values.front();
            humidity_values.pop();
        }
    }
}

/**
 * @brief Measures distance using an ultrasonic sensor and sends the value to a queue.
 * @param ultrassonic_parameters Pointer to the Ultrasonic sensor object.
 * @return None
 */
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

                float distance_m = distance / 10.0;

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

/**
 * @brief Controls a servo motor, sending its angle to a queue.
 * @param controller Pointer to the Servo object.
 * @return None
 */
void servo_controller_task(void *controller)
{
    Servo *servo = (Servo *)controller;

    xSemaphoreTake(start_scan, portMAX_DELAY);

    for (int posDegrees = 0; posDegrees <= 180; posDegrees++)
    {
        xQueueSend(servo_angle_queue, &posDegrees, 0);
        servo->write(posDegrees);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    xSemaphoreGive(start_scan);
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

        int angle = 0;
        float distance = 0;

        xSemaphoreTake(send_mqtt, portMAX_DELAY);
        xSemaphoreGive(send_mqtt);

        MQTT.publish(TOPIC_PUBLISH_DISTANCE, "start:");
        while (xQueueReceive(distance_queue, &distance, portMAX_DELAY) == pdTRUE)
        {
            char msg[50];
            snprintf(msg, 50, "%.2f", distance);
            MQTT.publish(TOPIC_PUBLISH_DISTANCE, msg);
        }
        MQTT.publish(TOPIC_PUBLISH_DISTANCE, "end");
        MQTT.publish(TOPIC_PUBLISH_ANGLE, "start");
        while (xQueueReceive(servo_angle_queue, &angle, portMAX_DELAY) == pdTRUE)
        {
            char msg[50];
            snprintf(msg, 50, "%d", angle);
            MQTT.publish(TOPIC_PUBLISH_ANGLE, msg);
        }
        MQTT.publish(TOPIC_PUBLISH_ANGLE, "end");

        vTaskDelay(100 / portTICK_PERIOD_MS);
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
 * @brief Initializes WiFi connection parameters and connects to the network.
 * @return None
 */
void initWiFi(void)
{
    delay(10);
    Serial.println("------Wi-Fi Connection------");
    Serial.print("Connecting to network: ");
    Serial.println(SSID);
    Serial.println("Please wait");

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
        Serial.print("* Trying to connect to MQTT Broker: ");
        Serial.println(BROKER_MQTT);
        if (MQTT.connect(ID_MQTT))
        {
            Serial.println("Successfully connected to MQTT broker!");
            MQTT.subscribe(TOPIC_PUBLISH_DISTANCE);
            MQTT.subscribe(TOPIC_PUBLISH_ANGLE);
        }
        else
        {
            Serial.print("Failed to reconnect to broker. ERROR: ");
            Serial.println(MQTT.state());
            Serial.println("There will be a new connection attempt in 2s");
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
        Serial.print(".");
    }

    Serial.println();
    Serial.print("Successfully connected to network ");
    Serial.print(SSID);
    Serial.println("\nIP obtained: ");
    Serial.println(WiFi.localIP());
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
    initWiFi();

    /* Initializes the connection to the MQTT broker */
    initMQTT();

    Serial.begin(115200);

    pinMode(OT1_PIN, INPUT_PULLDOWN);
    pinMode(OT2_PIN, INPUT_PULLDOWN);

    DHT dht(DHTPIN, DHT11);
    Ultrasonic ultrasonic(trigPin, echoPin);
    Servo servo;
    servo.attach(SERVO_PIN);
    servo.write(0);

    start_scan = xSemaphoreCreateBinary();
    start_distance_measurement = xSemaphoreCreateBinary();
    send_mqtt = xSemaphoreCreateBinary();

    xSemaphoreGive(start_scan);
    xSemaphoreGive(start_distance_measurement);
    xSemaphoreGive(send_mqtt);

    humidity_queue = xQueueCreate(10, sizeof(float));
    presence_queue = xQueueCreate(10, sizeof(Presence_sensor));
    distance_queue = xQueueCreate(10, sizeof(float));
    servo_angle_queue = xQueueCreate(10, sizeof(int));

    xTaskCreate(scan_humidity, "ScanHumidity", 2048, (void *)&dht, 1, NULL);
    xTaskCreate(process_data, "ProcessData", 2048, NULL, 2, NULL);
    xTaskCreate(measure_distance, "MeasureDistance", 2048, (void *)&ultrasonic, 3, NULL);
    xTaskCreate(scan_presence, "ScanPresence", 2048, NULL, 1, NULL);
    xTaskCreate(servo_controller_task, "ServoController", 2048, (void *)&servo, 3, NULL);
    xTaskCreate(mqtt_task, "MQTTLoop", 2048, NULL, 6, NULL);
}

/**
 * @brief Arduino main loop function. Not used in this application.
 * @return None
 */
void loop()
{
}