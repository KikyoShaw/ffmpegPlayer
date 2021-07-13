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
	//m_sampleSize = sampleSize; // �̶�16
}

void MyAudio::start()
{
	stop();

	QAudioFormat fmt; //Qt��Ƶ�ĸ�ʽ
	fmt.setSampleRate(m_sampleRate);				//1��ɼ�48000������
	fmt.setSampleSize(m_sampleSize);				//16λ
	fmt.setChannelCount(m_channel);					//����2˫����
	fmt.setCodec("audio/pcm");						//��Ƶ�ĸ�ʽ
	fmt.setByteOrder(QAudioFormat::LittleEndian);	//����
	fmt.setSampleType(QAudioFormat::UnSignedInt);	//���������

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

// ��������豸��������
bool MyAudio::write(const char *data, int datasize)
{
	if (!data || (datasize <= 0))
		return false;

	if (m_pIODevice)
	{
		// ��������豸��������,�����ݴ�����ֹ��8λ�ַ��ַ���д���豸
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
