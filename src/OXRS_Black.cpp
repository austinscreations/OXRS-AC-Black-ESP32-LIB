/*
 * OXRS_Black.cpp
 */

#include "Arduino.h"
#include "OXRS_Black.h"

#include <Wire.h>                     // For I2C
#include <Ethernet.h>                 // For networking
#include <WiFi.h>                     // Required for Ethernet to get MAC
#include <LittleFS.h>                 // For file system access
#include <MqttLogger.h>               // For logging

// Macro for converting env vars to strings
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

// Network client (for MQTT)/server (for REST API)
EthernetClient _client;
EthernetServer _server(REST_API_PORT);

// MQTT client
PubSubClient _mqttClient(_client);
OXRS_MQTT _mqtt(_mqttClient);

// REST API
OXRS_API _api(_mqtt);

// LCD screen
OXRS_LCD _screen(Ethernet, _mqtt);

// Logging (topic updated once MQTT connects successfully)
MqttLogger _logger(_mqttClient, "log", MqttLoggerMode::MqttAndSerial);

// Firmware logo
const uint8_t * _fwLogo;
 
// Supported firmware config and command schemas
JsonDocument _fwConfigSchema;
JsonDocument _fwCommandSchema;

// MQTT callbacks wrapped by _mqttConfig/_mqttCommand
jsonCallback _onConfig;
jsonCallback _onCommand;

/* JSON helpers */
void _mergeJson(JsonVariant dst, JsonVariantConst src)
{
  if (src.is<JsonObjectConst>())
  {
    for (JsonPairConst kvp : src.as<JsonObjectConst>())
    {
      if (dst[kvp.key()])
      {
        _mergeJson(dst[kvp.key()], kvp.value());
      }
      else
      {
        dst[kvp.key()] = kvp.value();
      }
    }
  }
  else
  {
    dst.set(src);
  }
}

/* Adoption info builders */
void _getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json["firmware"].to<JsonObject>();

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);
  
#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void _getSystemJson(JsonVariant json)
{
  JsonObject system = json["system"].to<JsonObject>();

  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();
  system["flashChipSizeBytes"] = ESP.getFlashChipSize();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  system["fileSystemUsedBytes"] = LittleFS.usedBytes();
  system["fileSystemTotalBytes"] = LittleFS.totalBytes();
}

void _getNetworkJson(JsonVariant json)
{
  JsonObject network = json["network"].to<JsonObject>();

  byte mac[6];
  Ethernet.MACAddress(mac);

  network["mode"] = "ethernet";
  network["ip"] = Ethernet.localIP();

  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  network["mac"] = mac_display;
}

void _getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json["configSchema"].to<JsonObject>();
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema["properties"].to<JsonObject>();

  // Firmware config schema (if any)
  if (!_fwConfigSchema.isNull())
  {
    _mergeJson(properties, _fwConfigSchema.as<JsonVariant>());
  }

  // LCD config
  JsonObject activeBrightnessPercent = properties["activeBrightnessPercent"].to<JsonObject>();
  activeBrightnessPercent["title"] = "LCD Active Brightness (%)";
  activeBrightnessPercent["description"] = "Brightness of the LCD when active (defaults to 100%). Must be a number between 0 and 100.";
  activeBrightnessPercent["type"] = "integer";
  activeBrightnessPercent["minimum"] = 0;
  activeBrightnessPercent["maximum"] = 100;
 
  JsonObject inactiveBrightnessPercent = properties["inactiveBrightnessPercent"].to<JsonObject>();
  inactiveBrightnessPercent["title"] = "LCD Inactive Brightness (%)";
  inactiveBrightnessPercent["description"] = "Brightness of the LCD when in-active (defaults to 10%). Must be a number between 0 and 100.";
  inactiveBrightnessPercent["type"] = "integer";
  inactiveBrightnessPercent["minimum"] = 0;
  inactiveBrightnessPercent["maximum"] = 100;

  JsonObject activeDisplaySeconds = properties["activeDisplaySeconds"].to<JsonObject>();
  activeDisplaySeconds["title"] = "LCD Active Display Timeout (seconds)";
  activeDisplaySeconds["description"] = "How long the LCD remains 'active' after an event is detected (defaults to 10 seconds, setting to 0 disables the timeout). Must be a number between 0 and 600 (i.e. 10 minutes).";
  activeDisplaySeconds["type"] = "integer";
  activeDisplaySeconds["minimum"] = 0;
  activeDisplaySeconds["maximum"] = 600;

  JsonObject eventDisplaySeconds = properties["eventDisplaySeconds"].to<JsonObject>();
  eventDisplaySeconds["title"] = "LCD Event Display Timeout (seconds)";
  eventDisplaySeconds["description"] = "How long the last event is displayed on the LCD (defaults to 3 seconds, setting to 0 disables the timeout). Must be a number between 0 and 600 (i.e. 10 minutes).";
  eventDisplaySeconds["type"] = "integer";
  eventDisplaySeconds["minimum"] = 0;
  eventDisplaySeconds["maximum"] = 600;
}

void _getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json["commandSchema"].to<JsonObject>();
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema["properties"].to<JsonObject>();

  // Firmware command schema (if any)
  if (!_fwCommandSchema.isNull())
  {
    _mergeJson(properties, _fwCommandSchema.as<JsonVariant>());
  }

  // Rack32 commands
  JsonObject restart = properties["restart"].to<JsonObject>();
  restart["title"] = "Restart";
  restart["type"] = "boolean";
}

/* API callbacks */
void _apiAdopt(JsonVariant json)
{
  // Build device adoption info
  _getFirmwareJson(json);
  _getSystemJson(json);
  _getNetworkJson(json);
  _getConfigSchemaJson(json);
  _getCommandSchemaJson(json);
}

/* MQTT callbacks */
void _mqttConnected() 
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  _logger.setTopic(_mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  JsonDocument json;
  _mqtt.publishAdopt(_api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  _logger.println("[black] mqtt connected");
}

void _mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      _logger.println(F("[black] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      _logger.println(F("[black] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      _logger.println(F("[black] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      _logger.println(F("[black] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      _logger.println(F("[black] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      _logger.println(F("[black] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      _logger.println(F("[black] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      _logger.println(F("[black] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      _logger.println(F("[black] mqtt unauthorised"));
      break;      
  }
}

void _mqttConfig(JsonVariant json)
{
  // LCD config
  if (json.containsKey("activeBrightnessPercent"))
  {
    _screen.setBrightnessOn(json["activeBrightnessPercent"].as<int>());
  }

  if (json.containsKey("inactiveBrightnessPercent"))
  {
    _screen.setBrightnessDim(json["inactiveBrightnessPercent"].as<int>());
  }

  if (json.containsKey("activeDisplaySeconds"))
  {
    _screen.setOnTimeDisplay(json["activeDisplaySeconds"].as<int>());
  }
  
  if (json.containsKey("eventDisplaySeconds"))
  {
    _screen.setOnTimeEvent(json["eventDisplaySeconds"].as<int>());
  }

  // Pass on to the firmware callback
  if (_onConfig) { _onConfig(json); }
}

void _mqttCommand(JsonVariant json)
{
  // Check for Rack32 commands
  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }

  // Pass on to the firmware callback
  if (_onCommand) { _onCommand(json); }
}

void _mqttCallback(char * topic, byte * payload, int length) 
{
  // Update screen
  _screen.triggerMqttRxLed();

  // Pass down to our MQTT handler and check it was processed ok
  int state = _mqtt.receive(topic, payload, length);
  switch (state)
  {
    case MQTT_RECEIVE_ZERO_LENGTH:
      _logger.println(F("[black] empty mqtt payload received"));
      break;
    case MQTT_RECEIVE_JSON_ERROR:
      _logger.println(F("[black] failed to deserialise mqtt json payload"));
      break;
    case MQTT_RECEIVE_NO_CONFIG_HANDLER:
      _logger.println(F("[black] no mqtt config handler"));
      break;
    case MQTT_RECEIVE_NO_COMMAND_HANDLER:
      _logger.println(F("[black] no mqtt command handler"));
      break;
  }
}

/* Main program */
OXRS_Black::OXRS_Black(const uint8_t * fwLogo)
{
  _fwLogo = fwLogo;
}

void OXRS_Black::begin(jsonCallback config, jsonCallback command)
{
  // Get our firmware details
  JsonDocument json;
  _getFirmwareJson(json.as<JsonVariant>());

  // Log firmware details
  _logger.print(F("[black] "));
  serializeJson(json, _logger);
  _logger.println();

  // We wrap the callbacks so we can intercept messages intended for the Rack32
  _onConfig = config;
  _onCommand = command;
  
  // Set up the screen
  _initialiseScreen();

  // Set up network and obtain an IP address
  byte mac[6];
  _initialiseNetwork(mac);

  // Set up MQTT (don't attempt to connect yet)
  _initialiseMqtt(mac);

  // Set up the REST API
  _initialiseRestApi();
}

void OXRS_Black::loop(void)
{
  // Check our network connection
  if (_isNetworkConnected())
  {
    // Maintain our DHCP lease
    Ethernet.maintain();
    
    // Handle any MQTT messages
    _mqtt.loop();
    
    // Handle any REST API requests
    EthernetClient client = _server.available();
    _api.loop(&client);
  }
    
  // Update screen
  _screen.loop();
}

void OXRS_Black::setConfigSchema(JsonVariant json)
{
  _fwConfigSchema.clear();
  _mergeJson(_fwConfigSchema.as<JsonVariant>(), json);
}

void OXRS_Black::setCommandSchema(JsonVariant json)
{
  _fwCommandSchema.clear();
  _mergeJson(_fwCommandSchema.as<JsonVariant>(), json);
}

OXRS_MQTT * OXRS_Black::getMQTT()
{
  return &_mqtt;
}

OXRS_API * OXRS_Black::getAPI()
{
  return &_api;
}

OXRS_LCD * OXRS_Black::getLCD()
{
  return &_screen;
}

bool OXRS_Black::publishStatus(JsonVariant json)
{
  // Check for something we can show on the screen
  if (json.containsKey("index"))
  {
    // Pad the index to 3 chars - to ensure a consistent display for all indices
    char event[32];
    sprintf_P(event, PSTR("[%3d]"), json["index"].as<uint8_t>());

    if (json.containsKey("type") && json.containsKey("event"))
    {
      if (strcmp(json["type"], json["event"]) == 0)
      {
        sprintf_P(event, PSTR("%s %s"), event, json["type"].as<const char *>());
      }
      else
      {
        sprintf_P(event, PSTR("%s %s %s"), event, json["type"].as<const char *>(), json["event"].as<const char *>());
      }
    }
    else if (json.containsKey("type"))
    {
      sprintf_P(event, PSTR("%s %s"), event, json["type"].as<const char *>());
    }
    else if (json.containsKey("event"))
    {
      sprintf_P(event, PSTR("%s %s"), event, json["event"].as<const char *>());
    }

    _screen.showEvent(event);
  }
  
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }

  bool success = _mqtt.publishStatus(json);
  if (success) { _screen.triggerMqttTxLed(); }
  return success;
}

bool OXRS_Black::publishTelemetry(JsonVariant json)
{
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }

  bool success = _mqtt.publishTelemetry(json);
  if (success) { _screen.triggerMqttTxLed(); }
  return success;
}

size_t OXRS_Black::write(uint8_t character)
{
  // Pass to logger - allows firmware to use `rack32.println("Log this!")`
  return _logger.write(character);
}

void OXRS_Black::_initialiseScreen(void)
{
  // Initialise the LCD
  _screen.begin();

  // Display the firmware and logo (either from SPIFFS or PROGMEM)
  int returnCode = _screen.drawHeader(FW_SHORT_NAME, FW_MAKER, STRINGIFY(FW_VERSION), "ESP32", _fwLogo);
  
  switch (returnCode)
  {
    case LCD_INFO_LOGO_FROM_SPIFFS:
      _logger.println(F("[black] logo loaded from SPIFFS"));
      break;
    case LCD_INFO_LOGO_FROM_PROGMEM:
      _logger.println(F("[black] logo loaded from PROGMEM"));
      break;
    case LCD_INFO_LOGO_DEFAULT:
      _logger.println(F("[black] no logo found, using default OXRS logo"));
      break;
    case LCD_ERR_NO_LOGO:
      _logger.println(F("[black] no logo found"));
      break;
  }
}

void OXRS_Black::_initialiseNetwork(byte * mac)
{
  // Get WiFi base MAC address
  WiFi.macAddress(mac);

  // Ethernet MAC address is base MAC + 3
  // See https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system.html#mac-address
  mac[5] += 3;

  // Format the MAC address for logging
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  _logger.print(F("[black] ethernet mac address: "));
  _logger.println(mac_display);

  // Initialise ethernet library
  Ethernet.init(ETHERNET_CS_PIN);

  // Reset Wiznet W5500
  pinMode(WIZNET_RESET_PIN, OUTPUT);
  digitalWrite(WIZNET_RESET_PIN, HIGH);
  delay(250);
  digitalWrite(WIZNET_RESET_PIN, LOW);
  delay(50);
  digitalWrite(WIZNET_RESET_PIN, HIGH);
  delay(350);

  // Connect ethernet and get an IP address via DHCP
  bool success = Ethernet.begin(mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_TIMEOUT_MS);
  
  _logger.print(F("[black] ip address: "));
  _logger.println(success ? Ethernet.localIP() : IPAddress(0, 0, 0, 0));
}

void OXRS_Black::_initialiseMqtt(byte * mac)
{
  // NOTE: this must be called *before* initialising the REST API since
  //       that will load MQTT config from file, which has precendence

  // Set the default client ID to last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  // Register our callbacks
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  // Start listening for MQTT messages
  _mqttClient.setCallback(_mqttCallback);
}

void OXRS_Black::_initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  _api.begin();
  
  // Register our callbacks
  _api.onAdopt(_apiAdopt);

  // Start listening
  _server.begin();
}

bool OXRS_Black::_isNetworkConnected(void)
{
  return Ethernet.linkStatus() == LinkON;
}