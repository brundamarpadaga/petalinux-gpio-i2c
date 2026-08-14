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
    /* The ">> 14" must bind to the RIGHT multiplicand only (Bosch reference formula).
       Because '*' has higher precedence than '>>' in C, writing it as "(L * R) >> 14"
       multiplies first and overflows int32 (~3.2e12), wrapping to garbage humidity.
       The extra parens below keep it as "L * (R >> 14)", which stays within int32. */
    v_x1 = (((((adc_H << 14) - (((int32_t)calib.dig_H4) << 20) - (((int32_t)calib.dig_H5) * v_x1))
            + (int32_t)16384) >> 15)
            * (((((((v_x1 * (int32_t)calib.dig_H6) >> 10)
                 * (((v_x1 * (int32_t)calib.dig_H3) >> 11) + (int32_t)32768)) >> 10)
                 + (int32_t)2097152) * (int32_t)calib.dig_H2 + 8192) >> 14));
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

/* ---------------- SSD1306 OLED (128x64) on the same I2C bus ---------------- */
#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64
#define OLED_BUFSZ  ((OLED_W * OLED_H) / 8)
#define SSD1306_CMD 0x00
#define SSD1306_DAT 0x40

static int     oled_fd = -1;
static uint8_t oled_buf[OLED_BUFSZ];

/* 5x7 font, ASCII 0x20 (space) .. 0x5A ('Z'); each glyph is 5 column bytes (LSB=top) */
static const uint8_t oled_font[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20   */  {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */       {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */       {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */       {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */       {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */       {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */       {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */       {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */       {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */       {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */       {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */       {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */       {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */       {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */       {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */       {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */       {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */       {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */       {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */       {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */       {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */       {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */       {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */       {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */       {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */       {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */       {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */       {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */       {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

static int oled_cmd(uint8_t c) {
    uint8_t b[2] = { SSD1306_CMD, c };
    return (write(oled_fd, b, 2) == 2) ? 0 : -1;
}

static int oled_data(const uint8_t *d, size_t len) {
    uint8_t b[17];
    b[0] = SSD1306_DAT;
    memcpy(b + 1, d, len);
    return (write(oled_fd, b, len + 1) == (ssize_t)(len + 1)) ? 0 : -1;
}

static int oled_init(void) {
    oled_fd = open(I2C_BUS, O_RDWR);
    if (oled_fd < 0) { perror("OLED: open i2c"); return -1; }
    if (ioctl(oled_fd, I2C_SLAVE, OLED_ADDR) < 0) {
        perror("OLED: set addr");
        close(oled_fd);
        oled_fd = -1;
        return -1;
    }
    /* Standard SSD1306 128x64 init sequence (each byte issued as a command) */
    static const uint8_t init_seq[] = {
        0xAE, 0xD5,0x80, 0xA8,0x3F, 0xD3,0x00, 0x40, 0x8D,0x14,
        0x20,0x00, 0xA1, 0xC8, 0xDA,0x12, 0x81,0xCF, 0xD9,0xF1,
        0xDB,0x40, 0xA4, 0xA6, 0xAF
    };
    for (size_t i = 0; i < sizeof(init_seq); i++)
        if (oled_cmd(init_seq[i]) < 0) return -1;
    printf("OLED initialized\n");
    return 0;
}

static void oled_clear(void) { memset(oled_buf, 0, OLED_BUFSZ); }

static void oled_flush(void) {
    oled_cmd(0x22); oled_cmd(0); oled_cmd(7);           /* page addr 0..7 */
    oled_cmd(0x21); oled_cmd(0); oled_cmd(OLED_W - 1);  /* col addr 0..127 */
    for (int i = 0; i < OLED_BUFSZ; i += 16)
        oled_data(&oled_buf[i], 16);
}

static void oled_pixel(int x, int y) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    oled_buf[x + (y / 8) * OLED_W] |= (1 << (y % 8));
}

static void oled_char(int x, int y, char c, int scale) {
    if (c < 0x20 || c > 0x5A) c = 0x20;   /* font covers space..'Z' only */
    const uint8_t *g = oled_font[c - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                for (int sx = 0; sx < scale; sx++)
                    for (int sy = 0; sy < scale; sy++)
                        oled_pixel(x + col * scale + sx, y + row * scale + sy);
}

static void oled_string(int x, int y, const char *s, int scale) {
    for (; *s; s++) {
        oled_char(x, y, *s, scale);
        x += 6 * scale;   /* 5px glyph + 1px gap */
    }
}

static void oled_show(float temp, float humidity) {
    char tbuf[16], hbuf[16];
    /* font is uppercase-only; keep labels to digits/letters/punctuation it covers */
    snprintf(tbuf, sizeof(tbuf), "T:%.1fC", temp);
    snprintf(hbuf, sizeof(hbuf), "H:%.1f%%", humidity);
    oled_clear();
    oled_string(0, 0,  "ZYNQ BME280", 1);
    oled_string(4, 20, tbuf, 2);
    oled_string(4, 44, hbuf, 2);
    oled_flush();
}

int main(void) {
    Config cfg = {0};
    if (load_config(&cfg) < 0)
        return 1;

    if (bme280_init() < 0) {
        fprintf(stderr, "Failed to initialize BME280\n");
        return 1;
    }

    /* OLED is optional: warn but keep publishing if it isn't present */
    int have_oled = (oled_init() == 0);
    if (!have_oled)
        fprintf(stderr, "OLED not available — continuing without local display\n");

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

        if (have_oled)
            oled_show(temp, humidity);

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

    if (have_oled) {
        oled_clear();
        oled_flush();
        oled_cmd(0xAE);        /* display off */
        close(oled_fd);
    }

    printf("Disconnected.\n");
    return 0;
}
