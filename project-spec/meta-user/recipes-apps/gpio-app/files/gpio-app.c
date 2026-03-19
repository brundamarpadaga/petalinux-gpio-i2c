/*
* Copyright (C) 2013 - 2016  Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without restriction,
* including without limitation the rights to use, copy, modify, merge,
* publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so,
* subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in this
* Software without prior written authorization from Xilinx.
*
*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define NUM_PINS 4

int sw_gpio[NUM_PINS]  = {1012, 1013, 1014, 1015};
int led_gpio[NUM_PINS] = {1020, 1021, 1022, 1023};

void write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, value, strlen(value));
    close(fd);
}

int read_gpio(int gpio)
{
    char path[64];
    char value;
    int fd;

    snprintf(path, sizeof(path),
             "/sys/class/gpio/gpio%d/value", gpio);

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    read(fd, &value, 1);
    close(fd);

    return (value == '1') ? 1 : 0;
}

int main()
{
    char path[64];
    char buf[8];

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
        snprintf(path, sizeof(path),
                 "/sys/class/gpio/gpio%d/direction", sw_gpio[i]);
        write_sysfs(path, "in");

        snprintf(path, sizeof(path),
                 "/sys/class/gpio/gpio%d/direction", led_gpio[i]);
        write_sysfs(path, "out");
    }

    /* Main loop */
    while (1) {
        for (int i = 0; i < NUM_PINS; i++) {
            int val = read_gpio(sw_gpio[i]);

            snprintf(path, sizeof(path),
                     "/sys/class/gpio/gpio%d/value", led_gpio[i]);

            write_sysfs(path, val ? "1" : "0");
        }
        usleep(50000);  // debounce / reduce CPU usage
    }

    return 0;
}
