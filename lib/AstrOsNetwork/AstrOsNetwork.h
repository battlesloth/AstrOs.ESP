#ifndef ASTROSNETWORK_H
#define ASTROSNETWORK_H


#include "AstrOsUtility.h" 

#include <esp_http_server.h>
#include <freertos/event_groups.h>
// needed for QueueHandle_t, must be in this order
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class AstrOsNetwork
{

private:  
    const char* ssid;
    const char* password;
    httpd_handle_t webServer;
    bool startApWebServer();
    bool stopApWebServer();
    bool startStaWebServer();
    bool stopStaWebServer();
public:
    AstrOsNetwork();
    ~AstrOsNetwork();
    static QueueHandle_t serviceQueue;
    static QueueHandle_t animationQueue;
    static QueueHandle_t hardwareQueue;
    static EventGroupHandle_t wifiEvenGroup;
    esp_err_t init(const char *ssid, const char *password, QueueHandle_t serviceQueue, QueueHandle_t animationQueue, QueueHandle_t hardwareQueue);
    bool startWifiAp();
    bool stopWifiAp();
    bool connectToNetwork(const char *ssid, const char *password);
    bool disconnectFromNetwork();
};

extern AstrOsNetwork astrOsNetwork;

#endif