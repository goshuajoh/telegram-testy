#include "ESP_IDF_Telebot.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <time.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_HTTP_OUTPUT_BUFFER 4096
#define TELEGRAM_BOT_TOKEN "7433396702:AAEjJx4f1eV7V0GRe7pRDLvQ0v0sk-C8XOQ"

static const char *TAG = "ESP_TELE";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static char telegram_bot_token[128] = {0};
static long offset = 0;
static int longPoll = 15;
static int HANDLE_MESSAGES = 10;

extern const char telegram_certificate_pem_start[] asm("_binary_telegram_certificate_pem_start");

typedef struct {
    char command[32];
    void (*handler)(void);
} command_handler_t;

static command_handler_t command_handlers[10];
static int command_handler_count = 0;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < 10)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void esp_tele_init(const char *ssid, const char *password)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ssid,
            .password = password,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;
    static int output_len;
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

char *url_encode(const char *str)
{
    char *enc_str = malloc(strlen(str) * 3 + 1);
    char *penc = enc_str;

    while (*str)
    {
        if (('a' <= *str && *str <= 'z') ||
            ('A' <= *str && *str <= 'Z') ||
            ('0' <= *str && *str <= '9'))
        {
            *penc++ = *str;
        }
        else
        {
            penc += sprintf(penc, "%%%02X", (unsigned char)*str);
        }
        str++;
    }
    *penc = '\0';
    return enc_str;
}

void bot_send_message(const char *chat_id, const char *text)
{
    if (chat_id == NULL || text == NULL)
    {
        ESP_LOGE(TAG, "Chat ID or text is null");
        return;
    }

    char *encoded_text = url_encode(text);
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
             TELEGRAM_BOT_TOKEN, chat_id, encoded_text);

    free(encoded_text);

    esp_http_client_config_t config = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = telegram_certificate_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Message sent successfully");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send message: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

void handleNewMessages(int numNewMessages, cJSON *json)
{
    ESP_LOGI(TAG, "handleNewMessages %d", numNewMessages);

    for (int i = 0; i < numNewMessages; i++)
    {
        cJSON *result = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "result"), i);
        cJSON *message = cJSON_GetObjectItem(result, "message");
        cJSON *text = cJSON_GetObjectItem(message, "text");
        cJSON *from_name = cJSON_GetObjectItem(cJSON_GetObjectItem(message, "from"), "first_name");

        const char *text_str = cJSON_GetStringValue(text);
        const char *from_name_str = cJSON_GetStringValue(from_name);

        for (int j = 0; j < command_handler_count; j++)
        {
            if (strcmp(text_str, command_handlers[j].command) == 0)
            {
                command_handlers[j].handler();
            }
        }

        if (text_str && strcmp(text_str, "/start") == 0)
        {
            char welcome[256];
            snprintf(welcome, sizeof(welcome),
                     "Welcome to the bot, %s.\n"
                     "Available commands:\n",
                     from_name_str ? from_name_str : "Guest");
            for (int j = 0; j < command_handler_count; j++)
            {
                strcat(welcome, command_handlers[j].command);
                strcat(welcome, "\n");
            }
            bot_send_message("219745533", welcome);
        }
    }
}

void get_updates_task(void *pvParameters)
{
    char url[256];
    char buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    while (1)
    {
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&limit=%d&timeout=%d",
                 TELEGRAM_BOT_TOKEN, offset, HANDLE_MESSAGES, longPoll);

        esp_http_client_config_t config = {
            .url = url,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .event_handler = _http_event_handler,
            .cert_pem = telegram_certificate_pem_start,
            .user_data = buffer,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            int status_code = esp_http_client_get_status_code(client);
            int content_length = esp_http_client_get_content_length(client);
            ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d", status_code, content_length);

            if (status_code == 200 && content_length > 0)
            {
                ESP_LOGI(TAG, "Response: %s", buffer);

                cJSON *json = cJSON_Parse(buffer);
                if (json)
                {
                    cJSON *result = cJSON_GetObjectItem(json, "result");
                    if (result && cJSON_IsArray(result))
                    {
                        int resultArrayLength = cJSON_GetArraySize(result);
                        handleNewMessages(resultArrayLength, json);
                        offset = cJSON_GetObjectItem(cJSON_GetArrayItem(result, resultArrayLength - 1), "update_id")->valueint + 1;
                    }
                    cJSON_Delete(json);
                }
            }
        }
        else
        {
            ESP_LOGD(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        memset(buffer, 0, sizeof(buffer));

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void add_command(const char *command, void (*handler)(void))
{
    if (command_handler_count < 10)
    {
        strncpy(command_handlers[command_handler_count].command, command, sizeof(command_handlers[command_handler_count].command) - 1);
        command_handlers[command_handler_count].handler = handler;
        command_handler_count++;
    }
    else
    {
        ESP_LOGE(TAG, "Max number of commands reached");
    }
}

void start_get_updates_task(void)
{
    xTaskCreate(&get_updates_task, "get_updates_task", 2 * 8192, NULL, 5, NULL);
}
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    configure_gpio();
    wifi_init_sta();
    xTaskCreate(&get_updates_task, "get_updates_task", 8192, NULL, 5, NULL);
}