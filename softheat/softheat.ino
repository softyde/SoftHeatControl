/*
   Heizungssteuerungsscript ESP8266

   Copyright (c) 2021 Philipp Anné

   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the
   Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
   ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
   THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#include "properties.h"
/*
   Either create a properties.h file containing the following values or just define them here:

   #define MQTT_SERVER "<MQTT server ip>"

   #define WLAN_SSID "<WLAN ssid>"
   #define WLAN_PASSWORD "<WLAN password>"
*/

/*
   Definiert das MQTT-Topic um die aktuelle Konfiguration zu ermitteln.
   Diese sollte dauerhaft gespeichert werden (retain) und folgendes JSON-Format aufweisen:

*/
#define MQTT_TOPIC_CONFIG "softheat/configuration"
#define MQTT_TOPIC_LOG_FORMAT "softheat/log/%s"

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


#define MQTT_DEFAULT_MODE "softheat/mode"

byte blinkMode = 4;
byte blinkCounter = 0;
unsigned long blinkLast = 0;

char mqttTopicCurrentTemperature[MAX_MQTT_TOPIC_LENGTH];
char mqttTopicHeatingSwitchCommand[MAX_MQTT_TOPIC_LENGTH];
char mqttTopicTargetTemperatureCommand[MAX_MQTT_TOPIC_LENGTH];
char mqttTopicTargetTemperature[MAX_MQTT_TOPIC_LENGTH];

char mqttTopicMode[MAX_MQTT_TOPIC_LENGTH] = MQTT_DEFAULT_MODE;





//char mqttTopicTargetTemperatureCommand[MAX_MQTT_TOPIC_LENGTH];


WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
int mode = MODE_STARTING;

String clientId;
char logTopic[100];


void Log(const char* format, ...)
{
  char output[512];

  va_list argptr;
  va_start(argptr, format);
  vsnprintf(output, sizeof(output), format, argptr);
  va_end(argptr);

  Serial.println(output);

  if (client.connected()) {
    client.publish(logTopic, output);
  }
}



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

  client.setBufferSize(1024);


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

  if (strlen(mqttTopicTargetTemperature) > 0) {
    String s = String(temperatureTargetValue);
    const char *v = s.c_str();
    client.publish(mqttTopicTargetTemperature, (const unsigned char *)v, strlen(v), true);
  }
}

void setCurrentTemperature(String tempString) {

  temperatureCurrentValue = min(max(tempString.toFloat(), 0.0f), 40.0f);

  Log("Received current temperature: %.2f°C", temperatureCurrentValue);
}

void readNewValue(char* target, size_t len, const char*source, const char* topic, bool* reconnectMqttTarget) {

  if (source) {
    if (strcmp(source, target) != 0) {
      Log("Topic '%s' changed to %s", topic, source);
      strncpy(target, source, len);
      if (reconnectMqttTarget != NULL) {
        *reconnectMqttTarget = true;
      }
    }
  } else if (strlen(target) > 0) {
    target[0] = 0;
    Log("topic '%s' cleared", topic);
    if (reconnectMqttTarget != NULL) {
      *reconnectMqttTarget = true;
    }
  }
}


void updateConfig(const char* payload) {

  DynamicJsonDocument doc(1024);

  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Log("DeserializeJson() failed: %s", error.f_str());
    return;
  }

  const char* jsonVersion = doc["version"];
  if (strcmp(jsonVersion, JSON_VERSION) != 0) {
    Log("Error: invalid config json");
    return;
  }

  Log("Config: reloading");

  bool reconnectMqtt = false;

  readNewValue(mqttTopicCurrentTemperature, sizeof(mqttTopicCurrentTemperature), doc["currentTemperature"], "currentTemperature", &reconnectMqtt);
  readNewValue(mqttTopicTargetTemperature, sizeof(mqttTopicTargetTemperature), doc["targetTemperature"], "targetTemperature", NULL);
  readNewValue(mqttTopicTargetTemperatureCommand, sizeof(mqttTopicTargetTemperatureCommand), doc["targetTemperatureCommand"], "targetTemperature (command)", &reconnectMqtt);
  readNewValue(mqttTopicHeatingSwitchCommand, sizeof(mqttTopicHeatingSwitchCommand), doc["heatingSwitchCommand"], "heatingSwitch (command)", NULL);
  readNewValue(mqttTopicMode, sizeof(mqttTopicMode), doc["mode"], "mode", NULL);

  Log("Config: reloaded");

  if (reconnectMqtt) {
    Log("Config changed: reconnecting");
    client.disconnect();
  }
}

/**
   Die Methode wird ausgeführt, wenn eine neue MQTT-Nachricht empfangen wurde.

*/
void callback(const char* topic, byte* payload, unsigned int length) {

  if (length > 1024) {
    Log("Error: payload too large");
    return;
  }

  // Copy the payload
  char cstr_payload[length + 1];
  memcpy(cstr_payload, payload, length);
  cstr_payload[length] = 0;

  // Copy the topic
  char copyTopic[strlen(topic) + 1];
  strcpy(copyTopic, topic);


  // IMPORTANT: topic and payload must be copied before calling "Log"
  //            because client.publish overwrites these buffers
  Log("Received %s (%d)", topic, length);

  /* MQTT Konfigurationsnachricht */
  if (strcmp(copyTopic, MQTT_TOPIC_CONFIG) == 0) {
    updateConfig(cstr_payload);
  }

  else if ((strlen(mqttTopicTargetTemperatureCommand) > 0) && (strcmp(copyTopic, mqttTopicTargetTemperatureCommand) == 0)) {
    setTargetTemperature(String(cstr_payload));
  } else if ((strlen(mqttTopicCurrentTemperature) > 0) && (strcmp(copyTopic, mqttTopicCurrentTemperature) == 0)) {
    setCurrentTemperature(String(cstr_payload));
  }

}

void updateHeating() {

  if (strlen(mqttTopicHeatingSwitchCommand) == 0) {
    blinkMode = BLINK_MODE_HEATING_MISSING;
    return;
  }

  if (temperatureCurrentValue < (float)temperatureTargetValue) {
    client.publish(mqttTopicHeatingSwitchCommand, "on");
    if(strlen(mqttTopicMode) > 0) {
      client.publish(mqttTopicMode, "heating");
    }
    mode = MODE_HEATING;
  } else {
    client.publish(mqttTopicHeatingSwitchCommand, "off");
    if(strlen(mqttTopicMode) > 0) {
      client.publish(mqttTopicMode, "cooling");
    }    
    mode = MODE_COOLING;
  }


}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    // Create a random client ID
    clientId = "softheatcontrol-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str())) {


      snprintf(logTopic, sizeof(logTopic),  MQTT_TOPIC_LOG_FORMAT, clientId.c_str());

      int bufSize = client.getBufferSize();
      Log("Connected (obviously) as %s (Buffer size is %d)", logTopic, bufSize);

      client.subscribe(MQTT_TOPIC_CONFIG);
      Log("Subscribed to config topic");

      if (strlen(mqttTopicCurrentTemperature) > 0) {

        Log("Subscribing topic 'currentTemperature': %s", mqttTopicCurrentTemperature);
        client.subscribe(mqttTopicCurrentTemperature);
      }

      if (strlen(mqttTopicTargetTemperatureCommand) > 0) {

        Log("Subscribing topic 'targetTemperature (command)': %s", mqttTopicTargetTemperatureCommand);
        client.subscribe(mqttTopicTargetTemperatureCommand);
      }

      /*  if(strlen(mqttTopicTargetTemperature) > 0) {
          String s = String(temperatureTargetValue);
          const char *v = s.c_str();
          client.publish(mqttTopicTargetTemperature, (const unsigned char *)v, strlen(v), true);
        }*/
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

  if (now - blinkLast >= BLINK_DURATION_MS) {

    blinkLast = now;
    blinkCounter = (blinkCounter + 1) % (MAX_BLINK * 2);

    bool isOn = (blinkCounter % 2 == 0) && (blinkCounter < 2 * blinkMode);
    digitalWrite(BUILTIN_LED, isOn ? LOW : HIGH);
  }
}

void updateMode() {

  static unsigned long lastModeUpdate = 0;

  unsigned long now = millis();

  if(strlen(mqttTopicMode) > 0) {
    if(now - lastModeUpdate > 2000) {

      lastModeUpdate = now;
      if(mode == MODE_HEATING) {
          client.publish(mqttTopicMode, "heating");
      } else if(mode == MODE_COOLING) {
          client.publish(mqttTopicMode, "cooling");
      }  
    }
  }  
}

void loop() {

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  delay(1);

  updateBlink();
  updateMode();



  unsigned long now = millis();
  if (now - lastMsg >= 60000) {    
    
      Log("SoftHeatControl is alive");
      lastMsg = now;

      updateHeating();
  }

}
