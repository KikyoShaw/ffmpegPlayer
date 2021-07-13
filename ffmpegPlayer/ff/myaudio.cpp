#include "myaudio.h"

MyAudio::MyAudio(QObject *parent/* = Q_NULLPTR*/)
	: QObject(parent)
{
}

MyAudio::~MyAudio()
{
}

void MyAudio::init(int sampleRate, int channel, int sampleSize)
{
	m_sampleRate = sampleRate;
	m_channel = channel;
	//m_sampleSize = sampleSize; // 固定16
}

void MyAudio::start()
{
	stop();

	QAudioFormat fmt; //Qt音频的格式
	fmt.setSampleRate(m_sampleRate);				//1秒采集48000个声音
	fmt.setSampleSize(m_sampleSize);				//16位
	fmt.setChannelCount(m_channel);					//声道2双声道
	fmt.setCodec("audio/pcm");						//音频的格式
	fmt.setByteOrder(QAudioFormat::LittleEndian);	//次序
	fmt.setSampleType(QAudioFormat::UnSignedInt);	//样本的类别

	m_pAudioOutput = new QAudioOutput(fmt);
	m_pIODevice = m_pAudioOutput->start();
}

void MyAudio::stop()
{
	if (m_pAudioOutput)
	{
		m_pAudioOutput->stop();
		delete m_pAudioOutput;
		m_pAudioOutput = NULL;
		m_pIODevice = NULL;
	}
}

void MyAudio::play(bool isPlay)
{
	if (m_pAudioOutput)
	{
		if (isPlay)
		{
			m_pAudioOutput->resume();
		}
		else
		{
			m_pAudioOutput->suspend();
		}
	}
}

// 输入输出设备播放声音
bool MyAudio::write(const char *data, int datasize)
{
	if (!data || (datasize <= 0))
		return false;

	if (m_pIODevice)
	{
		// 输入输出设备播放声音,将数据从零终止的8位字符字符串写入设备
		m_pIODevice->write(data, datasize);
		return true;
	}
	else
	{
		return false;
	}
}

int MyAudio::getFree(bool& bBufferEmpty)
{
	bBufferEmpty = false;

	if (m_pAudioOutput)
	{
		int iBytesFree = m_pAudioOutput->bytesFree();
		int iBufferSize = m_pAudioOutput->bufferSize();
		bBufferEmpty = (iBytesFree == iBufferSize);
		return iBytesFree;
	}
	else
	{
		return 0;
	}
}
