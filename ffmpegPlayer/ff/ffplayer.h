#ifndef FFPLAYER_H
#define FFPLAYER_H

#include "myaudio.h"
#include <QThread>
#include <QImage>
#include <QMutex>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVFrame;

class FFPlayer : public QThread
{
	Q_OBJECT

public:
	explicit FFPlayer();
	~FFPlayer();

	void startPlay(QString strUrl);
	void stopPlay();
	bool isPlay();
	void play(bool isPlay);
	bool isSuspend();
	void seek(int pos, int base, bool bAuto);

signals:
	void sig_GetOneFrame(QImage);
	void sigSetTotalTime(qint64 iTime);
	void sigSetCurrentTime(qint64 iTime);
	//void sigFinishPlay();

public slots:
	void sltPlay(bool isPlay);
	void sltSeek(int pos, int base);

protected:
	virtual void run();

	int convertPCM(AVCodecContext *pCodecCtxAudio,
		SwrContext *audio_convert_ctx,
		AVFrame *pFramePcm,
		char *bufferPCM);

	void _seek(int pos, int base);

private:
	QString m_strUrl;
	volatile bool m_bStop = true;
	volatile bool m_bSuspend = false;
	MyAudio m_myAudio;
	int64_t m_totalMs = 0; // 当前视频的总时间
	int64_t m_pts = 0; // 当前视频播放进度
	AVFormatContext *m_pFormatCtx = NULL;
	int m_videoStream = -1;
	int m_audioStream = -1;
	QMutex m_mutexPlay;
public:
	bool m_bReadFrame = false;
	int m_timeoutReadFrame = 0;
};

#endif // FFPLAYER_H
