/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include <string.h>
#include <linux/input.h>
#include <cutils/properties.h>

#include "recovery_ui.h"
#include "common.h"

int bp_master_clear(void);

char* MENU_HEADERS[] = { "Use volume keys to highlight; power button to select.",
                         "",
                         NULL };

char* MENU_ITEMS[] = { "reboot system now",
                       "apply update from USB drive",
                       "wipe data/factory reset",
                       "wipe cache partition",
                       NULL };

void device_ui_init(UIParameters* ui_parameters) {
}

int device_recovery_start() {
    return 0;
}

int device_toggle_display(volatile char* key_pressed, int key_code) {
    // hold power key and press volume-up
    return key_pressed[KEY_END] && key_code == KEY_VOLUMEUP;
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    // Reboot if the power key is pressed five times in a row, with
    // no other keys in between.
    static int presses = 0;
    if (key_code == KEY_END) {   // power button
        ++presses;
        return presses == 5;
    } else {
        presses = 0;
        return 0;
    }
}

int device_handle_key(int key_code, int visible) {
    if (visible) {
        switch (key_code) {
            case KEY_DOWN:
            case KEY_VOLUMEDOWN:
                return HIGHLIGHT_DOWN;

            case KEY_UP:
            case KEY_VOLUMEUP:
                return HIGHLIGHT_UP;

            case KEY_END:
                return SELECT_ITEM;
        }
    }

    return NO_ACTION;
}

int device_perform_action(int which) {
    return which;
}

static int device_has_bp(void) {
    char value[PROPERTY_VALUE_MAX];

    property_get("ro.carrier", value, "");
    if (strcmp("wifi-only", value) == 0)
        return 0;
    else
        return 1;
}

int device_wipe_data() {
    int result = 0;

    if (device_has_bp()) {
        ui_print("Performing BP clear...\n");
        result = bp_master_clear();
        if(result == 0)
            ui_print("BP clear complete successfully.\n");
        else
            ui_print("BP clear failed.\n");
    }

    return 0;
}
