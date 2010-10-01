/*
** Copyright 2008-2010, The Android Open-Source Project
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

#include <math.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareTegra"
#include <utils/Log.h>
#include <utils/String8.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "AudioHardware.h"
#include <media/AudioRecord.h>
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
#include "src_lib.h"
#endif

namespace android {
const uint32_t AudioHardware::inputSamplingRates[] = {
    8000, 11025, 12000, 16000, 22050, 32000, 44100, 48000
};
// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(true), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), mCpcapCtlFd(-1)
{
    LOGV("AudioHardware constructor");

    mCpcapCtlFd = ::open("/dev/audio_ctl", O_RDWR);
    if (mCpcapCtlFd < 0) {
        LOGE("Failed to initialized: %s\n", strerror(errno));
        return;
    }

    ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_OUTPUT, &mCurOutDevice);
    ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_INPUT, &mCurInDevice);
    // For bookkeeping only
    ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_RATE, &mHwOutRate);
    ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_RATE, &mHwInRate);

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    // Init the MM Audio Post Processing
    mAudioPP.setAudioDev(&mCurOutDevice);
#endif

    mInit = true;
}

AudioHardware::~AudioHardware()
{
    LOGV("AudioHardware destructor");
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
    ::close(mCpcapCtlFd);
}

status_t AudioHardware::initCheck()
{
    return mInit ? NO_ERROR : NO_INIT;
}

AudioStreamOut* AudioHardware::openOutputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status)
{
    { // scope for the lock
        Mutex::Autolock lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            if (status) {
                *status = INVALID_OPERATION;
            }
            return 0;
        }

        // create new output stream
        AudioStreamOutTegra* out = new AudioStreamOutTegra();
        status_t lStatus = out->set(this, devices, format, channels, sampleRate);
        if (status) {
            *status = lStatus;
        }
        if (lStatus == NO_ERROR) {
            mOutput = out;
        } else {
            delete out;
        }
    }
    return mOutput;
}

void AudioHardware::closeOutputStream(AudioStreamOut* out) {
    Mutex::Autolock lock(mLock);
    if (mOutput == 0 || mOutput != out) {
        LOGW("Attempt to close invalid output stream");
    }
    else {
        delete mOutput;
        mOutput = 0;
    }
}

AudioStreamIn* AudioHardware::openInputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    // check for valid input source
    if (!AudioSystem::isInputDevice((AudioSystem::audio_devices)devices)) {
        return 0;
    }

    Mutex::Autolock lock(mLock);

    AudioStreamInTegra* in = new AudioStreamInTegra();
    status_t lStatus = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) {
        *status = lStatus;
    }
    if (lStatus != NO_ERROR) {
        delete in;
        return 0;
    }

    mInputs.add(in);

    return in;
}

void AudioHardware::closeInputStream(AudioStreamIn* in)
{
    Mutex::Autolock lock(mLock);

    ssize_t index = mInputs.indexOf((AudioStreamInTegra *)in);
    if (index < 0) {
        LOGW("Attempt to close invalid input stream");
    } else {
        mLock.unlock();
        delete mInputs[index];
        mLock.lock();
        mInputs.removeAt(index);
    }
}

status_t AudioHardware::setMode(int mode)
{
    return AudioHardwareBase::setMode(mode);
}

status_t AudioHardware::doStandby(bool output, bool enable)
{
    status_t status = NO_ERROR;
    struct cpcap_audio_stream standby;

    LOGV("AudioHardware::doStandby() putting %s in %s mode",
            output ? "output" : "input",
            enable ? "standby" : "online" );

//  Mutex::Autolock lock(mLock);

    if (output) {
        standby.id = CPCAP_AUDIO_OUT_STANDBY;
        standby.on = enable;

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_OUTPUT, &standby) < 0) {
            LOGE("could not turn off current output device: %s\n",
                 strerror(errno));
            status = errno;
        }
        ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_OUTPUT, &mCurOutDevice);
        LOGV("%s: after standby %s, output is %s", __FUNCTION__,
             enable ? "enable" : "disable",
             mCurOutDevice.on ? "on" : "off");
    } else {
        standby.id = CPCAP_AUDIO_IN_STANDBY;
        standby.on = enable;

        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_INPUT, &standby) < 0) {
            LOGE("could not turn off current input device: %s\n",
                 strerror(errno));
            status = errno;
        }
        ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_INPUT, &mCurInDevice);
        LOGV("%s: after standby %s, input is %s", __FUNCTION__,
             enable ? "enable" : "disable",
             mCurInDevice.on ? "on" : "off");
    }

    return status;
}

status_t AudioHardware::setMicMute(bool state)
{
    Mutex::Autolock lock(mLock);
    return setMicMute_nosync(state);
}

// always call with mutex held
status_t AudioHardware::setMicMute_nosync(bool state)
{
    if (mMicMute != state) {
        mMicMute = state;
        return NO_ERROR; //doAudioRouteOrMute(SND_DEVICE_CURRENT);
    }
    return NO_ERROR;
}

status_t AudioHardware::getMicMute(bool* state)
{
    *state = mMicMute;
    return NO_ERROR;
}

status_t AudioHardware::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 value;
    String8 key;
    const char BT_NREC_KEY[] = "bt_headset_nrec";
    const char BT_NAME_KEY[] = "bt_headset_name";
    const char BT_NREC_VALUE_ON[] = "on";


    LOGV("setParameters() %s", keyValuePairs.string());

    if (keyValuePairs.length() == 0) return BAD_VALUE;

    key = String8(BT_NREC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == BT_NREC_VALUE_ON) {
            mBluetoothNrec = true;
        } else {
            mBluetoothNrec = false;
            LOGI("Turning noise reduction and echo cancellation off for BT "
                 "headset");
        }
    }
    key = String8(BT_NAME_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mBluetoothId = 0;
#if 0
        for (int i = 0; i < mNumSndEndpoints; i++) {
            if (!strcasecmp(value.string(), mSndEndpoints[i].name)) {
                mBluetoothId = mSndEndpoints[i].id;
                LOGI("Using custom acoustic parameters for %s", value.string());
                break;
            }
        }
#endif
        if (mBluetoothId == 0) {
            LOGI("Using default acoustic parameters "
                 "(%s not in acoustic database)", value.string());
            doRouting();
        }
    }
    return NO_ERROR;
}

String8 AudioHardware::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    return param.toString();
}

size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount < 1 || channelCount > 2) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    return 2048*channelCount;
}

status_t AudioHardware::setVoiceVolume(float v)
{
#if 0
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int vol = lrint(v * 15.0);
    LOGD("setVoiceVolume(%f)\n", v);
    LOGI("Setting in-call volume to %d (available range is 0 to 15)\n", vol);

    Mutex::Autolock lock(mLock);
    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_VOLUME, vol) < 0)
        LOGE("could not set volume: %s\n", strerror(errno));
#endif
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    Mutex::Autolock lock(mLock);
    // CPCAP volumes : (vol * 3) - 33 == dB of gain. (table 7-31 CPCAP DTS version 1.5)
    // 0 dB is correct for VOL_DAC for loudspeaker.
    int vol = ceil(v * 11.0);
    LOGI("Set master volume to %d.\n", vol);

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_VOLUME, vol) < 0)
        LOGE("could not set volume: %s\n", strerror(errno));

    return NO_ERROR;
}

int AudioHardware::getActiveInputRate()
{
    AudioStreamInTegra *input = getActiveInput_l();
    return (input != NULL) ? input->sampleRate() : 0;
}
status_t AudioHardware::doRouting()
{
    Mutex::Autolock lock(mLock);
    uint32_t outputDevices = mOutput->devices();
    AudioStreamInTegra *input = getActiveInput_l();
    uint32_t inputDevice = (input == NULL) ? 0 : input->devices();

    int sndOutDevice = -1;
    int sndInDevice = -1;

    LOGV("%s: inputDevice %x, outputDevices %x", __FUNCTION__,
         inputDevice, outputDevices);

    switch (inputDevice) {
    case AudioSystem::DEVICE_IN_DEFAULT:
    case AudioSystem::DEVICE_IN_BUILTIN_MIC:
        sndInDevice = CPCAP_AUDIO_IN_MIC1;
        break;
    case AudioSystem::DEVICE_IN_WIRED_HEADSET:
        sndInDevice = CPCAP_AUDIO_IN_MIC2;
        break;
    default:
        break;
    }

    switch (outputDevices) {
    case AudioSystem::DEVICE_OUT_EARPIECE:
    case AudioSystem::DEVICE_OUT_DEFAULT:
    case AudioSystem::DEVICE_OUT_SPEAKER:
        sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
        break;
    case AudioSystem::DEVICE_OUT_WIRED_HEADSET:
    case AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
        sndOutDevice = CPCAP_AUDIO_OUT_HEADSET;
        break;
    case AudioSystem::DEVICE_OUT_SPEAKER | AudioSystem::DEVICE_OUT_WIRED_HEADSET:
    case AudioSystem::DEVICE_OUT_SPEAKER | AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
        sndOutDevice = CPCAP_AUDIO_OUT_HEADSET_AND_SPEAKER;
        break;
    default:
        break;
    }

    if (sndInDevice == -1) {
        LOGV("input device set %x not supported, defaulting to on-board mic",
             inputDevice);
        mCurInDevice.id = CPCAP_AUDIO_IN_MIC1;
    }
    else
        mCurInDevice.id = sndInDevice;

    if (sndOutDevice == -1) {
        LOGW("output device set %x not supported, defaulting to speaker",
             outputDevices);
        mCurOutDevice.id = CPCAP_AUDIO_OUT_SPEAKER;
    }
    else
        mCurOutDevice.id = sndOutDevice;

    LOGV("current input %d, %s",
         mCurInDevice.id,
         mCurInDevice.on ? "on" : "off");

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_INPUT,
              &mCurInDevice) < 0)
        LOGE("could not set input (%d, on %d): %s\n",
             mCurInDevice.id, mCurInDevice.on, strerror(errno));

    LOGV("current output %d, %s", mCurOutDevice.id,
         mCurOutDevice.on ? "on" : "off");

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_OUTPUT,
              &mCurOutDevice) < 0)
        LOGE("could not set output (%d, on %d): %s\n",
             mCurOutDevice.id, mCurOutDevice.on,
             strerror(errno));

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    mAudioPP.setAudioDev(&mCurOutDevice);

    //TODO: EC/NS decision that doesn't isn't so presumptuous.
    bool ecnsEnabled = mCurOutDevice.on && mCurInDevice.on &&
                       (getActiveInputRate() == 8000 || getActiveInputRate() == 16000);
    mAudioPP.enableEcns(ecnsEnabled);

    // Check input/output rates for HW.
    int oldInRate=mHwInRate, oldOutRate=mHwOutRate;
    if (ecnsEnabled) {
        mHwInRate = getActiveInputRate();
        mHwOutRate = mHwInRate;
        LOGD("EC/NS active, requests rate as %d for in/out", mHwInRate);
    }
    else {
        mHwInRate = getActiveInputRate();
        if(mHwInRate == 0)
            mHwInRate = 44100;
        mHwOutRate = 44100;
        LOGD("No EC/NS, set input rate %d, output %d.", mHwInRate, mHwOutRate);
    }
    if (mHwInRate != oldInRate) {
        LOGD("Minor TODO: Flush input if active.");
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_RATE,
                  mHwInRate) < 0)
            LOGE("could not set input rate(%d): %s\n",
                  mHwInRate, strerror(errno));
        if(::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_RATE, &mHwInRate))
            LOGE("CPCAP driver error reading rates.");
    }
    if (mHwOutRate != oldOutRate) {
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_RATE,
                  mHwOutRate) < 0)
            LOGE("could not set output rate(%d): %s\n",
                  mHwOutRate, strerror(errno));
        // Clear old data (wrong rate) from I2S driver.
        if(mOutput)
           mOutput->flush();
        if(::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_RATE, &mHwOutRate))
            LOGE("CPCAP driver error reading rates.");
    }
#endif
    return NO_ERROR;
}

status_t AudioHardware::checkMicMute()
{
    Mutex::Autolock lock(mLock);
    if (mMode != AudioSystem::MODE_IN_CALL) {
        setMicMute_nosync(true);
    }

    return NO_ERROR;
}

status_t AudioHardware::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioHardware::dumpInternals\n");
    snprintf(buffer, SIZE, "\tmInit: %s\n", mInit? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmMicMute: %s\n", mMicMute? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothNrec: %s\n", mBluetoothNrec? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothId: %d\n", mBluetoothId);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    for (size_t index = 0; index < mInputs.size(); index++) {
        mInputs[index]->dump(fd, args);
    }

    if (mOutput) {
        mOutput->dump(fd, args);
    }
    return NO_ERROR;
}

uint32_t AudioHardware::getInputSampleRate(uint32_t sampleRate)
{
    uint32_t i;
    uint32_t prevDelta;
    uint32_t delta;

    for (i = 0, prevDelta = 0xFFFFFFFF; i < sizeof(inputSamplingRates)/sizeof(uint32_t); i++, prevDelta = delta) {
        delta = abs(sampleRate - inputSamplingRates[i]);
        if (delta > prevDelta) break;
    }
    // i is always > 0 here
    return inputSamplingRates[i-1];
}

// getActiveInput_l() must be called with mLock held
AudioHardware::AudioStreamInTegra *AudioHardware::getActiveInput_l()
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        // return first input found not being in standby mode
        // as only one input can be in this state
        if (mInputs[i]->state() > AudioStreamInTegra::AUDIO_INPUT_CLOSED) {
            return mInputs[i];
        }
    }

    return NULL;
}

// ----------------------------------------------------------------------------

AudioHardware::AudioStreamOutTegra::AudioStreamOutTegra() :
    mHardware(0), mFd(-1), mFdCtl(-1), mStartCount(0), mRetryCount(0), mDevices(0)
{
    mFd = ::open("/dev/audio0_out", O_RDWR);
    mFdCtl = ::open("/dev/audio0_out_ctl", O_RDWR);
}

status_t AudioHardware::AudioStreamOutTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate)
{
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    mHardware = hw;

    // fix up defaults
    if (lFormat == 0) lFormat = format();
    if (lChannels == 0) lChannels = channels();
    if (lRate == 0) lRate = sampleRate();

    // check values
    if ((lFormat != format()) ||
        (lChannels != channels()) ||
        (lRate != sampleRate())) {
        if (pFormat) *pFormat = format();
        if (pChannels) *pChannels = channels();
        if (pRate) *pRate = sampleRate();
        return BAD_VALUE;
    }

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    mHardware->mAudioPP.setPlayAudioRate(lRate);
#endif

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mDevices = devices;

    return NO_ERROR;
}

AudioHardware::AudioStreamOutTegra::~AudioStreamOutTegra()
{
    if (mFd >= 0) ::close(mFd);
    if (mFdCtl >= 0) ::close(mFdCtl);
}

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
void AudioHardware::AudioStreamOutTegra::srcInit(int inRate, int outRate)
{
    SRC_MODE_T srcMode = MODE_END;
    SRC_STATUS_T initResult;

    mSrcInitted = false;
    mSrcStaticData.scratch_buffer = mSrcScratchMem;

    if(inRate == 44100) {
        srcMode = (
            outRate == 8000  ? SRC_44_08 :
            outRate == 11025 ? SRC_44_11 :
            outRate == 12000 ? SRC_44_12 :
            outRate == 16000 ? SRC_44_16 :
            outRate == 22050 ? SRC_44_22 :
            outRate == 24000 ? SRC_44_24 :
            outRate == 32000 ? SRC_44_32 :
            /* Invalid */ MODE_END
        );
    }

    if(srcMode == MODE_END) {
        LOGE("Failed to initialize sample rate converter - bad rate");
        return;
    }
    // Do MONO sample rate conversion.  Should be half the MCPS of stereo.
    // Use SRC_STEREO_INTERLEAVED for stereo data.
    initResult = src_init(&mSrcStaticData, srcMode, SRC_MONO);
    if (initResult != SRC_SUCCESS) {
        LOGE("Failed to initialize sample rate converter - %d",initResult);
        return;
    }

    mSrcInitted = true;
    mSrcInRate = inRate;
    mSrcOutRate = outRate;
}
#endif

ssize_t AudioHardware::AudioStreamOutTegra::write(const void* buffer, size_t bytes)
{
    // LOGD("AudioStreamOutTegra::write(%p, %u)", buffer, bytes);
    int status = NO_INIT;
    struct tegra_audio_error_counts errors;
    ssize_t written = 0;
    const uint8_t* p = static_cast<const uint8_t*>(buffer);
    size_t outsize = bytes;

    // Prevent someone from flushing the fd during a write.
    Mutex::Autolock lock(mFdLock);

    status = online(); // if already online, a no-op
    if (status < 0) {
        goto error;
    }

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    // Do Multimedia processing if appropriate for device and usecase.
    mHardware->mAudioPP.doMmProcessing((void *)buffer, bytes / frameSize());

    // Check if sample rate conversion or ECNS are required.
    // Caution: Upconversion (from 44.1 to 48) would require a new output buffer larger than the
    // original one.
    if(mHardware->mHwOutRate != (int)sampleRate()) {
        if (!mSrcInitted ||
             mSrcInRate != (int)sampleRate() ||
             mSrcOutRate != mHardware->mHwOutRate) {
            LOGW("%s: downconvert started from %d to %d",__FUNCTION__,
                 sampleRate(), mHardware->mHwOutRate);
            srcInit(sampleRate(), mHardware->mHwOutRate);
            if(!mSrcInitted) {
                status = -1;
                goto error;
            }
        }
    } else {
        mSrcInitted = false;
    }

    if(mHardware->mAudioPP.isEcnsEnabled() || mSrcInitted)
    {
        // cut audio down to Mono for SRC or ECNS
        if (channels() == AudioSystem::CHANNEL_OUT_STEREO)
        {
            // Do stereo-to-mono downmix before SRC, in-place
            int16_t *destBuf = (int16_t *) buffer;
            for (int i = 0; i < (int)bytes/2; i++) {
                 destBuf[i] = (destBuf[i*2]>>1) + (destBuf[i*2+1]>>1);
            }
            outsize >>= 1;
        }
    }

    if(mSrcInitted) {
        // Apply the sample rate conversion.
        mSrcIoData.input_ptrL = (SRC_INT16_T *) (buffer);
        mSrcIoData.input_count = outsize / sizeof(SRC_INT16_T);
        mSrcIoData.input_ptrR = (SRC_INT16_T *) (buffer) + 1;
        mSrcIoData.output_ptr = (SRC_INT16_T *) (buffer);
        mSrcIoData.output_count = outsize / sizeof(SRC_INT16_T);
        src_convert(&mSrcStaticData, 0x0800, &mSrcIoData); // 0x0800 is unity gain in Q4.11 format
        //LOGD("Converted %d bytes at %d to %d bytes at %d",
        //     outsize, sampleRate(), mSrcIoData.output_count*2, mHardware->mHwOutRate);
        outsize = mSrcIoData.output_count*2;
    }
    if(mHardware->mAudioPP.isEcnsEnabled()) {
        // EC/NS is a blocking interface, to synchronise with read.
        // It also consumes data when EC/NS is running.
        // It expects MONO data.
        // If EC/NS is not running, it will return 0, and we need to write this data to the
        // driver ourselves.
        written = mHardware->mAudioPP.writeDownlinkEcns(mFd,(void *)buffer, outsize);
    }
    if(mHardware->mAudioPP.isEcnsEnabled() || mSrcInitted) {
        // Move audio back up to Stereo, if the EC/NS wasn't in fact running and we're
        // writing to a stereo device.
        if (channels() == AudioSystem::CHANNEL_OUT_STEREO &&
            written != (ssize_t)outsize) {
            // Back up to stereo, in place.
            int16_t *destBuf = (int16_t *) buffer;
            for (int i = outsize/2-1; i >= 0; i--) {
                destBuf[i*2] = destBuf[i];
                destBuf[i*2+1] = destBuf[i];
            }
            outsize <<= 1;
        }
    }
#endif

    if(written != (ssize_t)outsize) {
        written = ::write(mFd, buffer, outsize);
    }
    if (written < 0)
        LOGE("Error writing %d bytes to output: %s", outsize, strerror(errno));
    else {
        if (::ioctl(mFdCtl, TEGRA_AUDIO_OUT_GET_ERROR_COUNT, &errors) < 0)
            LOGE("Could not retrieve playback error count: %s\n", strerror(errno));
        else if (errors.late_dma || errors.full_empty)
            LOGW("Played %d bytes with %d late, %d underflow errors\n", (int)written,
                 errors.late_dma, errors.full_empty);
    }

    return written==(ssize_t)outsize? bytes : bytes * written / outsize;

error:
    LOGE("write(): error");
    usleep(bytes * 1000000 / frameSize() / sampleRate());
    return status;
}

void AudioHardware::AudioStreamOutTegra::flush()
{
    // Prevent someone from writing the fd while we do this operation.
    Mutex::Autolock lock(mFdLock);
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    // Make sure read thread isn't stuck writing to this fd in EC/NS
    mHardware->mAudioPP.writeDownlinkEcns(0,0,0); // fd, buffer, size
#endif
    ::close(mFd);
    mFd = ::open("/dev/audio0_out", O_RDWR);
}

status_t AudioHardware::AudioStreamOutTegra::online()
{
    if (mHardware->mCurOutDevice.on) {
        return NO_ERROR;
    }

    return mHardware->doStandby(true, false); // output, online
}

status_t AudioHardware::AudioStreamOutTegra::standby()
{
    status_t status = NO_ERROR;
    if (!mHardware->mCurOutDevice.on) {
        LOGV("%s: output already in standby", __FUNCTION__);
        return NO_ERROR;
    }

    status = mHardware->doStandby(true, true); // output, standby
    return status;
}

status_t AudioHardware::AudioStreamOutTegra::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamOutTegra::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStartCount: %d\n", mStartCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    if (mHardware)
        snprintf(buffer, SIZE, "\tmStandby: %s\n",
                 mHardware->mCurOutDevice.on ? "false": "true");
    else
        snprintf(buffer, SIZE, "\tmStandby: unknown\n");

    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

bool AudioHardware::AudioStreamOutTegra::getStandby()
{
    return mHardware ? !mHardware->mCurOutDevice.on : true;
}

status_t AudioHardware::AudioStreamOutTegra::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamOutTegra::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        mDevices = device;
        LOGV("set output routing %x", mDevices);
        status = mHardware->doRouting();
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamOutTegra::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamOutTegra::getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioHardware::AudioStreamOutTegra::getRenderPosition(uint32_t *dspFrames)
{
    //TODO: enable when supported by driver
    return INVALID_OPERATION;
}

// ----------------------------------------------------------------------------

AudioHardware::AudioStreamInTegra::AudioStreamInTegra() :
    mHardware(0), mState(AUDIO_INPUT_CLOSED), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0)
{
}

status_t AudioHardware::AudioStreamInTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    status_t status = BAD_VALUE;
    if (pFormat == 0)
        return status;
    if (*pFormat != AUDIO_HW_IN_FORMAT) {
        LOGE("wrong in format %d, expecting %lld", *pFormat, AUDIO_HW_IN_FORMAT);
        *pFormat = AUDIO_HW_IN_FORMAT;
        return status;
    }

    if (pRate == 0)
        return status;

    uint32_t rate = hw->getInputSampleRate(*pRate);
    if (rate != *pRate) {
        LOGE("wrong sample rate %d, expecting %d", *pRate, rate);
        *pRate = rate;
        return status;
    }

    if (pChannels == 0)
        return status;

    if (*pChannels != AudioSystem::CHANNEL_IN_MONO &&
        *pChannels != AudioSystem::CHANNEL_IN_STEREO) {
        LOGE("wrong number of channels %d", *pChannels);
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return status;
    }

    LOGV("AudioStreamInTegra::set(%d, %d, %u)", *pFormat, *pChannels, *pRate);
    mHardware = hw;

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = *pRate;
    // 20 millisecond buffers default
    mBufferSize = mSampleRate * sizeof(int16_t) * AudioSystem::popCount(mChannels) / 50;
    if(mBufferSize & 0x3) {
       // Not divisible by 4.
       mBufferSize +=4;
       mBufferSize &= ~0x3;
    }

    return NO_ERROR;
}

AudioHardware::AudioStreamInTegra::~AudioStreamInTegra()
{
    LOGV("AudioStreamInTegra destructor");

    standby();

    if (mFd >= 0)
        ::close(mFd);

    if (mFdCtl >= 0)
        ::close(mFdCtl);
}

ssize_t AudioHardware::AudioStreamInTegra::read(void* buffer, ssize_t bytes)
{
    ssize_t ret;
    struct tegra_audio_error_counts errors;

    LOGV("AudioStreamInTegra::read(%p, %ld)", buffer, bytes);
    if (!mHardware) {
        LOGE("%s: mHardware is null", __FUNCTION__);
        return -1;
    }

    if (mState < AUDIO_INPUT_STARTED) {
        mState = AUDIO_INPUT_STARTED;
        mHardware->doRouting();
    }

    ret = online();
    if(ret != NO_ERROR) {
       LOGE("%s: Problem switching to online.",__FUNCTION__);
       return -1;
    }

    ret = ::read(mFd, buffer, bytes);
    if (ret < 0)
        LOGE("Error reading from audio in: %s", strerror(errno));

    if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_ERROR_COUNT, &errors) < 0)
        LOGE("Could not retrieve recording error count: %s\n", strerror(errno));
    else if (errors.late_dma || errors.full_empty)
        LOGW("Recorded %d bytes with %d late, %d overflow errors\n", (int)ret,
             errors.late_dma, errors.full_empty);

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    mHardware->mAudioPP.applyUplinkEcns(buffer, bytes, mSampleRate);
#endif
    return ret;
}

bool AudioHardware::AudioStreamInTegra::getStandby()
{
    return mState == AUDIO_INPUT_CLOSED;
}

status_t AudioHardware::AudioStreamInTegra::standby()
{
    mState = AUDIO_INPUT_CLOSED;

    if (!mHardware)
        return -1;

    mHardware->doRouting();

    status_t rc = mHardware->doStandby(false, true); // input, standby
    if (!rc) {
        /* Stop recording, if ongoing.  Muting the microphone will cause CPCAP
         * to not send data through the i2s interface, and read() will block
         * until recording is resumed.
         */
        LOGV("%s: stop recording", __FUNCTION__);
        ::ioctl(mFd, TEGRA_AUDIO_IN_STOP);
    }
    return rc;
}

status_t AudioHardware::AudioStreamInTegra::online()
{
    status_t status;
    if (mHardware->mCurInDevice.on) {
        return NO_ERROR;
    }

    LOGV("%s", __FUNCTION__);

    // open audio input device
    mFd = ::open("/dev/audio1_in", O_RDWR);
    mFdCtl = ::open("/dev/audio1_in_ctl", O_RDWR);

    // configuration
    struct tegra_audio_in_config config;
    status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config);
    if (status < 0) {
        LOGE("cannot read input config: %s", strerror(errno));
        return status;
    }
    config.stereo = AudioSystem::popCount(mChannels) == 2;
    // TODO: Disable SRC in Kernel
    // config.rate = mSampleRate;
    config.rate = 44100;
    status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_SET_CONFIG, &config);

    if (status < 0) {
        LOGE("cannot set input config: %s", strerror(errno));
        if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config) == 0) {
            if (config.stereo) {
                mChannels = AudioSystem::CHANNEL_IN_STEREO;
            } else {
                mChannels = AudioSystem::CHANNEL_IN_MONO;
            }
        }
    }

    // Shoot for 20 msec minimum of chunk size, mBufferSize is 20 msec.
    struct tegra_audio_buf_config buf_config;
    int chunk_temp = mBufferSize;
    buf_config.chunk = 0;
    do {
       buf_config.chunk++;
       chunk_temp >>=1;
    } while(chunk_temp);
    buf_config.size = buf_config.chunk+1;
    buf_config.threshold = buf_config.chunk;

    if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_SET_BUF_CONFIG, &buf_config)) {
       LOGE("Error setting buffer sizes, is capture running?");
    }

    mState = AUDIO_INPUT_OPENED;
    return mHardware->doStandby(false, false); // input, online
}

status_t AudioHardware::AudioStreamInTegra::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamInTegra::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd count: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmState: %d\n", mState);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInTegra::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamInTegra::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        LOGV("set input routing %x", device);
        if (device & (device - 1)) {
            status = BAD_VALUE;
        } else {
            mDevices = device;
            status = mHardware->doRouting();
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamInTegra::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamInTegra::getParameters() %s", param.toString().string());
    return param.toString();
}

// ----------------------------------------------------------------------------

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

}; // namespace android
