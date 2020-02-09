#include <stdint.h>
#include <stdio.h>

#include "Arduino.h"
#include "WifiEspNow.h"
#include "ESP8266WiFi.h"
#include "EEPROM.h"

// ==================================================================
// Skipbutton things

const uint8_t numButtons = 3;
const uint8_t buttonPins[] = {D5, D1, D2}; // Skip, volup, voldown

bool buttonStates[] = {false, false, false};
bool noButtonsPressed = true;

// Skip: revspace/button/skip - jump_fwd
// Stop: revspace/button/stop

#define MQTT_TOPIC_BASE "revspace/button/"
#define TOPIC_SKIP "skip"
#define TOPIC_VOL_UP "vol_up"
#define TOPIC_VOL_DN "vol_down"
#define TOPIC_SHUF "shuffle"
#define TOPIC_STOP "stop"

#define MQTT_MSG_SKIP "jump_fwd"
#define MQTT_MSG "hackalot"

#define ACTION_NOTHING 0
#define ACTION_SKIP 1
#define ACTION_VOL_UP 2
#define ACTION_VOL_DOWN 3
#define ACTION_SHUFFLE 4
#define ACTION_STOP 5

uint8_t action = ACTION_NOTHING;

// ==================================================================
// ESP-Now Things

static const char AP_NAME[] = "revspace-espnow";

typedef enum {
  E_SEND,
  E_ACK,
  E_DISCOVER,
  E_SLEEP
} skip_mode_t;

static skip_mode_t mode = E_SEND;

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
  
  while(true) {
    delay(10);
  }
}

void setup() {
  // Keep power enabled
  pinMode(D0, OUTPUT);
  digitalWrite(D0, HIGH);

  // initialise pins
  for(uint8_t i = 0; i < numButtons; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }

  // Debounce
  delay(5);

  // Read pins
  for(uint8_t i = 0; i < numButtons; i++) {
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

  WifiEspNow.begin();
  EEPROM.begin(512);  

  if (buttonStates[0]) { // Main dome button
    if (!buttonHeld(0))
      action = ACTION_SKIP;
    else
      action = ACTION_STOP;
    return;
  }

  if (buttonStates[1]) { // Top button
    action = ACTION_VOL_UP;
    return;
  }

  if (buttonStates[2]) { // Bottom button
    action = ACTION_VOL_DOWN;
    return;
  }
}

void loop() {
  WifiEspNowSendStatus status;
  struct WifiEspNowPeerInfo recv;
  char line[128];

  switch (mode) {
    case E_SEND:
        // read last known receiver info from EEPROM
        EEPROM.get(0, recv);
        if (valid_peer(&recv)) {
            // send SKIP message to last known address
            sprintf(line, "Sending SKIP to %02X:%02X:%02X:%02X:%02X:%02X (chan %d)...",
                    recv.mac[0], recv.mac[1], recv.mac[2], recv.mac[3], recv.mac[4], recv.mac[5], recv.channel);
            Serial.print(line);
  
            WifiEspNow.addPeer(recv.mac, recv.channel, nullptr);

            switch (action) {
              case ACTION_SKIP:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_SKIP, MQTT_MSG_SKIP); // Different message for backwards-compatibility
                break;
              case ACTION_VOL_UP:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_VOL_UP, MQTT_MSG);
                break;
              case ACTION_VOL_DOWN:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_VOL_DN, MQTT_MSG);
                break;
              case ACTION_SHUFFLE:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_SHUF, MQTT_MSG);
                break;
              case ACTION_STOP:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_STOP, MQTT_MSG);
                break;
              default:
                send_topic_text(recv.mac, MQTT_TOPIC_BASE TOPIC_SKIP, MQTT_MSG_SKIP);
                break;
            }
            
  
            mode = E_ACK;
        } else {
            mode = E_DISCOVER;
        }
        break;
  
    case E_ACK:
        // wait for tx ack
        status = WifiEspNow.getSendStatus();
        switch (status) {
          case WifiEspNowSendStatus::NONE:
              if (millis() > 3000) {
                  Serial.println("TX ack timeout");
                  mode = E_DISCOVER;
              }
              break;
          case WifiEspNowSendStatus::OK:
              Serial.println("TX success");
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
          case WifiEspNowSendStatus::FAIL:
          default:
              Serial.println("TX failed");
              mode = E_DISCOVER;
              break;
        }
        break;
  
    case E_DISCOVER:
        Serial.println("Discovering master ...");
        if (find_ap(AP_NAME, &recv)) {
            // save it in EEPROM
            sprintf(line, "found '%s' at %02X:%02X:%02X:%02X:%02X:%02X (chan %d), saving to EEPROM", AP_NAME,
                recv.mac[0], recv.mac[1], recv.mac[2], recv.mac[3], recv.mac[4], recv.mac[5], recv.channel);
            Serial.println(line);
            EEPROM.put(0, recv);
            EEPROM.end();
        } else {
            Serial.println("no master found!");
        }
        mode = E_SLEEP;
        break;
  
    case E_SLEEP:
    default:
        Serial.print("Shutting down...");
        doShutdown();
        break;
    }  
}

// ==================================================================
// ESP-Now Functions

static bool find_ap(const char *name, struct WifiEspNowPeerInfo *peer)
{
    // scan for networks and try to find our AP
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        Serial.println(WiFi.SSID(i));
        if (strcmp(name, WiFi.SSID(i).c_str()) == 0) {
            // copy receiver data
            peer->channel = WiFi.channel(i);
            memcpy(peer->mac, WiFi.BSSID(i), sizeof(peer->mac));
            return true;
        }
    }
    // not found
    return false;
}

static void send_topic_text(uint8_t *mac, const char *topic, const char *text)
{
    char buf[250];
    int n = snprintf(buf, sizeof(buf), "%s %s", topic, text);
    WifiEspNow.send(mac, (uint8_t *)buf, n);
}

static bool valid_peer(struct WifiEspNowPeerInfo *peer)
{
    return (peer->channel >= 1) && (peer->channel <= 14); 
}

