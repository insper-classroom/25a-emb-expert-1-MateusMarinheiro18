# RP2040 freertos

## Para Rodar o LDR

sudo systemctl stop mosquitto
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v

mosquitto_sub -h 172.20.10.7 -t /light_sensor -v

Rodar o código

## Para Rodar o LED

sudo systemctl stop mosquitto
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v

mosquitto_sub -h 172.20.10.7 -t "#" -v

Rodar o código

mosquitto_pub -h 172.20.10.7 -t /light -m 1

basic freertos project, with code quality enabled.

## SMP

To enable SMP kernel please modify `freertos/FreeRTOSConfig.h`:

```diff
#if FREE_RTOS_KERNEL_SMP // set by the RP2040 SMP port of FreeRTOS
/* SMP port only */
+#define configNUMBER_OF_CORES 2
#define configUSE_PASSIVE_IDLE_HOOK 0
#define configTICK_CORE 0
#define configRUN_MULTIPLE_PRIORITIES 1
+#define configUSE_CORE_AFFINITY 1
#endif
````
