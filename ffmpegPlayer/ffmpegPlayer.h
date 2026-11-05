#pragma once

#include <QImage>
#include <QWidget>
#include "ui_ffmpegPlayer.h"
#include "vplayerslider.h"
#include "ffplayer.h"

class ffmpegPlayer : public QWidget
{
    Q_OBJECT

public:
    explicit ffmpegPlayer(QWidget *parent = Q_NULLPTR);
    ~ffmpegPlayer();

    void playStream(const QString &source);
    void stopStream();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void openFile();
    void playSourceFromEditor();
    void setPlaybackState(int state);
    void updateStatus();
    void toggleFullScreen();
    void seekBy(qint64 offsetMs);

    Ui::player ui;
    FFPlayer m_ffplayer;
    VPlayerSlider m_playerSlider;
    QImage m_frame;
    QString m_lastSource;
    QString m_error;
    FFPlayer::PlaybackState m_state = FFPlayer::Stopped;
    qint64 m_position = 0;
    qint64 m_duration = -1;
    bool m_canSeek = false;
    bool m_canRate = false;
    bool m_capabilitiesKnown = false;
    bool m_wasMaximized = false;
};
