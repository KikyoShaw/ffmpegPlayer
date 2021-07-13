#include "ffmpegPlayer.h"
#include <QKeyEvent>
#include <QDebug>

ffmpegPlayer::ffmpegPlayer(QWidget *parent)
	: QWidget(parent)
{
	ui.setupUi(this);
	setAttribute(Qt::WA_TranslucentBackground);
	
	//活动页播放视频
	connect(&m_ffplayer, &FFPlayer::sig_GetOneFrame, ui.label, [this](QImage image) {
		auto temp = image.scaledToHeight(height(), Qt::SmoothTransformation);
		ui.label->setPixmap(QPixmap::fromImage(temp));
	});
	//回放进度条
	m_playerSlider.setParent(this);
	connect(&m_ffplayer, &FFPlayer::sigSetTotalTime, &m_playerSlider, &VPlayerSlider::sltSetTotalTime);
	connect(&m_ffplayer, &FFPlayer::sigSetCurrentTime, &m_playerSlider, &VPlayerSlider::sltSetCurrentTime, Qt::QueuedConnection);
	//connect(&m_ffplayer, SIGNAL(sigFinishPlay()), &m_playerSlider, SLOT(sltFinishPlay()));
	connect(&m_playerSlider, &VPlayerSlider::sigPlay, &m_ffplayer, &FFPlayer::sltPlay);
	connect(&m_playerSlider, &VPlayerSlider::sigSeek, &m_ffplayer, &FFPlayer::sltSeek);
}

ffmpegPlayer::~ffmpegPlayer()
{
}

void ffmpegPlayer::playStream(const QString & url)
{
	if (url.isEmpty()) {
		qInfo() << QStringLiteral("视频地址空");
		return;
	}
	m_ffplayer.stopPlay();
	m_ffplayer.startPlay(url);
	if (isMinimized()) {
		this->showNormal();
	}
	else {
		this->show();
	}
	this->raise();
}

void ffmpegPlayer::stopStream(const QString & url)
{
	m_ffplayer.stopPlay();
}

void ffmpegPlayer::resizeEvent(QResizeEvent * event)
{
	__super::resizeEvent(event);
	int y = this->height() - m_playerSlider.height() - 10;
	m_playerSlider.move(QPoint(0, y));
	m_playerSlider.setFixedWidth(this->width());
}

void ffmpegPlayer::closeEvent(QCloseEvent * event)
{
	m_ffplayer.stopPlay();
	return __super::closeEvent(event);
}

void ffmpegPlayer::mouseDoubleClickEvent(QMouseEvent * event)
{
	if (isFullScreen()) {
		showNormal();
	}
	else {
		showFullScreen();
	}
}

void ffmpegPlayer::keyReleaseEvent(QKeyEvent * event)
{
	auto key = event->key();
	switch (key)
	{
	case Qt::Key_Escape: {
		if (isFullScreen()) {
			showNormal();
		}
	}
	default:
		break;
	}
}
