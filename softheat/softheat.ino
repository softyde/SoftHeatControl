/*
 * Heizungssteuerungsscript ESP8266
 * 
 * Copyright (c) 2021 Philipp Anné
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the 
 * Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, 
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software. 
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR 
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH 
 * THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#include "properties.h"
/*
 * Either create a properties.h file containing the following values or just define them here:
 * 
 * #define MQTT_HEATING_SWITCH_COMMAND "<MQTT topic switch heating thermostate>"
 * #define MQTT_MODE "<MQTT topic mode>"
 * #define MQTT_STATUS "<MQTT topic status>"
 * #define MQTT_TARGET "<MQTT topic target temperature>"
 * #define MQTT_TARGET_COMMAND "<MQTT topic set target temperature>"
 * #define MQTT_SERVER "<MQTT server ip>"
 * #define WLAN_SSID "<WLAN ssid>"
 * #define WLAN_PASSWORD "<WLAN password>"
 * 
 */

/*
 * Definiert das MQTT-Topic um die aktuelle Konfiguration zu ermitteln.
 * Diese sollte dauerhaft gespeichert werden (retain) und folgendes JSON-Format aufweisen:
 * 
 */
#define MQTT_TOPIC_CONFIG "softheat/configuration"

#define MAX_MQTT_TOPIC_LENGTH 128

#define JSON_VERSION "2021.1"

#define EEPROM_TEMP_TARGET 0

#define MODE_STARTING 0
#define MODE_HEATING 1
#define MODE_COOLING 2

const char* ssid = WLAN_SSID;
const char* password = WLAN_PASSWORD;
const char* mqtt_server = MQTT_SERVER;

int temperatureTargetValue;
float temperatureCurrentValue = 100.0;

#define MAX_BLINK 10
#define BLINK_DURATION_MS 250

#define BLINK_MODE_WAIT_CONFIG 7
#define BLINK_MODE_WAIT_TEMPERATURE 2

#define BLINK_MODE_HEATING_MISSING 3

byte blinkMode = 4;
byte blinkCounter = 0;
unsigned long blinkLast = 0;


char mqttTopicCurrentTemperature[MAX_MQTT_TOPIC_LENGTH];
char mqttTopicHeatingSwitchCommand[MAX_MQTT_TOPIC_LENGTH];


//char mqttTopicTargetTemperatureCommand[MAX_MQTT_TOPIC_LENGTH];


WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
int mode = MODE_STARTING;

void setup_wifi() {

  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void setTargetTemperature(String tempString) {
  
  temperatureTargetValue = min(max((int)tempString.toInt(), 16), 30);
  
  Serial.print("Setting new temperature: ");
  Serial.print(temperatureTargetValue);
  Serial.println("°C");

  EEPROM.put(EEPROM_TEMP_TARGET, (byte)temperatureTargetValue);
  EEPROM.commit();

  // reset counter - prevents switches to occur too fast/often
  lastMsg = millis();
  
  String s = String(temperatureTargetValue);
  const char *v = s.c_str(); 
  client.publish(MQTT_TARGET, (const unsigned char *)v, strlen(v), true);
}

void setCurrentTemperature(String tempString) {

  temperatureCurrentValue = min(max(tempString.toFloat(), 0.0f), 40.0f);

  Serial.print("Received current temperature: ");
  Serial.print(temperatureCurrentValue);
  Serial.println("°C");
}

void updateConfig(const char* payload) {

  DynamicJsonDocument doc(2048);

  DeserializationError error = deserializeJson(doc, payload);

    if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  const char* jsonVersion = doc["version"];
  if(strcmp(jsonVersion, JSON_VERSION) != 0) {
    Serial.println("error: invalid config json");    
    return;
  }

  bool reconnectMqtt = false;

  const char* jsonCurrentTemp = doc["currentTemp"];
  if(strcmp(jsonCurrentTemp, mqttTopicCurrentTemperature) != 0) {

    Serial.print("topic 'currentTemperature' changed to ");  
    Serial.println(jsonCurrentTemp);  
    
    strncpy(mqttTopicCurrentTemperature, jsonCurrentTemp, sizeof(mqttTopicCurrentTemperature));
    reconnectMqtt = true;    
  }
  
  const char* jsonHeatingSwitch = doc["heatingSwitch"];
  if(strcmp(jsonHeatingSwitch, mqttTopicHeatingSwitchCommand) != 0) {

    Serial.print("topic 'heatingSwitch' changed to ");  
    Serial.print(jsonHeatingSwitch);
    Serial.println(" (no reconnect needed)");
    
    strncpy(mqttTopicHeatingSwitchCommand, jsonHeatingSwitch, sizeof(mqttTopicHeatingSwitchCommand));    
  }
  
  if(reconnectMqtt) {
    client.disconnect(); 
  }
}

/**
 * Die Methode wird ausgeführt, wenn eine neue MQTT-Nachricht empfangen wurde.
 * 
 */
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");

  if(length > 2048) {
    Serial.println("Error: payload too large");
    return;
  }

  char cstr_payload[length + 1];
  memcpy(cstr_payload, payload, length);
  cstr_payload[length] = 0;

  /* MQTT Konfigurationsnachricht */
  if(strcmp(topic, MQTT_TOPIC_CONFIG) == 0) {
    updateConfig(cstr_payload);
  }

  else if(strcmp(topic, MQTT_TARGET_COMMAND) == 0) {
    setTargetTemperature(String(cstr_payload));
  } else if((strlen(mqttTopicCurrentTemperature) > 0) && (strcmp(topic, mqttTopicCurrentTemperature) == 0)) {
    setCurrentTemperature(String(cstr_payload));
  } 

} 

void updateHeating() {

  if(strlen(mqttTopicHeatingSwitchCommand) == 0) {
    blinkMode = BLINK_MODE_HEATING_MISSING;
    return;
  }

  if(temperatureCurrentValue < (float)temperatureTargetValue) {
    client.publish(mqttTopicHeatingSwitchCommand, "on");    
    client.publish(MQTT_MODE, "heating");
    mode = MODE_HEATING;
  } else {
    client.publish(mqttTopicHeatingSwitchCommand, "off");    
    client.publish(MQTT_MODE, "cooling");
    mode = MODE_COOLING;
  }

  
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      
      // Once connected, publish an announcement...
      client.publish(MQTT_STATUS, "starting");

      client.subscribe(MQTT_TOPIC_CONFIG);

      if(strlen(mqttTopicCurrentTemperature) > 0) {

        Serial.print("Subscribing topic 'currentTemperature': ");
        Serial.println(mqttTopicCurrentTemperature);

        client.subscribe(mqttTopicCurrentTemperature);       
      }

      
      // ... and resubscribe
      client.subscribe(MQTT_TARGET_COMMAND);
      
      String s = String(temperatureTargetValue);
      const char *v = s.c_str(); 
      client.publish(MQTT_TARGET, (const unsigned char *)v, strlen(v), true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup_eeprom() {

  EEPROM.begin(1);

  temperatureTargetValue = EEPROM.read(EEPROM_TEMP_TARGET);

  temperatureTargetValue = min(max(temperatureTargetValue, 16), 30);

  Serial.print("Temperature target value: ");
  Serial.print(temperatureTargetValue);
  Serial.println("°C");  
}

void setup() {
  pinMode(BUILTIN_LED, OUTPUT);     
  digitalWrite(BUILTIN_LED, LOW);

  
  Serial.begin(115200);
  setup_wifi();
  setup_eeprom();
  
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  digitalWrite(BUILTIN_LED, HIGH);

  blinkMode = BLINK_MODE_WAIT_CONFIG;
}

void updateBlink() {

  unsigned long now = millis();
  
  if(now - blinkLast >= BLINK_DURATION_MS) {

    blinkLast = now;
    blinkCounter = (blinkCounter + 1) % (MAX_BLINK * 2);

    bool isOn = (blinkCounter % 2 == 0) && (blinkCounter < 2 * blinkMode);
    digitalWrite(BUILTIN_LED, isOn ? LOW : HIGH);
  }  
}

void loop() {

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  updateBlink();
  
//  unsigned long now = millis();


/*
  if(now % 100 == 0) {

    switch(mode) {
    case MODE_STARTING:
      // hectic flashing during startup
      digitalWrite(BUILTIN_LED, (now / 100) % 2 == 0 ? HIGH : LOW);
      break;
    case MODE_HEATING:
      // fast flashing while heating
      digitalWrite(BUILTIN_LED, (now / 100) % 10 == 0 ? LOW : HIGH);
      break;
    case MODE_COOLING:
      // slow flashing while cooling
      digitalWrite(BUILTIN_LED, (now / 100) % 50 == 0 ? LOW: HIGH);
      break;
    }
  }

  if(now % 5000 == 0) {

    if(mode == MODE_HEATING) {
      client.publish(MQTT_MODE, "heating");  
    } else if(mode == MODE_COOLING) {
      client.publish(MQTT_MODE, "cooling");  
    }
  }
  
  if (now - lastMsg >= 60000) {
    client.publish(MQTT_STATUS, "alive");
    lastMsg = now;

    updateHeating();
  }*/
    
    
}
