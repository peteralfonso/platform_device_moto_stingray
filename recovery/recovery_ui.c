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

#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"

char* MENU_HEADERS[] = { "Use volume keys or menu/back to highlight; home button to select.",
                         "",
                         NULL };

char* MENU_ITEMS[] = { "reboot system now",
                       "apply update from USB drive",
                       "wipe data/factory reset",
                       "wipe cache partition",
                       "restart recovery",   // STOPSHIP: remove this option
                       NULL };

int device_recovery_start() {
    return 0;
}

int device_toggle_display(volatile char* key_pressed, int key_code) {
    return key_pressed[KEY_VOLUMEUP] && key_code == KEY_MENU;
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    // Reboot if the power key is pressed three times in a row, with
    // no other keys in between.
    static int presses = 0;
    if (key_code == KEY_END) {   // power button
        ++presses;
        return presses == 3;
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
            case KEY_BACK:
                return HIGHLIGHT_DOWN;

            case KEY_UP:
            case KEY_VOLUMEUP:
            case KEY_MENU:
                return HIGHLIGHT_UP;

            case KEY_HOME:
                return SELECT_ITEM;
        }
    }

    return NO_ACTION;
}

int device_perform_action(int which) {
    if (which < 4) return which;
    switch (which) {
        case 4: exit(0); break;        // STOPSHIP: remove this option
    }
    return -1;
}

int device_wipe_data() {
    return 0;
}
