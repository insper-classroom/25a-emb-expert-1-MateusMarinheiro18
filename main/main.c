#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/timeouts.h"   // sys_check_timeouts()
#include "lwip/inet.h"       // ipaddr_aton

#define WIFI_SSID                   "iPhone do Mateus (2)"
#define WIFI_PASSWORD               "matinheiro"
#define MQTT_SERVER_IP              "172.20.10.7"
#define MQTT_BROKER_PORT            1883

#define LDR_CHANNEL                 0    // ADC0 → GPIO26
#define LED_PIN                     15

#define LIGHT_PUBLISH_INTERVAL_MS   5000

#define TOPIC_LIGHT_CONTROL         "/light"
#define TOPIC_LIGHT_SENSOR          "/light_sensor"

#define MQTT_PUBLISH_QOS            1
#define MQTT_PUBLISH_RETAIN         0

typedef struct {
    mqtt_client_t                      *mqtt_client_inst;
    struct mqtt_connect_client_info_t   mqtt_client_info;
    char                                topic[64];
} MQTT_CLIENT_DATA_T;

const char * full_topic(MQTT_CLIENT_DATA_T *state, const char *suffix) {
    return suffix;
}
void pub_request_cb(void *arg, err_t err) {
    if (err != ERR_OK) {
        printf("Publish callback erro = %d\n", err);
    }
}

static MQTT_CLIENT_DATA_T mqtt_state = {0};
static volatile bool      mqtt_connected = false;
static absolute_time_t    next_publish_time;

static void wifi_connect(void);
static void mqtt_setup_connection(void);
static void publish_light_sensor(MQTT_CLIENT_DATA_T *state);
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);

int main(){
    stdio_init_all();
    sleep_ms(2000);
    printf("\n===== Pico W MQTT Light Sensor =====\n");

    adc_init();
    adc_gpio_init(26);                  // ADC0 no GPIO26
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    wifi_connect();
    mqtt_setup_connection();

    next_publish_time = make_timeout_time_ms(LIGHT_PUBLISH_INTERVAL_MS);

    while (true) {
        cyw43_arch_lwip_begin();
        sys_check_timeouts();
        cyw43_arch_lwip_end();

        publish_light_sensor(&mqtt_state);
        sleep_ms(10);
    }

    if (mqtt_state.mqtt_client_inst) {
        mqtt_disconnect(mqtt_state.mqtt_client_inst);
    }
    cyw43_arch_deinit();
    return 0;
}

static void wifi_connect(void){
    if (cyw43_arch_init()) panic("Wi-Fi init failed");
    cyw43_arch_enable_sta_mode();
    while (cyw43_arch_wifi_connect_timeout_ms(
           WIFI_SSID, WIFI_PASSWORD,
           CYW43_AUTH_WPA2_AES_PSK, 10000) != 0) {
        printf("Tentando Wi-Fi...\n");
        sleep_ms(1000);
    }
    printf("✓ Wi-Fi OK, IP %s\n",
           ipaddr_ntoa(&cyw43_state.netif[0].ip_addr));
}

static void mqtt_setup_connection(void){
    if (mqtt_connected) return;

    ip_addr_t broker_ip;
    if (!ipaddr_aton(MQTT_SERVER_IP, &broker_ip)) {
        printf("✗ IP broker inválido: %s\n", MQTT_SERVER_IP);
        return;
    }

    mqtt_state.mqtt_client_info.client_id   = "pico_client";
    mqtt_state.mqtt_client_info.client_user = NULL;
    mqtt_state.mqtt_client_info.client_pass = NULL;
    mqtt_state.mqtt_client_info.keep_alive  = 0;  // sem keep-alive

    cyw43_arch_lwip_begin();
    if (!mqtt_state.mqtt_client_inst) {
        mqtt_state.mqtt_client_inst = mqtt_client_new();
        if (!mqtt_state.mqtt_client_inst) panic("mqtt_client_new failed");
    }
    mqtt_client_connect(
        mqtt_state.mqtt_client_inst,
        &broker_ip,
        MQTT_BROKER_PORT,
        mqtt_connection_cb,
        NULL,
        &mqtt_state.mqtt_client_info
    );
    cyw43_arch_lwip_end();

    printf("→ MQTT connect solicitado\n");
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status){
    uint8_t code = (uint8_t)status;
    printf("MQTT status = %u\n", code);
    if (code == MQTT_CONNECT_ACCEPTED) {
        mqtt_connected = true;

        cyw43_arch_lwip_begin();
        err_t err = mqtt_subscribe(
            mqtt_state.mqtt_client_inst,
            full_topic(&mqtt_state, TOPIC_LIGHT_CONTROL),
            MQTT_PUBLISH_QOS,
            NULL, NULL
        );
        mqtt_set_inpub_callback(
            mqtt_state.mqtt_client_inst,
            NULL,
            mqtt_incoming_data_cb,
            NULL
        );
        cyw43_arch_lwip_end();

        printf("→ SUBSCRIBED %s, err=%d\n", TOPIC_LIGHT_CONTROL, err);
    } else {
        mqtt_connected = false;
        printf("✗ MQTT refused, retry in %dms\n", LIGHT_PUBLISH_INTERVAL_MS);
        sleep_ms(LIGHT_PUBLISH_INTERVAL_MS);
        mqtt_setup_connection();
    }
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags){
    printf("Dados em %s: %.*s\n", TOPIC_LIGHT_CONTROL, (int)len, (char*)data);
    bool ligar = (len>0 && (data[0]=='1' || data[0]=='O' || data[0]=='o'));
    gpio_put(LED_PIN, ligar);
    printf("LED %s\n", ligar ? "LIGADO" : "DESLIGADO");
}

static void publish_light_sensor(MQTT_CLIENT_DATA_T *state){
    static uint16_t old_light = UINT16_MAX;
    const char *light_key = full_topic(state, TOPIC_LIGHT_SENSOR);

    if (absolute_time_diff_us(get_absolute_time(), next_publish_time) < 0)
        return;

    adc_select_input(LDR_CHANNEL);
    uint16_t val = adc_read();
    if (val == old_light) {
        next_publish_time = make_timeout_time_ms(LIGHT_PUBLISH_INTERVAL_MS);
        return;
    }
    old_light = val;

    char buf[16];
    snprintf(buf, sizeof(buf), "%u", val);
    printf("Publishing %s to %s\n", buf, light_key);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_publish(
        state->mqtt_client_inst,
        light_key,
        buf, strlen(buf),
        MQTT_PUBLISH_QOS,
        MQTT_PUBLISH_RETAIN,
        pub_request_cb,
        state
    );
    cyw43_arch_lwip_end();
    printf("→ mqtt_publish retornou %d\n", err);

    next_publish_time = make_timeout_time_ms(LIGHT_PUBLISH_INTERVAL_MS);
}
