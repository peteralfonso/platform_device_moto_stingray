/*
** Copyright 2010, The Android Open-Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioPostProcessor"
#include <utils/Log.h>
#include "AudioHardware.h"
#include "AudioPostProcessor.h"

// hardware specific functions
extern uint16_t HC_CTO_AUDIO_MM_PARAMETER_TABLE[];

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

// Define the mode offsets in the "VOIPgainfile.bin"
enum {
   ECNS_MODE_HANDSET_8K = 0, // Not on Everest
   ECNS_MODE_HEADSET_8K,
   ECNS_MODE_LOUDSPEAKER_8K,
   ECNS_MODE_RESERVED1_8K,
   ECNS_MODE_RESERVED2_8K,
   ECNS_MODE_16K_OFFSET,  // Next 5 are duplicates of first 5, at 16k.
   ECNS_MODE_HANDSET_16K = ECNS_MODE_16K_OFFSET,
   ECNS_MODE_HEADSET_16K,
   ECNS_MODE_LOUDSPEAKER_16K,
   ECNS_MODE_RESERVED1_16K,
   ECNS_MODE_RESERVED2_16K,
};

namespace android {

AudioPostProcessor::AudioPostProcessor()
{
    LOGD("%s",__FUNCTION__);

    // One-time CTO Audio configuration
    mAudioMmEnvVar.cto_audio_mm_param_block_ptr              = HC_CTO_AUDIO_MM_PARAMETER_TABLE;
    mAudioMmEnvVar.cto_audio_mm_pcmlogging_buffer_block_ptr  = mPcmLoggingBuf;
    mAudioMmEnvVar.pcmlogging_buffer_block_size              = ARRAY_SIZE(mPcmLoggingBuf);
    mAudioMmEnvVar.cto_audio_mm_runtime_param_mem_ptr        = mRuntimeParam;
    mAudioMmEnvVar.cto_audio_mm_static_memory_block_ptr      = mStaticMem;
    mAudioMmEnvVar.cto_audio_mm_scratch_memory_block_ptr     = mScratchMem;
    mAudioMmEnvVar.accy = CTO_AUDIO_MM_ACCY_INVALID;
    mAudioMmEnvVar.sample_rate = CTO_AUDIO_MM_SAMPL_44100;

    // Initial conditions for EC/NS
    // TODO: Consider Android Mutex/Condition
    pthread_mutex_init(&mEcnsBufLock, 0);
    pthread_cond_init(&mEcnsBufCond, 0);
    stopEcns();
}

AudioPostProcessor::~AudioPostProcessor()
{
    LOGD("%s",__FUNCTION__);
    if(mEcnsRunning)
        stopEcns();
    pthread_cond_destroy(&mEcnsBufCond);
    pthread_mutex_destroy(&mEcnsBufLock);
}

uint32_t AudioPostProcessor::convOutDevToCTO(uint32_t outDev)
{
    // Only loudspeaker and audio docks are currently in this table
    switch (outDev) {
       case CPCAP_AUDIO_OUT_SPEAKER:
           return CTO_AUDIO_MM_ACCY_LOUDSPEAKER;
//     case CPCAP_AUDIO_OUT_EMU:  Not yet implemented, Basic Dock.
//         return CTO_AUDIO_MM_ACCY_DOCK;
       default:
           return CTO_AUDIO_MM_ACCY_INVALID;
    }
}

uint32_t AudioPostProcessor::convRateToCto(uint32_t rate)
{
    switch (rate) {
        case 44100: // Most likely.
            return CTO_AUDIO_MM_SAMPL_44100;
        case 8000:
            return CTO_AUDIO_MM_SAMPL_8000;
        case 11025:
            return CTO_AUDIO_MM_SAMPL_11025;
        case 12000:
            return CTO_AUDIO_MM_SAMPL_12000;
        case 16000:
            return CTO_AUDIO_MM_SAMPL_16000;
        case 22050:
            return CTO_AUDIO_MM_SAMPL_22050;
        case 24000:
            return CTO_AUDIO_MM_SAMPL_24000;
        case 32000:
            return CTO_AUDIO_MM_SAMPL_32000;
        case 48000:
            return CTO_AUDIO_MM_SAMPL_48000;
        default:
            return CTO_AUDIO_MM_SAMPL_44100;
    }
}

void AudioPostProcessor::configMmAudio()
{
    if (mAudioMmEnvVar.accy != CTO_AUDIO_MM_ACCY_INVALID) {
        LOGD("Configure CTO Audio MM processing");
        // fetch the corresponding runtime audio parameter
        api_cto_audio_mm_param_parser(&(mAudioMmEnvVar), (int16_t *)0, (int16_t *)0);
        // Initialize algorithm static memory
        api_cto_audio_mm_init(&(mAudioMmEnvVar), (int16_t *)0, (int16_t *)0);
    } else {
        LOGD("CTO Audio MM processing is disabled.");
    }
}

void AudioPostProcessor::enableEcns(bool value)
{
    LOGD("enableEcns(%s)",value?"true":"false");
    mEcnsEnabled = value;
}

void AudioPostProcessor::setAudioDev(struct cpcap_audio_stream *outDev)
{
    uint32_t mm_accy = convOutDevToCTO(outDev->id);
    Mutex::Autolock lock(mMmLock);

    if(outDev->id==CPCAP_AUDIO_OUT_HEADSET)
        mEcnsMode = ECNS_MODE_HEADSET_8K;
//    else if(outDev->id==CPCAP_AUDIO_OUT_EMU)  -- basic dock
//        mEcnsmode = ECNS_MODE_RESERVED1_8K;
    else
        mEcnsMode = ECNS_MODE_LOUDSPEAKER_8K;

    if(mEcnsEnabled) {
        // We may need to reset the EC/NS if the output device changed.
        stopEcns();
    }

    LOGD("setAudioDev %d", outDev->id);
    if (mm_accy != mAudioMmEnvVar.accy) {
        mAudioMmEnvVar.accy = mm_accy;
        configMmAudio();
    }
}

// Setting the HW sampling rate may require reconfiguration of audio processing.
void AudioPostProcessor::setPlayAudioRate(int sampRate)
{
    uint32_t rate = convRateToCto(sampRate);
    Mutex::Autolock lock(mMmLock);

    LOGD("AudioPostProcessor::setPlayAudioRate %d", sampRate);
    if (rate != mAudioMmEnvVar.sample_rate) {
        mAudioMmEnvVar.sample_rate = rate;
        configMmAudio();
    }
}

void AudioPostProcessor::doMmProcessing(void * buffer, int numSamples)
{
    Mutex::Autolock lock(mMmLock);

    if (mAudioMmEnvVar.accy != CTO_AUDIO_MM_ACCY_INVALID &&
        !mEcnsEnabled) {
        // Apply the CTO audio effects in-place.
        mAudioMmEnvVar.frame_size = numSamples;
        api_cto_audio_mm_main(&mAudioMmEnvVar, (int16_t *)buffer, (int16_t *)buffer);
    }
}

int AudioPostProcessor::getEcnsRate (void)
{
    return mEcnsRate;
}

void AudioPostProcessor::initEcns(int rate, int bytes)
{
    LOGD("%s",__FUNCTION__);
    int mode = mEcnsMode;
    pthread_mutex_lock(&mEcnsBufLock);
    mEcnsRate = rate;

    if(mEcnsRate != 8000 && mEcnsRate != 16000) {
        LOGW("Invalid rate for EC/NS, disabling");
        mEcnsEnabled = 0;
        mEcnsRunning = 0;
    }

    if(mEcnsRate==16000) {
       // Offset to the 16K block in the coefficients file
       mode += ECNS_MODE_16K_OFFSET;
    }
    LOGD("%s for mode %d at %d size %d",__FUNCTION__, mode, mEcnsRate, bytes);
    mEcnsCtrl.framesize = bytes/2;
    mEcnsCtrl.micFlag = 0; // 0- one mic.  1- dual mic. 2- three mic.
    mEcnsCtrl.rate = (rate == 8000) ? 0 : 1;  // 8K or 16K
    mMemBlocks.staticMemory_1 = mStaticMemory_1;
    mMemBlocks.staticMemory_2 = NULL;
    mMemBlocks.mot_datalog = mMotDatalog;
    mMemBlocks.scratchMemory = mScratchMemory;

    FILE * fp = fopen("/system/etc/VOIPgainfile.bin", "r");
    if(fp) {
        fseek(fp, AUDIO_PROFILE_PARAMETER_BLOCK_WORD16_SIZE*2*mode, SEEK_SET);
        if(fread(mAudioProfile, AUDIO_PROFILE_PARAMETER_BLOCK_WORD16_SIZE*2,1, fp) < 1) {
            LOGE("Cannot read VOIP gain file.  Disabling EC/NS.");
            fclose(fp);
            mEcnsEnabled = 0;
            mEcnsRunning = 0;
            pthread_mutex_unlock(&mEcnsBufLock);
            return;
        }
        fclose(fp);
    }
    else {
        LOGE("Cannot open VOIP gain file.  Disabling EC/NS.");
        mEcnsEnabled = 0;
        mEcnsRunning = 0;
        pthread_mutex_unlock(&mEcnsBufLock);
        return;
    }

    mMemBlocks.audioprofileMemory = mAudioProfile;
    mEcnsRunning = 1;
    mEcnsOutBuf = 0;
    mEcnsOutBufSize = 0;
    mEcnsOutBufReadOffset = 0;

    // Send setup parameters to the EC/NS module, init the module.
    API_MOT_SETUP(&mEcnsCtrl, &mMemBlocks);
    API_MOT_INIT(&mEcnsCtrl, &mMemBlocks);

    pthread_mutex_unlock(&mEcnsBufLock);
}
void AudioPostProcessor::stopEcns (void)
{
    LOGD("%s",__FUNCTION__);
    pthread_mutex_lock(&mEcnsBufLock);
    mEcnsRunning = 0;
    mEcnsRate = 0;
    if (mEcnsScratchBuf) {
        free(mEcnsScratchBuf);
        mEcnsScratchBuf = 0;
    }
    mEcnsScratchBufSize = 0;
    mEcnsOutFd = -1;
    if(mLogFp) {
       fclose(mLogFp);
       mLogFp = 0;
    }
    // In case write() is blocked, set it free.
    pthread_cond_signal(&mEcnsBufCond);
    pthread_mutex_unlock(&mEcnsBufLock);
}

// Returns: Bytes written (actually "to-be-written" by read thread).
int AudioPostProcessor::writeDownlinkEcns(int fd, void * buffer, int bytes)
{
    int written = 0;
    pthread_mutex_lock(&mEcnsBufLock);
    if (mEcnsRunning) {
        // Only run through here after initEcns has been done by read thread.
        //LOGD("%s",__FUNCTION__);
        mEcnsOutFd = fd;
        mEcnsOutBuf = buffer;
        mEcnsOutBufSize = bytes;
        mEcnsOutBufReadOffset = 0;
        pthread_cond_wait(&mEcnsBufCond, &mEcnsBufLock);
        //LOGD("writeDownlinkEcns returns.");
        if(mEcnsOutBufSize != 0)
            LOGE("writeDownlinkEcns: Buffer not consumed");
        else
            written = bytes;  // All data consumed
    }
    pthread_mutex_unlock(&mEcnsBufLock);
    return written;
}

// Returns: Bytes processed.
int AudioPostProcessor::applyUplinkEcns(void * buffer, int bytes, int rate)
{
    static int16 ul_gbuff2[160];
    int16_t *dl_buf;  // The downlink speech may not be contiguous, so copy it here.
    int16_t *ul_buf = (int16_t *)buffer;
    int dl_buf_bytes=0;

    if (!mEcnsEnabled)
        return 0;
    // Need a contiguous stereo playback buffer in the end.
    // TODO: keep one of these statically, and free it when we stop ECNS or #bytes changes.
    dl_buf = (int16_t *)malloc(bytes*2);
    if (!dl_buf)
        return -1;

    //LOGD("%s %d bytes at %d Hz",__FUNCTION__, bytes, rate);
    if (mEcnsEnabled && !mEcnsRunning)
        initEcns(rate, bytes);

    // In case the rate switched..
    if (mEcnsEnabled && rate != mEcnsRate) {
        stopEcns();
        initEcns(rate, bytes);
    }

    if(!mEcnsRunning) {
        LOGE("EC/NS failed to init, read returns.");
        return -1;
    }

    for(int i=0; mEcnsOutBuf==0 && i<2; i++) {
        // This situation is unlikely, but go ahead and do our best.
        LOGD("Signal write thread - need data.");
        pthread_cond_signal(&mEcnsBufCond);
        LOGD("Stalling read thread for %d usec",10000);
        usleep(10000);
    }
    //LOGD("ecns init done. dl_buf %p, ScratchBuf %p, ScratchBufSize %d, OutBuf %p, OutBufSize %d",
    //      dl_buf, mEcnsScratchBuf, mEcnsScratchBufSize, mEcnsOutBuf, mEcnsOutBufSize);
    pthread_mutex_lock(&mEcnsBufLock);
    // Need to gather appropriate amount of downlink speech.
    // Take oldest scratch data first.  The scratch buffer holds fractions of buffers
    // that were too small for processing.
    if (mEcnsScratchBuf && mEcnsScratchBufSize) {
        dl_buf_bytes = mEcnsScratchBufSize > bytes ? bytes:mEcnsScratchBufSize;
        memcpy(dl_buf, mEcnsScratchBuf, dl_buf_bytes);
        //LOGD("Took %d bytes from mEcnsScratchBuf", dl_buf_bytes);
        mEcnsScratchBufSize -= dl_buf_bytes;
        if(mEcnsScratchBufSize==0) {
            // This should always be true.
            free(mEcnsScratchBuf);
            mEcnsScratchBuf = 0;
            mEcnsScratchBufSize = 0;
        }
    }
    // Take fresh data from write thread second.
    if (dl_buf_bytes < bytes) {
        int bytes_to_copy = mEcnsOutBufSize - mEcnsOutBufReadOffset;
        bytes_to_copy = bytes_to_copy + dl_buf_bytes > bytes?
                      bytes-dl_buf_bytes:bytes_to_copy;
        if(bytes_to_copy) {
            memcpy((void *)((unsigned int)dl_buf+dl_buf_bytes),
                   (void *)((unsigned int)mEcnsOutBuf+mEcnsOutBufReadOffset),
                   bytes_to_copy);
            dl_buf_bytes += bytes_to_copy;
        }
        //LOGD("Took %d bytes from mEcnsOutBuf.  Need %d more.", bytes_to_copy,
        //      bytes-dl_buf_bytes);
        mEcnsOutBufReadOffset += bytes_to_copy;
        if(mEcnsOutBufSize - mEcnsOutBufReadOffset < bytes) {
            // We've depleted the output buffer, it's smaller than one uplink "frame".
            // First take any unused data into scratch, then free the write thread.
            if (mEcnsScratchBuf) {
                LOGE("Memleak - coding error");
                free(mEcnsScratchBuf);
            }
            if (mEcnsOutBufSize - mEcnsOutBufReadOffset > 0) {
                if ((mEcnsScratchBuf=malloc(mEcnsOutBufSize - mEcnsOutBufReadOffset)) == 0) {
                    LOGE("%s: Alloc failed, scratch data lost.",__FUNCTION__);
                } else {
                    mEcnsScratchBufSize = mEcnsOutBufSize - mEcnsOutBufReadOffset;
                    //LOGD("....store %d bytes into scratch buf %p",
                    //     mEcnsScratchBufSize, mEcnsScratchBuf);
                    memcpy(mEcnsScratchBuf,
                           (void *)((unsigned int)mEcnsOutBuf+mEcnsOutBufReadOffset),
                           mEcnsScratchBufSize);
                }
            }
            mEcnsOutBuf = 0;
            mEcnsOutBufSize = 0;
            mEcnsOutBufReadOffset = 0;
            //LOGD("Signal write thread - need data.");
            pthread_cond_signal(&mEcnsBufCond);
        }
    }

    pthread_mutex_unlock(&mEcnsBufLock);

    // Pad downlink with zeroes as last resort.  We have to process the UL speech.
    if (dl_buf_bytes < bytes) {
        LOGW("%s:EC/NS Starved for downlink data. have %d need %d.",
             __FUNCTION__,dl_buf_bytes, bytes);
        memset(&dl_buf[dl_buf_bytes/sizeof(int16_t)],
               0,
               bytes-dl_buf_bytes);
    }

    // Do Echo Cancellation
    API_MOT_DOWNLINK(&mEcnsCtrl, &mMemBlocks, (int16*)dl_buf, (int16*)ul_buf, &(ul_gbuff2[0]));
    API_MOT_UPLINK(&mEcnsCtrl, &mMemBlocks, (int16*)dl_buf, (int16*)ul_buf, &(ul_gbuff2[0]));

    // Save the logging output of the ECNS module
    #define ECNS_LOG_ENABLE_OFFSET 0 // First word
    #define ECNS_LOGGING_BITS 0x3F00 // bits 8-13 are log points 0-5
    if(mAudioProfile[ECNS_LOG_ENABLE_OFFSET] & ECNS_LOGGING_BITS) {
        int numPoints = 0;
        static int logsize = 0;
        if (!mLogFp) {
           LOGE("EC/NS AUDIO LOGGER CONFIGURATION:");
           LOGE("file /data/cto_ecns_log.pcm, log enable %04X",
               mAudioProfile[ECNS_LOG_ENABLE_OFFSET]);
           mLogFp = fopen("/data/cto_ecns_log.pcm", "w");
           // Bytes to log is (#log points) x (8 + #bytes per frame)
           for (uint16_t i=1;i>0;i<<=1) {
               if (i&ECNS_LOGGING_BITS&mAudioProfile[ECNS_LOG_ENABLE_OFFSET]) {
                  numPoints++;
               }
           }
           LOGE("Number of log points is %d.",numPoints);
           logsize = numPoints * (8 + bytes);
        }
        if(mLogFp) {
           LOGE("Writing %d bytes to /data/cto_ecns_log.pcm",logsize);
           fwrite(mMotDatalog, logsize, 1, mLogFp);
        } else {
           LOGE("EC/NS logging enabled, but failing to open.");
        }
    }
    // Playback the echo-cancelled speech to driver.
    // Include zero padding.  Our echo canceller needs a consistent path.
    if(dl_buf_bytes) {
        // Convert up to stereo, in place.
        for (int i = dl_buf_bytes/2-1; i >= 0; i--) {
            dl_buf[i*2] = dl_buf[i];
            dl_buf[i*2+1] = dl_buf[i];
        }
        dl_buf_bytes *= 2;
    }
    if(mEcnsOutFd != -1) {
        // Ignore write() retval, nothing we can do.
        ::write(mEcnsOutFd, dl_buf, bytes*2); // Stereo
        // To skip zero padding, do ::write(mEcnsOutFd, dl_buf, dl_buf_bytes) instead.
    }
    free(dl_buf);
    return bytes;
}

} //namespace android
