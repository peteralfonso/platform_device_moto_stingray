ifneq ($(TARGET_SIMULATOR),true)
ifeq ($(TARGET_ARCH),arm)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := eng

LOCAL_SRC_FILES := recovery_ui.c
LOCAL_C_INCLUDES += bootable/recovery
# should match TARGET_RECOVERY_UI_LIB set in BoardConfig.mk
LOCAL_MODULE := librecovery_ui_stingray
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

# LOCAL_SRC_FILES := recovery_updater.c \
#                    update_cdma_bp.c \
#                    cdma_bp.c \
#                    bputils.c

# LOCAL_C_INCLUDES += bootable/recovery vendor/moto/sholes
# # should match TARGET_RECOVERY_UPDATER_LIBS set in BoardConfig.mk
# LOCAL_MODULE := librecovery_updater_sholes
# include $(BUILD_STATIC_LIBRARY)

endif   # TARGET_ARCH == arm
endif   # !TARGET_SIMULATOR
