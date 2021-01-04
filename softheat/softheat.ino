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

#include "properties.h"
/*
 * Either create a properties.h file containing the following values or just define them here:
 * 
 * #define MQTT_CURRENT_TEMPERATURE "<MQTT topic current temperature>"
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

#define EEPROM_TEMP_TARGET 0

#define MODE_STARTING 0
#define MODE_HEATING 1
#define MODE_COOLING 2

const char* ssid = WLAN_SSID;
const char* password = WLAN_PASSWORD;
const char* mqtt_server = MQTT_SERVER;

int temperatureTargetValue;
float temperatureCurrentValue = 100.0;

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
#define MSG_BUFFER_SIZE	(50)
char msg[MSG_BUFFER_SIZE];
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

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  if(length > 10) {
    Serial.println("Error: payload too large");
    return;
  }

  char x[length + 1];
  memcpy(x, payload, length);
  x[length] = 0;

  if(strcmp(topic, MQTT_TARGET_COMMAND) == 0) {
    setTargetTemperature(String(x));
  } else if(strcmp(topic, MQTT_CURRENT_TEMPERATURE) == 0) {
    setCurrentTemperature(String(x));
  }

} 

void updateHeating() {

  if(temperatureCurrentValue < (float)temperatureTargetValue) {
    client.publish(MQTT_HEATING_SWITCH_COMMAND, "on");    
    client.publish(MQTT_MODE, "heating");
    mode = MODE_HEATING;
  } else {
    client.publish(MQTT_HEATING_SWITCH_COMMAND, "off");    
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
      
      // ... and resubscribe
      client.subscribe(MQTT_TARGET_COMMAND);
      client.subscribe(MQTT_CURRENT_TEMPERATURE);

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
  Serial.begin(115200);
  setup_wifi();
  setup_eeprom();
  
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  digitalWrite(BUILTIN_LED, HIGH);
}

void loop() {

  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  
  unsigned long now = millis();

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
  }
    
    
}
