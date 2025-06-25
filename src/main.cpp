// Base library:
#include <Arduino.h>
#include <vector>
#include <queue>

// Sensors libraries
#include "DHT.h"
#include <ESP32Servo.h>
#include <Ultrasonic.h>

// MQTT libraries
#include <WiFi.h>
#include <PubSubClient.h>

// Pins definition
#define connected_led 32
#define send_led 33

#define trigPin 13
#define echoPin 12
Ultrasonic ultrasonic(trigPin, echoPin, 20000UL);

#define DHTPIN 14
DHT dht(DHTPIN, DHT11);

#define SERVO_PIN 27

Servo servo;

// MQTT configuration

#define ID_MQTT "Lifefinder"
#define TOPIC_PUBLISH_DISTANCE "topic_data"

const char *SSID = "CAM2-4";       // SSID / name of the Wi-Fi network to connect to
const char *PASSWORD = "33754367"; // Password of the Wi-Fi network to connect to
// const char *SSID = "Helderwifi";       // SSID / name of the Wi-Fi network to connect to
// const char *PASSWORD = "thadeu12"; // Password of the Wi-Fi network to connect to

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

// Que declarations
QueueHandle_t presence_queue;
QueueHandle_t humidity_queue;
QueueHandle_t distance_queue;
QueueHandle_t servo_angle_queue;


// Semaphore declarations
SemaphoreHandle_t start_humidity = xSemaphoreCreateBinary();
SemaphoreHandle_t start_smoke = xSemaphoreCreateBinary();

SemaphoreHandle_t start_scan = xSemaphoreCreateBinary();
SemaphoreHandle_t start_distance_measurement = xSemaphoreCreateBinary();
SemaphoreHandle_t send_mqtt = xSemaphoreCreateBinary();
SemaphoreHandle_t mqtt_connecting = xSemaphoreCreateBinary();

SemaphoreHandle_t done_distance = xSemaphoreCreateBinary();
SemaphoreHandle_t done_servo = xSemaphoreCreateBinary();
SemaphoreHandle_t done_MQTT = xSemaphoreCreateBinary();

SemaphoreHandle_t serial_mutex = xSemaphoreCreateMutex();

// Function declarations
void process_data(void *parameters);
void measure_distance(void *ultrassonic_parameters);
void scan_smoke(void *parameters);
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
    // xSemaphoreTake(serial_mutex, portMAX_DELAY);
    // Serial.println("------Process Data Task Started------");
    // xSemaphoreGive(serial_mutex);
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

                xSemaphoreTake(done_distance, portMAX_DELAY);
                xSemaphoreTake(done_servo, portMAX_DELAY);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                xSemaphoreGive(send_mqtt);
                xSemaphoreTake(done_MQTT, portMAX_DELAY);

                xSemaphoreTake(start_scan, 0);
                xSemaphoreTake(start_distance_measurement, 0);
                xSemaphoreTake(send_mqtt, 0);

                total_humidity = 0;
            }
            else
            {
                total_humidity -= humidity_values.front();
                humidity_values.pop();
            }
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
            vTaskDelay(100 / portTICK_PERIOD_MS);
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
            servo.write(posDegrees);
            xQueueSend(servo_angle_queue, &posDegrees, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        // xSemaphoreGive(start_scan);
        xSemaphoreGive(done_servo);

        vTaskDelay(3000 / portTICK_PERIOD_MS);
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
        xSemaphoreTake(start_humidity, portMAX_DELAY);
        xSemaphoreGive(start_humidity);

        humidity = dht.readHumidity();
        // xSemaphoreTake(serial_mutex, portMAX_DELAY);
        // Serial.print("Humidity: ");
        // Serial.println(humidity);
        // xSemaphoreGive(serial_mutex);
        if (!isnan(humidity))
        {
            xQueueSend(humidity_queue, &humidity, 0);
        }
        else
        {
            // xSemaphoreTake(serial_mutex, portMAX_DELAY);
            // Serial.println("Failed to read humidity from DHT sensor!");
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
    const int JSON_BUFFER_SIZE = 5000;

    char *json_payload = (char *)malloc(JSON_BUFFER_SIZE);

    if (json_payload == NULL)
    {
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("ERRO FATAL: Falha ao alocar memória para o payload MQTT!");
        xSemaphoreGive(serial_mutex);
        while (1)
        {
            vTaskDelay(1000);
        }
    }

    for (;;)
    {
        VerificaConexoesWiFIEMQTT();
        MQTT.loop();

        // Publish distance data (non-blocking)
        xSemaphoreTake(send_mqtt, portMAX_DELAY);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.println("------ Send to MQTT ------");
        xSemaphoreGive(serial_mutex);

        char *p = json_payload;

        char *end = json_payload + JSON_BUFFER_SIZE;

        p += snprintf(p, end - p, "{\"readings\":[");

        float distance;
        int angle;
        bool first_element = true;

        while (xQueueReceive(distance_queue, &distance, 0) == pdTRUE && xQueueReceive(servo_angle_queue, &angle, 0) == pdTRUE)
        {

            int written = 0;

            if (!first_element)
            {
                written = snprintf(p, end - p, ",");

                if (written > 0 && p + written < end)
                {
                    p += written;
                }
                else
                {
                    xSemaphoreTake(serial_mutex, portMAX_DELAY);
                    Serial.println("ERRO: Perigo de estouro de buffer do JSON (na virgula). Payload truncado.");
                    xSemaphoreGive(serial_mutex);
                    break;
                }
            }

            written = snprintf(p, end - p, "{\"a\":%d,\"d\":%.2f}", angle, distance);

            if (written > 0 && p + written < end)
            {
                p += written;
            }
            else
            {
                xSemaphoreTake(serial_mutex, portMAX_DELAY);
                Serial.println("ERRO: Perigo de estouro de buffer do JSON (no dado). Payload truncado.");
                xSemaphoreGive(serial_mutex);
                break;
            }

            first_element = false;
        }

        snprintf(p, end - p, "]}");

        if (MQTT.publish(TOPIC_PUBLISH_DISTANCE, json_payload))
        {
            digitalWrite(send_led, HIGH);
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.println("Payload JSON publicado com sucesso!");
            xSemaphoreGive(serial_mutex);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            digitalWrite(send_led, LOW);
        }

        xSemaphoreGive(done_MQTT);

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Initializes WiFi connection parameters and connects to the network.
 * @return None
 */
void initWiFi(void)
{
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

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
    MQTT.setBufferSize(4096);
    MQTT.setKeepAlive(60);
}

/**
 * @brief Reconnects to the MQTT broker if the connection is lost.
 * @return None
 */
void reconnectMQTT(void)
{
    xSemaphoreTake(start_humidity, 0);
    xSemaphoreTake(start_smoke, 0);
    xSemaphoreTake(mqtt_connecting, portMAX_DELAY);
    while (!MQTT.connected())
    {
        xQueueReset(distance_queue);
        xQueueReset(servo_angle_queue);
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
        Serial.print("* Trying to connect to MQTT Broker: ");
        xSemaphoreGive(serial_mutex);
        Serial.println(BROKER_MQTT);
        if (MQTT.connect(ID_MQTT))
        {
            digitalWrite(connected_led, HIGH);
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.println("Successfully connected to MQTT broker!");
            xSemaphoreGive(serial_mutex);
            xSemaphoreGive(mqtt_connecting);
            MQTT.subscribe(TOPIC_PUBLISH_DISTANCE);
            xSemaphoreGive(start_humidity);
            xSemaphoreGive(start_smoke);
        }
        else
        {
            digitalWrite(connected_led, LOW);
            xSemaphoreTake(serial_mutex, portMAX_DELAY);
            Serial.print("Failed to reconnect to broker. ERROR: ");
            Serial.println(MQTT.state());
            Serial.println("There will be a new connection attempt in 2s");
            xSemaphoreGive(serial_mutex);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
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
    // xSemaphoreTake(start_humidity, portMAX_DELAY);
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
    xSemaphoreGive(serial_mutex);               // Inicializa liberado
    xSemaphoreGive(mqtt_connecting);            // Inicializa liberado
    xSemaphoreGive(start_humidity);

    Serial.begin(115200);

    pinMode(DHTPIN, INPUT_PULLUP);
    dht.begin();

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    servo.setPeriodHertz(50);
    servo.attach(SERVO_PIN, 500, 2400);
    servo.write(0);

    pinMode(connected_led, OUTPUT);
    pinMode(send_led, OUTPUT);

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
    xTaskCreate(mqtt_task, "MQTTLoop", 8192, NULL, 3, NULL);
    xTaskCreate(scan_humidity, "ScanHumidity", 4096, NULL, 1, NULL);
    xTaskCreate(process_data, "ProcessData", 4096, NULL, 4, NULL);
    xTaskCreate(measure_distance, "MeasureDistance", 4096, NULL, 2, NULL);
    xTaskCreate(servo_controller_task, "ServoController", 2048, NULL, 2, NULL);
}

/**
 * @brief Arduino main loop function. Not used in this application.
 * @return None
 */
void loop()
{
}