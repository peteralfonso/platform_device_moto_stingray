#
# Copyright (C) 2010 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

DEVICE_PACKAGE_OVERLAYS := \
    device/moto/stingray/overlay

PRODUCT_PROPERTY_OVERRIDES := \
    wifi.interface=eth0 \
    wifi.supplicant_scan_interval=15 \
    dalvik.vm.heapsize=32m

PRODUCT_COPY_FILES += \
    device/moto/stingray/bcm4329.ko:system/lib/modules/bcm4329.ko \
    device/moto/stingray/mXT1386_08_AA.bin:system/etc/firmware/mXT1386_08_AA.bin \
    device/moto/stingray/mXT1386_08_E1.bin:system/etc/firmware/mXT1386_08_E1.bin \
    device/moto/stingray/mXT1386_09_AA.bin:system/etc/firmware/mXT1386_09_AA.bin \
    device/moto/stingray/mXT1386_10_AA.bin:system/etc/firmware/mXT1386_10_AA.bin

PRODUCT_COPY_FILES += \
    frameworks/base/data/etc/tablet_core_hardware.xml:system/etc/permissions/tablet_core_hardware.xml \
    frameworks/base/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
    frameworks/base/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    frameworks/base/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/base/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/base/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    packages/wallpapers/LivePicker/android.software.live_wallpaper.xml:system/etc/permissions/android.software.live_wallpaper.xml

PRODUCT_COPY_FILES += \
        device/moto/stingray/vold.fstab:system/etc/vold.fstab \
        device/moto/stingray/qtouch-touchscreen.idc:system/usr/idc/qtouch-touchscreen.idc

PRODUCT_PACKAGES := \
    sensors.stingray \
    lights.stingray \
    librs_jni \
    make_ext4fs

# we have enough storage space to hold precise GC data
PRODUCT_TAGS += dalvik.gc.type-precise

# media config xml file
PRODUCT_COPY_FILES += \
    device/moto/stingray/media_profiles.xml:system/etc/media_profiles.xml
