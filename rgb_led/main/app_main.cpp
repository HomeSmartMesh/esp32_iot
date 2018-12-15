#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <list>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "WS2812.h"
#include "../ArduinoJson/ArduinoJson.hpp"

static const char *TAG                  = "MQTT_EXAMPLE";
static const char* TOPIC_ONE            = "esp/curvy/pixels/one";
static const char* TOPIC_ALL            = "esp/curvy/pixels/all";
static const char* TOPIC_LIST           = "esp/curvy/pixels/list";
static const char* TOPIC_GRAD           = "esp/curvy/pixels/grad";
static const char* TOPIC_LINES_GRAD     = "esp/curvy/lines/grad";
static const char* TOPIC_PANEL          = "esp/curvy/panel";
static const char* TOPIC_STATUS         = "esp/curvy/status";
static const char* TOPIC_SUB            = "esp/curvy/#";
static const char* TOPIC_BATTERY        = "esp/curvy/battery";



static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

esp_mqtt_client_handle_t g_client;
bool is_client_ready = false;

const gpio_num_t BLUE_LED=(gpio_num_t)2;
const gpio_num_t RGB_GPIO=(gpio_num_t)13;
const gpio_num_t V_BAT_GPIO=(gpio_num_t)13;

#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
static esp_adc_cal_characteristics_t *adc_chars;
static const adc1_channel_t channel = ADC1_CHANNEL_5; //GPIO33 for ADC1
static const adc_atten_t atten = ADC_ATTEN_DB_11;
static const adc_unit_t unit = ADC_UNIT_1;

struct grad_t{
    uint8_t start_red   ;
    uint8_t start_green ;
    uint8_t start_blue  ;
    uint8_t stop_red    ;
    uint8_t stop_green  ;
    uint8_t stop_blue   ;
};

struct action_flash_t{
    pixel_t color;
    int     progress_ms;
    int     duration_ms;
};

enum class action_type_t { flash, wave, wavelet };

class action_t{
    public:
        bool run(WS2812* leds,int delay_ms);
    public:
        std::string name;
        action_type_t a_type;
        union{
            action_flash_t flash;
        };
};

class animation_t{
    public:
        animation_t(WS2812* v_leds):leds(v_leds){}
        void run();
        void add_flash(action_flash_t &v_flash);
    public:
        bool enabled;
        WS2812* leds;
        std::list<action_t> actions;

};

float flash_progress_to_intensity(int progress_ms,int duration_ms)
{
    float pos = progress_ms;
    pos /= duration_ms;//div / 0 exception
    if(pos > 0.5)
    {
        pos = 1 - pos;
    }
    return pos;
}

bool action_t::run(WS2812* leds,int delay_ms)
{
    bool done = false;
    switch(a_type)
    {
        case action_type_t::flash :
            {
                float intensity = flash_progress_to_intensity(flash.progress_ms,flash.duration_ms);
                my_rgb.add_to_all(flash.color.red * intensity, flash.color.green * intensity, flash.color.blue * intensity);
                flash.progress_ms+=delay_ms;
                if(flash.progress_ms > flash.duration_ms)
                {
                    done = true;
                }
            }
        break;
        default:
        break;
    }
    return done;
}

void animation_t::add_flash(action_flash_t &v_flash)
{
    action_t flash_action;
    flash_action.a_type = action_type_t::flash;
    flash_action.flash = v_flash;
    actions.push_back(flash_action);
}

void animation_t::run()
{
    leds->clear();
    for(action_t&action : actions )
    {
        action.run(leds,20);
    }
    std::list<action_t>::iterator action = actions.begin();
    while (action != actions.end())
    {
        bool isDone = action->run(leds,20);
        if (isDone)
        {
            actions.erase(action++);
        }
        else
        {
            ++action;
        }
    }
}


static void animation_timer_callback(void* arg);

//static const uint8_t g_nb_led = 24;
static const uint16_t g_nb_led = 256;

WS2812 my_rgb(RGB_GPIO,g_nb_led);

animation_t animation(&my_rgb);



void timers_init()
{
    esp_timer_create_args_t oneshot_timer_args;
    esp_timer_handle_t oneshot_timer;
    ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, 31536000000000ull));//one year of microseconds
    ESP_LOGI(TAG, "timer> %lld us",esp_timer_get_time());

    esp_timer_create_args_t periodic_timer_args;
    periodic_timer_args.callback = &animation_timer_callback;
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 20000));

}

int64_t g_time;

void timestamp_start()
{
    g_time = esp_timer_get_time();
}

int64_t timestamp_stop()
{
    return (esp_timer_get_time() - g_time);
}

void delay_ms(int delay)
{
    vTaskDelay(delay / portTICK_PERIOD_MS);
}

static void animation_timer_callback(void* arg)
{
    animation.run();
}


void leds_set_all(uint8_t red,uint8_t green,uint8_t blue, bool show = true)
{
    for(int i=0;i<g_nb_led;i++)
    {
        my_rgb.setPixel(i,red,green,blue);
    }
    if(show)
    {
        my_rgb.show();
    }
}

void leds_set_gradient(int led_start,int nb_leds,grad_t grad, bool show = true)
{
    for(int i=led_start;i<(led_start + nb_leds);i++)
    {
        float coeff = (i-led_start);
        coeff/=nb_leds;
        float coeffm1 = 1 - coeff;
        uint8_t red     = grad.start_red   *coeff  + grad.stop_red  *coeffm1;
        uint8_t green   = grad.start_green *coeff  + grad.stop_green*coeffm1;
        uint8_t blue    = grad.start_blue  *coeff  + grad.stop_blue *coeffm1;
        my_rgb.setPixel(i,  red, green, blue);
        //ESP_LOGI(TAG, "MQTT-JSON> (%d)(%u , %u , %u)",i,red, green, blue);
    }
    if(show)
    {
        my_rgb.show();
    }
}

void json_led_set_all(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<200> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    uint8_t red = root["red"];
    uint8_t green = root["green"];
    uint8_t blue = root["blue"];
    ESP_LOGI(TAG, "MQTT-JSON> rgb(%u , %u , %u)",red, green, blue);
    leds_set_all(red,green,blue);
}

void json_led_set_one(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<200> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    uint8_t index = root["index"];
    uint8_t red = root["red"];
    uint8_t green = root["green"];
    uint8_t blue = root["blue"];
    ESP_LOGI(TAG, "MQTT-JSON> rgb(%u , %u , %u)",red, green, blue);

    my_rgb.setPixel(index,red,green,blue);
    my_rgb.show();
}

void json_led_set_list(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<4096> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    if (!root.success()) 
    {
        ESP_LOGE(TAG, "MQTT-JSON> Parsing error");
        return;
    }
    int size = root["leds"].size();
    int nb_leds = size / 3;
    ESP_LOGI(TAG, "MQTT-JSON> size = %u",size);
    for(int i=0;i<nb_leds;i++)
    {
        uint8_t red = root["leds"][3*i];
        uint8_t green = root["leds"][3*i+1];
        uint8_t blue = root["leds"][3*i+2];
        my_rgb.setPixel(i,red,green,blue);
        ESP_LOGI(TAG, "MQTT-JSON> rgb[%u](%u , %u , %u)",i,red, green, blue);
    }
    my_rgb.show();
}

void json_led_set_grad(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<600> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    if (!root.success()) 
    {
        ESP_LOGE(TAG, "MQTT-JSON> Parsing error");
        return;
    }
    int led_start = root["led_start"];
    ESP_LOGI(TAG, "MQTT-JSON> led_start = %d",led_start);
    int nb_leds = root["nb_leds"];
    ESP_LOGI(TAG, "MQTT-JSON> nb_leds = %d",nb_leds);
    grad_t grad;
    grad.start_red   = root["col_start"]["r"];
    grad.start_green = root["col_start"]["g"];
    grad.start_blue  = root["col_start"]["b"];
    grad.stop_red    = root["col_stop"]["r"];
    grad.stop_green  = root["col_stop"]["g"];
    grad.stop_blue   = root["col_stop"]["b"];

    leds_set_gradient(led_start, nb_leds, grad);
}

void json_led_set_panel(const char * payload,int len)
{
    ArduinoJson::StaticJsonBuffer<600> jsonBuffer;
    ArduinoJson::JsonObject& root = jsonBuffer.parseObject(payload);
    if (!root.success()) 
    {
        ESP_LOGE(TAG, "MQTT-JSON> Parsing error");
        return;
    }

    std::string action = root["action"].as<std::string>();
    if(action.compare("off") == 0)
    {
        timestamp_start();
        leds_set_all(0,0,0,false);
        int64_t t_set_all = timestamp_stop();
        timestamp_start();
        my_rgb.show();
        int64_t t_show = timestamp_stop();
        ESP_LOGI(TAG, "MQTT-JSON> Panel Off");
        ESP_LOGI(TAG, "MQTT-JSON> time to set all: %lld us ; time to show: %lld us", t_set_all, t_show);
    }
    else if(action.compare("flash") == 0)
    {
        action_flash_t flash;
        flash.duration_ms = root["duration_ms"];
        flash.color.red = root["red"];
        flash.color.green = root["green"];
        flash.color.blue = root["blue"];
        animation.add_flash(flash);
        ESP_LOGI(TAG, "MQTT-JSON> Panel Off");
    }
}

typedef void (*t_mqtt_handler)(const char *,int);

bool match_and_call(esp_mqtt_event_handle_t &event,const char* topic,t_mqtt_handler func)
{
    if(strncmp(event->topic,topic,event->topic_len) == 0)
    {
        (*func)(event->data,event->data_len);
        return true;
    }
    else
    {
        return false;
    }
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
            msg_id = esp_mqtt_client_publish(g_client, TOPIC_STATUS, "online", 0, 1, 0);
            ESP_LOGI(TAG, "MQTT> sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(g_client, TOPIC_SUB, 0);
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
            printf("MQTT> TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("MQTT> DATA=%.*s\r\n", event->data_len, event->data);

            if      (match_and_call(event,TOPIC_ALL,  &json_led_set_all))     {}
            else if (match_and_call(event,TOPIC_LIST, &json_led_set_list))    {}
            else if (match_and_call(event,TOPIC_ONE,  &json_led_set_one))     {}
            else if (match_and_call(event,TOPIC_GRAD, &json_led_set_grad))    {}
            else if (match_and_call(event,TOPIC_PANEL,&json_led_set_panel))   {}
            else
            {
                ESP_LOGI(TAG, "MQTT> unhandled topic len=%d", event->topic_len);
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
char mqtt_lwt_topic[32];
char mqtt_lwt_mesg[32];

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg,0,sizeof(esp_mqtt_client_config_t));
    strcpy(mqtt_uri,CONFIG_BROKER_URL);
    strcpy(mqtt_lwt_topic,TOPIC_STATUS);
    strcpy(mqtt_lwt_mesg,"offline");
    mqtt_cfg.uri = mqtt_uri;
    mqtt_cfg.event_handle = mqtt_event_handler;
    mqtt_cfg.lwt_topic = mqtt_lwt_topic;
    mqtt_cfg.lwt_msg = mqtt_lwt_mesg;
    mqtt_cfg.lwt_msg_len = 1;
    mqtt_cfg.lwt_retain = true;

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(g_client);
}

void rgb_gpio_task(void *pvParameter)
{
    #if MEASURE_BATTERY
        static int count = 0;
    #endif
    gpio_pad_select_gpio(BLUE_LED);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLUE_LED, GPIO_MODE_OUTPUT);
    while(1) {
        gpio_set_level(BLUE_LED, 1);
        delay_ms(10);
        gpio_set_level(BLUE_LED, 0);
        delay_ms(990);
    }
}


extern "C" void app_main();
void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");

    timers_init();

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);

    leds_set_all(0,1,0);
    delay_ms(500);
    leds_set_all(0,0,0);

    nvs_flash_init();
    wifi_init();

    xTaskCreate(&rgb_gpio_task, "rgb_gpio_task", 2048, NULL, 5, NULL);

    mqtt_app_start();

    leds_set_all(1,0,0);
    delay_ms(500);
    leds_set_all(0,1,0);
    delay_ms(500);
    leds_set_all(0,0,1);
    delay_ms(500);
    leds_set_all(0,0,0);
}
