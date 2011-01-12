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

namespace android {
const uint32_t AudioHardware::inputSamplingRates[] = {
    8000, 11025, 12000, 16000, 22050, 32000, 44100, 48000
};

// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(false), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), mCpcapCtlFd(-1), mMasterVol(1.0), mVoiceVol(1.0)
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
    mAudioPP.setAudioDev(&mCurOutDevice, &mCurInDevice, false, false, false);
#endif

    readHwGainFile();

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

void AudioHardware::readHwGainFile()
{
    int fd;
    int rc=0;
    int i;
    uint32_t format, version, barker;
    fd = open("/system/etc/cpcap_gain.bin", O_RDONLY);
    if (fd>=0) {
        ::read(fd, &format, sizeof(uint32_t));
        ::read(fd, &version, sizeof(uint32_t));
        ::read(fd, &barker, sizeof(uint32_t));
        rc = ::read(fd, mCpcapGain, sizeof(mCpcapGain));
        LOGD("Read gain file, format %X version %X", format, version);
        ::close(fd);
    }
    if (rc != sizeof(mCpcapGain) || format != 0x30303032) {
        int gain;
        LOGE("CPCAP gain file not valid. Using defaults.");
        for (int i=0; i<AUDIO_HW_GAIN_NUM_DIRECTIONS; i++) {
            if (i==AUDIO_HW_GAIN_SPKR_GAIN)
                gain = 11;
            else
                gain = 31;
            for (int j=0; j<AUDIO_HW_GAIN_NUM_USECASES; j++)
                for (int k=0; k<AUDIO_HW_GAIN_NUM_PATHS; k++)
                    mCpcapGain[i][j][k]=gain;
        }
    }
    return;
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
            mLock.unlock();
            delete out;
            mLock.lock();
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
        mLock.unlock();
        delete mOutput;
        mLock.lock();
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
        mLock.unlock();
        delete in;
        mLock.lock();
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
    AutoMutex lock(mLock);
    bool wasInCall = isInCall();
    LOGV("setMode() : new %d, old %d", mode, mMode);
    status_t status = AudioHardwareBase::setMode(mode);
    if (status == NO_ERROR) {
        if (wasInCall ^ isInCall()) {
            doRouting_l();
            if (wasInCall) {
                setMicMute_l(false);
            }
        }
    }

    return status;
}

// Must be called with mLock held
status_t AudioHardware::doStandby(int stop_fd, bool output, bool enable)
{
    status_t status = NO_ERROR;
    struct cpcap_audio_stream standby;

    LOGV("AudioHardware::doStandby() putting %s in %s mode",
            output ? "output" : "input",
            enable ? "standby" : "online" );

// Debug code
    if (!mLock.tryLock()) {
        LOGE("doStandby called without mLock held.");
        mLock.unlock();
    }
// end Debug code

    if (output) {
        standby.id = CPCAP_AUDIO_OUT_STANDBY;
        standby.on = enable;

        if (enable) {
            /* Flush the queued playback data.  Putting the output in standby
             * will cause CPCAP to not drive the i2s interface, and write()
             * will block until playback is resumed.
             */
            if (mOutput)
                mOutput->flush();
        }

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

        if (enable && stop_fd >= 0) {
            /* Stop recording, if ongoing.  Muting the microphone will cause
             * CPCAP to not send data through the i2s interface, and read()
             * will block until recording is resumed.
             */
            LOGV("%s: stop recording", __FUNCTION__);
            if (::ioctl(stop_fd, TEGRA_AUDIO_IN_STOP) < 0) {
                LOGE("could not stop recording: %s\n",
                     strerror(errno));
            }
        }

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
    return setMicMute_l(state);
}

status_t AudioHardware::setMicMute_l(bool state)
{
    if (mMicMute != state) {
        mMicMute = state;
        LOGV("setMicMute() %s", (state)?"ON":"OFF");
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
            LOGI("Turn on bluetooth NREC");
        } else {
            mBluetoothNrec = false;
            LOGI("Turning noise reduction and echo cancellation off for BT "
                 "headset");
        }
        doRouting();
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
    AudioParameter request = AudioParameter(keys);
    AudioParameter reply = AudioParameter();
    String8 value;
    String8 key;

    LOGV("getParameters() %s", keys.string());

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    key = "ec_supported";
    if (request.get(key, value) == NO_ERROR) {
        value = "yes";
        reply.add(key, value);
    }
#endif

    return reply.toString();
}

size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    size_t bufsize;

    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount < 1 || channelCount > 2) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    // Return 20 msec input buffer size.
    bufsize = sampleRate * sizeof(int16_t) * channelCount / 50;
    if (bufsize & 0x7) {
       // Not divisible by 8.
       bufsize +=8;
       bufsize &= ~0x7;
    }
    LOGD("%s: returns %d for rate %d", __FUNCTION__, bufsize, sampleRate);
    return bufsize;
}

//setVoiceVolume is only useful for setting sidetone gains with a baseband
//controlling volume.  Don't adjust hardware volume with this API.
//
//(On Stingray, don't use mVoiceVol for anything.)
status_t AudioHardware::setVoiceVolume(float v)
{
    if (v < 0.0)
        v = 0.0;
    else if (v > 1.0)
        v = 1.0;

    LOGI("Setting unused in-call vol to %f",v);
    mVoiceVol = v;

    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    if (v < 0.0)
        v = 0.0;
    else if (v > 1.0)
        v = 1.0;

    LOGV("Set master vol to %f.\n", v);
    mMasterVol = v;
    Mutex::Autolock lock(mLock);
    int useCase = AUDIO_HW_GAIN_USECASE_MM;
    AudioStreamInTegra *input = getActiveInput_l();
    if (input) {
        if (isInCall() && !mOutput->getStandby() &&
                input->source() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
            useCase = AUDIO_HW_GAIN_USECASE_VOICE;
        } else if (input->source() == AUDIO_SOURCE_VOICE_RECOGNITION) {
            useCase = AUDIO_HW_GAIN_USECASE_VOICE_REC;
        }
    }
    setVolume_l(v, useCase);
    return NO_ERROR;
}

// Call with mLock held.
status_t AudioHardware::setVolume_l(float v, int usecase)
{
    status_t ret;
    int spkr = getGain(AUDIO_HW_GAIN_SPKR_GAIN, usecase);
    int mic = getGain(AUDIO_HW_GAIN_MIC_GAIN, usecase);

    if (spkr==0) {
       // no device to set volume on.  Ignore request.
       return -1;
    }

    spkr = ceil(v * spkr);
    LOGD("Set tx volume to %d, rx to %d.\n", spkr, mic);

    ret = ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_VOLUME, spkr);
    if (ret < 0)
        LOGE("could not set spkr volume: %s\n", strerror(errno));
    else {
        ret = ::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_VOLUME, mic);
        if (ret < 0)
            LOGE("could not set mic volume: %s\n", strerror(errno));
    }

    return ret;
}

uint8_t AudioHardware::getGain(int direction, int usecase)
{
    int path;
    AudioStreamInTegra *input = getActiveInput_l();
    uint32_t inDev = (input == NULL) ? 0 : input->devices();
    if (!mOutput) {
       LOGE("No output device.");
       return 0;
    }
    uint32_t outDev = mOutput->devices();

// In case of an actual phone, with an actual earpiece, uncomment.
//    if (outDev & AudioSystem::DEVICE_OUT_EARPIECE)
//        path = AUDIO_HW_GAIN_EARPIECE;
//    else
    if (outDev & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)
        path = AUDIO_HW_GAIN_HEADSET_NO_MIC;
    else if (outDev & AudioSystem::DEVICE_OUT_WIRED_HEADSET)
        path = AUDIO_HW_GAIN_HEADSET_W_MIC;
    else if (outDev & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)
        path = AUDIO_HW_GAIN_EMU_DEVICE;
    else
       path = AUDIO_HW_GAIN_SPEAKERPHONE;

    LOGV("Picked gain[%d][%d][%d] which is %d.",direction, usecase, path,
          mCpcapGain[direction][usecase][path]);

    return mCpcapGain[direction][usecase][path];
}

int AudioHardware::getActiveInputRate()
{
    AudioStreamInTegra *input = getActiveInput_l();
    return (input != NULL) ? input->sampleRate() : 0;
}

status_t AudioHardware::doRouting()
{
    Mutex::Autolock lock(mLock);
    return doRouting_l();
}

// Call this with mLock held.
status_t AudioHardware::doRouting_l()
{
    uint32_t outputDevices = mOutput->devices();
    AudioStreamInTegra *input = getActiveInput_l();
    uint32_t inputDevice = (input == NULL) ? 0 : input->devices();
    uint32_t btScoOutDevices = outputDevices & (
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO |
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET |
                           AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT );
    uint32_t spdifOutDevices = outputDevices & (
                           AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET |
                           AudioSystem::DEVICE_OUT_AUX_DIGITAL );
    uint32_t speakerOutDevices = outputDevices ^ btScoOutDevices ^ spdifOutDevices;
    uint32_t btScoInDevice = inputDevice & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    uint32_t micInDevice   = inputDevice ^ btScoInDevice;
    int sndOutDevice = -1;
    int sndInDevice = -1;
    bool btScoOn = btScoOutDevices||btScoInDevice;

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

    switch (speakerOutDevices) {
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
    case AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET:
        sndOutDevice = CPCAP_AUDIO_OUT_ANLG_DOCK_HEADSET;
        break;
    case AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET:
      // To be implemented
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

    LOGV("current output %d, %s",
         mCurOutDevice.id,
         mCurOutDevice.on ? "on" : "off");

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_INPUT,
              &mCurInDevice) < 0)
        LOGE("could not set input (%d, on %d): %s\n",
             mCurInDevice.id, mCurInDevice.on, strerror(errno));

    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_OUTPUT,
              &mCurOutDevice) < 0)
        LOGE("could not set output (%d, on %d): %s\n",
             mCurOutDevice.id, mCurOutDevice.on,
             strerror(errno));

    mOutput->setDriver(speakerOutDevices?true:false,
                       btScoOn,
                       spdifOutDevices?true:false);
    if (input)
        input->setDriver(micInDevice?true:false,
                         btScoInDevice?true:false);

    bool ecnsEnabled = false;
    // enable EC if:
    // - the audio mode is IN_CALL or IN_COMMUNICATION  AND
    // - the output stream is active AND
    // - an input stream with VOICE_COMMUNICATION source is active
    if (isInCall() && !mOutput->getStandby() &&
        input && input->source() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        ecnsEnabled = true;
    }

    int oldInRate=mHwInRate, oldOutRate=mHwOutRate;
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    int ecnsRate = getActiveInputRate() < 16000? 8000 : 16000;
    mAudioPP.setAudioDev(&mCurOutDevice, &mCurInDevice,
                         btScoOn, mBluetoothNrec,
                         spdifOutDevices?true:false);
    mAudioPP.enableEcns(ecnsEnabled);
    // Check input/output rates for HW.
    if (ecnsEnabled) {
        mHwInRate = ecnsRate;
        mHwOutRate = mHwInRate;
        LOGD("EC/NS active, requests rate as %d for in/out", mHwInRate);
    }
    else
#endif
    {
        mHwInRate = getActiveInputRate();
        if (mHwInRate == 0)
            mHwInRate = 44100;
        mHwOutRate = 44100;
        LOGV("No EC/NS, set input rate %d, output %d.", mHwInRate, mHwOutRate);
    }
    if (btScoOn) {
        mHwOutRate = 8000;
        mHwInRate = 8000;
        LOGD("Bluetooth SCO active, rate forced to 8K");
    }

    int speakerOutRate = 0;
    if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_GET_RATE, &speakerOutRate))
        LOGE("could not read output rate: %s\n",
                   strerror(errno));
    if (mHwOutRate != oldOutRate ||
        (speakerOutRate!=44100 && btScoOn)) {
        int speaker_rate = mHwOutRate;
        if (btScoOn) {
            speaker_rate = 44100;
        }
        // Flush old data (wrong rate) from I2S driver before changing rate.
        if (mOutput)
            mOutput->flush();
        // Now the DMA is empty, change the rate.
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_OUT_SET_RATE,
                  speaker_rate) < 0)
            LOGE("could not set output rate(%d): %s\n",
                  speaker_rate, strerror(errno));
    }
    if (mHwInRate != oldInRate) {
        LOGV("Minor TODO: Flush input if active.");
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_SET_RATE,
                  mHwInRate) < 0)
            LOGE("could not set input rate(%d): %s\n",
                  mHwInRate, strerror(errno));
        if (::ioctl(mCpcapCtlFd, CPCAP_AUDIO_IN_GET_RATE, &mHwInRate))
            LOGE("CPCAP driver error reading rates.");
    }

    // Since HW path may have changed, set the hardware gains.
    int useCase = AUDIO_HW_GAIN_USECASE_MM;
    if (ecnsEnabled) {
        useCase = AUDIO_HW_GAIN_USECASE_VOICE;
    } else if (input && input->source() == AUDIO_SOURCE_VOICE_RECOGNITION) {
        useCase = AUDIO_HW_GAIN_USECASE_VOICE_REC;
    }
    setVolume_l(mMasterVol, useCase);

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
        if (!mInputs[i]->getStandby()) {
            return mInputs[i];
        }
    }

    return NULL;
}

// ----------------------------------------------------------------------------
// Sample Rate Converter wrapper
//
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
AudioHardware::AudioStreamSrc::AudioStreamSrc() :
  mSrcInitted(false)
{
}
AudioHardware::AudioStreamSrc::~AudioStreamSrc()
{
}

void AudioHardware::AudioStreamSrc::init(int inRate, int outRate)
{
    if (mSrcBuffer == NULL)
        mSrcBuffer = new char[src_memory_required_stereo(MAX_FRAME_LEN, MAX_CONVERT_RATIO)];
    if (mSrcBuffer == NULL) {
        LOGE("Failed to allocate memory for sample rate converter.");
        return;
    }
    mSrcInit.memory = (SRC16*)(mSrcBuffer);
    mSrcInit.input_rate = inRate;
    mSrcInit.output_rate = outRate;
    mSrcInit.frame_length = MAX_FRAME_LEN;
    mSrcInit.stereo_flag = SRC_OFF;
    mSrcInit.input_interleaved = SRC_OFF;
    mSrcInit.output_interleaved = SRC_OFF;
    rate_convert_init(&mSrcInit, &mSrcObj);

    mSrcInitted = true;
    mSrcInRate = inRate;
    mSrcOutRate = outRate;
}
#endif

// ----------------------------------------------------------------------------

AudioHardware::AudioStreamOutTegra::AudioStreamOutTegra() :
    mHardware(0), mFd(-1), mFdCtl(-1), mStartCount(0), mRetryCount(0), mDevices(0),
    mIsSpkrEnabled(0), mIsBtEnabled(0), mIsSpdifEnabled(0),
    mIsSpkrEnabledReq(0), mIsBtEnabledReq(0), mIsSpdifEnabledReq(0),
    mState(AUDIO_STREAM_IDLE), mLocked(false)
{
    mFd = ::open("/dev/audio0_out", O_RDWR);
    mFdCtl = ::open("/dev/audio0_out_ctl", O_RDWR);
    mBtFd = ::open("/dev/audio1_out", O_RDWR);
    mBtFdCtl = ::open("/dev/audio1_out_ctl", O_RDWR);
    mBtFdIoCtl = ::open("/dev/audio1_ctl", O_RDWR);
    mSpdifFd = ::open("/dev/spdif_out", O_RDWR);
    mSpdifFdCtl = ::open("/dev/spdif_out_ctl", O_RDWR);
}

// Called with mHardware->mLock held.
void AudioHardware::AudioStreamOutTegra::setDriver(bool speaker, bool bluetooth, bool spdif)
{
    int bit_format = TEGRA_AUDIO_BIT_FORMAT_DEFAULT;
    bool is_bt_bypass = false;

    // acquire mutex if not already locked by write().
    if (!mLocked) {
        mLock.lock();
    }

    LOGV("%s: Analog speaker? %s. Bluetooth? %s. S/PDIF? %s.", __FUNCTION__,
        speaker?"yes":"no", bluetooth?"yes":"no", spdif?"yes":"no");

    // force some reconfiguration at next write()
    if (mState == AUDIO_STREAM_CONFIGURED &&
        (mIsSpkrEnabled != speaker || mIsBtEnabled != bluetooth || mIsSpdifEnabled != spdif)) {
        mState = AUDIO_STREAM_CONFIG_REQ;
    }

    mIsSpkrEnabledReq = speaker;
    mIsBtEnabledReq = bluetooth;
    mIsSpdifEnabledReq = spdif;

    if (!mLocked) {
        mLock.unlock();
    }
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
    if (mFd >= 0 && mFdCtl >= 0 &&
                mBtFd >= 0 &&
                mBtFdCtl >= 0 &&
                mBtFdIoCtl >= 0) {
        if (mSpdifFd < 0 || mSpdifFdCtl < 0)
            LOGW("s/pdif driver not present");
        return NO_ERROR;
    } else {
        LOGE("Problem opening device files - Is your kernel compatible?");
        return NO_INIT;
    }
}

AudioHardware::AudioStreamOutTegra::~AudioStreamOutTegra()
{
    standby();
    // Prevent someone from flushing the fd during a close.
    Mutex::Autolock lock(mFdLock);
    if (mFd >= 0) ::close(mFd);
    if (mFdCtl >= 0) ::close(mFdCtl);
    if (mBtFd >= 0) ::close(mBtFd);
    if (mBtFdCtl >= 0) ::close(mBtFdCtl);
    if (mBtFdIoCtl >= 0) ::close(mBtFdIoCtl);
    if (mSpdifFd >= 0) ::close(mSpdifFd);
    if (mSpdifFdCtl >= 0) ::close(mSpdifFdCtl);
}

ssize_t AudioHardware::AudioStreamOutTegra::write(const void* buffer, size_t bytes)
{
    status_t status;
    if (!mHardware) {
        LOGE("%s: mHardware is null", __FUNCTION__);
        return NO_INIT;
    }
    // LOGD("AudioStreamOutTegra::write(%p, %u) TID %d", buffer, bytes, gettid());
    // Protect output state during the write process.
    mHardware->mLock.lock();


    { // scope for the lock
        Mutex::Autolock lock(mLock);

        ssize_t written = 0;
        const uint8_t* p = static_cast<const uint8_t*>(buffer);
        size_t outsize = bytes;
        int outFd = mFd;
        bool stereo;
        int driverRate;
        ssize_t writtenToSpdif = 0;

        status = online_l(); // if already online, a no-op

        if (status < 0) {
            mHardware->mLock.unlock();
            goto error;
        }
        driverRate = mHardware->mHwOutRate;
        stereo = mIsBtEnabled ? false : (channels() == AudioSystem::CHANNEL_OUT_STEREO);

        mHardware->mLock.unlock();

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
        // Do Multimedia processing if appropriate for device and usecase.
        mHardware->mAudioPP.doMmProcessing((void *)buffer, bytes / frameSize());
#endif

        if (mIsSpkrEnabled && mIsBtEnabled) {
            // When dual routing to CPCAP and Bluetooth, piggyback CPCAP audio now,
            // and then down convert for the BT.
            // CPCAP is always 44.1 in this case.
            // This also works in the three-way routing case.
            Mutex::Autolock lock2(mFdLock);
            ::write(outFd, buffer, outsize);
        }
        if (mIsSpdifEnabled) {
            // When dual routing to Speaker and HDMI, piggyback HDMI now, since it
            // has no mic we'll leave the rest of the acoustic processing for the
            // CPCAP hardware path.
            // This also works in the three-way routing case, except the acoustic
            // tuning will be done on Bluetooth, since it has the exclusive mic amd
            // it also needs the sample rate conversion
            Mutex::Autolock lock2(mFdLock);
            writtenToSpdif = ::write(mSpdifFd, buffer, outsize);
            LOGV("%s: written %d bytes to SPDIF", __FUNCTION__, (int)writtenToSpdif);
        }
        if (mIsBtEnabled) {
            outFd = mBtFd;
        } else if (mIsSpdifEnabled && !mIsSpkrEnabled) {
            outFd = -1;
        }

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
        // Check if sample rate conversion or ECNS are required.
        // Caution: Upconversion (from 44.1 to 48) would require a new output buffer larger than the
        // original one.
        if (driverRate != (int)sampleRate()) {
            if (!mSrc.initted() ||
                 mSrc.inRate() != (int)sampleRate() ||
                 mSrc.outRate() != driverRate) {
                LOGI("%s: downconvert started from %d to %d",__FUNCTION__,
                     sampleRate(), driverRate);
                mSrc.init(sampleRate(), driverRate);
                if (!mSrc.initted()) {
                    status = -1;
                    goto error;
                }
                // Workaround to give multiple of 4 bytes to driver: Keep one sample
                // buffered in case SRC returns an odd number of samples.
                mHaveSpareSample = false;
            }
        } else {
            mSrc.deinit();
        }

        if (mHardware->mAudioPP.isEcnsEnabled() || mSrc.initted())
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

        if (mSrc.initted()) {
            // Apply the sample rate conversion.
            mSrc.mIoData.in_buf_ch1 = (SRC16 *) (buffer);
            mSrc.mIoData.in_buf_ch2 = 0;
            mSrc.mIoData.input_count = outsize / sizeof(SRC16);
            mSrc.mIoData.out_buf_ch1 = (SRC16 *) (buffer);
            mSrc.mIoData.out_buf_ch2 = 0;
            mSrc.mIoData.output_count = outsize / sizeof(SRC16);
            if (mHaveSpareSample) {
                // Leave room for placing the spare.
                mSrc.mIoData.out_buf_ch1++;
                mSrc.mIoData.output_count--;
            }
            mSrc.srcConvert();
            LOGV("Converted %d bytes at %d to %d bytes at %d",
                 outsize, sampleRate(), mSrc.mIoData.output_count*2, driverRate);
            if (mHaveSpareSample) {
                int16_t *bufp = (int16_t*)buffer;
                bufp[0]=mSpareSample;
                mSrc.mIoData.output_count++;
                mHaveSpareSample = false;
            }
            outsize = mSrc.mIoData.output_count*2;
            LOGV("Outsize is now %d", outsize);
        }
        if (mHardware->mAudioPP.isEcnsEnabled()) {
            // EC/NS is a blocking interface, to synchronise with read.
            // It also consumes data when EC/NS is running.
            // It expects MONO data.
            // If EC/NS is not running, it will return 0, and we need to write this data to the
            // driver ourselves.

            // Indicate that it is safe to call setDriver() without locking mLock: if the input
            // stream is started, doRouting_l() will not block when setDriver() is called.
            mLocked = true;
            LOGV("writeDownlinkEcns size %d", outsize);
            written = mHardware->mAudioPP.writeDownlinkEcns(outFd,(void *)buffer,
                                                            stereo, outsize, &mFdLock);
            mLocked = false;
        }
        if (mHardware->mAudioPP.isEcnsEnabled() || mSrc.initted()) {
            // Move audio back up to Stereo, if the EC/NS wasn't in fact running and we're
            // writing to a stereo device.
            if (stereo &&
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

        if (written != (ssize_t)outsize) {
            // The sample rate conversion modifies the output size.
            if (outsize&0x3) {
                int16_t* bufp = (int16_t *)buffer;
                LOGV("Keep the spare sample away from the driver.");
                mHaveSpareSample = true;
                mSpareSample = bufp[outsize/2 - 1];
            }

            if (outFd != -1) {
                Mutex::Autolock lock2(mFdLock);
                written = ::write(outFd, buffer, outsize&(~0x3));
                if (written != ((ssize_t)outsize&(~0x3))) {
                    status = written;
                    goto error;
                }
            } else {
                written = writtenToSpdif;
            }
        }
        if (written < 0) {
            LOGE("Error writing %d bytes to output: %s", outsize, strerror(errno));
            status = written;
            goto error;
        }

        // Sample rate converter may be stashing a couple of bytes here or there,
        // so just report that all bytes were consumed. (it would be a bug not to.)
        LOGV("write() written %d", bytes);
        return bytes;

    }
error:
    LOGE("write(): error, return %d", status);
    standby();
    usleep(bytes * 1000 / frameSize() / sampleRate() * 1000);

    return status;
}

void AudioHardware::AudioStreamOutTegra::flush()
{
    // Prevent someone from writing the fd while we flush
    Mutex::Autolock lock(mFdLock);
    LOGD("AudioStreamOutTegra::flush()");
    if (::ioctl(mFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       LOGE("could not flush playback: %s\n", strerror(errno));
    if (::ioctl(mBtFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       LOGE("could not flush bluetooth: %s\n", strerror(errno));
    if (::ioctl(mSpdifFdCtl, TEGRA_AUDIO_OUT_FLUSH) < 0)
       LOGE("could not flush spdif: %s\n", strerror(errno));
    LOGD("AudioStreamOutTegra::flush() returns");
}

// Called with mLock and mHardware->mLock held
status_t AudioHardware::AudioStreamOutTegra::online_l()
{
    status_t status = NO_ERROR;

    if (mState < AUDIO_STREAM_CONFIGURED) {
        if (mState == AUDIO_STREAM_IDLE) {
            LOGV("output %p going online", this);
            mState = AUDIO_STREAM_CONFIG_REQ;
            // update EC state if necessary
            AudioStreamInTegra *input = mHardware->getActiveInput_l();
            if (mHardware->isInCall() &&
                input && input->source() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                // setDriver() will not try to lock mLock when called by doRouting_l()
                mLocked = true;
                mHardware->doRouting_l();
                mLocked = false;
            }
        }

        // If there's no hardware speaker, leave the HW alone. (i.e. SCO/SPDIF is on)
        if (mIsSpkrEnabledReq) {
            status = mHardware->doStandby(mFdCtl, true, false); // output, online
        } else {
            status = mHardware->doStandby(mFdCtl, true, true); // output, standby
        }
        mIsSpkrEnabled = mIsSpkrEnabledReq;

        if ((mIsBtEnabled && !mIsBtEnabledReq) ||
            (mIsSpdifEnabled && !mIsSpdifEnabledReq)) {
            flush();
        }
        mIsBtEnabled = mIsBtEnabledReq;
        mIsSpdifEnabled = mIsSpdifEnabledReq;

        int bit_format = TEGRA_AUDIO_BIT_FORMAT_DEFAULT;
        bool is_bt_bypass = false;
        if (mIsBtEnabled) {
            bit_format = TEGRA_AUDIO_BIT_FORMAT_DSP;
            is_bt_bypass = true;
        }
        // Setup the I2S2-> DAP2/4 capture/playback path.
        ::ioctl(mBtFdIoCtl, TEGRA_AUDIO_SET_BIT_FORMAT, &bit_format);
        ::ioctl(mHardware->mCpcapCtlFd, CPCAP_AUDIO_SET_BLUETOOTH_BYPASS, is_bt_bypass);

        mState = AUDIO_STREAM_CONFIGURED;
    }

    return status;
}

status_t AudioHardware::AudioStreamOutTegra::standby()
{
    if (!mHardware) {
        return NO_INIT;
    }

    status_t status = NO_ERROR;
    Mutex::Autolock lock(mHardware->mLock);
    Mutex::Autolock lock2(mLock);

    if (mState != AUDIO_STREAM_IDLE) {
        LOGV("output %p going into standby", this);
        mState = AUDIO_STREAM_IDLE;

        // update EC state if necessary
        AudioStreamInTegra *input = mHardware->getActiveInput_l();
        if (mHardware->isInCall() &&
            input && input->source() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
            // setDriver() will not try to lock mLock when called by doRouting_l()
            mLocked = true;
            mHardware->doRouting_l();
            mLocked = false;
        }

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
    // Prevent EC/NS from writing to the file anymore.
        mHardware->mAudioPP.writeDownlinkEcns(-1,0,false,0,&mFdLock);
#endif
        if (mIsSpkrEnabled) {
            // doStandby() calls flush() which also handles the case where multiple devices
            // including bluetooth or SPDIF are selected
            status = mHardware->doStandby(mFdCtl, true, true); // output, standby
        } else if (mIsBtEnabled || mIsSpdifEnabled) {
            flush();
        }
    }

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
    return mState == AUDIO_STREAM_IDLE;;
}

status_t AudioHardware::AudioStreamOutTegra::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamOutTegra::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        if (device != 0) {
            mDevices = device;
            LOGV("set output routing %x", mDevices);
            status = mHardware->doRouting();
        }
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
    mHardware(0), mFd(-1), mFdCtl(-1), mState(AUDIO_STREAM_IDLE), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0),
    mIsMicEnabled(0), mIsBtEnabled(0),
    mSource(AUDIO_SOURCE_DEFAULT), mLocked(false), mTotalBuffersRead(0)
{
}

status_t AudioHardware::AudioStreamInTegra::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    Mutex::Autolock lock(mLock);
    status_t status = BAD_VALUE;
    mHardware = hw;
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

    mDevices = devices;
    mFormat = AUDIO_HW_IN_FORMAT;
    mChannels = *pChannels;
    mSampleRate = *pRate;
    mBufferSize = mHardware->getInputBufferSize(mSampleRate, AudioSystem::PCM_16_BIT,
                                                AudioSystem::popCount(mChannels));
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

// Called with mHardware->mLock held.
void AudioHardware::AudioStreamInTegra::setDriver(bool mic, bool bluetooth)
{
    // acquire mutex if not already locked by read().
    if (!mLocked) {
        mLock.lock();
    }
    LOGD("%s: Analog mic? %s. Bluetooth? %s.", __FUNCTION__,
            mic?"yes":"no", bluetooth?"yes":"no");

    // force some reconfiguration at next read()
    // Note: mState always == AUDIO_STREAM_CONFIGURED when setDriver() is called on an input
    if (mic != mIsMicEnabled || bluetooth != mIsBtEnabled) {
        mState = AUDIO_STREAM_CONFIG_REQ;
    }

    mIsMicEnabled = mic;
    mIsBtEnabled = bluetooth;

    if (!mLocked) {
        mLock.unlock();
    }
}

ssize_t AudioHardware::AudioStreamInTegra::read(void* buffer, ssize_t bytes)
{
    status_t status;
    if (!mHardware) {
        LOGE("%s: mHardware is null", __FUNCTION__);
        return NO_INIT;
    }
    // LOGV("AudioStreamInTegra::read(%p, %ld) TID %d", buffer, bytes, gettid());


    mHardware->mLock.lock();

    {   // scope for mLock
        Mutex::Autolock lock(mLock);

        ssize_t ret;
        int driverRate;
        bool srcReqd;
        int  hwReadBytes;
        int16_t * inbuf;

        status = online_l();
        if (status != NO_ERROR) {
           LOGE("%s: Problem switching to online.",__FUNCTION__);
           mHardware->mLock.unlock();
           goto error;
        }
        // Snapshot of the driver rate to stay coherent in this function
        driverRate = mHardware->mHwInRate;
        mHardware->mLock.unlock();

        srcReqd = (driverRate != (int)mSampleRate);

#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS
        if (srcReqd) {
            hwReadBytes = ( bytes*driverRate/mSampleRate ) & (~0x7);
            LOGV("Running capture SRC.  HW=%d bytes at %d, Flinger=%d bytes at %d",
                  hwReadBytes, driverRate, (int)bytes, mSampleRate);
            inbuf = mInScratch;
            if ((size_t)bytes > sizeof(mInScratch)) {
                LOGE("read: buf size problem. %d>%d",(int)bytes,sizeof(mInScratch));
                status = BAD_VALUE;
                goto error;
            }
            // Check if we need to init the rate converter
            if (!mSrc.initted() ||
                 mSrc.inRate() != driverRate ||
                 mSrc.outRate() != (int)mSampleRate) {
                LOGI ("%s: Upconvert started from %d to %d", __FUNCTION__,
                       driverRate, mSampleRate);
                mSrc.init(driverRate, mSampleRate);
                if (!mSrc.initted()) {
                    status = NO_INIT;
                    goto error;
                }
                reopenReconfigDriver();
            }
        } else {
            hwReadBytes = bytes;
            inbuf = (int16_t *)buffer;
            mSrc.deinit();
        }
        // Read from driver, or ECNS thread, as appropriate.
        ret = mHardware->mAudioPP.read(mFd, inbuf, hwReadBytes, driverRate);
        if (ret>0 && srcReqd) {
            mSrc.mIoData.in_buf_ch1 = (SRC16 *) (inbuf);
            mSrc.mIoData.in_buf_ch2 = 0;
            mSrc.mIoData.input_count = hwReadBytes / sizeof(SRC16);
            mSrc.mIoData.out_buf_ch1 = (SRC16 *) (buffer);
            mSrc.mIoData.out_buf_ch2 = 0;
            mSrc.mIoData.output_count = bytes/sizeof(SRC16);
            mSrc.srcConvert();
            ret = mSrc.mIoData.output_count*sizeof(SRC16);
            if (ret > bytes) {
                LOGE("read: buffer overrun");
            }
        }
#else
        if (srcReqd) {
            LOGE("%s: sample rate mismatch HAL %d, driver %d",
                 __FUNCTION__, mSampleRate, driverRate);
            status = INVALID_OPERATION;
            goto error;
        }
        ret = ::read(mFd, buffer, hwReadBytes);
#endif

        // It is not optimal to mute after all the above processing but it is necessary to
        // keep the clock sync from input device. It also avoids glitches on output streams due
        // to EC being turned on and off
        bool muted;
        mHardware->getMicMute(&muted);
        if (muted) {
            LOGV("%s muted",__FUNCTION__);
            memset(buffer, 0, bytes);
        }

        LOGV("%s returns %d.",__FUNCTION__, (int)ret);
        if (ret < 0) {
            status = ret;
            goto error;
        }

        mTotalBuffersRead++;
        return ret;
    }

error:
    LOGE("read(): error, return %d", status);
    standby();
    usleep(bytes * 1000 / frameSize() / sampleRate() * 1000);
    return status;
}

bool AudioHardware::AudioStreamInTegra::getStandby() const
{
    return mState == AUDIO_STREAM_IDLE;
}

status_t AudioHardware::AudioStreamInTegra::standby()
{
    if (!mHardware) {
        return NO_INIT;
    }

    Mutex::Autolock lock(mHardware->mLock);
    Mutex::Autolock lock2(mLock);
    status_t status = NO_ERROR;
    if (mState != AUDIO_STREAM_IDLE) {
        LOGV("input %p going into standby", this);
        mState = AUDIO_STREAM_IDLE;
        // setDriver() will not try to lock mLock when called by doRouting_l()
        mLocked = true;
        mHardware->doRouting_l();
        mLocked = false;
        status = mHardware->doStandby(mFdCtl, false, true); // input, standby
    }

    return status;
}

// Called with mLock and mHardware->mLock held
status_t AudioHardware::AudioStreamInTegra::online_l()
{
    status_t status = NO_ERROR;

    if (mState < AUDIO_STREAM_CONFIGURED) {

        reopenReconfigDriver();

        // configuration
        struct tegra_audio_in_config config;
        status = ::ioctl(mFdCtl, TEGRA_AUDIO_IN_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("cannot read input config: %s", strerror(errno));
            return status;
        }
        config.stereo = AudioSystem::popCount(mChannels) == 2;
        config.rate = mSampleRate;
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

        // Use standby to flush the driver.  mHardware->mLock should already be held

        mHardware->doStandby(mFdCtl, false, true);
        if (mDevices & ~AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            status = mHardware->doStandby(mFdCtl, false, false);
        }

        if (mState == AUDIO_STREAM_IDLE) {
            mState = AUDIO_STREAM_CONFIG_REQ;
            LOGV("input %p going online", this);
            // setDriver() will not try to lock mLock when called by doRouting_l()
            mLocked = true;
            mHardware->doRouting_l();
            mLocked = false;
            mTotalBuffersRead = 0;
            mStartTimeNs = systemTime();
        }

        mState = AUDIO_STREAM_CONFIGURED;
    }

    return status;
}

void AudioHardware::AudioStreamInTegra::reopenReconfigDriver()
{
    // Need to "restart" the driver when changing the buffer configuration.
    if (mFdCtl != -1 && ::ioctl(mFdCtl, TEGRA_AUDIO_IN_STOP) < 0)
        LOGE("%s: could not stop recording: %s\n", __FUNCTION__, strerror(errno));
    if (mFd != -1)
        ::close(mFd);
    if (mFdCtl != -1)
        ::close(mFdCtl);

    mFd = ::open("/dev/audio1_in", O_RDWR);
    mFdCtl = ::open("/dev/audio1_in_ctl", O_RDWR);
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
    int source;
    LOGV("AudioStreamInTegra::setParameters() %s", keyValuePairs.string());

    // read source before device so that it is upto date when doRouting() is called
    if (param.getInt(String8(AudioParameter::keyInputSource), source) == NO_ERROR) {
        mSource = source;
        param.remove(String8(AudioParameter::keyInputSource));
    }

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

unsigned int  AudioHardware::AudioStreamInTegra::getInputFramesLost() const
{
    Mutex::Autolock _l(mLock);
    unsigned int lostFrames = 0;
    if (!getStandby()) {
        unsigned int framesPerBuffer = bufferSize() / frameSize();
        uint64_t expectedFrames = ((systemTime() - mStartTimeNs) * mSampleRate) / 1000000000;
        expectedFrames = (expectedFrames / framesPerBuffer) * framesPerBuffer;
        uint64_t actualFrames = (uint64_t)mTotalBuffersRead * framesPerBuffer;
        if (expectedFrames > actualFrames) {
            lostFrames = (unsigned int)(expectedFrames - actualFrames);
            LOGW("getInputFramesLost() expected %d actual %d lost %d",
                 (unsigned int)expectedFrames, (unsigned int)actualFrames, lostFrames);
        }
    }

    mTotalBuffersRead = 0;
    mStartTimeNs = systemTime();

    return lostFrames;
}

// ----------------------------------------------------------------------------

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

}; // namespace android
