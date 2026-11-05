#ifndef FFPLAYER_H
#define FFPLAYER_H

#include "myaudio.h"
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <functional>

class FFPlayer : public QThread
{
    Q_OBJECT

public:
    enum PlaybackState { Stopped, Opening, Playing, Paused, Stopping, Ended, Error };
    Q_ENUM(PlaybackState)

    explicit FFPlayer();
    ~FFPlayer() override;

    void startPlay(const QString& source);
    void stopPlay();
    bool isPlay() const;
    bool isSuspend() const;
    void setRate(double rate);
    void setVolume(int percent);
    void setMuted(bool muted);
    qint64 position() const;
    qint64 duration() const;
    double rate() const;
    bool canSeek() const;
    bool canRate() const;

public slots:
    void sltPlay(bool isPlay);
    void sltSeek(int pos, int base);
    void seekTo(qint64 positionMs);

signals:
    void sig_GetOneFrame(QImage image);
    void sigSetTotalTime(qint64 durationMs);
    void sigSetCurrentTime(qint64 positionMs);
    void sigStateChanged(int state);
    void sigCapabilitiesChanged(bool canSeek, bool canRate);
    void sigRateChanged(double rate);
    void sigError(QString error);

protected:
    void run() override;

private:
    struct Requests
    {
        bool paused = false;
        double rate = 1.0;
        qint64 seekMs = 0;
        quint64 seekRevision = 0;
        quint64 rateRevision = 0;
        quint64 generation = 0;
        quint64 serial = 0;
    };

    Requests requests() const;
    void launchPending();
    void threadFinished();
    void waitForRequest(unsigned long milliseconds);
    void post(quint64 session, quint64 generation, const std::function<void()>& action);
    void postFrame(quint64 session, quint64 generation, const QImage& image);
    void postState(quint64 session, quint64 generation, PlaybackState state);
    void postPosition(quint64 session, quint64 generation, qint64 positionMs);
    void postError(quint64 session, quint64 generation, const QString& error);

    mutable QMutex m_controlMutex;
    QWaitCondition m_controlChanged;
    QString m_source;
    QString m_runSource;
    Requests m_requests;
    quint64 m_session = 0;
    quint64 m_runSession = 0;
    bool m_workerActive = false;
    bool m_pendingStart = false;
    bool m_stopPending = false;
    bool m_acceptingRequests = false;
    bool m_liveDisconnected = false;
    bool m_destroying = false;
    QImage m_pendingImage;
    quint64 m_imageSession = 0;
    quint64 m_imageGeneration = 0;
    bool m_imageQueued = false;
    std::atomic<bool> m_cancelled{false};
    std::atomic<quint64> m_controlSerial{0};
    std::atomic<int> m_state{Stopped};
    std::atomic<qint64> m_positionMs{0};
    std::atomic<qint64> m_durationMs{-1};
    std::atomic<double> m_actualRate{1.0};
    std::atomic<bool> m_seekable{false};
    std::atomic<bool> m_rateable{false};
    MyAudio m_audio;
};

#endif // FFPLAYER_H
