#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "WS2812.h"
#include "../ArduinoJson/ArduinoJson.hpp"

static const char *TAG = "MQTT_EXAMPLE";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

esp_mqtt_client_handle_t g_client;
bool is_client_ready = false;

const gpio_num_t BLUE_LED=(gpio_num_t)2;
const gpio_num_t RGB_GPIO=(gpio_num_t)13;

WS2812 my_rgb(RGB_GPIO,1);

void rgb_led_set_color(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<200> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    uint8_t red = root["red"];
    uint8_t green = root["red"];
    uint8_t blue = root["blue"];
    ESP_LOGI(TAG, "MQTT-JSON> rgb(%u , %u , %u)",red, green, blue);

    my_rgb.setPixel(0,red,green,blue);    
    my_rgb.show();
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    g_client = event->client;//update g_client
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            is_client_ready = true;
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(g_client, "esp/rgb led/status", "online", 0, 1, 1);
            ESP_LOGI(TAG, "MQTT> sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(g_client, "esp/rgb led/color", 0);
            ESP_LOGI(TAG, "MQTT> sent subscribe successful, msg_id=%d", msg_id);

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            is_client_ready = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGD(TAG, "TOPIC length : %d\r\n", event->topic_len);
            ESP_LOGD(TAG, "DATA length : %d\r\n", event->data_len);
            if(strncmp(event->topic,"esp/rgb led/color",event->topic_len) == 0)
            {
                ESP_LOGI(TAG, "MQTT> color request");
                rgb_led_set_color(event->data,event->data_len);
            }
            else
            {
                ESP_LOGI(TAG, "MQTT> unhandled topic %s", event->topic);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
    }
    return ESP_OK;
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config;
    memset(&wifi_config,0,sizeof(wifi_config_t));
    strcpy((char*)wifi_config.sta.ssid,CONFIG_WIFI_SSID);
    strcpy((char*)wifi_config.sta.password,CONFIG_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

char mqtt_uri[32];

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg,0,sizeof(esp_mqtt_client_config_t));
    strcpy(mqtt_uri,CONFIG_BROKER_URL);
    mqtt_cfg.uri = mqtt_uri;
    mqtt_cfg.event_handle = mqtt_event_handler;

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(g_client);
}

void publish_heat_status(int heat,int timer)
{
    if(is_client_ready)
    {
        char payload[10];
        sprintf(payload,"%d",heat);
        int msg_id = esp_mqtt_client_publish(g_client, "esp/rgb led/heating", payload, 0, 1, 0);
        ESP_LOGD(TAG, "heater> sent publish successful, msg_id=%d", msg_id);
        sprintf(payload,"%d",timer);
        msg_id = esp_mqtt_client_publish(g_client, "esp/rgb led/timer", payload, 0, 1, 0);
        ESP_LOGD(TAG, "heater> sent publish successful, msg_id=%d", msg_id);
    }
    else
    {
        ESP_LOGW(TAG, "mqtt client not ready");
    }
}

void rgb_gpio_task(void *pvParameter)
{
    gpio_pad_select_gpio(BLUE_LED);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLUE_LED, GPIO_MODE_OUTPUT);
    while(1) {
        gpio_set_level(BLUE_LED, 1);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        gpio_set_level(BLUE_LED, 0);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

extern "C" void app_main();

void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    nvs_flash_init();
    wifi_init();

    xTaskCreate(&rgb_gpio_task, "rgb_gpio_task", 2048, NULL, 5, NULL);

    mqtt_app_start();


    my_rgb.setPixel(0,25,4,0);    my_rgb.show();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    my_rgb.setPixel(0,4,25,2);    my_rgb.show();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    my_rgb.setPixel(0,10,4,25);    my_rgb.show();
}
