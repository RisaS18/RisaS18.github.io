/* -------------------------------
  Risa Suzuki
  Real-Time System Spring 2026
  Application 6
  04/27/2026
--------------------------------*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <uri/UriBraces.h>

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
// Defining the WiFi channel speeds up the connection:
#define WIFI_CHANNEL 6

WebServer server(80);

SemaphoreHandle_t sem_button;
SemaphoreHandle_t sem_sensor;
SemaphoreHandle_t print_mutex;

#define Alert_LED  26
#define Heart_beat  27
#define BUTTON_PIN 19
#define POT_PIN 34

#define SENSOR_THRESHOLD 3000
#define MAX_COUNT_SEM 300 

volatile int SEMCNT = 0;
static volatile TickType_t last_isr = 0;

bool led1State = false;
bool led2State = false;

// Emergency call from patient
void IRAM_ATTR button_isr_handler() {
    TickType_t now = xTaskGetTickCountFromISR();

    // debounce (50 ms)
    if ((now - last_isr) < pdMS_TO_TICKS(50)) return;
    last_isr = now;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(sem_button, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void sendHtml() {
  String response = R"(
    <!DOCTYPE html><html>
      <head>
        <title>ESP32 Web Server Demo</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
          html { font-family: sans-serif; text-align: center; }
          body { display: inline-flex; flex-direction: column; }
          h1 { margin-bottom: 1.2em; } 
          h2 { margin: 0; }
          div { display: grid; grid-template-columns: 1fr 1fr; grid-template-rows: auto auto; grid-auto-flow: column; grid-gap: 1em; }
          .btn { background-color: #5B5; border: none; color: #fff; padding: 0.5em 1em;
                 font-size: 2em; text-decoration: none }
          .btn.OFF { background-color: #333; }
        </style>
      </head>
            
      <body>
        <h1>ESP32 Web Server</h1>

        <div>
          <h2>LED 1</h2>
          <a href="/toggle/1" class="btn LED1_TEXT">LED1_TEXT</a>
          
        </div>
      </body>
    </html>
  )";
  response.replace("LED1_TEXT", led1State ? "ON" : "OFF");
  response.replace("LED2_TEXT", led2State ? "ON" : "OFF");
  server.send(200, "text/html", response);
}

void setup(void) {
  Serial.begin(115200);

  pinMode(Heart_beat, OUTPUT);
  pinMode(Alert_LED, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr_handler, FALLING);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", sendHtml);

  server.on(UriBraces("/toggle/{}"), []() {
    String led = server.pathArg(0);
    Serial.print("Toggle LED #");
    Serial.println(led);

    switch (led.toInt()) {
      case 1:
        led1State = !led1State;
        digitalWrite(Alert_LED, led1State);
        break;
      
    }

    sendHtml();
  });

  server.begin();
  Serial.println("HTTP server started");

  sem_button = xSemaphoreCreateBinary();
  sem_sensor = xSemaphoreCreateCounting(MAX_COUNT_SEM, 0);
  print_mutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(sensor_task, "sensor", 2048, NULL, 2, NULL, 1);
  //xTaskCreatePinnedToCore(button_task, "button", 2048, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(event_handler_task, "event", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(logger_task, "loggert", 2048, NULL, 1, NULL, 1);
}

void loop(void) {
  server.handleClient();
  delay(2);
}

// Patient's heart rate
// Hard real-time
// Period = 100ms, Deadline = 100ms
void sensor_task(void *pvParameters) {
    static int sensor_state = 0;
    TickType_t wake_time = xTaskGetTickCount();
    while (1) {
        int val = analogRead(POT_PIN);

        xSemaphoreTake(print_mutex, portMAX_DELAY);
        printf("[TIME %lu ms] Heart Rate: %d\n", millis(), val);
        xSemaphoreGive(print_mutex);

        if (val > SENSOR_THRESHOLD && sensor_state == 0) {
            sensor_state = 1;
            if(SEMCNT < MAX_COUNT_SEM+1) SEMCNT++; // DO NOT REMOVE THIS LINE
            
            xSemaphoreGive(sem_sensor);  // Signal sensor event
        }
        else if (val <= SENSOR_THRESHOLD){
          sensor_state = 0;
        }

        vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(100));
    }
}

//Hard real-time
//Period = 10ms, Deadline = 10ms
void event_handler_task(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(sem_sensor, 0)) {
            SEMCNT--;  // DO NOT MODIFY THIS LINE

            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("[TIME %lu ms] ALERT: PATIENT IN DANGER\n", millis());
            xSemaphoreGive(print_mutex);

            digitalWrite(Alert_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(Alert_LED, 0);
        }

        if (xSemaphoreTake(sem_button, 0)) {
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("ALERT!: EMERGENCY BUTTON WAS PRESSED!!\n");
            xSemaphoreGive(print_mutex);

            digitalWrite(Alert_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(300));
            digitalWrite(Alert_LED, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Idle delay to yield CPU
    }
}

// Patient's Heart beat 
//Period = 1000ms, Deadline = 1000ms
// Soft real-time
void heartbeat_task(void *pvParameters) {
  bool led_status = false;

    while (1) {
        digitalWrite(Heart_beat, led_status);
        led_status = !led_status;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

// Logging delay
// Period = 50 - 300ms, Deadline = 50 - 300ms
//Soft real-time
void logger_task(void *pvParameters) {
    while (1) {
        int delay_time = random(50, 300); // variable execution time

        xSemaphoreTake(print_mutex, portMAX_DELAY);
        Serial.printf("Logger running... delay=%d ms\n", delay_time);
        xSemaphoreGive(print_mutex);

        vTaskDelay(pdMS_TO_TICKS(delay_time));
    }
}

