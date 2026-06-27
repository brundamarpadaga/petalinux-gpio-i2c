#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "MQTTClient.h"

#define CONFIG_FILE        "/etc/mqtt-sensor.conf"
#define CLIENT_ID          "zynq-sensor-01"
#define TOPIC              "zynq/sensor/bme280"
#define QOS                1
#define PUBLISH_INTERVAL_S 5
#define MAX_STR            256

typedef struct {
    char broker[MAX_STR];
    char username[MAX_STR];
    char password[MAX_STR];
} Config;

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static int load_config(Config *cfg) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        fprintf(stderr, "Cannot open config file: %s\n", CONFIG_FILE);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (strcmp(key, "broker") == 0)
            strncpy(cfg->broker, val, MAX_STR - 1);
        else if (strcmp(key, "username") == 0)
            strncpy(cfg->username, val, MAX_STR - 1);
        else if (strcmp(key, "password") == 0)
            strncpy(cfg->password, val, MAX_STR - 1);
    }

    fclose(f);

    if (cfg->broker[0] == '\0' || cfg->username[0] == '\0' || cfg->password[0] == '\0') {
        fprintf(stderr, "Config missing required fields (broker, username, password)\n");
        return -1;
    }

    return 0;
}

/* Mock sensor — replace with real BME280 I2C reads in step 7 */
static void read_sensor(float *temp, float *humidity) {
    *temp     = 25.0f;
    *humidity = 60.0f;
}

int main(void) {
    Config cfg = {0};
    if (load_config(&cfg) < 0)
        return 1;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    MQTTClient client;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_SSLOptions ssl_opts     = MQTTClient_SSLOptions_initializer;
    int rc;

    MQTTClient_create(&client, cfg.broker, CLIENT_ID,
                      MQTTCLIENT_PERSISTENCE_NONE, NULL);

    ssl_opts.enableServerCertAuth = 1;
    ssl_opts.trustStore           = "/etc/ssl/certs/ca-certificates.crt";
    conn_opts.ssl               = &ssl_opts;
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession      = 1;
    conn_opts.username          = cfg.username;
    conn_opts.password          = cfg.password;

    printf("Connecting to %s ...\n", cfg.broker);
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to connect, rc=%d\n", rc);
        MQTTClient_destroy(&client);
        return 1;
    }
    printf("Connected.\n");

    char payload[128];
    while (running) {
        float temp, humidity;
        read_sensor(&temp, &humidity);

        int len = snprintf(payload, sizeof(payload),
                           "{\"temperature\":%.2f,\"humidity\":%.2f}",
                           temp, humidity);

        MQTTClient_message msg   = MQTTClient_message_initializer;
        MQTTClient_deliveryToken token;
        msg.payload    = payload;
        msg.payloadlen = len;
        msg.qos        = QOS;
        msg.retained   = 0;

        rc = MQTTClient_publishMessage(client, TOPIC, &msg, &token);
        if (rc != MQTTCLIENT_SUCCESS) {
            fprintf(stderr, "Publish error, rc=%d\n", rc);
        } else {
            rc = MQTTClient_waitForCompletion(client, token, 5000UL);
            if (rc == MQTTCLIENT_SUCCESS)
                printf("Published: %s\n", payload);
            else
                fprintf(stderr, "Delivery failed, rc=%d\n", rc);
        }

        sleep(PUBLISH_INTERVAL_S);
    }

    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    printf("Disconnected.\n");
    return 0;
}
