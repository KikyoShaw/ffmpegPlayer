#include "videoFrame.h"
#include <QPainter>
#include <QStyleOption>
#include <QTimer>

VideoFrame::VideoFrame(QWidget *parent)
	:QWidget(parent), m_descImage(QImage())
{
	m_TimerUpdate = new QTimer(this);
	if (m_TimerUpdate) {
		typedef void (QWidget:: *widgetUpdate)(void);
		connect(m_TimerUpdate, &QTimer::timeout, this, static_cast<widgetUpdate>(&QWidget::update));
		m_TimerUpdate->setInterval(50);
		m_TimerUpdate->setTimerType(Qt::PreciseTimer);
		m_TimerUpdate->start();
	}
}

VideoFrame::~VideoFrame()
{
}

void VideoFrame::setImageFrame(const QImage & image)
{
	m_descImage = image;
	update();
}

void VideoFrame::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);
	//����� + ƽ����Ե����
	painter.setRenderHints(QPainter::Antialiasing, true);
	painter.setRenderHints(QPainter::SmoothPixmapTransform, true);
	//���ͼƬ�����ڻ���ԭ����
	if (m_descImage.isNull()) {
		paintNull(&painter);
	}
	else {
		m_descImage = m_descImage.scaledToHeight(height(), Qt::SmoothTransformation);
		//���ڱ�
		painter.setBrush(QBrush(Qt::black));
		painter.fillRect(rect(), Qt::SolidPattern);
		//������ʾ
		QRect rect(m_descImage.rect());
		QRect devRect(0, 0, painter.device()->width(), painter.device()->height());
		rect.moveCenter(devRect.center());
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.drawPixmap(rect.topLeft(), QPixmap::fromImage(m_descImage));
	}
}

void VideoFrame::paintNull(QPainter * painter)
{
	if (Q_NULLPTR == painter)
		return;

	QStyleOption opt;
	opt.initFrom(this);
	style()->drawPrimitive(QStyle::PE_Widget, &opt, painter, this);
}