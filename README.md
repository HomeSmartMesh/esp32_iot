# esp32_iot
ESP32 projects reference based on the official ESP-IDF taking advantage of OTA MQTT JSON and others

# Requirements
* the [official esp-idf SDK](https://github.com/espressif/esp-idf)

    cd ~/esp-idf
    git fetch
    git checkout cc8ad721f98ffbc7953ece70616c07422b58e06b

using esp-idf commit from Tue Aug 28 11:01:22

# Used module
* ESP-WROOM-32

# rgb_led
test vectors
## all
all leds with the same rgb

    mosquitto_pub -t 'esp/rgb led/all' -m '{"red":0,"green":20,"blue":3}'
## one
one indexed led access

    mosquitto_pub -t 'esp/rgb led/one' -m '{"index":0,"red":0,"green":20,"blue":30}'
## list
list with a color for every led

570 bytes buffer required instead of 1080 for multiple r,g,b json objects

    mosquitto_pub -t 'esp/rgb led/list' -m '{"leds":[10,2,3,0,2,30,10,2,3,0,2,30,10,2,3,0,2,30,20,3,15]}'
