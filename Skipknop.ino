#include "Arduino.h"
#include <espnow.h>
#include "ESP8266WiFi.h"

// ==================================================================
// Skipbutton things

const uint8_t numButtons = 3;
const uint8_t buttonPins[] = { D5, D1, D2 };  // Skip, volup, voldown

bool buttonStates[] = { false, false, false };
bool noButtonsPressed = true;

// Skip: revspace/button/skip - jump_fwd
// Stop: revspace/button/stop

#define MQTT_TOPIC_BASE "#Urevspace/button/"
#define TOPIC_SKIP "skip"
#define TOPIC_VOL_UP "vol_up"
#define TOPIC_VOL_DN "vol_down"
#define TOPIC_SHUF "shuffle"
#define TOPIC_STOP "stop"

#define MQTT_MSG_SKIP "jump_fwd"
#define MQTT_MSG "hackalot"

typedef enum {
  ACTION_NOTHING,
  ACTION_SKIP,
  ACTION_VOL_UP,
  ACTION_VOL_DOWN,
  ACTION_SHUFFLE,
  ACTION_STOP
} action_t;

static action_t action = ACTION_NOTHING;

typedef enum {
  E_SEND,
  E_DONE,
  E_SLEEP
} skip_mode_t;

static skip_mode_t mode = E_SEND;

// ==================================================================
// ESP-Now Things

#define ESP_NOW_CHANNEL 9
uint8_t broadcastAddress[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ==================================================================
// Cut here ✂️------------------------------------------------------

bool buttonHeld(uint8_t butt, unsigned long timeout = 1250UL) {
  unsigned long startMillis = millis();
  while (!digitalRead(buttonPins[butt])) {
    if (millis() - startMillis >= timeout) {
      return true;
    }
    delay(5);
  }
  return false;
}

void doShutdown() {
  // Disable power
  digitalWrite(D0, LOW);

  while (true) {
    delay(10);
  }
}

void setup() {
  // Keep power enabled
  pinMode(D0, OUTPUT);
  digitalWrite(D0, HIGH);

  // initialise pins
  for (uint8_t i = 0; i < numButtons; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }

  // Debounce
  delay(5);

  // Read pins
  for (uint8_t i = 0; i < numButtons; i++) {
    bool state = !digitalRead(buttonPins[i]);
    buttonStates[i] = state;

    if (state)
      noButtonsPressed = false;
  }

  Serial.begin(115200);
  Serial.println("\nESPNOW-SKIP");

  // Do nothing if no buttons were pressed
  if (noButtonsPressed) {
    Serial.println("Nothing pressed, shutting down");
    doShutdown();
  }

  // Initialize esp-now on right channel
  WiFi.forceSleepWake();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin("aapnoot", "miesmies", ESP_NOW_CHANNEL, NULL, false);
  delay(10);
  WiFi.disconnect();
  wifi_set_channel(ESP_NOW_CHANNEL);

  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    doShutdown();
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, ESP_NOW_CHANNEL, NULL, 0);

  if (buttonStates[0]) {  // Main dome button
    if (!buttonHeld(0))
      action = ACTION_SKIP;
    else
      action = ACTION_STOP;
    return;
  }

  if (buttonStates[1]) {  // Top button
    action = ACTION_VOL_UP;
    return;
  }

  if (buttonStates[2]) {  // Bottom button
    action = ACTION_VOL_DOWN;
    return;
  }
}

void loop() {

  switch (mode) {
    case E_SEND:
      switch (action) {
        case ACTION_SKIP:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_SKIP, MQTT_MSG_SKIP);  // Different message for backwards-compatibility
          break;
        case ACTION_VOL_UP:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_VOL_UP, MQTT_MSG);
          break;
        case ACTION_VOL_DOWN:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_VOL_DN, MQTT_MSG);
          break;
        case ACTION_SHUFFLE:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_SHUF, MQTT_MSG);
          break;
        case ACTION_STOP:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_STOP, MQTT_MSG);
          break;
        default:
          send_topic_text(MQTT_TOPIC_BASE TOPIC_SKIP, MQTT_MSG_SKIP);
          break;
      }
      // Move to next section to check for held buttons
      delay(10);
      mode = E_DONE;
      break;

    case E_DONE:
      // First send done
      mode = E_SLEEP;
      switch (action) {
        case ACTION_VOL_UP:
          if (buttonHeld(1, 900))
            mode = E_SEND;
          break;
        case ACTION_VOL_DOWN:
          if (buttonHeld(2, 900))
            mode = E_SEND;
          break;
        case ACTION_STOP:
          if (buttonHeld(0, 2500)) {
            action = ACTION_SHUFFLE;
            mode = E_SEND;
          }
          break;
      }
      break;

    case E_SLEEP:
    default:
      delay(5);
      Serial.print("Shutting down...");
      doShutdown();
      break;
  }
}

// ==================================================================
// ESP-Now Functions

bool send_topic_text(const char *topic, const char *text) {
  uint8_t *mac = broadcastAddress;
  char buf[250];
  int n = snprintf(buf, sizeof(buf), "%s %s", topic, text);
  return esp_now_send(mac, (uint8_t *)buf, n) == 0;
}
