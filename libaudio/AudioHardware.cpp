/*
** Copyright 2008, The Android Open-Source Project
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

// hardware specific functions

#include "AudioHardware.h"
#include <media/AudioRecord.h>

namespace android {
const uint32_t AudioHardware::inputSamplingRates[] = {
    8000, 11025, 22050, 44100
};
// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(true), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), mCurOutDevice(-1), mCurInDevice(-1)
{
    int fd = ::open("/dev/audio_ctl", O_RDWR);
    if (fd >= 0) {
        if (::ioctl(fd, CPCAP_AUDIO_OUT_GET_OUTPUT, &mCurOutDevice))
            LOGE("Could not retrieve output device: %s", strerror(errno));
        else
            LOGI("current output device: %d", mCurOutDevice);

        if (::ioctl(fd, CPCAP_AUDIO_IN_GET_INPUT, &mCurInDevice))
            LOGE("Could not retrieve input device: %s", strerror(errno));
        else
            LOGI("current input device: %d", mCurInDevice);
    }
    mInit = true;
}

AudioHardware::~AudioHardware()
{
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
    mInit = false;
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

    mLock.lock();

    AudioStreamInTegra* in = new AudioStreamInTegra();
    status_t lStatus = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) {
        *status = lStatus;
    }
    if (lStatus != NO_ERROR) {
        mLock.unlock();
        delete in;
        return 0;
    }

    mInputs.add(in);
    mLock.unlock();

    return in;
}

void AudioHardware::closeInputStream(AudioStreamIn* in) {
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
    status_t status = AudioHardwareBase::setMode(mode);
    if (status == NO_ERROR) {
        // make sure that doAudioRouteOrMute() is called by doRouting()
        // even if the new device selected is the same as current one.
        clearCurDevice();
    }
    return status;
}

bool AudioHardware::checkOutputStandby()
{
    if (mOutput)
        if (!mOutput->checkStandby())
            return false;

    return true;
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
    {
        int fd = open("/dev/audio_ctl", O_RDWR);
        if (fd < 0) {
            LOGE("could not open audio_ctl: %s\n", strerror(errno));
            return NO_ERROR;
        }
        if (ioctl(fd, CPCAP_AUDIO_OUT_SET_VOLUME, vol) < 0)
            LOGE("could not set volume: %s\n", strerror(errno));
        close(fd);
    }
#endif
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    Mutex::Autolock lock(mLock);
    int vol = ceil(v * 15.0);
    LOGI("Set master volume to %d.\n", vol);

    {
        int fd = open("/dev/audio_ctl", O_RDWR);
        if (fd < 0) {
            LOGE("could not open audio_ctl: %s\n", strerror(errno));
            return NO_ERROR;
        }
        if (ioctl(fd, CPCAP_AUDIO_OUT_SET_VOLUME, vol) < 0)
            LOGE("could not set volume: %s\n", strerror(errno));
        close(fd);
    }

    return NO_ERROR;
}

// always call with mutex held
status_t AudioHardware::doAudioRouteOrMute()
{
    int fd = open("/dev/audio_ctl", O_RDWR);
    if (fd < 0) {
        LOGE("could not open audio_ctl: %s\n", strerror(errno));
        return NO_ERROR;
    }

    struct cpcap_audio_stream in;
    in.id = mCurInDevice;
    in.on = 1;

    if (ioctl(fd, CPCAP_AUDIO_IN_SET_INPUT, &in) < 0)
        LOGE("could not set input: %s\n", strerror(errno));

    struct cpcap_audio_stream out;
    out.id = mCurOutDevice;
    out.on = 1;

    if (ioctl(fd, CPCAP_AUDIO_OUT_SET_OUTPUT, &out) < 0)
        LOGE("could not set output: %s\n", strerror(errno));
    close(fd);

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

    LOGV("inputDevice = 0x%x", inputDevice);
    LOGV("outputDevices = 0x%x", outputDevices);

    /* We do not support more than one input device simultaneously */
    if (inputDevice & (inputDevice - 1)) {
        LOGE("Multiple input devices (0x%x) are not supported", inputDevice);
        return INVALID_OPERATION;
    }

    if (inputDevice != 0) {
        LOGI("do input routing device %x\n", inputDevice);
        if (inputDevice & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            LOGI("Routing audio to Bluetooth PCM: NOT IMPLEMENTED\n");
            sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
            sndInDevice = CPCAP_AUDIO_IN_MIC1;
        } else if (inputDevice & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
            if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
                sndOutDevice = CPCAP_AUDIO_OUT_HEADSET_AND_SPEAKER;
                if (inputDevice & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
                    LOGI("Routing audio to Wired Headset and Speaker\n");
                    sndInDevice = CPCAP_AUDIO_IN_MIC2;
                }
                else {
                    LOGI("Routing audio to Wired Headset and Speaker (input from headset)\n");
                    sndInDevice = CPCAP_AUDIO_IN_MIC1;
                }
            } else {
                LOGI("Routing audio to Wired Headset\n");
                sndOutDevice = CPCAP_AUDIO_OUT_HEADSET;
                sndInDevice = CPCAP_AUDIO_IN_MIC2;
            }
        } else {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to Speakerphone\n");
                sndDevice = CPCAP_AUDIO_OUT_SPEAKER;
            } else {
                LOGI("Routing audio to Handset\n");
                sndDevice = CPCAP_AUDIO_OUT_HEADSET;
            }
        }
    }
    // if inputDevice == 0, restore output routing

    if (sndOutDevice == -1) {
        // We do not multiple output devices if one of them is not the speaker
        // and the other one is not a headset.
        if (outputDevices & (outputDevices - 1)) {
            if ((outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) == 0) {
                LOGE("Hardware does not support requested route combination (%#X),"
                     " picking closest possible route...", outputDevices);
                return INVALID_OPERATION;
            }
        }

        if (outputDevices &
                    (AudioSystem::DEVICE_OUT_BLUETOOTH_SCO | AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET)) {
            LOGI("Routing audio to Bluetooth PCM: NOT IMPLEMENTED\n");
            sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
            sndInDevice = CPCAP_AUDIO_IN_MIC1;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT) {
            LOGI("Routing audio to Bluetooth PCM: NOT IMPLEMENTED\n");
            sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
            sndInDevice = CPCAP_AUDIO_IN_MIC1;
        } else if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                   (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
            LOGI("Routing audio to Wired Headset and Speaker\n");
            sndOutDevice = CPCAP_AUDIO_OUT_HEADSET_AND_SPEAKER;
            sndInDevice = CPCAP_AUDIO_IN_MIC1;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to No-microphone Wired Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = CPCAP_AUDIO_OUT_SPEAKER;
            } else {
                LOGI("Routing audio to No-microphone Wired Headset (%d,%x)\n", mMode, outputDevices);
                sndOutDevice = CPCAP_AUDIO_OUT_HEADSET;
                sndInDevice = CPCAP_AUDIO_IN_MIC1;
            }
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            LOGI("Routing audio to Wired Headset\n");
            sndOutDevice = CPCAP_AUDIO_OUT_HEADSET;
            sndInDevice = CPCAP_AUDIO_IN_MIC2;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
            LOGI("Routing audio to Speakerphone\n");
            sndOutDevice = CPCAP_AUDIO_OUT_SPEAKER;
            sndInDevice = CPCAP_AUDIO_IN_MIC1;
        } else {
            LOGE("Invalid routing combination (in %x, out %x)", inputDevice, outputDevices);
            return INVALID_OPERATION;
        }
    }

    if (sndOutDevice != -1)
        mCurOutDevice = sndOutDevice;

    if (sndInDevice != -1)
        mCurInDevice = sndInDevice;

    return doAudioRouteOrMute();
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
    mHardware(0), mFd(-1), mFdCtl(-1), mStartCount(0), mRetryCount(0), mStandby(true), mDevices(0)
{
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

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mDevices = devices;

    return NO_ERROR;
}

AudioHardware::AudioStreamOutTegra::~AudioStreamOutTegra()
{
    if (mFd >= 0) close(mFd);
    if (mFdCtl >= 0) close(mFdCtl);
}

ssize_t AudioHardware::AudioStreamOutTegra::write(const void* buffer, size_t bytes)
{
    // LOGD("AudioStreamOutTegra::write(%p, %u)", buffer, bytes);
    int status = NO_INIT;
    unsigned errors;
    ssize_t written;

    if (mStandby) {
        // open driver
        LOGV("open driver");
        status = ::open("/dev/audio0_out", O_RDWR);
        if (status < 0) {
            LOGE("Cannot open /dev/audio0_out errno: %d", errno);
            // Simulate audio output timing in case of error
            usleep(bytes * 1000000 / frameSize() / sampleRate());
            return status;
        }

        mFdCtl = ::open("/dev/audio0_out_ctl", O_RDWR);
        if (mFdCtl < 0)
            LOGE("Could not open audio-output-control: %s\n", strerror(errno));

        mFd = status;
        mStandby = false;
    }

    written = ::write(mFd, buffer, bytes);
    if (written < 0)
        LOGE("Error writing %d bytes to output: %s", bytes, strerror(errno));
    else {
        if (ioctl(mFdCtl, TEGRA_AUDIO_OUT_GET_ERROR_COUNT, &errors) < 0)
            LOGE("Could not retrieve playback error count: %s\n", strerror(errno));
        else if (errors)
            LOGW("Played %d bytes with %d errors\n", (int)written, errors);
    }

    return written;
}

status_t AudioHardware::AudioStreamOutTegra::standby()
{
    status_t status = NO_ERROR;
    if (!mStandby && mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    mStandby = true;
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
    snprintf(buffer, SIZE, "\tmStandby: %s\n", mStandby? "true": "false");
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

bool AudioHardware::AudioStreamOutTegra::checkStandby()
{
    return mStandby;
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
    mHardware(0), mFd(-1), mFdCtl(-1), mState(AUDIO_INPUT_CLOSED), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0)
{
}

status_t AudioHardware::AudioStreamInTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    if (pFormat == 0)
        return BAD_VALUE;
    if (*pFormat != AUDIO_HW_IN_FORMAT) {
        LOGE("wrong in format %d, expecting %lld", *pFormat, AUDIO_HW_IN_FORMAT);
        *pFormat = AUDIO_HW_IN_FORMAT;
        return BAD_VALUE;
    }

    if (pRate == 0)
        return BAD_VALUE;

    uint32_t rate = hw->getInputSampleRate(*pRate);
    if (rate != *pRate) {
        LOGE("wrong sample rate %d, expecting %d", *pRate, rate);
        *pRate = rate;
        return BAD_VALUE;
    }

    if (pChannels == 0)
        return BAD_VALUE;

    if (*pChannels != AudioSystem::CHANNEL_IN_MONO &&
        *pChannels != AudioSystem::CHANNEL_IN_STEREO) {
        LOGE("wrong number of channels %d", *pChannels);
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return BAD_VALUE;
    }

    mHardware = hw;

    LOGV("AudioStreamInTegra::set(%d, %d, %u)", *pFormat, *pChannels, *pRate);
    if (mFd >= 0) {
        LOGE("Audio record already open");
        return -EPERM;
    }

    // open audio input device
    status_t status = ::open("/dev/audio0_in", O_RDWR);
    if (status < 0) {
        LOGE("Cannot open /dev/audio0_in: %s", strerror(errno));
        goto Error;
    }
    mFd = status;

    // open audio input-control device
    status = ::open("/dev/audio0_in_ctl", O_RDWR);
    if (status < 0) {
        LOGE("Cannot open /dev/audio0_in_ctl: %s", strerror(errno));
        goto Error;
    }
    mFdCtl = status;

    // configuration
    LOGV("get config");
    struct tegra_audio_in_config config;
    status = ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config);
    if (status < 0) {
        LOGE("cannot read input config: %s", strerror(errno));
        goto Error;
    }

    LOGV("set config");
    config.stereo = AudioSystem::popCount(*pChannels) == 2;
    config.rate = *pRate;
#if 0
    config.buffer_size = bufferSize();
    config.buffer_count = 2;
#endif
    status = ioctl(mFdCtl, TEGRA_AUDIO_IN_SET_CONFIG, &config);
    if (status < 0) {
        LOGE("cannot set input config: %s", strerror(errno));
        if (ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config) == 0) {
            if (config.stereo) {
                *pChannels = AudioSystem::CHANNEL_IN_STEREO;
            } else {
                *pChannels = AudioSystem::CHANNEL_IN_MONO;
            }
            *pRate = config.rate;
        }
        goto Error;
    }

    LOGV("stereo: %d", config.stereo);
    LOGV("sample rate: %d", config.rate);

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = config.rate;
    mBufferSize = AUDIO_HW_IN_BUFFERSIZE;

    //mHardware->setMicMute_nosync(false);
    mState = AUDIO_INPUT_OPENED;

    return NO_ERROR;

Error:
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    if (mFdCtl >= 0) {
        ::close(mFdCtl);
        mFdCtl = -1;
    }
    return status;
}

AudioHardware::AudioStreamInTegra::~AudioStreamInTegra()
{
    LOGV("AudioStreamInTegra destructor");
    standby();
}

ssize_t AudioHardware::AudioStreamInTegra::read(void* buffer, ssize_t bytes)
{
    ssize_t ret;
    unsigned errors;

//  LOGV("AudioStreamInTegra::read(%p, %ld)", buffer, bytes);
    if (!mHardware) return -1;

    if (mState < AUDIO_INPUT_OPENED) {
        Mutex::Autolock lock(mHardware->mLock);
        if (set(mHardware, mDevices, &mFormat, &mChannels, &mSampleRate, mAcoustics) != NO_ERROR) {
            return -1;
        }
    }

    if (mState < AUDIO_INPUT_STARTED) {
        mState = AUDIO_INPUT_STARTED;
        // force routing to input device
        mHardware->clearCurDevice();
        mHardware->doRouting();
    }

    ret = ::read(mFd, buffer, bytes);
    if (ret < 0)
        LOGE("Error reading from audio in: %s", strerror(errno));

    if (ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_ERROR_COUNT, &errors) < 0)
        LOGE("Could not retrieve recording error count: %s\n", strerror(errno));
    else if (errors)
        LOGW("Recorded %d bytes with %d errors\n", (int)ret, errors);

    return ret;
}

status_t AudioHardware::AudioStreamInTegra::standby()
{
    if (mState > AUDIO_INPUT_CLOSED) {
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
        if (mFdCtl >= 0) {
            ::close(mFdCtl);
            mFdCtl = -1;
        }
        mState = AUDIO_INPUT_CLOSED;
    }
    if (!mHardware) return -1;
    // restore output routing if necessary
    mHardware->clearCurDevice();
    mHardware->doRouting();
    return NO_ERROR;
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
