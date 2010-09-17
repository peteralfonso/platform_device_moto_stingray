/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"

#include <cutils/log.h>

#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <linux/input.h>

#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>

#include <hardware/lights.h>

#define	MANUAL		0
#define	AUTOMATIC	1
#define	MANUAL_SENSOR	2

/******************************************************************************/


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int lcd_brightness_mode = -1;

/**
 * device methods
 */

static int write_int(char const *path, int value)
{
	int fd;
	static int already_warned = -1;
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%d\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == -1) {
			LOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}

static int write_string(char const *path, char const *value)
{
	int fd;
	static int already_warned = -1;
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%s\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == -1) {
			LOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}


static void set_lcd_brightness_mode(int mode)
{
	if (lcd_brightness_mode != mode) {
		write_int("/sys/class/leds/lcd-backlight/als",
			(mode == BRIGHTNESS_MODE_SENSOR ? AUTOMATIC : MANUAL_SENSOR));
		lcd_brightness_mode = mode;
    }
}

static int rgb_to_brightness(struct light_state_t const *state)
{
	int color = state->color & 0x00ffffff;
	return ((77 * ((color >> 16) & 0x00ff))
		+ (150 * ((color >> 8) & 0x00ff)) +
		(29 * (color & 0x00ff))) >> 8;
}

static int
set_light_backlight(struct light_device_t *dev,
		    struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	set_lcd_brightness_mode(state->brightnessMode);
	err = write_int("/sys/class/leds/lcd-backlight/brightness", brightness);
	pthread_mutex_unlock(&g_lock);

	return err;
}

static int
set_msg_indicator(struct light_device_t *dev, struct light_state_t const *state)
{
	int blink;
	int onMS, offMS;
	unsigned int brightness;

	switch (state->flashMode) {
	case LIGHT_FLASH_TIMED:
		onMS = state->flashOnMS;
		offMS = state->flashOffMS;
		break;
	case LIGHT_FLASH_NONE:
	default:
		onMS = 0;
		offMS = 0;
		break;
	}

#if 1
	LOGD("set_notification colorRGB=%08X, onMS=%d, offMS=%d\n",
	     state->color, onMS, offMS);
#endif

	brightness = rgb_to_brightness(state);

	if (onMS > 0 && offMS > 0)
		blink = 1;
	else
		blink = 0;

	pthread_mutex_lock(&g_lock);

	if ( brightness == 0 ) {
		write_int("/sys/class/leds/notification-led/brightness", brightness);
		write_string("/sys/class/leds/notification-led/trigger", "timer");
	} else {
		write_int("/sys/class/leds/notification-led/brightness", brightness);
		write_int("/sys/class/leds/notification-led/delay_on", blink);
	}

	pthread_mutex_unlock(&g_lock);

	return 0;
}

/** Close the lights device */
static int close_lights(struct light_device_t *dev)
{
	if (dev)
		free(dev);
	return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t *module, char const *name,
		       struct hw_device_t **device)
{
	pthread_t lighting_poll_thread;

	int (*set_light) (struct light_device_t *dev,
			  struct light_state_t const *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
		set_light = set_light_backlight;
	else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
		set_light = set_msg_indicator;
	else
		return -EINVAL;

	pthread_mutex_init(&g_lock, NULL);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = (int (*)(struct hw_device_t *))close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
	.open = open_lights,
};

/*
 * The lights Module
 */
const struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 1,
	.version_minor = 0,
	.id = LIGHTS_HARDWARE_MODULE_ID,
	.name = "Nvidia lights Module",
	.author = "Motorola, Inc.",
	.methods = &lights_module_methods,
};
