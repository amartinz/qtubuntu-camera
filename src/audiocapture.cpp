/*
 * Copyright (C) 2014 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Jim Hodapp <jim.hodapp@canonical.com>
 */

#include "audiocapture.h"

#include <pulse/simple.h>
#include <pulse/error.h>

#include <errno.h>
#include <fcntl.h>

#include <QDebug>
#include <QThread>

AudioCapture::AudioCapture(MediaRecorderWrapper *mediaRecorder)
    : m_paStream(NULL),
      m_audioPipe(-1),
      m_flagExit(false),
      m_mediaRecorder(mediaRecorder)
{
}

AudioCapture::~AudioCapture()
{
    if (m_audioPipe >= 0)
        close(m_audioPipe);
    if (m_paStream != NULL)
        pa_simple_free(m_paStream);
}

/*!
 * \brief Initializes AudioCapture so that it's ready to read microphone data from Pulseaudio
 */
bool AudioCapture::init(StartWorkerThreadCb cb, void *context)
{
    // The MediaRecorderLayer will call method (cb) when it's ready to encode a new audio buffer
    android_recorder_set_audio_read_cb(m_mediaRecorder, cb, context);

    return true;
}

/*!
 * \brief Stops the microphone data capture thread loop
 */
void AudioCapture::stopCapture()
{
    qDebug() << __PRETTY_FUNCTION__;
    m_flagExit = true;
}

/*!
 * \brief The main microphone reader/writer loop. Reads from Pulseaudio, writes to named pipe
 */
void AudioCapture::run()
{
    qDebug() << __PRETTY_FUNCTION__;

    int bytesWritten = 0, bytesRead = 0;
    const size_t readSize = sizeof(m_audioBuf);

    if (!setupMicrophoneStream())
    {
        qWarning() << "Failed to setup PulseAudio microphone recording stream";
        return;
    }

    if (!setupPipe())
    {
        qWarning() << "Failed to open /dev/socket/micshm, cannot write data to pipe";
        return;
    }

    do {
        bytesRead = readMicrophone();
        if (bytesRead > 0)
        {
            bytesWritten = writeDataToPipe();
        }
    } while (bytesRead == readSize
                && bytesWritten == readSize
                && !m_flagExit);

    Q_EMIT finished();
}

/*!
 * \brief Reads microphone data from Pulseaudio
 */
int AudioCapture::readMicrophone()
{
    int ret = 0, error = 0;
    const size_t readSize = sizeof(m_audioBuf);
    ret = pa_simple_read(m_paStream, m_audioBuf, readSize, &error);
    if (ret < 0)
        qWarning() << "Failed to read audio from the microphone: " << pa_strerror(error);
    else
        ret = readSize;

    return ret;
}

/*!
 * \brief Signals AalMediaRecorderControl to start the main thread loop.
 * \detail This is necessary due to thread contexts. Starting of the main thread loop
 * for AudioCapture must be done in the main thread context and not in the AudioCapture
 * thread context, otherwise the loop start signal will never be seen.
 */
void AudioCapture::startThreadLoop()
{
    Q_EMIT startThread();
    qDebug() << "Emitted startThread(), should start reading from mic";
}

/*!
 * \brief Sets up the Pulseaudio microphone input channel
 */
bool AudioCapture::setupMicrophoneStream()
{
    // FIXME: Get these parameters more dynamically from the control
    static const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = 48000,
        .channels = 1
    };
    int error = 0;

    m_paStream = pa_simple_new(NULL, "qtubuntu-camera", PA_STREAM_RECORD, NULL, "record", &ss, NULL, NULL, &error);
    if (m_paStream == NULL)
    {
        qWarning() << "Failed to open a PulseAudio channel to read the microphone: " << pa_strerror(error);
        return false;
    }

    return true;
}

/*!
 * \brief Opens the named pipe /dev/socket/micshm for writing mic data to the Android (reader) side
 */
bool AudioCapture::setupPipe()
{
    if (m_audioPipe >= 0)
    {
        qWarning() << "/dev/socket/micshm already opened, not opening twice";
        return true;
    }

    // Open the named pipe for writing only
    m_audioPipe = open("/dev/socket/micshm", O_WRONLY);
    if (m_audioPipe < 0)
    {
        qWarning() << "Failed to open audio data pipe /dev/socket/micshm: " << strerror(errno);
        return false;
    }

    return true;
}

/*!
 * \brief Writes mic data to the named pipe /dev/socket/micshm
 */
int AudioCapture::writeDataToPipe()
{
    // Don't open the named pipe twice
    if (m_audioPipe < 0 && !setupPipe())
    {
        qWarning() << "Failed to open /dev/socket/micshm, cannot write data to pipe";
        return 0;
    }

    int num = 0;
    const size_t writeSize = sizeof(m_audioBuf);
    num = loopWrite(m_audioPipe, m_audioBuf, writeSize);
    if (num != writeSize)
        qWarning() << "Failed to write " << num << " bytes to /dev/socket/micshm: " << strerror(errno) << " (" << errno << ")";

    return num;
}

ssize_t AudioCapture::loopWrite(int fd, const void *data, size_t size)
{
    ssize_t ret = 0;
    while (size > 0)
    {
        ssize_t r;
        if ((r = write(fd, data, size)) < 0)
            return r;
        if (r == 0)
            break;
        ret += r;
        data = (const int16_t*) data + r;
        size -= (size_t) r;
    }
    return ret;
}