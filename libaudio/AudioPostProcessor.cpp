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
#include <sys/stat.h>
#include "mot_acoustics.h"
// hardware specific functions
extern uint16_t HC_CTO_AUDIO_MM_PARAMETER_TABLE[];

///////////////////////////////////
// Some logging #defines
#define ECNS_LOG_ENABLE_OFFSET 1 // 2nd word of the configuration buffer
#define ECNS_LOGGING_BITS 0x7FFF // 15 possible logpoints

// ID's for Uplink logs
#define MOT_LOG_DL_REF     0x8080
#define MOT_LOG_TX1_AS1    0x8040
#define MOT_LOG_TX2_AS2    0x8020
#define MOT_LOG_AS1_iENCS  0x8010
#define MOT_LOG_AS2_iENCS  0x8008
#define MOT_LOG_iENCS_AS   0x8004
#define MOT_LOG_AS_ANM     0x8002
#define MOT_LOG_ANM_ENC    0x8001

// ID's for downlink logs
#define MOT_LOG_DEC_ANM    0x0800
#define MOT_LOG_ANM_AS     0x0400
#define MOT_LOG_AS_iENCS   0x0200
#define MOT_LOG_iENCS_RAW  0x0100

// RUN-TIME audio profile
#define MOT_LOG_RUNTIME_AUDIOPROFILE  0x1000
#define MOT_LOG_T_MOT_CTRL         0x2000

#define MOT_LOG_DELIMITER_START  0xFEED
#define MOT_LOG_DELIMITER_END    0xF00D

#define ECNSLOGPATH "/data/ecns"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

namespace android {

AudioPostProcessor::AudioPostProcessor() :
    mEcnsScratchBuf(0), mEcnsDlBuf(0)
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
    stopEcns();
}

AudioPostProcessor::~AudioPostProcessor()
{
    LOGD("%s",__FUNCTION__);
    if (mEcnsRunning)
        stopEcns();
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

void AudioPostProcessor::setAudioDev(struct cpcap_audio_stream *outDev,
                                     struct cpcap_audio_stream *inDev,
                                     bool is_bt, bool is_bt_ec, bool is_spdif)
{
    uint32_t mm_accy = convOutDevToCTO(outDev->id);
    Mutex::Autolock lock(mMmLock);

    if (is_bt) {
        if (is_bt_ec)
            mEcnsMode = CTO_AUDIO_USECASE_NB_BLUETOOTH_WITH_ECNS;
        else
            mEcnsMode = CTO_AUDIO_USECASE_NB_BLUETOOTH_WITHOUT_ECNS;
    } else if (is_spdif) // May need a more complex check here for HDMI vs. others
        mEcnsMode = CTO_AUDIO_USECASE_NB_ACCY_1;
    else if (outDev->id==CPCAP_AUDIO_OUT_HEADSET && inDev->id==CPCAP_AUDIO_IN_MIC1)
        mEcnsMode = CTO_AUDIO_USECASE_NB_HEADSET_WITH_HANDSET_MIC;
    else if (outDev->id==CPCAP_AUDIO_OUT_HEADSET)
        mEcnsMode = CTO_AUDIO_USECASE_NB_HEADSET;
//    else if (outDev->id==CPCAP_AUDIO_OUT_EMU)  -- todo, check for speaker dock
//        mEcnsmode = CTO_AUDIO_USECASE_NB_DEDICATED_DOCK;
    else
        mEcnsMode = CTO_AUDIO_USECASE_NB_SPKRPHONE;

    if (mEcnsEnabled) {
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
    CTO_AUDIO_USECASES_CTRL mode;
    Mutex::Autolock lock(mEcnsBufLock);

    if (rate != 8000 && rate != 16000) {
        LOGW("Invalid rate for EC/NS, disabling");
        mEcnsEnabled = 0;
        mEcnsRunning = 0;
        return;
    }
    mode = mEcnsMode;
    mEcnsRate = rate;
    if (mEcnsRate==16000) {
       // Offset to the 16K (WB) block in the coefficients file
       mode = CTO_AUDIO_USECASES_CTRL(mode + CTO_AUDIO_USECASE_WB_HANDSET);
    }
    LOGD("%s for mode %d at %d size %d",__FUNCTION__, mode, mEcnsRate, bytes);
    mEcnsCtrl.framesize = bytes/2;
    mEcnsCtrl.micFlag = 0; // 0- one mic.  1- dual mic. 2- three mic.
    mEcnsCtrl.digital_mode = (rate == 8000) ? 0 : 1;  // 8K or 16K
    mEcnsCtrl.usecase = mode;
    mMemBlocks.staticMemory_1 = mStaticMemory_1;
    mMemBlocks.staticMemory_2 = NULL;
    mMemBlocks.mot_datalog = mMotDatalog;
    mMemBlocks.gainTableMemory = mParamTable;

    FILE * fp = fopen("/system/etc/voip_aud_params.bin", "r");
    if (fp) {
        if (fread(mParamTable, sizeof(mParamTable), 1, fp) < 1) {
            LOGE("Cannot read VOIP parameter file.  Disabling EC/NS.");
            fclose(fp);
            mEcnsEnabled = 0;
            mEcnsRunning = 0;
            return;
        }
        fclose(fp);
    }
    else {
        LOGE("Cannot open VOIP parameter file.  Disabling EC/NS.");
        mEcnsEnabled = 0;
        mEcnsRunning = 0;
        return;
    }

    mEcnsRunning = 1;
    mEcnsOutBuf = 0;
    mEcnsOutBufSize = 0;
    mEcnsOutBufReadOffset = 0;

    // Send setup parameters to the EC/NS module, init the module.
    API_MOT_SETUP(&mEcnsCtrl, &mMemBlocks);
    API_MOT_INIT(&mEcnsCtrl, &mMemBlocks);
}
void AudioPostProcessor::stopEcns (void)
{
    LOGD("%s",__FUNCTION__);
    AutoMutex lock(mEcnsBufLock);
    mEcnsRunning = 0;
    mEcnsRate = 0;
    if (mEcnsScratchBuf) {
        free(mEcnsScratchBuf);
        mEcnsScratchBuf = 0;
    }
    mEcnsScratchBufSize = 0;
    mEcnsOutFd = -1;

    for (int i=0;i<15;i++) {
        if (mLogFp[i])
            fclose(mLogFp[i]);
        mLogFp[i]=0;
    }
    if (mEcnsDlBuf) {
       free(mEcnsDlBuf);
       mEcnsDlBuf = 0;
    }
    mEcnsDlBufSize = 0;
    // In case write() is blocked, set it free.
    mEcnsBufCond.signal();
}

// Returns: Bytes written (actually "to-be-written" by read thread).
int AudioPostProcessor::writeDownlinkEcns(int fd, void * buffer, int bytes, Mutex *fdLock)
{
    int written = 0;
    mEcnsBufLock.lock();
    if (mEcnsEnabled && !mEcnsRunning) {
        long usecs = 20*1000;
        // Give the read thread a chance to catch up.
        LOGD("%s: delay %d msecs for ec/ns to start",__FUNCTION__, (int)(usecs/1000));
        mEcnsBufLock.unlock();
        usleep(usecs);
        mEcnsBufLock.lock();
        written = bytes;  // Pretend all data was consumed even if ecns isn't running
    }
    if (mEcnsRunning) {
        // Only run through here after initEcns has been done by read thread.
        mEcnsOutFd = fd;
        mEcnsOutBuf = buffer;
        mEcnsOutBufSize = bytes;
        mEcnsOutBufReadOffset = 0;
        mEcnsOutFdLockp = fdLock;
        if (mEcnsBufCond.waitRelative(mEcnsBufLock, seconds(1)) != NO_ERROR) {
            LOGE("%s: Capture thread is stalled.", __FUNCTION__);
        }
        if (mEcnsOutBufSize != 0)
            LOGD("%s: Buffer not consumed", __FUNCTION__);
        else
            written = bytes;  // All data consumed
    }
    mEcnsBufLock.unlock();
    return written;
}

// Returns: Bytes processed.
int AudioPostProcessor::applyUplinkEcns(void * buffer, int bytes, int rate)
{
    static int16 ul_gbuff2[160];
    int16_t *dl_buf;
    int16_t *ul_buf = (int16_t *)buffer;
    int dl_buf_bytes=0;
    // The write thread could have left us with one frame of data in the
    // driver when we started reading.
    static bool onetime;

    if (!mEcnsEnabled)
        return 0;

    LOGV("%s %d bytes at %d Hz",__FUNCTION__, bytes, rate);
    if (mEcnsEnabled && !mEcnsRunning) {
        initEcns(rate, bytes);
        onetime=true;
    }

    // In case the rate switched..
    if (mEcnsEnabled && rate != mEcnsRate) {
        stopEcns();
        initEcns(rate, bytes);
        onetime=true;
    }

    if (!mEcnsRunning) {
        LOGE("EC/NS failed to init, read returns.");
        return -1;
    }

    for (int i=0; mEcnsOutBuf==0 && i<1; i++) {
        // Try to wait a bit for downlink speech to come.
        usleep(5000);
    }

    mEcnsBufLock.lock();
    // Need a contiguous stereo playback buffer in the end.
    if (bytes*2 != mEcnsDlBufSize || !mEcnsDlBuf) {
        if (mEcnsDlBuf)
            free(mEcnsDlBuf);
        mEcnsDlBuf = (int16_t*)malloc(bytes*2);
        if (mEcnsDlBuf)
            mEcnsDlBufSize = bytes*2;
    }
    dl_buf = mEcnsDlBuf;
    if (!dl_buf) {
        mEcnsBufLock.unlock();
        return -1;
    }

    // Need to gather appropriate amount of downlink speech.
    // Take oldest scratch data first.  The scratch buffer holds fractions of buffers
    // that were too small for processing.
    if (mEcnsScratchBuf && mEcnsScratchBufSize) {
        dl_buf_bytes = mEcnsScratchBufSize > bytes ? bytes:mEcnsScratchBufSize;
        memcpy(dl_buf, mEcnsScratchBuf, dl_buf_bytes);
        //LOGD("Took %d bytes from mEcnsScratchBuf", dl_buf_bytes);
        mEcnsScratchBufSize -= dl_buf_bytes;
        if (mEcnsScratchBufSize==0) {
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
        if (bytes_to_copy) {
            memcpy((void *)((unsigned int)dl_buf+dl_buf_bytes),
                   (void *)((unsigned int)mEcnsOutBuf+mEcnsOutBufReadOffset),
                   bytes_to_copy);
            dl_buf_bytes += bytes_to_copy;
        }
        //LOGD("Took %d bytes from mEcnsOutBuf.  Need %d more.", bytes_to_copy,
        //      bytes-dl_buf_bytes);
        mEcnsOutBufReadOffset += bytes_to_copy;
        if (mEcnsOutBufSize - mEcnsOutBufReadOffset < bytes) {
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
            mEcnsBufCond.signal();
        }
    }

    mEcnsBufLock.unlock();

    // Pad downlink with zeroes as last resort.  We have to process the UL speech.
    if (dl_buf_bytes < bytes) {
        LOGW("%s:EC/NS Starved for downlink data. have %d need %d.",
             __FUNCTION__,dl_buf_bytes, bytes);
        memset(&dl_buf[dl_buf_bytes/sizeof(int16_t)],
               0,
               bytes-dl_buf_bytes);
    }

    if (onetime) {
        onetime = false;
        return bytes;
    }
    // Do Echo Cancellation
    API_MOT_LOG_RESET(&mEcnsCtrl, &mMemBlocks);
    API_MOT_DOWNLINK(&mEcnsCtrl, &mMemBlocks, (int16*)dl_buf, (int16*)ul_buf, &(ul_gbuff2[0]));
    API_MOT_UPLINK(&mEcnsCtrl, &mMemBlocks, (int16*)dl_buf, (int16*)ul_buf, &(ul_gbuff2[0]));

    // Playback the echo-cancelled speech to driver.
    // Include zero padding.  Our echo canceller needs a consistent path.
    // TODO: Don't playback here, move it to the write() thread.  Also, make sure
    // the output is really stereo before upconverting (i.e. SCO Audio)
    if (dl_buf_bytes) {
        // Convert up to stereo, in place.
        for (int i = dl_buf_bytes/2-1; i >= 0; i--) {
            dl_buf[i*2] = dl_buf[i];
            dl_buf[i*2+1] = dl_buf[i];
        }
        dl_buf_bytes *= 2;
    }
    if (mEcnsOutFd != -1) {
        mEcnsOutFdLockp->lock();
        ::write(mEcnsOutFd, dl_buf, bytes*2); // Stereo
        mEcnsOutFdLockp->unlock();
    }
    // Do the CTO SuperAPI internal logging.  It can log various steps of uplink and downlink speech.
    // (Do this after writing output to avoid adding latency.)
    ecnsLogToFile(bytes);
    return bytes;
}

void AudioPostProcessor::ecnsLogToFile(int bytes)
{
    static int numPoints;
    uint16_t *logp;
    int mode = mEcnsMode + (mEcnsRate==16000?CTO_AUDIO_USECASE_WB_HANDSET:0);
    uint16_t *audioProfile = &mParamTable[AUDIO_PROFILE_PARAMETER_BLOCK_WORD16_SIZE*mode];

    if (audioProfile[ECNS_LOG_ENABLE_OFFSET] & ECNS_LOGGING_BITS) {
        if (!mLogFp[0]) {
           numPoints = 0;
           LOGE("EC/NS AUDIO LOGGER CONFIGURATION:");
           LOGE("log enable %04X",
               audioProfile[ECNS_LOG_ENABLE_OFFSET]);
           mkdir(ECNSLOGPATH, 00770);
           for (uint16_t i=1;i>0;i<<=1) {
               if (i&ECNS_LOGGING_BITS&audioProfile[ECNS_LOG_ENABLE_OFFSET]) {
                  numPoints++;
               }
           }
           LOGE("Number of log points is %d.",numPoints);
           logp = mMotDatalog;
           for (int i=0;i<numPoints;i++) {
               char fname[80];
               fname[0]='\0';
               // Log format: FEED TAG1 LEN1 F00D [bytes]
               LOGD("feed? %04X",logp[0]);
               sprintf(fname, ECNSLOGPATH"/%cl-0x%04X.pcm",
                   logp[1] & 0x8000?'u':'d',
                   logp[1]);
               LOGE("fname[%d] = %s",i,fname);
               LOGD("len = %d*2", logp[2]);
               LOGD("food? %04X", logp[3]);
               mLogFp[i]=fopen((const char *)fname, "w");
               logp += 4 + logp[2];
           }
        }
        logp = mMotDatalog;
        for (int i=0; i<numPoints; i++) {
            if (mLogFp[i]) {
                fwrite(&logp[4], logp[2]*sizeof(uint16_t), 1, mLogFp[i]);
                logp += 4+logp[2];
            } else {
                LOGE("EC/NS logging enabled, but file not open.");
            }
        }
    }
}

} //namespace android
