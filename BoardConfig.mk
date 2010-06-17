TARGET_BOARD_PLATFORM := tegra

TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_SMP := true
TARGET_ARCH_VARIANT := armv7-a
ARCH_ARM_HAVE_TLS_REGISTER := true

TARGET_USERIMAGES_USE_EXT4 := true

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER := WEXT
WPA_SUPPLICANT_VERSION      := VER_0_6_X
BOARD_WLAN_DEVICE           := bcm4329
WIFI_DRIVER_MODULE_PATH     := "/system/lib/modules/bcm4329.ko"
WIFI_DRIVER_FW_STA_PATH     := "/system/etc/firmware/fw_bcm4329.bin"
WIFI_DRIVER_FW_AP_PATH      := "/system/etc/firmware/fw_bcm4329_apsta.bin"
WIFI_DRIVER_MODULE_ARG      := "firmware_path=/system/etc/firmware/fw_bcm4329.bin nvram_path=/system/etc/wifi/bcm4329.cal"
WIFI_DRIVER_MODULE_NAME     := "bcm4329"
WIFI_BAND                   := 802_11_BG

BOARD_USES_GENERIC_AUDIO := true
USE_CAMERA_STUB := true
USE_E2FSPROGS := true

# needed for source compilation of nvidia libraries
-include vendor/nvidia/proprietary_src/build/definitions.mk

BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_BCM := true

BOARD_EGL_CFG := device/moto/stingray/egl.cfg
BOARD_KERNEL_CMDLINE := mem=448M@0M console=ttyS0,115200n8 tegrapart=mmcblk0=system:3600:10000:800,cache:13600:4000:800,userdata:17600:80000:800 debug

COMMON_DIR := vendor/nvidia/common/
TARGET_TEGRA_VERSION := ap20
