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
#include "lwip/timeouts.h"   
#include "lwip/inet.h"       

#define WIFI_SSID                 "iPhone do Mateus (2)"
#define WIFI_PASSWORD             "matinheiro"
#define MQTT_SERVER_IP            "172.20.10.7"
#define MQTT_BROKER_PORT          1883

#define LDR_CHANNEL               0   
#define LED_PIN                   15  

#define LIGHT_PUBLISH_INTERVAL_MS 3000

#define TOPIC_LIGHT_CONTROL       "/light"
#define TOPIC_LIGHT_SENSOR        "/light_sensor"

#define MQTT_SUBSCRIBE_QOS        1
#define MQTT_PUBLISH_QOS          1
#define MQTT_PUBLISH_RETAIN       0

typedef struct {
    mqtt_client_t                     *mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    bool                               stop_client;
} MQTT_CLIENT_DATA_T;

const char * full_topic(MQTT_CLIENT_DATA_T *state, const char *suffix) {
    return suffix;
}
void pub_request_cb(void *arg, err_t err) {
    if (err != ERR_OK) printf("Publish callback erro = %d\n", err);
}
void sub_request_cb(void *arg, err_t err) {
    printf("Subscribe callback err = %d\n", err);
}
void unsub_request_cb(void *arg, err_t err) {
    printf("Unsubscribe callback err = %d\n", err);
}

static MQTT_CLIENT_DATA_T state = {0};
static volatile bool       mqtt_connected = false;
static absolute_time_t     next_publish_time;

static void wifi_connect(void);
static void mqtt_setup_connection(void);
static void sub_unsub_topics(MQTT_CLIENT_DATA_T *s, bool sub);
static void publish_light_sensor(MQTT_CLIENT_DATA_T *s);
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);
static void control_led(bool on);

int main() {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n===== Pico W MQTT Example =====\n");

    adc_init();
    adc_gpio_init(26);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    wifi_connect();
    mqtt_setup_connection();

    next_publish_time = make_timeout_time_ms(LIGHT_PUBLISH_INTERVAL_MS);

    while (!state.stop_client) {
        cyw43_arch_lwip_begin();
        sys_check_timeouts();
        cyw43_arch_lwip_end();

        publish_light_sensor(&state);
        sleep_ms(10);
    }

    printf("Saindo… unsubscribing e desconectando\n");
    sub_unsub_topics(&state, false);
    mqtt_disconnect(state.mqtt_client_inst);
    cyw43_arch_deinit();
    return 0;
}

static void wifi_connect(void) {
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

static void mqtt_setup_connection(void) {
    if (mqtt_connected) return;

    ip_addr_t broker_ip;
    if (!ipaddr_aton(MQTT_SERVER_IP, &broker_ip)) {
        printf("✗ IP broker inválido: %s\n", MQTT_SERVER_IP);
        return;
    }

    state.mqtt_client_info.client_id   = "pico_client";
    state.mqtt_client_info.client_user = NULL;
    state.mqtt_client_info.client_pass = NULL;
    state.mqtt_client_info.keep_alive  = 0;

    cyw43_arch_lwip_begin();
    if (!state.mqtt_client_inst) {
        state.mqtt_client_inst = mqtt_client_new();
        if (!state.mqtt_client_inst) panic("mqtt_client_new failed");
    }
    mqtt_client_connect(
        state.mqtt_client_inst,
        &broker_ip,
        MQTT_BROKER_PORT,
        mqtt_connection_cb,
        &state,
        &state.mqtt_client_info
    );
    cyw43_arch_lwip_end();

    printf("→ MQTT connect solicitado\n");
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    printf("MQTT status = %d\n", status);
    if (status == MQTT_CONNECT_ACCEPTED) {
        mqtt_connected = true;
        printf("✓ Conexão MQTT OK, subscrevendo tópicos…\n");
        sub_unsub_topics(&state, true);

        cyw43_arch_lwip_begin();
        mqtt_set_inpub_callback(
            state.mqtt_client_inst,
            NULL,
            mqtt_incoming_data_cb,
            &state
        );
        cyw43_arch_lwip_end();
    } else {
        mqtt_connected = false;
        printf("✗ MQTT refused, retry em 5s\n");
        sleep_ms(5000);
        mqtt_setup_connection();
    }
}

static void sub_unsub_topics(MQTT_CLIENT_DATA_T *s, bool sub) {
    mqtt_request_cb_t cb = sub ? sub_request_cb : unsub_request_cb;
    cyw43_arch_lwip_begin();
    mqtt_sub_unsub(s->mqtt_client_inst, full_topic(s, TOPIC_LIGHT_CONTROL), MQTT_SUBSCRIBE_QOS, cb, s, sub);
    mqtt_sub_unsub(s->mqtt_client_inst, full_topic(s, "/print"),  MQTT_SUBSCRIBE_QOS, cb, s, sub);
    mqtt_sub_unsub(s->mqtt_client_inst, full_topic(s, "/ping"),   MQTT_SUBSCRIBE_QOS, cb, s, sub);
    mqtt_sub_unsub(s->mqtt_client_inst, full_topic(s, "/exit"),   MQTT_SUBSCRIBE_QOS, cb, s, sub);
    cyw43_arch_lwip_end();
    printf("→ %s tópicos\n", sub ? "Subscribed" : "Unsubscribed");
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    const char *msg = (const char*)data;
    printf("Mensagem em %.*s\n", len, msg);

    if ((len == 1 && msg[0] == '1') || strcasecmp(msg, "On") == 0) {
        control_led(true);
    } else if ((len == 1 && msg[0] == '0') || strcasecmp(msg, "Off") == 0) {
        control_led(false);
    }
}

static void control_led(bool on) {
    gpio_put(LED_PIN, on);
    printf("LED %s\n", on ? "LIGADO" : "DESLIGADO");
}

static void publish_light_sensor(MQTT_CLIENT_DATA_T *s) {
    static uint16_t oldv = UINT16_MAX;
    const char *key = full_topic(s, TOPIC_LIGHT_SENSOR);

    if (absolute_time_diff_us(get_absolute_time(), next_publish_time) < 0) {
        return;
    }
    next_publish_time = make_timeout_time_ms(LIGHT_PUBLISH_INTERVAL_MS);

    adc_select_input(LDR_CHANNEL);
    uint16_t v = adc_read();
    if (v == oldv) return;
    oldv = v;

    char buf[16];
    snprintf(buf, sizeof(buf), "%u", v);
    printf("Publicando %s → %s\n", buf, key);

    cyw43_arch_lwip_begin();
    mqtt_publish(
        s->mqtt_client_inst,
        key,
        buf, strlen(buf),
        MQTT_PUBLISH_QOS,
        MQTT_PUBLISH_RETAIN,
        pub_request_cb,
        s
    );
    cyw43_arch_lwip_end();
}
