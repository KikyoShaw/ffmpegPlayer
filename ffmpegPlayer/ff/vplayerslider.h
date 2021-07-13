#ifndef VPLAYERSLIDER_H
#define VPLAYERSLIDER_H

#include "ui_vplayerslider.h"
#include <QWidget>

class VPlayerSlider : public QWidget
{
	Q_OBJECT

public:
	explicit VPlayerSlider(QWidget* parent = Q_NULLPTR);
	~VPlayerSlider();

	void clear();

signals:
	void sigPlay(bool isPlay);
	void sigSeek(int pos, int base);

public slots :
	void sltSetTotalTime(qint64 iTime);
	void sltSetCurrentTime(qint64 iTime);

protected slots:
	void sltPlayButton();
	void sltSliderMoved(int pos);
	void sltSliderPressed();
	void sltSliderReleased();
	//void sltFinishPlay();

protected:
	QString convertTime(int64_t iTime);

private:
	Ui::VPlayerSlider ui;
	int64_t m_iTotalTime = 0;
	int m_posPress = 0;
};

#endif // VPLAYERSLIDER_H
