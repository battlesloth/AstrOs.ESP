#include "AstrOsNetwork.h"
#include "AstrOsConstants.h"
#include "AstrOsHtml.h"
#include "StorageManager.h"

#include <stdio.h>
#include <functional>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <lwip/inet.h>
#include <mdns.h>
#include "cJSON.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define POST_BUFFER_SIZE 2000

static const char *TAG = "AstrOsNetwork";
static bool intentionalDisconnect = false;

AstrOsNetwork astrOsNetwork;

EventGroupHandle_t AstrOsNetwork::wifiEvenGroup;
QueueHandle_t AstrOsNetwork::serviceQueue;
QueueHandle_t AstrOsNetwork::animationQueue;

typedef struct rest_server_context {
    char scratch[POST_BUFFER_SIZE];
} rest_server_context_t;

AstrOsNetwork::AstrOsNetwork() {}

AstrOsNetwork::~AstrOsNetwork() {}

esp_err_t initializemDns()
{
    esp_err_t err = mdns_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return err;
    }

    err = mdns_hostname_set(AstrOsConstants::ModuleName);
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return err;
    }

    err = mdns_instance_name_set("AstrOs");
    if (logError(TAG, __FUNCTION__, __LINE__, err)){
        return err;
    }
    //structure with TXT records
    mdns_txt_item_t serviceTxtData[3] = {
        {"board", "esp32"},
        {"u", "user"},
        {"p", "password"}
    };

    //initialize service
    err = mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData, 3);
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
}

void wifiEventHandler(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData)
{
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START)
    {
        esp_err_t err = esp_wifi_connect();
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (intentionalDisconnect){
            ESP_LOGI(TAG, "WIFI STA disconnected");
            intentionalDisconnect = false;
        } 
        else 
        {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)eventData;
            ESP_LOGI(TAG, "WIFI STA disconnected, Reason:%d. Attempting to reconnect in 30 seconds", event->reason);
            vTaskDelay(pdMS_TO_TICKS(30000));
            esp_err_t err = esp_wifi_connect();
            logError(TAG, __FUNCTION__, __LINE__, err);
        }
    }
    else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)eventData;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(AstrOsNetwork::wifiEvenGroup, WIFI_CONNECTED_BIT);
        initializemDns();
    }
}

/**************************************************************
* AP URI definitions
***************************************************************/
esp_err_t apIndexHandler(httpd_req_t *req)
{
    esp_err_t err;
    const char *respStr = (const char *)req->user_ctx;
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);
    return err;
};

esp_err_t credentialsHandler(httpd_req_t *req)
{
    char *buf;
    size_t bufLen;
    esp_err_t err;

    svc_config_t svcConfig = {};

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    bufLen = httpd_req_get_url_query_len(req) + 1;
    if (bufLen > 1)
    {
        buf = (char *)malloc(bufLen);
        err = httpd_req_get_url_query_str(req, buf, bufLen);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            ESP_LOGI(TAG, "%s: Query string missing!", __FUNCTION__);
        }
        else
        {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char ssid[33];
            err = httpd_query_key_value(buf, "ssid", ssid, 33);
            logError(TAG, __FUNCTION__, __LINE__, err);

            char pass[65];
            err = httpd_query_key_value(buf, "pass", pass, 65);
            logError(TAG, __FUNCTION__, __LINE__, err);

            percentDecode(svcConfig.networkSSID, ssid);
            percentDecode(svcConfig.networkPass, pass);
        }
        free(buf);
    }

    if (Storage.saveServiceConfig(svcConfig))
    {
        const char *respStr = AstrOsHtml::SavedHtml;
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);

        queue_svc_cmd_t msg = {SERVICE_COMMAND::SWITCH_TO_NETWORK, NULL};
        xQueueSend(AstrOsNetwork::serviceQueue, &msg, pdMS_TO_TICKS(2000));
    }
    else
    {
        const char *respStr = AstrOsHtml::ErrorHtml;
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }

    return err;
}

const httpd_uri_t creds = {
    .uri = "/creds",
    .method = HTTP_GET,
    .handler = credentialsHandler,
    .user_ctx = NULL};

const httpd_uri_t apIndex = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = apIndexHandler,
    .user_ctx = (void *)AstrOsHtml::CredentialHtml};

/**************************************************************
* Access Point Webserver functions
***************************************************************/
bool AstrOsNetwork::stopApWebServer()
{
    esp_err_t err = httpd_stop(webServer);

    err = httpd_unregister_uri_handler(webServer, "/", HTTP_GET);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, "/creds", HTTP_GET);
    logError(TAG, __FUNCTION__, __LINE__, err);

    ESP_LOGI(TAG, "server stopped");
    return (err == ESP_OK);
}

bool AstrOsNetwork::startApWebServer()
{
    esp_err_t err;
    webServer = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    err = httpd_start(&webServer, &config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    err = httpd_register_uri_handler(webServer, &apIndex);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &creds);
    logError(TAG, __FUNCTION__, __LINE__, err);
    return true;
}


/**************************************************************
* Service Endpoints
***************************************************************/
esp_err_t staIndexHandler(httpd_req_t *req)
{
    esp_err_t err;
    const char *respStr = (const char *)req->user_ctx;
    err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    logError(TAG, __FUNCTION__, __LINE__, err);
    
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);
    return err;
};

const httpd_uri_t staIndex = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = staIndexHandler,
    .user_ctx = (void *)"{\"status\":\"up\"}"};

esp_err_t staClearSettingsHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "clearsettings called");

    esp_err_t err;

    err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    logError(TAG, __FUNCTION__, __LINE__, err);

    if (Storage.clearServiceConfig())
    {
        const char *respStr = "{\"success\":\"true\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);

        queue_svc_cmd_t msg = {SERVICE_COMMAND::SWITCH_TO_WIFI_AP, NULL};
        xQueueSend(AstrOsNetwork::serviceQueue, &msg, pdMS_TO_TICKS(2000));
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }

    return err;
};

const httpd_uri_t staClearSettings = {
    .uri = "/clearsettings",
    .method = HTTP_GET,
    .handler = staClearSettingsHandler,
    .user_ctx = NULL};


esp_err_t staFormatSdHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "formatsd called");

    esp_err_t err;

    err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    logError(TAG, __FUNCTION__, __LINE__, err);

    if (Storage.formatSdCard())
    {
        const char *respStr = "{\"success\":\"true\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }

    return err;
};

const httpd_uri_t staFormatSd = {
    .uri = "/formatsd",
    .method = HTTP_GET,
    .handler = staFormatSdHandler,
    .user_ctx = NULL};

/**************************************************************
* Script endpoints
***************************************************************/

esp_err_t staPanicStopHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "panicstop called");

    queue_ani_cmd_t msg = {ANIMATION_COMMAND::PANIC_STOP, NULL};
    xQueueSend(AstrOsNetwork::animationQueue, &msg, pdMS_TO_TICKS(2000));

    const char *respStr = "{\"result\":\"panic stop sent\"}";
    esp_err_t  err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
};

const httpd_uri_t staPanicStop = {
    .uri = "/panicstop",
    .method = HTTP_GET,
    .handler = staPanicStopHandler,
    .user_ctx = NULL};


esp_err_t staScriptExistsHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "scriptexists called");

    char *buf;
    size_t bufLen;
    esp_err_t err = ESP_OK;
    char scriptId[37];
    
    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    bufLen = httpd_req_get_url_query_len(req) + 1;
    if (bufLen > 1)
    {
        buf = (char *)malloc(bufLen);
        err = httpd_req_get_url_query_str(req, buf, bufLen);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            ESP_LOGI(TAG, "%s: Query string missing!", __FUNCTION__);
        }
        else
        {
            ESP_LOGI(TAG, "Found URL query => %s", buf);        
            err = httpd_query_key_value(buf, "scriptId", scriptId, 37);
            logError(TAG, __FUNCTION__, __LINE__, err);
        }
        free(buf);
    }

    char path[8 + strlen(scriptId) + 1];
    memset(path, 0, sizeof(path));

    strcpy(path, "scripts/");
    strcat(path, scriptId);

    ESP_LOGI(TAG, "%s", path);

    if (Storage.fileExists(path)){
        const char *respStr = "{\"result\":\"true\"}";
        esp_err_t  err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    } else {
        const char *respStr = "{\"result\":\"false\"}";
        esp_err_t  err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    
    return err;
};

const httpd_uri_t staScriptExists = {
    .uri = "/scriptexists",
    .method = HTTP_GET,
    .handler = staScriptExistsHandler,
    .user_ctx = NULL};


esp_err_t staRunScriptHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "runscript called");

    char *buf;
    size_t bufLen;
    esp_err_t err;
    char scriptId[37];
    
    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    bufLen = httpd_req_get_url_query_len(req) + 1;
    if (bufLen > 1)
    {
        buf = (char *)malloc(bufLen);
        err = httpd_req_get_url_query_str(req, buf, bufLen);
        if (logError(TAG, __FUNCTION__, __LINE__, err))
        {
            ESP_LOGI(TAG, "%s: Query string missing!", __FUNCTION__);
        }
        else
        {
            ESP_LOGI(TAG, "Found URL query => %s", buf);        
            err = httpd_query_key_value(buf, "scriptId", scriptId, 37);
            logError(TAG, __FUNCTION__, __LINE__, err);
        }
        free(buf);
    }

    queue_ani_cmd_t msg = {ANIMATION_COMMAND::RUN_ANIMATION, NULL};
    msg.data = scriptId;
    xQueueSend(AstrOsNetwork::animationQueue, &msg, pdMS_TO_TICKS(2000));

    const char *respStr = "{\"result\":\"script queued\"}";
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
};

const httpd_uri_t staRunScript = {
    .uri = "/runscript",
    .method = HTTP_GET,
    .handler = staRunScriptHandler,
    .user_ctx = NULL};


esp_err_t staListScriptsHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "listscripts called");

    esp_err_t err;

    err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    logError(TAG, __FUNCTION__, __LINE__, err);

    if (Storage.listFiles("scripts"))
    {
        const char *respStr = "{\"success\":\"true\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }

    return err;
};

const httpd_uri_t staListScripts = {
    .uri = "/listscripts",
    .method = HTTP_GET,
    .handler = staListScriptsHandler,
    .user_ctx = NULL};

esp_err_t staUploadScriptHandler(httpd_req_t *req){

    ESP_LOGI(TAG, "uploadscript called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post script");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    char* filenameTemp = cJSON_GetObjectItem(root, "scriptId")->valuestring;

    char* scriptTemp = cJSON_GetObjectItem(root, "script")->valuestring;

    char filename[strlen(filenameTemp) + 1];
    char script[strlen(scriptTemp) + 1];

    strncpy(filename, filenameTemp, strlen(filenameTemp) + 1);
    strncpy(script, scriptTemp, strlen(scriptTemp) + 1);

    cJSON_Delete(root);

    ESP_LOGI(TAG, "ID: %s, DATA: %s", filename, script);

    char path[8 + strlen(filename) + 1];
    memset(path, 0, sizeof(path));

    strcpy(path, "scripts/");
    strcat(path, filename);

    bool result = Storage.saveFile(path, script);

    if (!result){
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save script");
        return ESP_FAIL;
    }
        
    httpd_resp_sendstr(req, "Script saved");
    return ESP_OK;
}

const httpd_uri_t staUploadScript = {
    .uri = "/uploadscript",
    .method = HTTP_POST,
    .handler = staUploadScriptHandler,
    .user_ctx = NULL};

/**************************************************************
* STA Webserver functions
***************************************************************/

bool AstrOsNetwork::stopStaWebServer()
{
    esp_err_t err = httpd_stop(webServer);

    err = httpd_unregister_uri_handler(webServer, "/", HTTP_GET);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staClearSettings.uri, staClearSettings.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staFormatSd.uri, staFormatSd.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staListScripts.uri, staListScripts.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staUploadScript.uri, staUploadScript.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staRunScript.uri, staRunScript.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staPanicStop.uri, staPanicStop.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staScriptExists.uri, staScriptExists.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    ESP_LOGI(TAG, "server stopped");
    return (err == ESP_OK);
}

bool AstrOsNetwork::startStaWebServer()
{
    esp_err_t err;
    webServer = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.stack_size=20480;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    err = httpd_start(&webServer, &config);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    err = httpd_register_uri_handler(webServer, &staIndex);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staClearSettings);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staFormatSd);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staListScripts);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staUploadScript);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staRunScript);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staPanicStop);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staScriptExists);
    logError(TAG, __FUNCTION__, __LINE__, err);
    
    return true;
}

/**************************************************************
* 
***************************************************************/
esp_err_t AstrOsNetwork::init(const char *network, const char *pass, QueueHandle_t sQueue, QueueHandle_t aQueue)
{
    serviceQueue = sQueue;
    animationQueue = aQueue;
    ssid = network;
    password = pass;

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    esp_err_t err = esp_wifi_init(&cfg);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, NULL);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, NULL);
    logError(TAG, __FUNCTION__, __LINE__, err);

    return err;
}

bool AstrOsNetwork::startWifiAp()
{

    wifi_config_t wifiConfig = {};

    memcpy(wifiConfig.ap.ssid, ssid, strlen(ssid));
    memcpy(wifiConfig.ap.password, password, strlen(password));

    wifiConfig.ap.ssid_len = strlen(ssid);
    wifiConfig.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifiConfig.ap.max_connection = 1;

    if (strlen(password) == 0)
    {
        wifiConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config((wifi_interface_t)ESP_IF_WIFI_AP, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    AstrOsNetwork::startApWebServer();

    ESP_LOGI(TAG, "WIFI AP started");
    return true;
}

bool AstrOsNetwork::stopWifiAp()
{
    ESP_LOGI(TAG, "Stopping WIFI AP");
    AstrOsNetwork::stopApWebServer();
    esp_err_t err = esp_wifi_stop();
    logError(TAG, __FUNCTION__, __LINE__, err);
    ESP_LOGI(TAG, "WIFI AP stopped");
    return err == ESP_OK;
}

bool AstrOsNetwork::connectToNetwork(const char *network, const char *pass)
{
    wifi_config_t wifiConfig = {};
    esp_err_t err;

    AstrOsNetwork::wifiEvenGroup = xEventGroupCreate();

    memcpy(wifiConfig.sta.ssid, network, strlen(network));
    memcpy(wifiConfig.sta.password, pass, strlen(pass));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }
    err = esp_wifi_set_config((wifi_interface_t)ESP_IF_WIFI_STA, &wifiConfig);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }
    err = esp_wifi_start();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return false;
    }

    ESP_LOGI(TAG, "Wifi sta init finished. Waiting for connection...");

    EventBits_t bits = xEventGroupWaitBits(wifiEvenGroup,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to SSID:%s", network);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", network);
    }
    else
    {
        ESP_LOGE(TAG, "Unexpected event connecting to SSID:%s", network);
    }

    vEventGroupDelete(AstrOsNetwork::wifiEvenGroup);

    AstrOsNetwork::startStaWebServer();

    return true;
}

bool AstrOsNetwork::disconnectFromNetwork()
{
    ESP_LOGI(TAG, "Disconnecting from WIFI network");
    intentionalDisconnect = true;
    AstrOsNetwork::stopStaWebServer();
    esp_err_t err = esp_wifi_disconnect();
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = esp_wifi_stop();
    logError(TAG, __FUNCTION__, __LINE__, err);
    ESP_LOGI(TAG, "WIFI netowrk disconnected");
    return err == ESP_OK;
}
