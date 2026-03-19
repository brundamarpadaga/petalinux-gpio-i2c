/*
 * GPIO Switches to OLED Display
 * Reads 4 switches, converts to decimal (0-15), displays on SSD1306 OLED
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <signal.h>

/* GPIO Configuration */
#define NUM_PINS 4
int sw_gpio[NUM_PINS]  = {1012, 1013, 1014, 1015};
int led_gpio[NUM_PINS] = {1020, 1021, 1022, 1023};

/* I2C Configuration */
#define I2C_BUS "/dev/i2c-1"
#define OLED_ADDR 0x3C

/* SSD1306 Commands */
#define SSD1306_COMMAND 0x00
#define SSD1306_DATA 0x40
#define SSD1306_DISPLAYOFF 0xAE
#define SSD1306_DISPLAYON 0xAF
#define SSD1306_SETCONTRAST 0x81
#define SSD1306_NORMALDISPLAY 0xA6
#define SSD1306_SETDISPLAYCLOCKDIV 0xD5
#define SSD1306_SETMULTIPLEX 0xA8
#define SSD1306_SETDISPLAYOFFSET 0xD3
#define SSD1306_SETSTARTLINE 0x40
#define SSD1306_CHARGEPUMP 0x8D
#define SSD1306_MEMORYMODE 0x20
#define SSD1306_SEGREMAP 0xA0
#define SSD1306_COMSCANDEC 0xC8
#define SSD1306_SETCOMPINS 0xDA
#define SSD1306_SETPRECHARGE 0xD9
#define SSD1306_SETVCOMDETECT 0xDB
#define SSD1306_DISPLAYALLON_RESUME 0xA4
#define SSD1306_COLUMNADDR 0x21
#define SSD1306_PAGEADDR 0x22

/* Display dimensions */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define BUFFER_SIZE ((SCREEN_WIDTH * SCREEN_HEIGHT) / 8)

/* Global variables */
int i2c_fd = -1;
uint8_t display_buffer[BUFFER_SIZE];
volatile int running = 1;

/* 5x7 Font for digits 0-9 */
const uint8_t font5x7[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
};

/* Signal handler */
void signal_handler(int signum) {
    running = 0;
}

/* GPIO Helper Functions */
void write_sysfs(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, value, strlen(value));
    close(fd);
}

int read_gpio(int gpio) {
    char path[64];
    char value;
    int fd;
    
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    read(fd, &value, 1);
    close(fd);
    return (value == '1') ? 1 : 0;
}

void setup_gpio(void) {
    char path[64];
    char buf[8];
    
    printf("Setting up GPIO...\n");
    
    /* Export GPIOs */
    for (int i = 0; i < NUM_PINS; i++) {
        snprintf(buf, sizeof(buf), "%d", sw_gpio[i]);
        write_sysfs("/sys/class/gpio/export", buf);
        
        snprintf(buf, sizeof(buf), "%d", led_gpio[i]);
        write_sysfs("/sys/class/gpio/export", buf);
    }
    
    usleep(200000);
    
    /* Set directions */
    for (int i = 0; i < NUM_PINS; i++) {
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", sw_gpio[i]);
        write_sysfs(path, "in");
        
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", led_gpio[i]);
        write_sysfs(path, "out");
    }
    
    printf("GPIO setup complete\n");
}

/* Read all 4 switches and convert to decimal (0-15) */
int read_switches_as_decimal(void) {
    int decimal = 0;
    
    for (int i = 0; i < NUM_PINS; i++) {
        int bit = read_gpio(sw_gpio[i]);
        decimal |= (bit << i);  // Bit 0 is LSB, Bit 3 is MSB
        
        // Mirror switch state to LED
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", led_gpio[i]);
        write_sysfs(path, bit ? "1" : "0");
    }
    
    return decimal;
}

/* I2C Helper Functions */
int i2c_write_cmd(uint8_t cmd) {
    uint8_t buf[2] = {SSD1306_COMMAND, cmd};
    if (write(i2c_fd, buf, 2) != 2) {
        return -1;
    }
    return 0;
}

int i2c_write_data(uint8_t *data, size_t len) {
    static uint8_t buf[BUFFER_SIZE + 1];
    
    if (len > BUFFER_SIZE) return -1;
    
    buf[0] = SSD1306_DATA;
    memcpy(buf + 1, data, len);
    
    return (write(i2c_fd, buf, len + 1) == (ssize_t)(len + 1)) ? 0 : -1;
}

/* OLED Initialization */
int oled_init(void) {
    printf("Initializing OLED...\n");
    
    i2c_fd = open(I2C_BUS, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C bus");
        return -1;
    }
    
    if (ioctl(i2c_fd, I2C_SLAVE, OLED_ADDR) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        return -1;
    }
    
    /* Initialize display */
    i2c_write_cmd(SSD1306_DISPLAYOFF);
    i2c_write_cmd(SSD1306_SETDISPLAYCLOCKDIV);
    i2c_write_cmd(0x80);
    i2c_write_cmd(SSD1306_SETMULTIPLEX);
    i2c_write_cmd(0x3F);
    i2c_write_cmd(SSD1306_SETDISPLAYOFFSET);
    i2c_write_cmd(0x0);
    i2c_write_cmd(SSD1306_SETSTARTLINE | 0x0);
    i2c_write_cmd(SSD1306_CHARGEPUMP);
    i2c_write_cmd(0x14);
    i2c_write_cmd(SSD1306_MEMORYMODE);
    i2c_write_cmd(0x00);
    i2c_write_cmd(SSD1306_SEGREMAP | 0x1);
    i2c_write_cmd(SSD1306_COMSCANDEC);
    i2c_write_cmd(SSD1306_SETCOMPINS);
    i2c_write_cmd(0x12);
    i2c_write_cmd(SSD1306_SETCONTRAST);
    i2c_write_cmd(0xCF);
    i2c_write_cmd(SSD1306_SETPRECHARGE);
    i2c_write_cmd(0xF1);
    i2c_write_cmd(SSD1306_SETVCOMDETECT);
    i2c_write_cmd(0x40);
    i2c_write_cmd(SSD1306_DISPLAYALLON_RESUME);
    i2c_write_cmd(SSD1306_NORMALDISPLAY);
    i2c_write_cmd(SSD1306_DISPLAYON);
    
    printf("OLED initialized successfully\n");
    return 0;
}

/* Display Functions */
void clear_display(void) {
    memset(display_buffer, 0, BUFFER_SIZE);
}

void update_display(void) {
    i2c_write_cmd(SSD1306_PAGEADDR);
    i2c_write_cmd(0);
    i2c_write_cmd(7);
    i2c_write_cmd(SSD1306_COLUMNADDR);
    i2c_write_cmd(0);
    i2c_write_cmd(SCREEN_WIDTH - 1);
    
    for (int i = 0; i < BUFFER_SIZE; i += 16) {
        i2c_write_data(&display_buffer[i], 16);
    }
}

void draw_pixel(int x, int y, int color) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        if (color) {
            display_buffer[x + (y / 8) * SCREEN_WIDTH] |= (1 << (y % 8));
        } else {
            display_buffer[x + (y / 8) * SCREEN_WIDTH] &= ~(1 << (y % 8));
        }
    }
}

/* Draw a large digit (scaled 3x) */
void draw_large_digit(int x, int y, int digit, int scale) {
    if (digit < 0 || digit > 9) return;
    
    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[digit][col];
        for (int row = 0; row < 8; row++) {
            if (line & (1 << row)) {
                // Draw scaled pixel
                for (int sx = 0; sx < scale; sx++) {
                    for (int sy = 0; sy < scale; sy++) {
                        draw_pixel(x + col * scale + sx, y + row * scale + sy, 1);
                    }
                }
            }
        }
    }
}

/* Draw binary representation */
void draw_binary(int x, int y, int value) {
    char binary_str[5];
    sprintf(binary_str, "%04d", 
            ((value & 8) ? 1 : 0) * 1000 +
            ((value & 4) ? 1 : 0) * 100 +
            ((value & 2) ? 1 : 0) * 10 +
            ((value & 1) ? 1 : 0));
    
    for (int i = 0; i < 4; i++) {
        int digit = binary_str[i] - '0';
        draw_large_digit(x + i * 18, y, digit, 2);
    }
}

/* Draw decimal value (large, centered) */
void draw_decimal_value(int value) {
    clear_display();
    
    // Title
    char title[] = "SWITCH VALUE";
    for (int i = 0; i < 12; i++) {
        // Draw simple 5x7 characters for title (you can enhance this)
        draw_large_digit(5 + i * 10, 2, (i % 10), 1);
    }
    
    // Binary representation (top)
    draw_binary(10, 15, value);
    
    // Decimal value (large, center)
    if (value < 10) {
        // Single digit - center it
        draw_large_digit(45, 35, value, 4);
    } else {
        // Two digits
        int tens = value / 10;
        int ones = value % 10;
        draw_large_digit(30, 35, tens, 4);
        draw_large_digit(55, 35, ones, 4);
    }
    
    update_display();
}

/* Main Function */
int main(void) {
    printf("GPIO + OLED Application Starting...\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Setup hardware */
    setup_gpio();
    
    if (oled_init() < 0) {
        fprintf(stderr, "Failed to initialize OLED\n");
        return 1;
    }
    
    /* Display startup message */
    clear_display();
    draw_large_digit(20, 25, 8, 3);  // Show "8" initially
    draw_large_digit(50, 25, 8, 3);
    draw_large_digit(80, 25, 8, 3);
    update_display();
    sleep(2);
    
    /* Main loop */
    int last_value = -1;
    
    while (running) {
        int current_value = read_switches_as_decimal();
        
        /* Only update display if value changed */
        if (current_value != last_value) {
            printf("Switch value: %d (Binary: %d%d%d%d)\n",
                   current_value,
                   (current_value & 8) ? 1 : 0,
                   (current_value & 4) ? 1 : 0,
                   (current_value & 2) ? 1 : 0,
                   (current_value & 1) ? 1 : 0);
            
            draw_decimal_value(current_value);
            last_value = current_value;
        }
        
        usleep(50000);  // 50ms debounce
    }
    
    /* Cleanup */
    clear_display();
    draw_large_digit(40, 25, 0, 4);  // Show "0" on exit
    update_display();
    
    if (i2c_fd >= 0) {
        sleep(1);
        i2c_write_cmd(SSD1306_DISPLAYOFF);
        close(i2c_fd);
    }
    
    printf("Application terminated\n");
    return 0;
}