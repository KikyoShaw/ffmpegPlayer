#pragma once

#include <QWidget>

class QPainter;
class QTimer;

class VideoFrame : public QWidget
{
	Q_OBJECT
public:
	explicit VideoFrame(QWidget *parent = Q_NULLPTR);
	~VideoFrame();

	void setImageFrame(const QImage& image);

private:
	virtual void paintEvent(QPaintEvent *event) Q_DECL_OVERRIDE;
	void paintNull(QPainter *painter);

private:
	QImage m_descImage;
	//ͨ����ʱ��ˢ��
	QTimer *m_TimerUpdate = Q_NULLPTR;
};
