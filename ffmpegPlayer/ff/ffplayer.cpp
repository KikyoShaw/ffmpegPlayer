#include "ffplayer.h"
extern "C"
{
	//#include "libavcodec/avcodec.h"
	#include "libavformat/avformat.h"
	//#include "libavutil/pixfmt.h"
	#include "libswscale/swscale.h"
	#include "libswresample/swresample.h"
}
#include <QDebug>
#include <QTime>
#include <QCoreApplication>

static double r2d(AVRational r)
{
	return ((r.num == 0) || (r.den == 0)) ? 0. : ((double)r.num / (double)r.den);
}

static double r2dReverse(AVRational r)
{
	return ((r.num == 0) || (r.den == 0)) ? 0. : ((double)r.den / (double)r.num);
}

FFPlayer::FFPlayer()
{
}

FFPlayer::~FFPlayer()
{
}

void FFPlayer::startPlay(QString strUrl)
{
	m_strUrl = strUrl;
	m_bStop = false;
	m_bSuspend = false;
	m_totalMs = 0;
	m_pts = 0;

	this->start();
}

void FFPlayer::stopPlay()
{
	m_bStop = true;

	// 等待播放线程结束
	QTime timeEnd = QTime::currentTime().addMSecs(500);
	while (QTime::currentTime() < timeEnd)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
	} // while
}

bool FFPlayer::isPlay()
{
	return !m_bStop;
}

void FFPlayer::play(bool isPlay)
{
	if (isPlay)
		m_bSuspend = false;
	else
		m_bSuspend = true;
}

bool FFPlayer::isSuspend()
{
	if (m_bStop) // 退出式结束就不算暂停
		return false;
	else
		return m_bSuspend;
}

void FFPlayer::seek(int pos, int base, bool bAuto)
{
	if (!bAuto)
	{
		// seek不能重入
		QMutexLocker locker(&m_mutexPlay);

		_seek(pos, base);
	}
	else
	{
		// 外部已锁

		_seek(pos, base);
	}
}

void FFPlayer::_seek(int pos, int base)
{
	if (!m_pFormatCtx)
		return;

	double dblPos = (double)pos / base;
	dblPos = (dblPos >= 0.0) ? dblPos : 0.0;
	dblPos = (dblPos <= 1.0) ? dblPos : 1.0;

	if (m_videoStream >= 0)
	{
		int64_t timestamp = dblPos * m_pFormatCtx->duration * r2dReverse(m_pFormatCtx->streams[m_videoStream]->time_base) / AV_TIME_BASE + 0.5;
		timestamp = (timestamp >= 0) ? timestamp : 0;
		timestamp += m_pFormatCtx->streams[m_videoStream]->start_time;
		int ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧

		// seek失败就把时间调后1秒，暂时简单处理，如果重seek失败就会“跳回原处”或者“卡在尾端”
		if (ret < 0)
		{
			qInfo() << "av_seek_frame video fail 1" << pos << base << dblPos << m_videoStream << timestamp << ret;

			timestamp += m_pFormatCtx->streams[m_videoStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧
		}

		// seek尾端失败就尝试seek开端
		if ((ret < 0) && (pos == base))
		{
			qInfo() << "av_seek_frame video fail 2" << pos << base << dblPos << m_videoStream << timestamp << ret;

			timestamp = m_pFormatCtx->streams[m_videoStream]->start_time + m_pFormatCtx->streams[m_videoStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧
		}

		//清除之前的解码缓冲
		avcodec_flush_buffers(m_pFormatCtx->streams[m_videoStream]->codec);

		if (ret < 0)
		{
			qInfo() << "av_seek_frame video fail 3" << pos << base << dblPos << m_videoStream << timestamp << ret;

			if (m_audioStream >= 0)
			{
				//清除之前的解码缓冲
				avcodec_flush_buffers(m_pFormatCtx->streams[m_audioStream]->codec);
			}

			return;
		}
	}

	if (m_audioStream >= 0)
	{
		int64_t timestamp = dblPos * m_pFormatCtx->duration * r2dReverse(m_pFormatCtx->streams[m_audioStream]->time_base) / AV_TIME_BASE + 0.5;
		timestamp = (timestamp >= 0) ? timestamp : 0;
		timestamp += m_pFormatCtx->streams[m_audioStream]->start_time;
		int ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧

		// seek失败就把时间调后1秒，暂时简单处理，发现有时音频尾端总seek不到但不影响效果
		if (ret < 0)
		{
			qInfo() << "av_seek_frame audio fail 1" << pos << base << dblPos << m_audioStream << timestamp << ret;

			timestamp += m_pFormatCtx->streams[m_audioStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧
		}

		// seek尾端失败就尝试seek开端
		if ((ret < 0) && (pos == base))
		{
			qInfo() << "av_seek_frame audio fail 2" << pos << base << dblPos << m_audioStream << timestamp << ret;

			timestamp = m_pFormatCtx->streams[m_audioStream]->start_time + m_pFormatCtx->streams[m_audioStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //向后|关键帧
		}

		//清除之前的解码缓冲
		avcodec_flush_buffers(m_pFormatCtx->streams[m_audioStream]->codec);

		if (ret < 0)
		{
			qInfo() << "av_seek_frame audio fail 3" << pos << base << dblPos << m_audioStream << timestamp << ret;

			return;
		}
	}
}

void FFPlayer::sltPlay(bool isPlay)
{
	play(isPlay);
}

void FFPlayer::sltSeek(int pos, int base)
{
	seek(pos, base, false);
}

static int interrupt_cb(void *ctx)
{
	FFPlayer* pFFPlayer = (FFPlayer*)ctx;

	if (pFFPlayer && pFFPlayer->m_bReadFrame)
	{
		if (pFFPlayer->m_timeoutReadFrame <= 1000)
			++(pFFPlayer->m_timeoutReadFrame);
		else
			return 1; // 打断堵塞，例如av_read_frame堵塞
	}

	return 0;
}

void FFPlayer::run()
{
	//AVFormatContext *pFormatCtx = NULL;
	AVCodecContext *pCodecCtx = NULL, *pCodecCtxAudio = NULL;
	AVCodec *pCodec = NULL, *pCodecAudio = NULL;
	AVFrame *pFrame = NULL, *pFrameRGB = NULL, *pFramePcm = NULL;
	AVPacket *packet = NULL;
	uint8_t *out_buffer = NULL;
	char *bufferPCM = new char[AUDIOBUFFERSIZE];
	memset(bufferPCM, 0, AUDIOBUFFERSIZE);

	struct SwsContext *img_convert_ctx = NULL; // 视频转换
	struct SwrContext *audio_convert_ctx = NULL; // 音频转换

	m_videoStream = -1;
	m_audioStream = -1;

	int /*videoStream = -1, audioStream = -1, */i = 0, numBytes = 0;
	int ret = 0, got_picture = 0;

	avformat_network_init();
	av_register_all();


	//Allocate an AVFormatContext.
	m_pFormatCtx = avformat_alloc_context();

	// 注册用来打断的回调函数
	m_pFormatCtx->interrupt_callback.callback = interrupt_cb;
	m_pFormatCtx->interrupt_callback.opaque = this;

	// ffmpeg取rtsp流时av_read_frame阻塞的解决办法 设置参数优化
	AVDictionary* avdic = NULL;
	av_dict_set(&avdic, "buffer_size", "102400", 0); //设置缓存大小，1080p可将值调大
	av_dict_set(&avdic, "rtsp_transport", "udp", 0); //以udp方式打开，如果以tcp方式打开将udp替换为tcp
	av_dict_set(&avdic, "stimeout", "2000000", 0);   //设置超时断开连接时间，单位微秒
	av_dict_set(&avdic, "max_delay", "500000", 0);   //设置最大时延


	///rtsp地址，可根据实际情况修改
	//char url[]="rtsp://admin:admin@192.168.1.18:554/h264/ch1/main/av_stream";
	//char url[]="rtsp://192.168.17.112/test2.264";
	//char url[]="rtsp://admin:admin@192.168.43.1/stream/main";
	//char url[]="rtmp://58.200.131.2:1935/livetv/hunantv";//"rtmp://mobliestream.c3tv.com:554/live/goodtv.sdp";

	QByteArray baUrl = m_strUrl.toUtf8();
	if (baUrl.isEmpty())
	{
		qInfo("ffplayer url is empty");
		return;
	}
	else
	{
		qInfo() << "ffplayer start" << baUrl;
	}

	if (avformat_open_input(&m_pFormatCtx, baUrl.data()/*url*/, NULL, &avdic) != 0) {
		qInfo("can't open the file. \n");
		return;
	}

	if (avformat_find_stream_info(m_pFormatCtx, NULL) < 0) {
		qInfo("Could't find stream infomation.\n");
		return;
	}

	m_totalMs = (m_pFormatCtx->duration / AV_TIME_BASE) * 1000;  //视频的时间，结果是多少豪秒
	emit sigSetTotalTime(m_totalMs);

	///循环查找视频中包含的流信息，直到找到视频类型的流
	///便将其记录下来 保存到videoStream变量中
	///这里我们现在只处理视频流  音频流先不管他
	for (i = 0; i < m_pFormatCtx->nb_streams; i++) {
		if (m_pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			m_videoStream = i;
		}
		else if (m_pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			m_audioStream = i;
		}
	}

	// 视频准备
	///如果videoStream为-1 说明没有找到视频流
	if (m_videoStream == -1) {
		qInfo("Didn't find a video stream.\n");
		return;
	}

	///查找解码器
	pCodecCtx = m_pFormatCtx->streams[m_videoStream]->codec;
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	///2017.8.9---lizhen
	pCodecCtx->bit_rate = 0;   //初始化为0
	pCodecCtx->time_base.num = 1;  //下面两行：一秒钟25帧
	pCodecCtx->time_base.den = 25;
	pCodecCtx->frame_number = 1;  //每包一个视频帧

	if (pCodec == NULL) {
		qInfo("Codec not found.\n");
		return;
	}

	///打开解码器
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
		qInfo("Could not open codec.\n");
		return;
	}

	// 音频准备
	bool bHasAudio = false;
	if (-1 != m_audioStream)
	{
		// 查找解码器
		pCodecCtxAudio = m_pFormatCtx->streams[m_audioStream]->codec;
		pCodecAudio = avcodec_find_decoder(pCodecCtxAudio->codec_id);
		if (pCodecAudio)
		{
			// 打开解码器
			ret = avcodec_open2(pCodecCtxAudio, pCodecAudio, NULL);
			if (ret >= 0)
			{
				bHasAudio = true;

				// 音频参数
				int sampleRate = 48000; // 采样频率
				int channel = 2; // 通道数
				int sampleSize = 16; // 位深度
				sampleRate = pCodecCtxAudio->sample_rate;
				channel = pCodecCtxAudio->channels;
				switch (pCodecCtxAudio->sample_fmt)
				{
				case AV_SAMPLE_FMT_S16:
					sampleSize = 16;
					break;
				case AV_SAMPLE_FMT_S32:
					sampleSize = 32;
					break;
				default:
					break;
				}

				m_myAudio.init(sampleRate, channel, sampleSize);
				m_myAudio.start();

				// 音频帧
				pFramePcm = av_frame_alloc();

				audio_convert_ctx = swr_alloc();
				swr_alloc_set_opts(audio_convert_ctx, pCodecCtxAudio->channel_layout,
					AV_SAMPLE_FMT_S16,
					pCodecCtxAudio->sample_rate,
					pCodecCtxAudio->channels,
					pCodecCtxAudio->sample_fmt,
					pCodecCtxAudio->sample_rate,
					0, 0);
				swr_init(audio_convert_ctx);
			}
			else
			{
				qInfo() << "Could not open audio codec";
			}
		}
		else
		{
			qInfo() << "Codec audio not found";
		}
	}
	else
	{
		qInfo() << "Didn't find a audio stream";
	}

	// 视频帧
	pFrame = av_frame_alloc();
	pFrameRGB = av_frame_alloc();

	// 分辨率如果变化就必须重新分配内存
	// 记录最近分辨率
	int lastWidth = pCodecCtx->width;
	int lastHeight = pCodecCtx->height;

	///这里我们改成了 将解码后的YUV数据转换成RGB32
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
		pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
		AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

	numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height);

	out_buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
	avpicture_fill((AVPicture *)pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
		pCodecCtx->width, pCodecCtx->height);

	int y_size = pCodecCtx->width * pCodecCtx->height;

	packet = (AVPacket *)malloc(sizeof(AVPacket)); //分配一个packet
	av_new_packet(packet, y_size); //分配packet的数据

	bool bNoAudioBetweenVideo = false; // 是否在视频帧之间没有音频帧
	bool bLastIsAudio = true; // 是否上一帧是音频帧

	while (!m_bStop)
	{
		if (bHasAudio)
		{
			QMutexLocker locker(&m_mutexPlay);

			// 通过音频控制时间
			bool bBufferEmpty = false;
			int iBytesFree = m_myAudio.getFree(bBufferEmpty);
			if (iBytesFree < AUDIOBUFFERSIZE) // 需跟自用缓冲区大小比较
			{
				msleep(1);
				continue;
			}
		}

		// 暂停播放
		if (m_bSuspend)
		{
			bLastIsAudio = true; // 拖拉进度后的上帧已经变化

			msleep(1);
			continue;
		}

		QMutexLocker locker(&m_mutexPlay);

		m_bReadFrame = true; // 打断堵塞，例如av_read_frame堵塞
		m_timeoutReadFrame = 0; // 打断堵塞，例如av_read_frame堵塞
		if (av_read_frame(m_pFormatCtx, packet) < 0)
		{
			m_bReadFrame = false;
			qInfo() << "av_read_frame < 0";
			//break; //这里认为视频读取完了

			// 正常结束播放时间，就循环播放，跟APP一样
			// 目前用seek实现跳回开端，如果seek失败就会“卡在尾端”
			if (!m_bStop)
			{
				bLastIsAudio = true; // 循环进度后的上帧已经变化

				seek(0, 1, true);
			}

			continue;
		}
		m_bReadFrame = false;

		bool bCurIsVideo = false; // 当前帧是否视频帧
		if (packet->stream_index == m_videoStream) { // 视频数据
			bCurIsVideo = true;
			if (bLastIsAudio)
			{
				bNoAudioBetweenVideo = false;
			}
			else
			{
				bNoAudioBetweenVideo = true;
			}
			bLastIsAudio = false;

			ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);

			if (ret < 0) {
				qInfo("decode error.\n");
				// 解码错误就仍然继续 return;
				got_picture = 0;
				msleep(20); // 降低CPU占用率
			}

			if (got_picture) {
				// 分辨率如果变化就必须重新分配内存
				if ((lastWidth != pCodecCtx->width) || (lastHeight != pCodecCtx->height))
				{
					lastWidth = pCodecCtx->width;
					lastHeight = pCodecCtx->height;

					av_free(out_buffer); // 释放
					av_free(pFrameRGB); // 释放
					pFrameRGB = av_frame_alloc();

					///这里我们改成了 将解码后的YUV数据转换成RGB32
					img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
						pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
						AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

					numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height);

					out_buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
					avpicture_fill((AVPicture *)pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
						pCodecCtx->width, pCodecCtx->height);
				}

				sws_scale(img_convert_ctx,
					(uint8_t const * const *)pFrame->data,
					pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
					pFrameRGB->linesize);

				//把这个RGB数据 用QImage加载
				QImage tmpImg((uchar *)out_buffer, pCodecCtx->width, pCodecCtx->height, QImage::Format_RGB32);
				QImage image = tmpImg.copy(); //把图像复制一份 传递给界面显示
				if (!m_bStop)
				{
					emit sig_GetOneFrame(image);  //发送信号
				}

				// 因为有时没有音频帧，所以还需使用视频帧的时间
				{
					int64_t start_time = m_pFormatCtx->streams[m_videoStream]->start_time;
					if (start_time < 0)
						start_time = 0;
					m_pts = ((pFrame->pts - start_time) * r2d(m_pFormatCtx->streams[m_videoStream]->time_base)) * 1000; //毫秒数
					if (m_pts > 0)
					{
						emit sigSetCurrentTime(m_pts);
					}
				}
			}
			else qDebug() << "got_picture < 0";
		}
		else if ((packet->stream_index == m_audioStream) && bHasAudio) // 音频数据
		{
			bLastIsAudio = true;

			ret = avcodec_send_packet(pCodecCtxAudio, packet);
			if (ret == 0)
			{
				ret = avcodec_receive_frame(pCodecCtxAudio, pFramePcm);
				if (ret == 0)
				{
					int64_t start_time = m_pFormatCtx->streams[m_audioStream]->start_time;
					if (start_time < 0)
						start_time = 0;
					m_pts = ((pFramePcm->pts - start_time) * r2d(m_pFormatCtx->streams[m_audioStream]->time_base)) * 1000; //毫秒数
					emit sigSetCurrentTime(m_pts);

					int lenPCM = convertPCM(pCodecCtxAudio, audio_convert_ctx, pFramePcm, bufferPCM);
					m_myAudio.write(bufferPCM, lenPCM);
				}
			}
		}
//		else qDebug() << "packet->stream_index not video stream";

		av_free_packet(packet); //释放资源,否则内存会一直上升

		if (!bHasAudio)
		{
			msleep(20); //停一停  不然放的太快了
		}
		else if (bNoAudioBetweenVideo && bCurIsVideo)
		{
			msleep(47); //停一停  不然放的太快了
		}
	} // while

	// 清除绘制
	emit sig_GetOneFrame(QImage());

	// 停止音频
	m_myAudio.stop();

	delete[] bufferPCM;
	av_free(out_buffer);
	av_free(pFramePcm);
	av_free(pFrame);
	av_free(pFrameRGB);
	avcodec_close(pCodecCtxAudio);
	avcodec_close(pCodecCtx);
	avformat_close_input(&m_pFormatCtx);
	m_pFormatCtx = NULL;

	qInfo() << "ffplayer end" << baUrl;
}

int FFPlayer::convertPCM(AVCodecContext *pCodecCtxAudio,
	SwrContext *audio_convert_ctx,
	AVFrame *pFramePcm,
	char *bufferPCM)
{
	if (!pCodecCtxAudio || !audio_convert_ctx || !pFramePcm || !bufferPCM)
		return 0;

	uint8_t *outData[1] = {0};
	outData[0] = (uint8_t *)bufferPCM;
	int lenConvert = swr_convert(audio_convert_ctx, outData, AUDIOBUFFERSIZE, (const uint8_t **)pFramePcm->data, pFramePcm->nb_samples);
	if (lenConvert <= 0)
		return 0;

	int outSize = av_samples_get_buffer_size(NULL, pCodecCtxAudio->channels, pFramePcm->nb_samples, AV_SAMPLE_FMT_S16, 0);
	return outSize;
}
