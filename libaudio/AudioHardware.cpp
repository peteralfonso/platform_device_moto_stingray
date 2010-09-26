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

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
// hardware specific functions
extern uint16_t HC_CTO_AUDIO_MM_PARAMETER_TABLE[];
#endif

#include "AudioHardware.h"
#include <media/AudioRecord.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

namespace android {
const uint32_t AudioHardware::inputSamplingRates[] = {
    8000, 11025, 16000, 22050, 44100
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

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    // One-time CTO Audio configuration
    mAudioMmEnvVar.cto_audio_mm_param_block_ptr              = HC_CTO_AUDIO_MM_PARAMETER_TABLE;
    mAudioMmEnvVar.cto_audio_mm_pcmlogging_buffer_block_ptr  = mPcmLoggingBuf;
    mAudioMmEnvVar.pcmlogging_buffer_block_size              = ARRAY_SIZE(mPcmLoggingBuf);
    mAudioMmEnvVar.cto_audio_mm_runtime_param_mem_ptr        = mRuntimeParam;
    mAudioMmEnvVar.cto_audio_mm_static_memory_block_ptr      = mStaticMem;
    mAudioMmEnvVar.cto_audio_mm_scratch_memory_block_ptr     = mScratchMem;
    mAudioMmEnvVar.accy = CTO_AUDIO_MM_ACCY_INVALID;
    mAudioMmEnvVar.sample_rate = CTO_AUDIO_MM_SAMPL_44100;
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
    setCtoAudioDev(mCurOutDevice.id, mCurInDevice.id);
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

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS

uint32_t AudioHardware::convOutDevToCTO(uint32_t outDev)
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
uint32_t AudioHardware::convRateToCto(uint32_t rate)
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

void AudioHardware::configCtoAudio()
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

// The 2nd parameter is for an input device, not yet used
// For now, this is only configuring the multimedia audio playback output device.
void AudioHardware::setCtoAudioDev(uint32_t outDev, uint32_t )
{
    uint32_t mm_accy = convOutDevToCTO(outDev);
    Mutex::Autolock lock(mCtoLock);

    LOGD("setCtoAudioDev %d", outDev);
    if (mm_accy != mAudioMmEnvVar.accy) {
        mAudioMmEnvVar.accy = mm_accy;
        configCtoAudio();
    }
}

// Setting the HW sampling rate may require reconfiguration of audio processing.
void AudioHardware::setCtoAudioRate(int sampRate)
{
    uint32_t rate = convRateToCto(sampRate);
    Mutex::Autolock lock(mCtoLock);

    LOGD("AudioHardware::setCtoAudioRate %d", sampRate);
    if (rate != mAudioMmEnvVar.sample_rate) {
        mAudioMmEnvVar.sample_rate = rate;
        configCtoAudio();
    }
}

#endif /* USE_PROPRIETARY_AUDIO_EXTENSIONS */

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
    mHardware->setCtoAudioRate(lRate);
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

ssize_t AudioHardware::AudioStreamOutTegra::write(const void* buffer, size_t bytes)
{
    // LOGD("AudioStreamOutTegra::write(%p, %u)", buffer, bytes);
    int status = NO_INIT;
    unsigned errors;
    ssize_t written;
    const uint8_t* p = static_cast<const uint8_t*>(buffer);

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    mHardware->mCtoLock.lock();
    if(mHardware->mAudioMmEnvVar.accy != CTO_AUDIO_MM_ACCY_INVALID) {
        // Apply the CTO audio effects in-place.
        // frame_size in this context is the number of samples in the buffer.
        mHardware->mAudioMmEnvVar.frame_size =  bytes / frameSize();
        api_cto_audio_mm_main(&mHardware->mAudioMmEnvVar, (int16_t *)buffer, (int16_t *)buffer);
    }
    mHardware->mCtoLock.unlock();
#endif

    status = online(); // if already online, a no-op
    if (status < 0) {
        // Simulate audio output timing in case of error
        usleep(bytes * 1000000 / frameSize() / sampleRate());
        return status;
    }

    written = ::write(mFd, buffer, bytes);
    if (written < 0)
        LOGE("Error writing %d bytes to output: %s", bytes, strerror(errno));
    else {
        if (::ioctl(mFdCtl, TEGRA_AUDIO_OUT_GET_ERROR_COUNT, &errors) < 0)
            LOGE("Could not retrieve playback error count: %s\n", strerror(errno));
        else if (errors)
            LOGW("Played %d bytes with %d errors\n", (int)written, errors);
    }

    return written;
}

status_t AudioHardware::AudioStreamOutTegra::online()
{
    if (mHardware->mCurOutDevice.on) {
        LOGV("%s: output already online", __FUNCTION__);
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
    // open audio input device
    mFd = ::open("/dev/audio0_in", O_RDWR);
    mFdCtl = ::open("/dev/audio0_in_ctl", O_RDWR);
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

    // configuration
    struct tegra_audio_in_config config;
    status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config);
    if (status < 0) {
        LOGE("cannot read input config: %s", strerror(errno));
        return status;
    }

    config.stereo = AudioSystem::popCount(*pChannels) == 2;
    config.rate = *pRate;
#if 0
    config.buffer_size = bufferSize();
    config.buffer_count = 2;
#endif
    status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_SET_CONFIG, &config);
    if (status < 0) {
        LOGE("cannot set input config: %s", strerror(errno));
        if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config) == 0) {
            if (config.stereo) {
                *pChannels = AudioSystem::CHANNEL_IN_STEREO;
            } else {
                *pChannels = AudioSystem::CHANNEL_IN_MONO;
            }
            *pRate = config.rate;
        }
        return status;
    }

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = config.rate;
    mBufferSize = AUDIO_HW_IN_BUFFERSIZE;

    //mHardware->setMicMute_nosync(false);
    mState = AUDIO_INPUT_OPENED;

    return mHardware->doStandby(false, false); // input, online
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
    unsigned errors;

    LOGV("AudioStreamInTegra::read(%p, %ld)", buffer, bytes);
    if (!mHardware) {
        LOGE("%s: mHardware is null", __FUNCTION__);
        return -1;
    }

    online();

    if (mState < AUDIO_INPUT_OPENED) {
        Mutex::Autolock lock(mHardware->mLock);
        if (set(mHardware, mDevices, &mFormat, &mChannels, &mSampleRate, mAcoustics) != NO_ERROR) {
            LOGE("%s: set() failed", __FUNCTION__);
            return -1;
        }
    }

    if (mState < AUDIO_INPUT_STARTED) {
        mState = AUDIO_INPUT_STARTED;
        mHardware->doRouting();
    }

    ret = ::read(mFd, buffer, bytes);
    if (ret < 0)
        LOGE("Error reading from audio in: %s", strerror(errno));

    if (::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_ERROR_COUNT, &errors) < 0)
        LOGE("Could not retrieve recording error count: %s\n", strerror(errno));
    else if (errors)
        LOGW("Recorded %d bytes with %d errors\n", (int)ret, errors);

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
    if (mHardware->mCurInDevice.on) {
        LOGV("%s: input already online", __FUNCTION__); //@@@@@@
        return NO_ERROR;
    }

    LOGV("%s", __FUNCTION__);
    status_t rc = mHardware->doStandby(false, false); // input, online
#if 0
    if (!rc) {
        /* Start recording, if ongoing and previously paused.  Muting the
         * microphone will cause CPCAP to not send data through the i2s
         * interface, and read() will block until recording is resumed.
         */
        LOGV("%s: start/resume recording", __FUNCTION__);
        ::ioctl(mFd, TEGRA_AUDIO_IN_START);
    }
#endif
    return rc;
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
