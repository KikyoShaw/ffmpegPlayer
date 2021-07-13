#pragma once

#include <QWidget>
#include "ui_ffmpegPlayer.h"
#include "vplayerslider.h"
#include "ffplayer.h"

class ffmpegPlayer : public QWidget
{
	Q_OBJECT

public:
	ffmpegPlayer(QWidget *parent = Q_NULLPTR);
	~ffmpegPlayer();

	void playStream(const QString &url);
	void stopStream(const QString &url);

protected:
	virtual void resizeEvent(QResizeEvent *event);
	virtual void closeEvent(QCloseEvent *event);
	virtual void mouseDoubleClickEvent(QMouseEvent *event);
	virtual void keyReleaseEvent(QKeyEvent *event);

private:
	void sltPlayVideo(const QString &url);

private:
	Ui::player ui;
	//�طŹ���
	FFPlayer m_ffplayer;
	//�طŽ�����
	VPlayerSlider m_playerSlider;
};
