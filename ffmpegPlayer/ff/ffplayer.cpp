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

	// �ȴ������߳̽���
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
	if (m_bStop) // �˳�ʽ�����Ͳ�����ͣ
		return false;
	else
		return m_bSuspend;
}

void FFPlayer::seek(int pos, int base, bool bAuto)
{
	if (!bAuto)
	{
		// seek��������
		QMutexLocker locker(&m_mutexPlay);

		_seek(pos, base);
	}
	else
	{
		// �ⲿ����

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
		int ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡

		// seekʧ�ܾͰ�ʱ�����1�룬��ʱ�򵥴��������seekʧ�ܾͻᡰ����ԭ�������ߡ�����β�ˡ�
		if (ret < 0)
		{
			qInfo() << "av_seek_frame video fail 1" << pos << base << dblPos << m_videoStream << timestamp << ret;

			timestamp += m_pFormatCtx->streams[m_videoStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡
		}

		// seekβ��ʧ�ܾͳ���seek����
		if ((ret < 0) && (pos == base))
		{
			qInfo() << "av_seek_frame video fail 2" << pos << base << dblPos << m_videoStream << timestamp << ret;

			timestamp = m_pFormatCtx->streams[m_videoStream]->start_time + m_pFormatCtx->streams[m_videoStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_videoStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡
		}

		//���֮ǰ�Ľ��뻺��
		avcodec_flush_buffers(m_pFormatCtx->streams[m_videoStream]->codec);

		if (ret < 0)
		{
			qInfo() << "av_seek_frame video fail 3" << pos << base << dblPos << m_videoStream << timestamp << ret;

			if (m_audioStream >= 0)
			{
				//���֮ǰ�Ľ��뻺��
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
		int ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡

		// seekʧ�ܾͰ�ʱ�����1�룬��ʱ�򵥴���������ʱ��Ƶβ����seek��������Ӱ��Ч��
		if (ret < 0)
		{
			qInfo() << "av_seek_frame audio fail 1" << pos << base << dblPos << m_audioStream << timestamp << ret;

			timestamp += m_pFormatCtx->streams[m_audioStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡
		}

		// seekβ��ʧ�ܾͳ���seek����
		if ((ret < 0) && (pos == base))
		{
			qInfo() << "av_seek_frame audio fail 2" << pos << base << dblPos << m_audioStream << timestamp << ret;

			timestamp = m_pFormatCtx->streams[m_audioStream]->start_time + m_pFormatCtx->streams[m_audioStream]->time_base.den;

			ret = av_seek_frame(m_pFormatCtx, m_audioStream, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME); //���|�ؼ�֡
		}

		//���֮ǰ�Ľ��뻺��
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
			return 1; // ��϶���������av_read_frame����
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

	struct SwsContext *img_convert_ctx = NULL; // ��Ƶת��
	struct SwrContext *audio_convert_ctx = NULL; // ��Ƶת��

	m_videoStream = -1;
	m_audioStream = -1;

	int /*videoStream = -1, audioStream = -1, */i = 0, numBytes = 0;
	int ret = 0, got_picture = 0;

	avformat_network_init();
	av_register_all();


	//Allocate an AVFormatContext.
	m_pFormatCtx = avformat_alloc_context();

	// ע��������ϵĻص�����
	m_pFormatCtx->interrupt_callback.callback = interrupt_cb;
	m_pFormatCtx->interrupt_callback.opaque = this;

	// ffmpegȡrtsp��ʱav_read_frame�����Ľ���취 ���ò����Ż�
	AVDictionary* avdic = NULL;
	av_dict_set(&avdic, "buffer_size", "102400", 0); //���û����С��1080p�ɽ�ֵ����
	av_dict_set(&avdic, "rtsp_transport", "udp", 0); //��udp��ʽ�򿪣������tcp��ʽ�򿪽�udp�滻Ϊtcp
	av_dict_set(&avdic, "stimeout", "2000000", 0);   //���ó�ʱ�Ͽ�����ʱ�䣬��λ΢��
	av_dict_set(&avdic, "max_delay", "500000", 0);   //�������ʱ��


	///rtsp��ַ���ɸ���ʵ������޸�
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

	m_totalMs = (m_pFormatCtx->duration / AV_TIME_BASE) * 1000;  //��Ƶ��ʱ�䣬����Ƕ��ٺ���
	emit sigSetTotalTime(m_totalMs);

	///ѭ��������Ƶ�а���������Ϣ��ֱ���ҵ���Ƶ���͵���
	///�㽫���¼���� ���浽videoStream������
	///������������ֻ������Ƶ��  ��Ƶ���Ȳ�����
	for (i = 0; i < m_pFormatCtx->nb_streams; i++) {
		if (m_pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			m_videoStream = i;
		}
		else if (m_pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			m_audioStream = i;
		}
	}

	// ��Ƶ׼��
	///���videoStreamΪ-1 ˵��û���ҵ���Ƶ��
	if (m_videoStream == -1) {
		qInfo("Didn't find a video stream.\n");
		return;
	}

	///���ҽ�����
	pCodecCtx = m_pFormatCtx->streams[m_videoStream]->codec;
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	///2017.8.9---lizhen
	pCodecCtx->bit_rate = 0;   //��ʼ��Ϊ0
	pCodecCtx->time_base.num = 1;  //�������У�һ����25֡
	pCodecCtx->time_base.den = 25;
	pCodecCtx->frame_number = 1;  //ÿ��һ����Ƶ֡

	if (pCodec == NULL) {
		qInfo("Codec not found.\n");
		return;
	}

	///�򿪽�����
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
		qInfo("Could not open codec.\n");
		return;
	}

	// ��Ƶ׼��
	bool bHasAudio = false;
	if (-1 != m_audioStream)
	{
		// ���ҽ�����
		pCodecCtxAudio = m_pFormatCtx->streams[m_audioStream]->codec;
		pCodecAudio = avcodec_find_decoder(pCodecCtxAudio->codec_id);
		if (pCodecAudio)
		{
			// �򿪽�����
			ret = avcodec_open2(pCodecCtxAudio, pCodecAudio, NULL);
			if (ret >= 0)
			{
				bHasAudio = true;

				// ��Ƶ����
				int sampleRate = 48000; // ����Ƶ��
				int channel = 2; // ͨ����
				int sampleSize = 16; // λ���
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

				// ��Ƶ֡
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

	// ��Ƶ֡
	pFrame = av_frame_alloc();
	pFrameRGB = av_frame_alloc();

	// �ֱ�������仯�ͱ������·����ڴ�
	// ��¼����ֱ���
	int lastWidth = pCodecCtx->width;
	int lastHeight = pCodecCtx->height;

	///�������Ǹĳ��� ��������YUV����ת����RGB32
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
		pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
		AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

	numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height);

	out_buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
	avpicture_fill((AVPicture *)pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
		pCodecCtx->width, pCodecCtx->height);

	int y_size = pCodecCtx->width * pCodecCtx->height;

	packet = (AVPacket *)malloc(sizeof(AVPacket)); //����һ��packet
	av_new_packet(packet, y_size); //����packet������

	bool bNoAudioBetweenVideo = false; // �Ƿ�����Ƶ֮֡��û����Ƶ֡
	bool bLastIsAudio = true; // �Ƿ���һ֡����Ƶ֡

	while (!m_bStop)
	{
		if (bHasAudio)
		{
			QMutexLocker locker(&m_mutexPlay);

			// ͨ����Ƶ����ʱ��
			bool bBufferEmpty = false;
			int iBytesFree = m_myAudio.getFree(bBufferEmpty);
			if (iBytesFree < AUDIOBUFFERSIZE) // ������û�������С�Ƚ�
			{
				msleep(1);
				continue;
			}
		}

		// ��ͣ����
		if (m_bSuspend)
		{
			bLastIsAudio = true; // �������Ⱥ����֡�Ѿ��仯

			msleep(1);
			continue;
		}

		QMutexLocker locker(&m_mutexPlay);

		m_bReadFrame = true; // ��϶���������av_read_frame����
		m_timeoutReadFrame = 0; // ��϶���������av_read_frame����
		if (av_read_frame(m_pFormatCtx, packet) < 0)
		{
			m_bReadFrame = false;
			qInfo() << "av_read_frame < 0";
			//break; //������Ϊ��Ƶ��ȡ����

			// ������������ʱ�䣬��ѭ�����ţ���APPһ��
			// Ŀǰ��seekʵ�����ؿ��ˣ����seekʧ�ܾͻᡰ����β�ˡ�
			if (!m_bStop)
			{
				bLastIsAudio = true; // ѭ�����Ⱥ����֡�Ѿ��仯

				seek(0, 1, true);
			}

			continue;
		}
		m_bReadFrame = false;

		bool bCurIsVideo = false; // ��ǰ֡�Ƿ���Ƶ֡
		if (packet->stream_index == m_videoStream) { // ��Ƶ����
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
				// ����������Ȼ���� return;
				got_picture = 0;
				msleep(20); // ����CPUռ����
			}

			if (got_picture) {
				// �ֱ�������仯�ͱ������·����ڴ�
				if ((lastWidth != pCodecCtx->width) || (lastHeight != pCodecCtx->height))
				{
					lastWidth = pCodecCtx->width;
					lastHeight = pCodecCtx->height;

					av_free(out_buffer); // �ͷ�
					av_free(pFrameRGB); // �ͷ�
					pFrameRGB = av_frame_alloc();

					///�������Ǹĳ��� ��������YUV����ת����RGB32
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

				//�����RGB���� ��QImage����
				QImage tmpImg((uchar *)out_buffer, pCodecCtx->width, pCodecCtx->height, QImage::Format_RGB32);
				QImage image = tmpImg.copy(); //��ͼ����һ�� ���ݸ�������ʾ
				if (!m_bStop)
				{
					emit sig_GetOneFrame(image);  //�����ź�
				}

				// ��Ϊ��ʱû����Ƶ֡�����Ի���ʹ����Ƶ֡��ʱ��
				{
					int64_t start_time = m_pFormatCtx->streams[m_videoStream]->start_time;
					if (start_time < 0)
						start_time = 0;
					m_pts = ((pFrame->pts - start_time) * r2d(m_pFormatCtx->streams[m_videoStream]->time_base)) * 1000; //������
					if (m_pts > 0)
					{
						emit sigSetCurrentTime(m_pts);
					}
				}
			}
			else qDebug() << "got_picture < 0";
		}
		else if ((packet->stream_index == m_audioStream) && bHasAudio) // ��Ƶ����
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
					m_pts = ((pFramePcm->pts - start_time) * r2d(m_pFormatCtx->streams[m_audioStream]->time_base)) * 1000; //������
					emit sigSetCurrentTime(m_pts);

					int lenPCM = convertPCM(pCodecCtxAudio, audio_convert_ctx, pFramePcm, bufferPCM);
					m_myAudio.write(bufferPCM, lenPCM);
				}
			}
		}
//		else qDebug() << "packet->stream_index not video stream";

		av_free_packet(packet); //�ͷ���Դ,�����ڴ��һֱ����

		if (!bHasAudio)
		{
			msleep(20); //ͣһͣ  ��Ȼ�ŵ�̫����
		}
		else if (bNoAudioBetweenVideo && bCurIsVideo)
		{
			msleep(47); //ͣһͣ  ��Ȼ�ŵ�̫����
		}
	} // while

	// �������
	emit sig_GetOneFrame(QImage());

	// ֹͣ��Ƶ
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
