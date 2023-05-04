#include "AstrOsNetwork.h"
#include "AstrOsConstants.h"
#include "AstrOsHtml.h"
#include "StorageManager.h"
#include "uuid.h"
#include "DisplayCommand.h"

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
QueueHandle_t AstrOsNetwork::hardwareQueue;

typedef struct rest_server_context
{
    char scratch[POST_BUFFER_SIZE];
} rest_server_context_t;

AstrOsNetwork::AstrOsNetwork() {}

AstrOsNetwork::~AstrOsNetwork() {}

esp_err_t initializemDns()
{
    esp_err_t err = mdns_init();
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = mdns_hostname_set(AstrOsConstants::ModuleName);
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }

    err = mdns_instance_name_set("AstrOs");
    if (logError(TAG, __FUNCTION__, __LINE__, err))
    {
        return err;
    }
    // structure with TXT records
    mdns_txt_item_t serviceTxtData[3] = {
        {"board", "esp32"},
        {"u", "user"},
        {"p", "password"}};

    // initialize service
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
        if (intentionalDisconnect)
        {
            ESP_LOGI(TAG, "WIFI STA disconnected");
            intentionalDisconnect = false;
        }
        else
        {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)eventData;
            ESP_LOGI(TAG, "WIFI STA disconnected, Reason:%d. Attempting to reconnect in 30 seconds", event->reason);
            vTaskDelay(pdMS_TO_TICKS(30000));
            
            wifi_mode_t mode; 
            esp_err_t modeErr = esp_wifi_get_mode(&mode);
            logError(TAG, __FUNCTION__, __LINE__, modeErr);
            
            // we may have reset the wifi to AP mode during the delay
            if (mode != WIFI_MODE_AP){
                esp_err_t err = esp_wifi_connect();
                logError(TAG, __FUNCTION__, __LINE__, err);
            } else {
                ESP_LOGI(TAG, "Was going to try to reconnect to WIFI, but AP mode is active.");
                intentionalDisconnect = false;
                return;
            }
        }

        queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
        DisplayCommand cmd = DisplayCommand();
        cmd.setLine(1, "WIFI");
        cmd.setLine(3, "Disconnected");
        strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
        msg.data[sizeof(msg.data) - 1] = '\0';
        xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));
    }
    else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)eventData;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(AstrOsNetwork::wifiEvenGroup, WIFI_CONNECTED_BIT);
        initializemDns();

        queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
        DisplayCommand cmd = DisplayCommand();
        cmd.setLine(1, "Connected");
        char buff[100];
        snprintf(buff, sizeof(buff), IPSTR, IP2STR(&event->ip_info.ip));
        std::string ip = buff;
        cmd.setLine(3, ip);
        strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
        msg.data[sizeof(msg.data) - 1] = '\0';
        xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));
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
            char _ssid[33];
            err = httpd_query_key_value(buf, "ssid", _ssid, 33);
            logError(TAG, __FUNCTION__, __LINE__, err);

            char pass[65];
            err = httpd_query_key_value(buf, "pass", pass, 65);
            logError(TAG, __FUNCTION__, __LINE__, err);

            percentDecode(svcConfig.networkSSID, _ssid);
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

    char fingerprint[37];

    Storage.getControllerFingerprint(fingerprint);

    std::stringstream ss;
    ss << "{\"result\":\"up\",\"fingerprint\":\"";
    ss << fingerprint;
    ss << "\"}";

    std::string respStr = ss.str();

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, respStr.c_str(), strlen(respStr.c_str()));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return ESP_OK;
};

const httpd_uri_t staIndex = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = staIndexHandler,
    .user_ctx = NULL};

esp_err_t staClearSettingsHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "clearsettings called");

    esp_err_t err;

    err = httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    logError(TAG, __FUNCTION__, __LINE__, err);

    if (Storage.clearServiceConfig())
    {

        const char *respStr = "{\"success\":\"true\"}";
        httpd_resp_set_type(req, "application/json");
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);

        queue_svc_cmd_t msg = {SERVICE_COMMAND::SWITCH_TO_WIFI_AP, NULL};
        xQueueSend(AstrOsNetwork::serviceQueue, &msg, pdMS_TO_TICKS(2000));
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        httpd_resp_set_type(req, "application/json");
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
        httpd_resp_set_type(req, "application/json");
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        httpd_resp_set_type(req, "application/json");
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
 * Hardware Control endpoints
 ***************************************************************/

esp_err_t staSetConfigHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Set Config called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    if (total_len == 0)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no content");
        return ESP_FAIL;
    }
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0)
        {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post servo config");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    servo_channel config0[16];
    servo_channel config1[16];

    cJSON *root = cJSON_Parse(buf);

    cJSON *channels = NULL;
    channels = cJSON_GetObjectItem(root, "servoChannels");

    int id;
    int minPos;
    int maxPos;
    bool set;
    bool inverted;

    const cJSON *channel = NULL;

    cJSON_ArrayForEach(channel, channels)
    {
        id = cJSON_GetObjectItem(channel, "id")->valueint;
        minPos = cJSON_GetObjectItem(channel, "minPos")->valueint;
        maxPos = cJSON_GetObjectItem(channel, "maxPos")->valueint;
        set = cJSON_GetObjectItem(channel, "set")->valueint;
        inverted = cJSON_GetObjectItem(channel, "inverted")->valueint;

        servo_channel ch;

        ch.minPos = minPos;
        ch.maxPos = maxPos;
        ch.set = set;
        ch.inverted = inverted;

        if (id < 16)
        {
            ch.id = id;
            config0[id] = ch;
            ESP_LOGI(TAG, "Servo Config: brd 1, ch: %d", ch.id);
        }
        else
        {
            ch.id = id - 16;
            config1[(id - 16)] = ch;
            ESP_LOGI(TAG, "Servo Config: brd 2, ch: %d", ch.id);
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Saving Servo Config...");

    if (!Storage.saveServoConfig(0, config0, 16))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save servo config");
        return ESP_FAIL;
    }

    if (!Storage.saveServoConfig(1, config1, 16))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save servo config");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Servo Config saved!");

    ESP_LOGI(TAG, "Updating Fingerprint");

    std::string fingerprint = uuid::generate_uuid_v4();

    if (!Storage.setControllerFingerprint(fingerprint.c_str()))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save servo config");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "New Fingerprint: %s", fingerprint.c_str());

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::LOAD_SERVO_CONFIG, NULL};
    xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));

    esp_err_t err;

    std::stringstream ss;
    ss << "{\"result\":\"config saved\",\"fingerprint\":\"";
    ss << fingerprint;
    ss << "\"}";

    std::string respStr = ss.str();

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, respStr.c_str(), strlen(respStr.c_str()));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return ESP_OK;
}

const httpd_uri_t staSetConfig = {
    .uri = "/setconfig",
    .method = HTTP_POST,
    .handler = staSetConfigHandler,
    .user_ctx = NULL};

esp_err_t staMoveServoHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "move servo called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    if (total_len == 0)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no content");
        return ESP_FAIL;
    }
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0)
        {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post script");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    int servoId = cJSON_GetObjectItem(root, "id")->valueint;

    int position = cJSON_GetObjectItem(root, "position")->valueint;

    int speed = cJSON_GetObjectItem(root, "speed")->valueint;

    cJSON_Delete(root);

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::MOVE_SERVO, NULL};
    snprintf(msg.data, sizeof(msg.data), "1|0|%d|%d|%d", servoId, position, speed);
    msg.data[sizeof(msg.data) - 1] = '\0';
    xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));

    esp_err_t err;
    const char *respStr = "{\"result\":\"move queued\"}";
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return ESP_OK;
};

const httpd_uri_t staMoveServo = {
    .uri = "/moveservo",
    .method = HTTP_POST,
    .handler = staMoveServoHandler,
    .user_ctx = NULL};

esp_err_t staSendI2cHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "send I2C called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    if (total_len == 0)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no content");
        return ESP_FAIL;
    }
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0)
        {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post script");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    int i2cCh = cJSON_GetObjectItem(root, "id")->valueint;

    std::string cmd = cJSON_GetObjectItem(root, "val")->valuestring;

    cJSON_Delete(root);

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::SEND_I2C, NULL};
    snprintf(msg.data, sizeof(msg.data), "2|0|%d|%s", i2cCh, cmd.c_str());
    msg.data[sizeof(msg.data) - 1] = '\0';
    xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));

    esp_err_t err;
    const char *respStr = "{\"result\":\"command queued\"}";
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return ESP_OK;
};

const httpd_uri_t staSendI2c = {
    .uri = "/sendi2c",
    .method = HTTP_POST,
    .handler = staSendI2cHandler,
    .user_ctx = NULL};

esp_err_t staSendSerialHandler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "send serial called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    if (total_len == 0)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no content");
        return ESP_FAIL;
    }
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0)
        {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post script");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    int ch = cJSON_GetObjectItem(root, "ch")->valueint;
    std::string cmd = cJSON_GetObjectItem(root, "val")->valuestring;

    cJSON_Delete(root);

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::SEND_SERIAL, NULL};
    snprintf(msg.data, sizeof(msg.data), "3|0|%d|%s", ch, cmd.c_str());
    msg.data[sizeof(msg.data) - 1] = '\0';

    //queue_hw_cmd_t msg = {HARDWARE_COMMAND::SEND_SERIAL, NULL};
    //strncpy(msg.data, cmd.c_str(), sizeof(msg.data));
    //msg.data[cmd.length()] = '\n';
    //msg.data[cmd.length() + 1] = '\0';
    xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));

    esp_err_t err;
    const char *respStr = "{\"result\":\"command queued\"}";
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, respStr, strlen(respStr));
    logError(TAG, __FUNCTION__, __LINE__, err);

    return ESP_OK;
};

const httpd_uri_t staSendSerial = {
    .uri = "/sendserial",
    .method = HTTP_POST,
    .handler = staSendSerialHandler,
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
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, respStr, strlen(respStr));
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

    std::string path = "scripts/" + std::string(scriptId);

    if (Storage.fileExists(path))
    {
        const char *respStr = "{\"result\":\"true\"}";
        httpd_resp_set_type(req, "application/json");
        esp_err_t err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else
    {
        const char *respStr = "{\"result\":\"false\"}";
        httpd_resp_set_type(req, "application/json");
        esp_err_t err = httpd_resp_send(req, respStr, strlen(respStr));
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
    strncpy(msg.data, scriptId, sizeof(msg.data));
    msg.data[sizeof(msg.data) - 1] = '\0';
    xQueueSend(AstrOsNetwork::animationQueue, &msg, pdMS_TO_TICKS(2000));

    const char *respStr = "{\"result\":\"script queued\"}";
    httpd_resp_set_type(req, "application/json");
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

    std::vector<std::string> files = Storage.listFiles("scripts");

    ESP_LOGI(TAG, "scripts found: %d", files.size());

    if (!files.empty())
    {
        char *response = NULL;

        cJSON *status = NULL;
        cJSON *fileArray = NULL;
        cJSON *filename = NULL;

        cJSON *body = cJSON_CreateObject();
        if (body == NULL)
        {
            goto end;
        }

        status = cJSON_CreateString("success");
        if (status == NULL)
        {
            goto end;
        }
        cJSON_AddItemToObject(body, "true", status);

        fileArray = cJSON_CreateArray();
        if (fileArray == NULL)
        {
            goto end;
        }

        cJSON_AddItemToObject(body, "scripts", fileArray);

        for (auto &element : files)
        {
            filename = cJSON_CreateString(element.c_str());
            if (status == NULL)
            {
                goto end;
            }
            cJSON_AddItemToArray(fileArray, filename);
        }

        response = cJSON_Print(body);

    end:
        cJSON_Delete(body);
        if (response == NULL)
        {
            response = "{\"success\":\"false\"}";
        }

        const char *respStr = response;
        httpd_resp_set_type(req, "application/json");
        err = httpd_resp_send(req, respStr, strlen(respStr));
        logError(TAG, __FUNCTION__, __LINE__, err);
    }
    else
    {
        const char *respStr = "{\"success\":\"false\"}";
        httpd_resp_set_type(req, "application/json");
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

esp_err_t staUploadScriptHandler(httpd_req_t *req)
{

    ESP_LOGI(TAG, "uploadscript called");

    int total_len = req->content_len;
    ESP_LOGI(TAG, "total_len: %d", total_len);
    if (total_len == 0)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no content");
        return ESP_FAIL;
    }
    int cur_len = 0;
    char buf[2000]; // = ((rest_server_context_t *)(req->user_ctx))->scratch;
    memset(buf, 0, sizeof(buf));
    int received = 0;
    if (total_len >= POST_BUFFER_SIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0)
        {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post script");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    char *filename = cJSON_GetObjectItem(root, "scriptId")->valuestring;

    char *scriptTemp = cJSON_GetObjectItem(root, "script")->valuestring;

    std::string path = "scripts/" + std::string(filename);
    std::string script = std::string(scriptTemp);

    cJSON_Delete(root);

    bool result = Storage.saveFile(path, script);

    if (!result)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save script");
        return ESP_FAIL;
    }

    const char *respStr = "{\"success\":\"true\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, respStr);
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
    esp_err_t err = httpd_unregister_uri(webServer, "/");
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staClearSettings.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staFormatSd.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staSetConfig.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staMoveServo.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staSendI2c.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staSendSerial.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staListScripts.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staUploadScript.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staRunScript.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri(webServer, staPanicStop.uri);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_unregister_uri_handler(webServer, staScriptExists.uri, staScriptExists.method);
    logError(TAG, __FUNCTION__, __LINE__, err);

    err = httpd_stop(webServer);
    
    ESP_LOGI(TAG, "server stopped");
    return (err == ESP_OK);
}

bool AstrOsNetwork::startStaWebServer()
{
    esp_err_t err;
    webServer = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.stack_size = 20480;
    config.max_uri_handlers = 12;

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
    err = httpd_register_uri_handler(webServer, &staSetConfig);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staMoveServo);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staSendI2c);
    logError(TAG, __FUNCTION__, __LINE__, err);
    err = httpd_register_uri_handler(webServer, &staSendSerial);
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
esp_err_t AstrOsNetwork::init(const char *network, const char *pass, QueueHandle_t sQueue, QueueHandle_t aQueue, QueueHandle_t hQueue)
{

    ESP_LOGI(TAG, "%s", network);
    serviceQueue = sQueue;
    animationQueue = aQueue;
    hardwareQueue = hQueue;
    AstrOsNetwork::ssid = network;
    AstrOsNetwork::password = pass;

    ESP_LOGI(TAG, "%s", AstrOsNetwork::ssid.c_str());
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
    ESP_LOGI(TAG, "%s", AstrOsNetwork::ssid.c_str());
    memcpy(wifiConfig.ap.ssid, AstrOsNetwork::ssid.c_str(), AstrOsNetwork::ssid.size());
    memcpy(wifiConfig.ap.password, AstrOsNetwork::password.c_str(), AstrOsNetwork::password.size());

    wifiConfig.ap.ssid_len = AstrOsNetwork::ssid.size();
    wifiConfig.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifiConfig.ap.max_connection = 1;

    if (password.size() == 0)
    {
        wifiConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config((wifi_interface_t)ESP_IF_WIFI_AP, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    AstrOsNetwork::startApWebServer();

    queue_hw_cmd_t msg = {HARDWARE_COMMAND::DISPLAY_COMMAND, NULL};
    DisplayCommand cmd = DisplayCommand();
    cmd.setLine(1, AstrOsNetwork::ssid);
    cmd.setLine(2, password);
    cmd.setLine(3, "192.168.4.1");
    strncpy(msg.data, cmd.toString().c_str(), sizeof(msg.data));
    msg.data[sizeof(msg.data) - 1] = '\0';
    xQueueSend(AstrOsNetwork::hardwareQueue, &msg, pdMS_TO_TICKS(2000));

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
