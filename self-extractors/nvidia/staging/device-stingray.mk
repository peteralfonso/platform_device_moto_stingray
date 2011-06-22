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

# NVIDIA blobs necessary for stingray / wingray
PRODUCT_COPY_FILES := \
    vendor/nvidia/stingray/proprietary/camera.stingray.so:system/lib/hw/camera.stingray.so \
    vendor/nvidia/stingray/proprietary/gps.stingray.so:system/lib/hw/gps.stingray.so \
    vendor/nvidia/stingray/proprietary/gralloc.tegra.so:system/lib/hw/gralloc.tegra.so \
    vendor/nvidia/stingray/proprietary/hwcomposer.tegra.so:system/lib/hw/hwcomposer.tegra.so \
    vendor/nvidia/stingray/proprietary/libcgdrv.so:system/lib/libcgdrv.so \
    vendor/nvidia/stingray/proprietary/libEGL_tegra.so:system/lib/egl/libEGL_tegra.so \
    vendor/nvidia/stingray/proprietary/libGLESv1_CM_tegra.so:system/lib/egl/libGLESv1_CM_tegra.so \
    vendor/nvidia/stingray/proprietary/libGLESv2_tegra.so:system/lib/egl/libGLESv2_tegra.so \
    vendor/nvidia/stingray/proprietary/libnvddk_2d.so:system/lib/libnvddk_2d.so \
    vendor/nvidia/stingray/proprietary/libnvddk_2d_v2.so:system/lib/libnvddk_2d_v2.so \
    vendor/nvidia/stingray/proprietary/libnvddk_audiofx.so:system/lib/libnvddk_audiofx.so \
    vendor/nvidia/stingray/proprietary/libnvdispatch_helper.so:system/lib/libnvdispatch_helper.so \
    vendor/nvidia/stingray/proprietary/libnvdispmgr_d.so:system/lib/libnvdispmgr_d.so \
    vendor/nvidia/stingray/proprietary/libnvmm.so:system/lib/libnvmm.so \
    vendor/nvidia/stingray/proprietary/libnvmm_camera.so:system/lib/libnvmm_camera.so \
    vendor/nvidia/stingray/proprietary/libnvmm_contentpipe.so:system/lib/libnvmm_contentpipe.so \
    vendor/nvidia/stingray/proprietary/libnvmm_image.so:system/lib/libnvmm_image.so \
    vendor/nvidia/stingray/proprietary/libnvmm_manager.so:system/lib/libnvmm_manager.so \
    vendor/nvidia/stingray/proprietary/libnvmm_service.so:system/lib/libnvmm_service.so \
    vendor/nvidia/stingray/proprietary/libnvmm_tracklist.so:system/lib/libnvmm_tracklist.so \
    vendor/nvidia/stingray/proprietary/libnvmm_utils.so:system/lib/libnvmm_utils.so \
    vendor/nvidia/stingray/proprietary/libnvmm_video.so:system/lib/libnvmm_video.so \
    vendor/nvidia/stingray/proprietary/libnvodm_imager.so:system/lib/libnvodm_imager.so \
    vendor/nvidia/stingray/proprietary/libnvodm_query.so:system/lib/libnvodm_query.so \
    vendor/nvidia/stingray/proprietary/libnvomx.so:system/lib/libnvomx.so \
    vendor/nvidia/stingray/proprietary/libnvomxilclient.so:system/lib/libnvomxilclient.so \
    vendor/nvidia/stingray/proprietary/libnvos.so:system/lib/libnvos.so \
    vendor/nvidia/stingray/proprietary/libnvrm.so:system/lib/libnvrm.so \
    vendor/nvidia/stingray/proprietary/libnvrm_channel.so:system/lib/libnvrm_channel.so \
    vendor/nvidia/stingray/proprietary/libnvrm_graphics.so:system/lib/libnvrm_graphics.so \
    vendor/nvidia/stingray/proprietary/libnvsm.so:system/lib/libnvsm.so \
    vendor/nvidia/stingray/proprietary/libnvwsi.so:system/lib/libnvwsi.so \
    vendor/nvidia/stingray/proprietary/libstagefrighthw.so:system/lib/libstagefrighthw.so \
    vendor/nvidia/stingray/proprietary/nvddk_audiofx_core.axf:system/etc/firmware/nvddk_audiofx_core.axf \
    vendor/nvidia/stingray/proprietary/nvddk_audiofx_transport.axf:system/etc/firmware/nvddk_audiofx_transport.axf \
    vendor/nvidia/stingray/proprietary/nvmm_aacdec.axf:system/etc/firmware/nvmm_aacdec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_adtsdec.axf:system/etc/firmware/nvmm_adtsdec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_audiomixer.axf:system/etc/firmware/nvmm_audiomixer.axf \
    vendor/nvidia/stingray/proprietary/nvmm_h264dec.axf:system/etc/firmware/nvmm_h264dec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_jpegdec.axf:system/etc/firmware/nvmm_jpegdec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_jpegenc.axf:system/etc/firmware/nvmm_jpegenc.axf \
    vendor/nvidia/stingray/proprietary/nvmm_manager.axf:system/etc/firmware/nvmm_manager.axf \
    vendor/nvidia/stingray/proprietary/nvmm_mp2dec.axf:system/etc/firmware/nvmm_mp2dec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_mp3dec.axf:system/etc/firmware/nvmm_mp3dec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_mpeg4dec.axf:system/etc/firmware/nvmm_mpeg4dec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_reference.axf:system/etc/firmware/nvmm_reference.axf \
    vendor/nvidia/stingray/proprietary/nvmm_service.axf:system/etc/firmware/nvmm_service.axf \
    vendor/nvidia/stingray/proprietary/nvmm_sorensondec.axf:system/etc/firmware/nvmm_sorensondec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_sw_mp3dec.axf:system/etc/firmware/nvmm_sw_mp3dec.axf \
    vendor/nvidia/stingray/proprietary/nvmm_wavdec.axf:system/etc/firmware/nvmm_wavdec.axf \
    vendor/nvidia/stingray/proprietary/nvrm_avp.bin:system/etc/firmware/nvrm_avp.bin
