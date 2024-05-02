/*
 * OXRS_Black.h
 */

#ifndef OXRS_Black_H
#define OXRS_Black_H

#include <OXRS_MQTT.h>                // For MQTT pub/sub
#include <OXRS_API.h>                 // For REST API
#include <OXRS_LCD.h>                 // For LCD runtime displays

// LCD screen
#define       OXRS_LCD_ENABLE

// Ethernet
#define       ETHERNET_CS_PIN             5
#define       WIZNET_RESET_PIN            13
#define       DHCP_TIMEOUT_MS             15000
#define       DHCP_RESPONSE_TIMEOUT_MS    4000

// I2C
#define       I2C_SDA                     21
#define       I2C_SCL                     22

// REST API
#define       REST_API_PORT               80

class OXRS_Black : public Print
{
  public:
    OXRS_Black(const uint8_t * fwLogo = NULL);

    void begin(jsonCallback config, jsonCallback command);
    void loop(void);

    // Firmware can define the config/commands it supports - for device discovery and adoption
    void setConfigSchema(JsonVariant json);
    void setCommandSchema(JsonVariant json);

    // Return a pointer to the MQTT library
    OXRS_MQTT * getMQTT(void);

    // Return a pointer to the API library
    OXRS_API * getAPI(void);

    // Return a pointer to the LCD so firmware can customise if required
    // Should be called after .begin()
    OXRS_LCD * getLCD(void);
    
    // Helpers for publishing to stat/ and tele/ topics
    bool publishStatus(JsonVariant json);
    bool publishTelemetry(JsonVariant json);

    // Implement Print.h wrapper
    virtual size_t write(uint8_t);
    using Print::write;

  private:
    void _initialiseScreen(void);
    void _initialiseNetwork(byte * mac);
    void _initialiseMqtt(byte * mac);
    void _initialiseRestApi(void);
    
    bool _isNetworkConnected(void);
};

#endif
