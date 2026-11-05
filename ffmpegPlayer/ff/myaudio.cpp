#include "myaudio.h"

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QIODevice>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <cstring>
#include <limits>
#include <utility>

namespace {

struct AudioState
{
    mutable QMutex mutex;
    QByteArray buffer;
    int readOffset = 0;
    int queuedBytes = 0;
    int frameBytes = 0;
    bool open = false;
    bool drained = true;
    qint64 playedUSecs = 0;
    QString error;
};

// No Q_OBJECT is needed: Qt 5.12 supports functor-based invokeMethod.
class AudioWorker final : public QObject
{
public:
    explicit AudioWorker(AudioState &state) : m_state(state) {}

    bool open(int sampleRate, int channels)
    {
        close();

        const qint64 frameBytes = qint64(channels) * 2;
        const qint64 softwareBytes = qint64(qMax(1, sampleRate / 5)) * frameBytes;
        const qint64 deviceBytes = qint64(qMax(1, sampleRate / 10)) * frameBytes;
        const qint64 maxBytes = (std::numeric_limits<int>::max)();
        if (sampleRate <= 0 || channels <= 0 || frameBytes > maxBytes
            || softwareBytes > maxBytes || deviceBytes > maxBytes) {
            return fail(QStringLiteral("Invalid audio sample rate or channel count."));
        }

        const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
        if (device.isNull())
            return fail(QStringLiteral("No audio output device is available."));

        QAudioFormat format;
        format.setCodec(QStringLiteral("audio/pcm"));
        format.setSampleRate(sampleRate);
        format.setChannelCount(channels);
        format.setSampleSize(16);
        format.setSampleType(QAudioFormat::SignedInt);
        format.setByteOrder(QAudioFormat::LittleEndian);
        if (!device.isFormatSupported(format)) {
            return fail(QStringLiteral("Audio device \"%1\" does not support signed "
                                       "16-bit little-endian PCM at %2 Hz with %3 channels.")
                        .arg(device.deviceName()).arg(sampleRate).arg(channels));
        }

        m_sampleRate = sampleRate;
        m_frameBytes = int(frameBytes);
        // The scratch bytes duplicate the ring's head; they never add queue capacity.
        m_writeBuffer.resize(qMax(m_frameBytes, qMin(int(softwareBytes), 16 * 1024)));
        {
            QMutexLocker lock(&m_state.mutex);
            m_state.buffer.resize(int(softwareBytes));
            m_state.frameBytes = m_frameBytes;
        }

        ensureTimer();
        m_output = new QAudioOutput(device, format, this);
        m_output->setBufferSize(int(deviceBytes));
        m_output->setVolume(effectiveVolume());
        const quint64 serial = ++m_deviceSerial;
        QObject::connect(m_output, &QAudioOutput::stateChanged, this,
                         [this, serial](QAudio::State) {
            // Signals from a deleted output must not affect a later device.
            if (serial == m_deviceSerial && m_output)
                pump();
        }, Qt::QueuedConnection);

        if (!startDevice())
            return false;
        activate();
        return true;
    }

    void close()
    {
        if (m_timer)
            m_timer->stop();
        releaseDevice();
        resetCounters();
        m_sampleRate = 0;
        m_frameBytes = 0;
        m_writeBuffer.clear();
        QMutexLocker lock(&m_state.mutex);
        m_state.open = false;
        m_state.buffer.clear();
        m_state.readOffset = 0;
        m_state.queuedBytes = 0;
        m_state.frameBytes = 0;
        m_state.playedUSecs = 0;
        m_state.drained = true;
        m_state.error.clear();
    }

    void clear()
    {
        if (m_timer)
            m_timer->stop();
        {
            QMutexLocker lock(&m_state.mutex);
            // Reject producers while the hardware reset is in progress.
            m_state.open = false;
            m_state.readOffset = 0;
            m_state.queuedBytes = 0;
            m_state.playedUSecs = 0;
            m_state.drained = true;
        }
        resetCounters();
        if (!m_output)
            return;

        m_io = nullptr;
        m_output->reset();
        if (startDevice())
            activate();
    }

    void setPaused(bool paused)
    {
        if (m_paused == paused)
            return;
        if (!m_output) {
            m_paused = paused;
            return;
        }

        if (paused) {
            if (checkDevice()) {
                updateClock();
                m_output->suspend();
                // Include consumption up to suspend, then freeze the published clock.
                updateClock();
                checkError();
            }
            m_paused = true;
            if (m_timer)
                m_timer->stop();
            publish();
        } else {
            // Discard any backend counter movement during suspension.
            m_lastDeviceFrames = deviceFrames();
            m_paused = false;
            m_output->resume();
            if (!checkError())
                return;
            m_timer->start();
            pump();
        }
    }

    void setVolume(int percent)
    {
        m_volume = qBound(0, percent, 100);
        if (m_output)
            m_output->setVolume(effectiveVolume());
    }

    void setMuted(bool muted)
    {
        m_muted = muted;
        if (m_output)
            m_output->setVolume(effectiveVolume());
    }

private:
    void ensureTimer()
    {
        if (m_timer)
            return;
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(5);
        QObject::connect(m_timer, &QTimer::timeout, this, [this] { pump(); });
    }

    qreal effectiveVolume() const
    {
        return m_muted ? 0.0 : qreal(m_volume) / 100.0;
    }

    void resetCounters()
    {
        m_submittedBytes = 0;
        m_consumedFrames = 0;
        m_lastDeviceFrames = 0;
    }

    qint64 deviceFrames() const
    {
        if (!m_output || m_sampleRate <= 0)
            return 0;
        const qint64 usecs = qMax(qint64(0), m_output->processedUSecs());
        // Qt truncates frame durations to microseconds. Round back up to avoid
        // leaving the final consumed sample permanently pending at EOF.
        return (usecs / 1000000) * m_sampleRate
            + ((usecs % 1000000) * m_sampleRate + 999999) / 1000000;
    }

    qint64 clockUSecs() const
    {
        if (m_sampleRate <= 0)
            return 0;
        return (m_consumedFrames / m_sampleRate) * 1000000
            + (m_consumedFrames % m_sampleRate) * 1000000 / m_sampleRate;
    }

    void updateClock()
    {
        if (!m_output)
            return;
        const qint64 current = deviceFrames();
        if (!m_paused && current >= m_lastDeviceFrames) {
            const qint64 delta = current - m_lastDeviceFrames;
            // Counter resets and underrun/idle time must never consume future PCM.
            m_consumedFrames = qMin(m_submittedBytes / m_frameBytes,
                                    m_consumedFrames + delta);
        }
        m_lastDeviceFrames = current;
    }

    void publish()
    {
        QMutexLocker lock(&m_state.mutex);
        m_state.playedUSecs = clockUSecs();
        m_state.drained = m_state.queuedBytes == 0
            && (!m_output || m_submittedBytes == m_consumedFrames * m_frameBytes);
    }

    void releaseDevice()
    {
        ++m_deviceSerial;
        m_io = nullptr;
        if (m_output) {
            m_output->stop();
            delete m_output;
            m_output = nullptr;
        }
    }

    bool fail(const QString &message)
    {
        if (m_timer)
            m_timer->stop();
        releaseDevice();
        QMutexLocker lock(&m_state.mutex);
        m_state.open = false;
        m_state.readOffset = 0;
        m_state.queuedBytes = 0;
        m_state.buffer.clear();
        m_state.playedUSecs = clockUSecs();
        m_state.drained = true;
        m_state.error = message;
        return false;
    }

    bool checkError()
    {
        if (!m_output)
            return false;
        switch (m_output->error()) {
        case QAudio::NoError:
        case QAudio::UnderrunError:
            return true;
        case QAudio::OpenError:
            return fail(QStringLiteral("The audio output device could not be opened."));
        case QAudio::IOError:
            return fail(QStringLiteral("An audio output device I/O error occurred."));
        case QAudio::FatalError:
            return fail(QStringLiteral("The audio output device reported a fatal error."));
        default:
            return fail(QStringLiteral("The audio output device reported error %1.")
                        .arg(int(m_output->error())));
        }
    }

    bool startDevice()
    {
        m_output->setVolume(effectiveVolume());
        m_io = m_output->start();
        if (!checkError())
            return false;
        if (!m_io || m_output->state() == QAudio::StoppedState)
            return fail(QStringLiteral("The audio output device did not start."));
        m_lastDeviceFrames = deviceFrames();
        if (m_paused) {
            m_output->suspend();
            if (!checkError())
                return false;
        }
        return true;
    }

    void activate()
    {
        {
            QMutexLocker lock(&m_state.mutex);
            m_state.open = true;
            m_state.error.clear();
        }
        if (!m_paused)
            m_timer->start();
        publish();
    }

    bool checkDevice()
    {
        if (!checkError())
            return false;
        if (m_output->state() != QAudio::StoppedState)
            return true;
        if (m_output->error() != QAudio::UnderrunError)
            return fail(QStringLiteral("The audio output device stopped unexpectedly."));

        // Some backends stop on underrun instead of entering IdleState. Only the
        // exhausted device stream restarts; software PCM and the clock survive.
        m_consumedFrames = m_submittedBytes / m_frameBytes;
        return startDevice();
    }

    void pump()
    {
        if (!m_output || !checkDevice())
            return;
        if (m_paused) {
            publish();
            return;
        }
        updateClock();

        // Bound work per event so control requests are not starved by short writes.
        for (int attempt = 0; attempt < 32; ++attempt) {
            const int freeBytes = m_output->bytesFree();
            if (freeBytes <= 0)
                break;

            int count = 0;
            {
                QMutexLocker lock(&m_state.mutex);
                count = qMin(freeBytes, qMin(m_state.queuedBytes, m_writeBuffer.size()));
                // If a backend accepted a partial frame, the next write completes
                // that frame before ending at a new sample boundary.
                count -= int((m_submittedBytes % m_frameBytes + count) % m_frameBytes);
                if (count <= 0)
                    break;
                const int first = qMin(count, m_state.buffer.size() - m_state.readOffset);
                std::memcpy(m_writeBuffer.data(),
                            m_state.buffer.constData() + m_state.readOffset, size_t(first));
                if (count > first) {
                    std::memcpy(m_writeBuffer.data() + first,
                                m_state.buffer.constData(), size_t(count - first));
                }
            }

            // No shared lock is held during any QAudioOutput/QIODevice operation.
            const qint64 written = m_io->write(m_writeBuffer.constData(), count);
            if (written < 0) {
                if (m_output->error() == QAudio::UnderrunError)
                    break;
                const QString detail = m_io->errorString();
                fail(QStringLiteral("Writing PCM to the audio output failed: %1").arg(detail));
                return;
            }
            if (written > count) {
                fail(QStringLiteral("The audio output reported an invalid write length."));
                return;
            }
            if (written > 0) {
                {
                    QMutexLocker lock(&m_state.mutex);
                    m_state.readOffset = (m_state.readOffset + int(written)) % m_state.buffer.size();
                    m_state.queuedBytes -= int(written);
                }
                m_submittedBytes += written;
            }
            if (!checkError())
                return;
            if (written == 0)
                break;
        }
        updateClock();
        publish();
    }

    AudioState &m_state;
    QAudioOutput *m_output = nullptr;
    QIODevice *m_io = nullptr;
    QTimer *m_timer = nullptr;
    QByteArray m_writeBuffer;
    int m_sampleRate = 0;
    int m_frameBytes = 0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_paused = false;
    quint64 m_deviceSerial = 0;
    qint64 m_submittedBytes = 0;
    qint64 m_consumedFrames = 0;
    qint64 m_lastDeviceFrames = 0;
};

} // namespace

struct MyAudio::Private
{
    Private() : worker(new AudioWorker(state))
    {
        thread.setObjectName(QStringLiteral("MyAudioOutput"));
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }

    ~Private()
    {
        invoke([this] { worker->close(); });
        thread.quit();
        thread.wait();
        // QThread::finished processes the worker's deferred deletion in its thread,
        // including its child timer; no audio QObject is deleted by the caller.
    }

    template <typename Function>
    bool invoke(Function function)
    {
        if (QThread::currentThread() == &thread) {
            function();
            return true;
        }
        return QMetaObject::invokeMethod(worker, std::move(function), Qt::BlockingQueuedConnection);
    }

    AudioState state;
    QThread thread;
    AudioWorker *worker;
};

MyAudio::MyAudio(QObject *parent)
    : QObject(parent), m_private(new Private)
{
}

MyAudio::~MyAudio() = default;

bool MyAudio::open(int sampleRate, int channels, QString *error)
{
    bool opened = false;
    QString message;
    const bool invoked = m_private->invoke([&] {
        opened = m_private->worker->open(sampleRate, channels);
        QMutexLocker lock(&m_private->state.mutex);
        message = m_private->state.error;
    });
    if (!invoked) {
        message = QStringLiteral("The audio worker could not be invoked.");
        QMutexLocker lock(&m_private->state.mutex);
        m_private->state.error = message;
    }
    if (error)
        *error = message;
    return invoked && opened;
}

void MyAudio::close()
{
    m_private->invoke([this] { m_private->worker->close(); });
}

void MyAudio::clear()
{
    m_private->invoke([this] { m_private->worker->clear(); });
}

void MyAudio::setPaused(bool paused)
{
    m_private->invoke([this, paused] { m_private->worker->setPaused(paused); });
}

void MyAudio::setVolume(int percent)
{
    m_private->invoke([this, percent] { m_private->worker->setVolume(percent); });
}

void MyAudio::setMuted(bool muted)
{
    m_private->invoke([this, muted] { m_private->worker->setMuted(muted); });
}

int MyAudio::enqueue(const QByteArray &pcm)
{
    AudioState &state = m_private->state;
    QMutexLocker lock(&state.mutex);
    if (!state.open || pcm.isEmpty() || state.frameBytes <= 0)
        return 0;
    int accepted = qMin(pcm.size(), state.buffer.size() - state.queuedBytes);
    accepted -= accepted % state.frameBytes;
    if (accepted <= 0)
        return 0;

    const int writeOffset = (state.readOffset + state.queuedBytes) % state.buffer.size();
    const int first = qMin(accepted, state.buffer.size() - writeOffset);
    std::memcpy(state.buffer.data() + writeOffset, pcm.constData(), size_t(first));
    if (accepted > first)
        std::memcpy(state.buffer.data(), pcm.constData() + first, size_t(accepted - first));
    state.queuedBytes += accepted;
    state.drained = false;
    return accepted;
}

qint64 MyAudio::playedUSecs() const
{
    QMutexLocker lock(&m_private->state.mutex);
    return m_private->state.playedUSecs;
}

bool MyAudio::isDrained() const
{
    QMutexLocker lock(&m_private->state.mutex);
    return m_private->state.drained;
}

QString MyAudio::errorString() const
{
    QMutexLocker lock(&m_private->state.mutex);
    return m_private->state.error;
}
