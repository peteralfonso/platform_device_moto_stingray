$(call inherit-product, $(SRC_TARGET_DIR)/product/generic.mk)
$(call inherit-product, device/moto/stingray/device.mk)
$(call inherit-product-if-exists, vendor/moto/stingray/stingray-vendor.mk)

$(call inherit-product-if-exists, vendor/google/products/generic.mk)
$(call inherit-product-if-exists, vendor/google/products/generic_90mb.mk)

# Overrides
PRODUCT_DEVICE := stingray
PRODUCT_LOCALES += en_US
PRODUCT_MODEL := Motorola Stingray
PRODUCT_NAME := stingray

DEVICE_PACKAGE_OVERLAYS := device/moto/stingray/overlay
