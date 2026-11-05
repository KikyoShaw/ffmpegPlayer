#ifndef VPLAYERSLIDER_H
#define VPLAYERSLIDER_H

#include <QWidget>
#include "ui_vplayerslider.h"
#include "ffplayer.h"

class VPlayerSlider : public QWidget
{
    Q_OBJECT

public:
    explicit VPlayerSlider(QWidget *parent = Q_NULLPTR);
    ~VPlayerSlider();

    void clear();
    void setSourceAvailable(bool available);
    void togglePlayback();
    void changeVolume(int delta);
    void toggleMuted();
    int volume() const;
    bool isMuted() const;
    bool isDragging() const;

signals:
    void sigPlay(bool isPlay);
    void sigStop();
    void sigSeek(int pos, int base);
    void sigRateChanged(double rate);
    void sigVolumeChanged(int volume);
    void sigMutedChanged(bool muted);

public slots:
    void sltSetTotalTime(qint64 timeMs);
    void sltSetCurrentTime(qint64 timeMs);
    void sltSetPlaybackState(int state);
    void sltSetCapabilities(bool canSeek, bool canRate);
    void sltSetRate(double rate);

private slots:
    void sltSliderMoved(int position);
    void sltSliderPressed();
    void sltSliderReleased();

private:
    static QString convertTime(qint64 timeMs);
    void updateControls();
    void updatePositionDisplay();

    Ui::VPlayerSlider ui;
    FFPlayer::PlaybackState m_state = FFPlayer::Stopped;
    qint64 m_totalTime = -1;
    qint64 m_currentTime = -1;
    int m_posPress = 0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_dragging = false;
    bool m_sourceAvailable = false;
    bool m_canSeek = false;
    bool m_canRate = false;
};

#endif // VPLAYERSLIDER_H
