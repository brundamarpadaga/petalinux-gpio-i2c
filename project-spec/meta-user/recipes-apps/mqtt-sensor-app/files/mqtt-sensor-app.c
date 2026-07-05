#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "MQTTClient.h"

#define CONFIG_FILE        "/etc/mqtt-sensor.conf"
#define CLIENT_ID          "zynq-sensor-01"
#define TOPIC              "zynq/sensor/bme280"
#define QOS                1
#define PUBLISH_INTERVAL_S 5
#define MAX_STR            256

/* BME280 — same I2C bus already proven with the SSD1306 OLED */
#define I2C_BUS            "/dev/i2c-1"
#define BME280_ADDR        0x76   /* 0x76 if SDO -> GND, 0x77 if SDO -> VCC; confirm with i2cdetect -y 1 */

#define BME280_REG_CHIP_ID    0xD0
#define BME280_REG_RESET      0xE0
#define BME280_REG_CTRL_HUM   0xF2
#define BME280_REG_CTRL_MEAS  0xF4
#define BME280_REG_CONFIG     0xF5
#define BME280_REG_DATA       0xFA   /* temp_msb..hum_lsb, 5 bytes (pressure skipped) */
#define BME280_REG_CALIB_T    0x88   /* dig_T1..T3, 6 bytes */
#define BME280_REG_CALIB_H1   0xA1
#define BME280_REG_CALIB_H2_6 0xE1   /* dig_H2..H6, 7 bytes */
#define BME280_CHIP_ID        0x60

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4, dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

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

static int      bme280_fd = -1;
static bme280_calib_t calib;
static int32_t  t_fine;

static int bme280_read_regs(uint8_t reg, uint8_t *buf, size_t len) {
    if (write(bme280_fd, &reg, 1) != 1) return -1;
    if (read(bme280_fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

static int bme280_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return (write(bme280_fd, buf, 2) == 2) ? 0 : -1;
}

static int bme280_init(void) {
    bme280_fd = open(I2C_BUS, O_RDWR);
    if (bme280_fd < 0) {
        perror("BME280: failed to open I2C bus");
        return -1;
    }
    if (ioctl(bme280_fd, I2C_SLAVE, BME280_ADDR) < 0) {
        perror("BME280: failed to set I2C address");
        close(bme280_fd);
        bme280_fd = -1;
        return -1;
    }

    uint8_t chip_id = 0;
    if (bme280_read_regs(BME280_REG_CHIP_ID, &chip_id, 1) < 0 || chip_id != BME280_CHIP_ID) {
        fprintf(stderr, "BME280: unexpected chip ID 0x%02X (expected 0x%02X) — check wiring/address\n",
                chip_id, BME280_CHIP_ID);
        close(bme280_fd);
        bme280_fd = -1;
        return -1;
    }

    bme280_write_reg(BME280_REG_RESET, 0xB6);
    usleep(10000);

    uint8_t calib_t[6];
    if (bme280_read_regs(BME280_REG_CALIB_T, calib_t, sizeof(calib_t)) < 0)
        return -1;
    calib.dig_T1 = (uint16_t)(calib_t[1] << 8 | calib_t[0]);
    calib.dig_T2 = (int16_t)(calib_t[3] << 8 | calib_t[2]);
    calib.dig_T3 = (int16_t)(calib_t[5] << 8 | calib_t[4]);

    uint8_t h1;
    if (bme280_read_regs(BME280_REG_CALIB_H1, &h1, 1) < 0)
        return -1;
    calib.dig_H1 = h1;

    uint8_t calib_h[7];
    if (bme280_read_regs(BME280_REG_CALIB_H2_6, calib_h, sizeof(calib_h)) < 0)
        return -1;
    calib.dig_H2 = (int16_t)(calib_h[1] << 8 | calib_h[0]);
    calib.dig_H3 = calib_h[2];
    calib.dig_H4 = (int16_t)((calib_h[3] << 4) | (calib_h[4] & 0x0F));
    calib.dig_H5 = (int16_t)((calib_h[5] << 4) | (calib_h[4] >> 4));
    calib.dig_H6 = (int8_t)calib_h[6];

    /* Humidity oversampling x1 — must be written before ctrl_meas for it to take effect */
    bme280_write_reg(BME280_REG_CTRL_HUM, 0x01);
    bme280_write_reg(BME280_REG_CONFIG, 0x00);

    printf("BME280 initialized (chip ID 0x%02X)\n", chip_id);
    return 0;
}

/* Bosch datasheet section 4.2.3 reference compensation formulas */
static int32_t bme280_compensate_temp(int32_t adc_T) {
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)calib.dig_T1 << 1))) * ((int32_t)calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib.dig_T1))) >> 12)
            * ((int32_t)calib.dig_T3)) >> 14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;   /* DegC x100 */
}

static uint32_t bme280_compensate_humidity(int32_t adc_H) {
    int32_t v_x1;
    v_x1 = (t_fine - (int32_t)76800);
    v_x1 = (((((adc_H << 14) - (((int32_t)calib.dig_H4) << 20) - (((int32_t)calib.dig_H5) * v_x1))
            + (int32_t)16384) >> 15)
            * ((((((v_x1 * (int32_t)calib.dig_H6) >> 10)
                 * (((v_x1 * (int32_t)calib.dig_H3) >> 11) + (int32_t)32768)) >> 10)
                 + (int32_t)2097152) * (int32_t)calib.dig_H2 + 8192) >> 14);
    v_x1 = v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * (int32_t)calib.dig_H1) >> 4);
    v_x1 = (v_x1 < 0) ? 0 : v_x1;
    v_x1 = (v_x1 > 419430400) ? 419430400 : v_x1;
    return (uint32_t)(v_x1 >> 12);   /* %RH x1024 */
}

/* Forced mode: one-shot measurement (temp + humidity; pressure left disabled) */
static int read_sensor(float *temp, float *humidity) {
    /* osrs_t=1 (bits 7:5=001), osrs_p=0 skipped (bits 4:2=000), mode=forced (bits 1:0=01) */
    if (bme280_write_reg(BME280_REG_CTRL_MEAS, 0x21) < 0)
        return -1;

    /* Max conversion time for osrs_t=1, osrs_h=1 is ~6.4ms per datasheet; sleep with margin */
    usleep(10000);

    uint8_t data[5];
    if (bme280_read_regs(BME280_REG_DATA, data, sizeof(data)) < 0)
        return -1;

    int32_t adc_T = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_H = ((int32_t)data[3] << 8) | data[4];

    int32_t  temp_x100  = bme280_compensate_temp(adc_T);
    uint32_t hum_x1024  = bme280_compensate_humidity(adc_H);

    *temp     = temp_x100 / 100.0f;
    *humidity = hum_x1024 / 1024.0f;
    return 0;
}

int main(void) {
    Config cfg = {0};
    if (load_config(&cfg) < 0)
        return 1;

    if (bme280_init() < 0) {
        fprintf(stderr, "Failed to initialize BME280\n");
        return 1;
    }

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
        if (read_sensor(&temp, &humidity) < 0) {
            fprintf(stderr, "BME280 read failed, skipping this cycle\n");
            sleep(PUBLISH_INTERVAL_S);
            continue;
        }

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
