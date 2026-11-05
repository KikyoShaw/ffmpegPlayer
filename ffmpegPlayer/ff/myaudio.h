#ifndef MYAUDIO_H
#define MYAUDIO_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <memory>

class MyAudio : public QObject
{
    Q_OBJECT

public:
    explicit MyAudio(QObject *parent = nullptr);
    ~MyAudio() override;

    // Thread-safe facade. Audio objects belong exclusively to the owned thread.
    bool open(int sampleRate, int channels, QString *error = nullptr);
    void close();
    void clear();
    void setPaused(bool paused);
    void setVolume(int percent);
    void setMuted(bool muted);

    // Accepts complete interleaved S16 frames without waiting for device space.
    int enqueue(const QByteArray &pcm);
    qint64 playedUSecs() const;
    bool isDrained() const;
    QString errorString() const;

private:
    struct Private;
    std::unique_ptr<Private> m_private;
};

#endif
