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
static int batt_rgb_on = 0;
static int batt_blink = 0;
static int notification_rgb_on = 0;
static int notification_blink = 0;
static int attention_rgb_on = 0;
static int attention_blink = 0;

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

static void set_lcd_brightness_mode(int mode)
{
	if (lcd_brightness_mode != mode) {
		write_int("/sys/class/leds/lcd-backlight/als",
			(mode == BRIGHTNESS_MODE_SENSOR ? AUTOMATIC : MANUAL_SENSOR));
		lcd_brightness_mode = mode;
    }
}

static int is_lit(struct light_state_t const *state)
{
	return state->color & 0x00ffffff;
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
set_light_keyboard(struct light_device_t *dev,
		   struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	err = write_int("/sys/class/leds/keyboard-backlight/brightness",
		      brightness);
	pthread_mutex_unlock(&g_lock);

	return err;
}

static int
set_light_buttons(struct light_device_t *dev, struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	err = write_int("/sys/class/leds/button-backlight/brightness",
		      brightness);
	pthread_mutex_unlock(&g_lock);

	return err;
}

static int
set_attention_led(struct light_device_t *dev, struct light_state_t const *state)
{
	int len;
	int red, green, blue;
	int blink;
	int onMS, offMS;
	unsigned int colorRGB;

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

	colorRGB = state->color;

#if 0
	LOGD("set_attention colorRGB=%08X, onMS=%d, offMS=%d\n",
	     colorRGB, onMS, offMS);
#endif

	red = (colorRGB >> 16) & 0xFF;
	green = (colorRGB >> 8) & 0xFF;
	blue = colorRGB & 0xFF;

	/* Ignore the SOL beacon */
	if (red & green & blue)
		return 0;

	if (onMS > 0 && offMS > 0) {
		blink = 1;
		attention_blink = 1;
	} else {
		if (batt_blink || notification_blink) {
			blink = 1;
		}
		else {
			attention_blink = 0;
			blink = 0;
		}
	}

	if (red || green || blue) {
		attention_rgb_on = colorRGB & 0x00ffffff;
	} else  {
		attention_rgb_on = 0x00;
	}

	if (attention_rgb_on == 0) {
		if (batt_rgb_on) {
			red = (batt_rgb_on >> 16) & 0xFF;
			green = (batt_rgb_on >> 8) & 0xFF;
			blue = batt_rgb_on & 0xFF;
		} else if (notification_rgb_on) {
			red = (notification_rgb_on >> 16) & 0xFF;
			green = (notification_rgb_on >> 8) & 0xFF;
			blue = notification_rgb_on & 0xFF;
		}
	} else if(batt_rgb_on) {
		red = (batt_rgb_on >> 16) & 0xFF;
		green = (batt_rgb_on >> 8) & 0xFF;
		blue = batt_rgb_on & 0xFF;
	}

	pthread_mutex_lock(&g_lock);

	write_int("/sys/class/leds/red/blink", blink);
	write_int("/sys/class/leds/red/brightness", red);
	write_int("/sys/class/leds/green/brightness", green);
	write_int("/sys/class/leds/blue/brightness", blue);

	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int
set_msg_indicator(struct light_device_t *dev, struct light_state_t const *state)
{
	int len;
	int red, green, blue;
	int blink;
	int onMS, offMS;
	unsigned int colorRGB;

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

	colorRGB = state->color;

#if 0
	LOGD("set_notification colorRGB=%08X, onMS=%d, offMS=%d\n",
	     colorRGB, onMS, offMS);
#endif

	red = (colorRGB >> 16) & 0xFF;
	green = (colorRGB >> 8) & 0xFF;
	blue = colorRGB & 0xFF;

	/* Ignore the SOL beacon */
	if (red & green & blue)
		return 0;

	if (onMS > 0 && offMS > 0) {
		blink = 1;
		notification_blink = 1;

	} else {
		if (batt_blink || attention_blink) {
			blink = 1;
		} else {
			notification_blink = 0;
			blink = 0;
		}
	}

	if (red || green || blue) {
		notification_rgb_on = colorRGB & 0x00ffffff;
	} else  {
		notification_rgb_on = 0x00;
	}

	if (notification_rgb_on == 0) {
		if (batt_rgb_on) {
			red = (batt_rgb_on >> 16) & 0xFF;
			green = (batt_rgb_on >> 8) & 0xFF;
			blue = batt_rgb_on & 0xFF;
		} else if (attention_rgb_on) {
			red = (attention_rgb_on >> 16) & 0xFF;
			green = (attention_rgb_on >> 8) & 0xFF;
			blue = attention_rgb_on & 0xFF;
		}
	} else if(batt_rgb_on) {
		red = (batt_rgb_on >> 16) & 0xFF;
		green = (batt_rgb_on >> 8) & 0xFF;
		blue = batt_rgb_on & 0xFF;
	}

	pthread_mutex_lock(&g_lock);
	write_int("/sys/class/leds/red/blink", blink);
	write_int("/sys/class/leds/red/brightness", red);
	write_int("/sys/class/leds/green/brightness", green);
	write_int("/sys/class/leds/blue/brightness", blue);

	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int
set_batt_indicator(struct light_device_t *dev, struct light_state_t const *state)
{
	int len;
	int red, green, blue;
    int blink;
	int onMS, offMS;
	unsigned int colorRGB;

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

	colorRGB = state->color;
#if 0
	LOGD("set_batt colorRGB=%08X, onMS=%d, offMS=%d\n",
	     colorRGB, onMS, offMS);
#endif

	red = (colorRGB >> 16) & 0xFF;
	green = (colorRGB >> 8) & 0xFF;
	blue = colorRGB & 0xFF;

	if (onMS > 0 && offMS > 0) {
		batt_blink = 1;
		blink = 1;
	} else {
		if (notification_blink || attention_blink) {
			blink = 1;
		} else {
			batt_blink = 0;
			blink = 0;
		}
	}

	if (red) {
		if (green) {
			batt_rgb_on = 0x00;
			red = 0x0;
			green = 0x0;
			blue = 0x0;
		} else {
			batt_rgb_on = colorRGB & 0x00ffffff;
		}
	} else {
		batt_rgb_on = 0x00;
		red = 0x0;
		green = 0x0;
		blue = 0x0;
	}

	if (batt_rgb_on == 0) {
		if (notification_rgb_on) {
			red = (notification_rgb_on >> 16) & 0xFF;
			green = (notification_rgb_on >> 8) & 0xFF;
			blue = notification_rgb_on & 0xFF;
		} else if (attention_rgb_on) {
			red = (attention_rgb_on >> 16) & 0xFF;
			green = (attention_rgb_on >> 8) & 0xFF;
			blue = attention_rgb_on & 0xFF;
		}
	}

	pthread_mutex_lock(&g_lock);

	write_int("/sys/class/leds/red/blink", blink);
	write_int("/sys/class/leds/red/brightness", red);
	write_int("/sys/class/leds/green/brightness", green);
	write_int("/sys/class/leds/blue/brightness", blue);

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
	else if (0 == strcmp(LIGHT_ID_KEYBOARD, name))
		set_light = set_light_keyboard;
	else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
		set_light = set_light_buttons;
	else if (0 == strcmp(LIGHT_ID_BATTERY, name))
		set_light = set_batt_indicator;
	else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
		set_light = set_msg_indicator;
	else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
		set_light = set_attention_led;
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
