#ifndef MYAUDIO_H
#define MYAUDIO_H

#include <QObject>
#include <QAudioOutput>

#define AUDIOBUFFERSIZE (10000)

class MyAudio : public QObject
{
	Q_OBJECT

public:
	explicit MyAudio(QObject *parent = Q_NULLPTR);
	~MyAudio();

	void init(int sampleRate, int channel, int sampleSize);
	void start();
	void stop();
	void play(bool isPlay);
	bool write(const char *data, int datasize);
	int getFree(bool& bBufferEmpty);

private:
	int m_sampleRate = 48000; // 采样频率
	int m_channel = 2; // 通道数
	int m_sampleSize = 16; // 位深度
	QAudioOutput* m_pAudioOutput = NULL;
	QIODevice* m_pIODevice = NULL;
};

#endif
