#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <string>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <Ticker.h>

#define LED_PIN 2
#define DHT_PIN 23
#define DHT_TYPE DHT22
#define CONFIG_FILE_PATH "/config.json"
#define MEASUREMENT_INTERVAL 1000*60*30 // time in ms
#define SETTINGS_INTERVAL 1000*60*60 // time in ms
#define AP_SSID "ESP32"
#define AP_PASSWORD "password"
#define WIFI_CONNECTION_TIMEOUT 1000*10 // time in ms
#define WIFI_RECONNECT_ATTEMPT 1000*10*6*10 // time in ms

// current configuration variables
String wifi_ssid;
String wifi_password;
String api_url;
int device_type;
String api_token;
String device_name;
int device_id;
bool device_created;
JsonDocument device_settings;

String getMacAddress();
void startAccessPoint();
void listenForWiFiConfig();
void reconnectWifi();
void _testLittleFs();
void loadConfig();
void updateConfig();
void resetConfig();
bool connectToWiFi(const char* ssid, const char* password);
bool create_device();
void sendMeasurement(int device_id, float value, int type);
void sendMeasurements();
void fetchSettings();
void mainLoop();

Ticker measurementsTicker(sendMeasurements, MEASUREMENT_INTERVAL, 0, MILLIS);
Ticker settingsTicker(fetchSettings, SETTINGS_INTERVAL, 0, MILLIS);
Ticker mainLoopTicker(mainLoop, 2000, 0, MILLIS);
Ticker wifiReconnectTicker(reconnectWifi, WIFI_RECONNECT_ATTEMPT, 0, MILLIS);

WiFiServer server(8080);
JsonDocument doc;
DHT dht(DHT_PIN, DHT_TYPE);
bool CONNECTED_TO_WIFI = false;

void setup() {
  Serial.begin(115200);
  _testLittleFs();
  dht.begin();
  pinMode(LED_PIN, OUTPUT);
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }
  loadConfig();

  mainLoopTicker.start();
  measurementsTicker.start();
  settingsTicker.start();
}

void loop() {
  mainLoopTicker.update();
}

void mainLoop() {
  if ((WiFi.status() == WL_CONNECTED) && (device_id != -1)){
    // Serial.println("DEVICE IS CONNECTED TO Wi-Fi. CAN WORK");
    measurementsTicker.update();
    settingsTicker.update();
  } else {
    if (connectToWiFi(wifi_ssid.c_str(), wifi_password.c_str())){
      Serial.println("Connected to Wi-Fi");
    } else {
      CONNECTED_TO_WIFI = false;
      digitalWrite(LED_PIN, HIGH); // Turn on the LED to indicate configuration mode
      startAccessPoint();
      listenForWiFiConfig();
    }
  }

  digitalWrite(LED_PIN, LOW); // Turn off the LED to indicate normal mode 
  if (!device_created){
    Serial.println("Device not created, creating device...");
    create_device();
  }
}


void fetchSettings(){
  Serial.println("Fetching settings...");
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client){
    client->setInsecure();
    HTTPClient http;
    Serial.println("Sending request to: " + api_url + "settings/?device=" + device_id);
    http.begin(*client, api_url + "settings/?device=" + device_id);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + api_token);

    int httpRensponseCode = http.GET();

    if (httpRensponseCode > 0){
      String response = http.getString();
      Serial.println("Response: " + response);

      DeserializationError error = deserializeJson(device_settings, response);
      if (error){
        Serial.println("Error while deserializing JSON response");
      } else {
        updateConfig();
      }
    } else {
      Serial.println("Error on HTTP request:\n" + http.errorToString(httpRensponseCode));
    }
    http.end();
    delete client;
  }
}

void sendMeasurements(){
  Serial.println("Sending measurements...");
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)){
    Serial.println("Failed to read from DHT sensor");
    return;
  }

  Serial.println("Temperature: " + String(temperature) + "°C");
  Serial.println("Humidity: " + String(humidity) + "%");

  sendMeasurement(device_id, temperature, 1);
  sendMeasurement(device_id, humidity, 2);
  Serial.println("Measurements sent");
}

void sendMeasurement(int device_id, float value, int type){
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client){
    client->setInsecure();
    HTTPClient http;
    Serial.println("Sending request to: " + api_url + "measurements/");
    http.begin(*client, api_url + "measurements/");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + api_token);

    JsonDocument temp;
    temp["device"] = device_id;
    temp["value"] = value;
    temp["type"] = type;

    String data;
    serializeJson(temp, data);

    Serial.println("Sending data:");
    Serial.println(data);

    int httpRensponseCode = http.POST(data);

    if (httpRensponseCode > 0){
      String response = http.getString();
      Serial.println("Response: " + response);
    } else {
      Serial.println("Error on HTTP request:\n" + http.errorToString(httpRensponseCode));
    }
    http.end();
    delete client;
  }
}

bool create_device(){
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client){
    client->setInsecure();
    HTTPClient http;
    Serial.println("Sendind request to: " + api_url + "devices/");
    http.begin(*client, api_url + "devices/");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + api_token);

    JsonDocument temp;
    String mac = getMacAddress();
    temp["name"] = device_name;
    temp["mac_address"] = mac;
    temp["type"] = device_type;

    String data;
    serializeJson(temp, data);
    Serial.println("Creating device with data:");
    Serial.println(data);
  
    int httpRensponseCode = http.POST(data);
    Serial.println("Response code: " + String(httpRensponseCode));
    if (httpRensponseCode > 0){
      String response = http.getString();
      Serial.println("Response: " + response);

      JsonDocument responseDoc;
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (error){
        Serial.println("Failed to parse JSON response");
      } else if (httpRensponseCode != 400){
        device_id = responseDoc["id"];
        Serial.println("Device ID: " + String(device_id));
        device_created = true;
        updateConfig();
        http.end();
        delete client;
        return true;
      } else {
        Serial.println("Failed to create device");
      }

    } else {
      Serial.println("Error on HTTP request:\n" + http.errorToString(httpRensponseCode));
    }
    http.end();
    delete client;
    return false;
  }
  Serial.println("Failed to create a WiFi client");
  delete client;
  return false;
}

bool connectToWiFi(const char* ssid, const char* password){
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to Wi-Fi");

  // Wait for connection with timeout
  unsigned long start_attempt_time = millis();
  while (WiFi.status() != WL_CONNECTED){
    if (millis() - start_attempt_time > WIFI_CONNECTION_TIMEOUT){ // Timeout after 30 seconds
      Serial.println("Failed to connect to WiFi: connection timed out.");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected to: ");
  Serial.println(ssid);
  Serial.print("Network config: ");
  Serial.println(WiFi.localIP());
  CONNECTED_TO_WIFI = true;
  return true;
}

void reconnectWifi(){
  Serial.println("Attempt to reconnect to Wi-Fi");
  connectToWiFi(wifi_ssid.c_str(), wifi_password.c_str());
}

void resetConfig(){
  File config_file = LittleFS.open(CONFIG_FILE_PATH, "w");
  if (!config_file) {
    Serial.println("Failed to open config file for writing");
    return;
  }

  doc["WIFI_SSID"] = "";
  doc["WIFI_PASS"] = "";
  doc["TOKEN"] = "";
  doc["DEVICE_NAME"] = "";
  doc["DEVICE_ID"] = -1;
  doc["CREATED"] = false;
  try {
    doc["SETTINGS"] = JsonArray();
  } catch (const std::exception& e) {
    Serial.println("Error: Failed to reset settings array");
  }

  if (serializeJson(doc, config_file) == 0) {
      Serial.println("Failed to reset config");
  } else {
      Serial.println("Configuration reset finished");
  }

  config_file.close();
}

void updateConfig(){
  File config_file = LittleFS.open(CONFIG_FILE_PATH, "w");
  if (!config_file) {
    Serial.println("Failed to open config file for writing");
    return;
  }

  doc["WIFI_SSID"] = wifi_ssid;
  doc["WIFI_PASS"] = wifi_password;
  doc["TOKEN"] = api_token;
  doc["DEVICE_NAME"] = device_name;
  doc["DEVICE_ID"] = device_id;
  doc["CREATED"] = device_created;

  // If device_settings is a JsonArray, add it directly
  try
  {
    doc["SETTINGS"] = device_settings.as<JsonArray>();
  }
  catch(const std::exception& e)
  {
    Serial.println("Error: device_settings is not a valid array");
  }
  
  if (serializeJson(doc, config_file) == 0) {
      Serial.println("Failed to write to file");
  } else {
      Serial.println("Configuration saved successfully");
  }

  config_file.close();
}

void loadConfig(){
  File config_file = LittleFS.open("/config.json", "r");
  if (!config_file) {
    Serial.println("Failed to open config file");
    return;
  }

  size_t size = config_file.size();

  std::unique_ptr<char[]> buffer(new char[size]);
  config_file.readBytes(buffer.get(), size);
  config_file.close();

  DeserializationError error = deserializeJson(doc, buffer.get());

  if (error) {
    Serial.println("Failed to parse JSON");
    return;
  }

  wifi_ssid = doc["WIFI_SSID"].as<const char*>();
  wifi_password = doc["WIFI_PASS"].as<const char*>();
  api_url = doc["API_URL"].as<const char*>();
  device_type = doc["TYPE"].as<int>();
  api_token = doc["TOKEN"].as<const char*>();
  device_name = doc["DEVICE_NAME"].as<const char*>();
  device_id = doc["DEVICE_ID"].as<int>();
  device_created = doc["CREATED"].as<bool>();
  
  
  Serial.println("Parsed config.json:");
  Serial.println("SSID: " + String(wifi_ssid));
  Serial.println("Password: " + String(wifi_password));
  Serial.println("API URL: " + String(api_url));
  Serial.println("Device Type: " + String(device_type));
  Serial.println("Token: " + String(api_token));
  Serial.println("Device Name: " + String(device_name));
  Serial.println("Device ID: " + String(device_id));
  Serial.println("Created: " + String(device_created));

  // Check if "SETTINGS" is a valid array
  if (doc["SETTINGS"].is<JsonArray>()) {
    JsonArray settingsArray = doc["SETTINGS"].as<JsonArray>();

    // Iterate over the settings array
    for (JsonObject setting : settingsArray) {
      int type = setting["type"];
      const char* type_name = setting["type_name"];
      const char* unit = setting["unit"];
      float value = setting["value"];

      // Print the parsed settings data
      Serial.println("Type: " + String(type) + " Type Name: " + String(type_name) +
                     " Unit: " + String(unit) + " Value: " + String(value));
    }
  } else {
    Serial.println("Settings field is not an array or is missing.");
  }
}

void _testLittleFs(){
  if(!LittleFS.begin(true)){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  
  File file = LittleFS.open("/config.json");
  if(!file){
    Serial.println("Failed to open file for reading");
    return;
  }
  
  Serial.println("File Content:");
  while(file.available()){
    Serial.write(file.read());
  }
  file.close();
}

void startAccessPoint(){
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("Access Point Created");
  Serial.println("IP Address: " + WiFi.softAPIP().toString());
  Serial.println("AP MAC Address: " + WiFi.softAPmacAddress());
}

String getMacAddress(){
  return WiFi.macAddress();
}

void listenForWiFiConfig(){
  wifiReconnectTicker.resume();
  server.begin();
  Serial.println("Server started, waiting for clients...");
  while (!CONNECTED_TO_WIFI){
    wifiReconnectTicker.update();
    WiFiClient client = server.available();

    if (client){
      Serial.println("Client connected!");

      // waiting for client to send data
      String request = "";
      while(client.connected()){
        if (client.available()){
          char c = client.read();
          request += c;
          if (c == '\n') break;
        }
      }

      Serial.println("Received request:");
      Serial.println(request);

      int action = 0;
      String ssid = "";
      String password = "";
      String name = "";
      String token = "";

      if (
        request.indexOf("ACTION=") >= 0
          && request.indexOf("SSID=") > 0 
          && request.indexOf("PASSWORD=") > 0
          && request.indexOf("DEVICE_NAME=") > 0
          && request.indexOf("TOKEN=") > 0) {
        action = request.substring(request.indexOf("ACTION=") + 7, request.indexOf(';', request.indexOf("ACTION="))).toInt();
        ssid = request.substring(request.indexOf("SSID=") + 5, request.indexOf(';', request.indexOf("SSID=")));
        password = request.substring(request.indexOf("PASSWORD=") + 9, request.indexOf(';', request.indexOf("PASSWORD=")));

        if (action == 0) { // configuring a NEW DEVICE
            resetConfig();
            name = request.substring(request.indexOf("DEVICE_NAME=") + 12, request.indexOf(';', request.indexOf("DEVICE_NAME=")));
            token = request.substring(request.indexOf("TOKEN=") + 6);
            token.replace("\n", "");

            Serial.println("Parsed Device Name: " + name);
            Serial.println("Parsed Token: " + token);
            device_name = name;
            api_token = token;
        }       
        // Print received values
        Serial.println("Parsed SSID: " + ssid);
        Serial.println("Parsed Password: " + password);

        // Update the configuration variables
        wifi_ssid = ssid;
        wifi_password = password;


        // Update the configuration file
        updateConfig();
        ESP.restart();
      } else {
        Serial.println("Error parsing configuration");
        client.println("Failed to configure Wi-Fi");
      }

      // Close the connection
      client.stop();
      Serial.println("Client disconnected");
    }
  }
  wifiReconnectTicker.pause();
  server.stop();
  Serial.println("Server stopped");
}
