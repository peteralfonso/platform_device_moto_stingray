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

#ifndef ANDROID_AUDIO_POST_PROCESSOR_H
#define ANDROID_AUDIO_POST_PROCESSOR_H
#ifdef USE_PROPRIETARY_AUDIO_EXTENSIONS

extern "C" {
#include "cto_audio_mm.h"
}


namespace android {

class AudioPostProcessor
{
public:
                        AudioPostProcessor();
                        ~AudioPostProcessor();
            void        setPlayAudioRate(int rate);
            void        setAudioDev(class AudioHardware* hw,
                                    struct cpcap_audio_stream *outDev,
                                    struct cpcap_audio_stream *inDev);
            void        doMmProcessing(void * buffer, int numSamples);

private:
            AudioHardware* mHardware;

            void        configMmAudio(void);
            uint32_t    convOutDevToCTO(uint32_t outDev);
            uint32_t    convRateToCto(uint32_t rate);

        // CTO Multimedia Audio Processing storage buffers
            int16_t     mPcmLoggingBuf[((CTO_AUDIO_MM_DATALOGGING_BUFFER_BLOCK_BYTESIZE)/2)];
            uint32_t    mNoiseEst[((CTO_AUDIO_MM_NOISE_EST_BLOCK_BYTESIZE)/4)];
            uint16_t    mRuntimeParam[((CTO_AUDIO_MM_RUNTIME_PARAM_BYTESIZE)/2)];
            uint16_t    mStaticMem[((CTO_AUDIO_MM_STATICMEM_BLOCK_BYTESIZE)/2)];
            uint16_t    mScratchMem[((CTO_AUDIO_MM_SCRATCHMEM_BLOCK_BYTESIZE)/2)];
            CTO_AUDIO_MM_ENV_VAR mAudioMmEnvVar;
            Mutex       mMmLock;
};

} // namespace android

#endif // USE_PROPRIETARY_AUDIO_EXTENSIONS
#endif // ANDROID_AUDIO_POST_PROCESSOR_H
