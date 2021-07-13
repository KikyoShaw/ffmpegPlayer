#include "vplayerslider.h"
#include <QTime>

#define MAXRANGE (10000)

VPlayerSlider::VPlayerSlider(QWidget* parent/* = Q_NULLPTR*/)
	: QWidget(parent)
{
	ui.setupUi(this);

	connect(ui.pushButton_play, SIGNAL(clicked(bool)), this, SLOT(sltPlayButton()));
	connect(ui.horizontalSlider_player, SIGNAL(sliderMoved(int)), this, SLOT(sltSliderMoved(int)));
	connect(ui.horizontalSlider_player, SIGNAL(sliderPressed()), this, SLOT(sltSliderPressed()));
	connect(ui.horizontalSlider_player, SIGNAL(sliderReleased()), this, SLOT(sltSliderReleased()));

	clear();
}

VPlayerSlider::~VPlayerSlider()
{
}

void VPlayerSlider::clear()
{
	ui.label_totaltime->setText(QString("--:--"));
	ui.label_currenttime->setText(QString("--:--"));
	ui.pushButton_play->setText(QStringLiteral("暂停"));
	ui.horizontalSlider_player->setRange(0, MAXRANGE);
	ui.horizontalSlider_player->setValue(0);
	m_iTotalTime = 0;
	m_posPress = 0;
}

// 时间参数单位：毫秒
void VPlayerSlider::sltSetTotalTime(qint64 iTime)
{
	QString strTotalTime = convertTime(iTime);
	ui.label_totaltime->setText(strTotalTime);

	m_iTotalTime = ((iTime >= 0) ? iTime : 0);
}

void VPlayerSlider::sltSetCurrentTime(qint64 iTime)
{
	QString strCurrentTime = convertTime(iTime);
	ui.label_currenttime->setText(strCurrentTime);

	int64_t iValue = ((iTime >= 0) ? iTime : 0);
	if (m_iTotalTime > 0)
		iValue = ((double)iValue / m_iTotalTime) * MAXRANGE;
	else
		iValue = 0;
	ui.horizontalSlider_player->setValue(iValue);
}

void VPlayerSlider::sltPlayButton()
{
	if (ui.pushButton_play->text() == QStringLiteral("暂停"))
	{
		ui.pushButton_play->setText(QStringLiteral("播放"));
		emit sigPlay(false);
	}
	else
	{
		ui.pushButton_play->setText(QStringLiteral("暂停"));
		emit sigPlay(true);
	}
}

void VPlayerSlider::sltSliderMoved(int pos)
{
	if (m_iTotalTime <= 0)
		return;

	pos = ((pos >= 0) ? pos : 0);
	pos = ((pos <= MAXRANGE) ? pos : MAXRANGE);

	// 动态显示拖拉位置所对应的时间
	int64_t iTime = (double)m_iTotalTime * pos / MAXRANGE;
	QString strPosTime = convertTime(iTime);
	ui.label_currenttime->setText(strPosTime);
}

void VPlayerSlider::sltSliderPressed()
{
	m_posPress = ui.horizontalSlider_player->value();
	emit sigPlay(false);
}

void VPlayerSlider::sltSliderReleased()
{
	int pos = ui.horizontalSlider_player->value();
	if (pos != m_posPress)
	{
		emit sigSeek(pos, MAXRANGE);
	}
	emit sigPlay(true);
	// 为了简单实现，暂停时的拖拉自动取消暂停，所以直接保证控件正确
	ui.pushButton_play->setText(QStringLiteral("暂停"));
}

//void VPlayerSlider::sltFinishPlay()
//{
//	ui.pushButton_play->setText(QStringLiteral("播放"));
//	ui.label_currenttime->setText(QString("00:00"));
//	ui.horizontalSlider_player->setValue(0);
//}

QString VPlayerSlider::convertTime(int64_t iTime)
{
	if (iTime < 0)
		iTime = 0;

	int64_t iHour = 0;
	int64_t iMinute = 0;
	int64_t iSecond = 0;

	iSecond = iTime / 1000;
	iMinute = iSecond;
	iSecond = iSecond % 60;
	iMinute = iMinute - iSecond;
	iMinute = iMinute / 60;
	iHour = iMinute;
	iMinute = iMinute % 60;
	iHour = iHour - iMinute;
	iHour = iHour / 60;

	QString strTime;
	if (iHour > 0)
		strTime.append(QString("%1:").arg(iHour));
	if (iMinute < 10)
		strTime.append("0");
	strTime.append(QString("%1:").arg(iMinute));
	if (iSecond < 10)
		strTime.append("0");
	strTime.append(QString("%1").arg(iSecond));
	return strTime;
}
