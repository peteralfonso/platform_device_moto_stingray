/*
 * Copyright (C) 2010 Motorola, Inc.
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>

#include <linux/l3g4200d.h>

#include <cutils/log.h>

#include "GyroSensor.h"

/*****************************************************************************/

GyroSensor::GyroSensor()
    : SensorBase(GYROSCOPE_DEVICE_NAME, "gyroscope"),
      mEnabled(0),
      mInputReader(32)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_G;
    mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
    mPendingEvent.gyro.status = SENSOR_STATUS_ACCURACY_HIGH;
    memset(mPendingEvent.data, 0x00, sizeof(mPendingEvent.data));

    // read the actual value of all sensors if they're enabled already
    struct input_absinfo absinfo;
    int flags = 0;
    if (!ioctl(dev_fd, L3G4200D_IOCTL_GET_ENABLE, &flags)) {
	LOGE("%s: GyroSensor GET Enable Flag =%d", __PRETTY_FUNCTION__, flags);
        if (flags)  {
            mEnabled = 1;
            if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_P), &absinfo)) {
                mPendingEvent.gyro.x = absinfo.value * CONVERT_G_P;
            }
            if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_R), &absinfo)) {
                mPendingEvent.gyro.y = absinfo.value * CONVERT_G_R;
            }
            if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_Y), &absinfo)) {
                mPendingEvent.gyro.z = absinfo.value * CONVERT_G_Y;
            }
        }
    }
}

GyroSensor::~GyroSensor() {
}

int GyroSensor::enable(int32_t, int en)
{
    int flags = en ? 1 : 0;
    int err = 0;
    if (flags != mEnabled) {
	LOGE("%s: GyroSensor SET Enable Flag =%d", __PRETTY_FUNCTION__, flags);
        err = ioctl(dev_fd, L3G4200D_IOCTL_SET_ENABLE, &flags);
        err = err<0 ? -errno : 0;
        LOGE_IF(err, "L3G4200D_IOCTL_SET_ENABLE failed (%s)", strerror(-err));
        if (!err) {
            mEnabled = flags;
        }
    }
    return err;
}

int GyroSensor::setDelay(int64_t ns)
{
    if (ns < 0)
        return -EINVAL;

    short delay = ns / 1000000;
    if (!ioctl(dev_fd, L3G4200D_IOCTL_SET_DELAY, &delay)) {
        return -errno;
    }
    return 0;
}

int GyroSensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;
    int numEventReceived = 0;
    input_event const* event;

    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_ABS) {
            processEvent(event->code, event->value);
        } else if (type == EV_SYN) {
            int64_t time = timevalToNano(event->time);
            mPendingEvent.timestamp = time;
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            LOGE("GyroSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

    return numEventReceived;
}

void GyroSensor::processEvent(int code, int value)
{
    switch (code) {
        case EVENT_TYPE_GYRO_P:
            mPendingEvent.gyro.x = value * CONVERT_G_P;
            break;
        case EVENT_TYPE_GYRO_R:
            mPendingEvent.gyro.y = value * CONVERT_G_R;
            break;
        case EVENT_TYPE_GYRO_Y:
            mPendingEvent.gyro.z = value * CONVERT_G_Y;
            break;
    }
}
