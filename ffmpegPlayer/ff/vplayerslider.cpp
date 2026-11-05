#include "vplayerslider.h"

#include <QComboBox>
#include <QSignalBlocker>
#include <QSlider>

namespace {
const int SeekRange = 10000;
}

VPlayerSlider::VPlayerSlider(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);
    ui.comboBox_rate->addItem(QStringLiteral("0.5×"), 0.5);
    ui.comboBox_rate->addItem(QStringLiteral("1×"), 1.0);
    ui.comboBox_rate->addItem(QStringLiteral("1.5×"), 1.5);
    ui.comboBox_rate->addItem(QStringLiteral("2×"), 2.0);
    ui.comboBox_rate->addItem(QStringLiteral("3×"), 3.0);
    ui.comboBox_rate->setCurrentIndex(1);

    connect(ui.pushButton_play, &QPushButton::clicked, this, &VPlayerSlider::togglePlayback);
    connect(ui.pushButton_stop, &QPushButton::clicked, this, &VPlayerSlider::sigStop);
    connect(ui.horizontalSlider_player, &QSlider::sliderMoved, this, &VPlayerSlider::sltSliderMoved);
    connect(ui.horizontalSlider_player, &QSlider::sliderPressed, this, &VPlayerSlider::sltSliderPressed);
    connect(ui.horizontalSlider_player, &QSlider::sliderReleased, this, &VPlayerSlider::sltSliderReleased);
    connect(ui.horizontalSlider_player, &QSlider::valueChanged, this, [this](int position) {
        if (!m_dragging && ui.horizontalSlider_player->isEnabled()) {
            sltSliderMoved(position);
            emit sigSeek(position, SeekRange);
        }
    });
    connect(ui.comboBox_rate, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        this, [this](int index) {
            if (ui.comboBox_rate->isEnabled()) {
                emit sigRateChanged(ui.comboBox_rate->itemData(index).toDouble());
            }
        });
    connect(ui.horizontalSlider_volume, &QSlider::valueChanged, this, [this](int volume) {
        m_volume = volume;
        ui.horizontalSlider_volume->setToolTip(QStringLiteral("音量：%1%").arg(volume));
        emit sigVolumeChanged(volume);
    });
    connect(ui.pushButton_mute, &QPushButton::toggled, this, [this](bool muted) {
        m_muted = muted;
        ui.pushButton_mute->setText(muted ? QStringLiteral("恢复声音") : QStringLiteral("静音"));
        emit sigMutedChanged(muted);
    });

    clear();
}

VPlayerSlider::~VPlayerSlider()
{
}

void VPlayerSlider::clear()
{
    m_totalTime = -1;
    m_currentTime = -1;
    m_posPress = 0;
    m_dragging = false;
    m_canSeek = false;
    m_canRate = false;
    const QSignalBlocker blocker(ui.horizontalSlider_player);
    ui.horizontalSlider_player->setRange(0, SeekRange);
    ui.horizontalSlider_player->setValue(0);
    ui.label_totaltime->setText(convertTime(-1));
    updatePositionDisplay();
    updateControls();
}

void VPlayerSlider::setSourceAvailable(bool available)
{
    m_sourceAvailable = available;
    updateControls();
}

void VPlayerSlider::sltSetTotalTime(qint64 timeMs)
{
    m_totalTime = timeMs >= 0 ? timeMs : -1;
    ui.label_totaltime->setText(convertTime(m_totalTime));
    updateControls();
    if (!m_dragging) {
        updatePositionDisplay();
    }
}

void VPlayerSlider::sltSetCurrentTime(qint64 timeMs)
{
    m_currentTime = timeMs >= 0 ? timeMs : -1;
    // 拖动期间仍记录后台位置，但只显示用户正在选择的位置。
    if (!m_dragging) {
        updatePositionDisplay();
    }
}

void VPlayerSlider::sltSetPlaybackState(int state)
{
    if (state < FFPlayer::Stopped || state > FFPlayer::Error) {
        return;
    }
    m_state = static_cast<FFPlayer::PlaybackState>(state);
    updateControls();
}

void VPlayerSlider::sltSetCapabilities(bool canSeek, bool canRate)
{
    m_canSeek = canSeek;
    m_canRate = canRate;
    updateControls();
}

void VPlayerSlider::sltSetRate(double rate)
{
    const QSignalBlocker blocker(ui.comboBox_rate);
    for (int index = 0; index < ui.comboBox_rate->count(); ++index) {
        if (qFuzzyCompare(ui.comboBox_rate->itemData(index).toDouble(), rate)) {
            ui.comboBox_rate->setCurrentIndex(index);
            return;
        }
    }
}

void VPlayerSlider::togglePlayback()
{
    if (!ui.pushButton_play->isEnabled()) {
        return;
    }
    emit sigPlay(m_state != FFPlayer::Playing);
}

void VPlayerSlider::changeVolume(int delta)
{
    ui.horizontalSlider_volume->setValue(qBound(0, m_volume + delta, 100));
}

void VPlayerSlider::toggleMuted()
{
    ui.pushButton_mute->toggle();
}

int VPlayerSlider::volume() const
{
    return m_volume;
}

bool VPlayerSlider::isMuted() const
{
    return m_muted;
}

bool VPlayerSlider::isDragging() const
{
    return m_dragging;
}

void VPlayerSlider::updateControls()
{
    const bool active = m_state == FFPlayer::Playing || m_state == FFPlayer::Paused;
    const bool busy = m_state == FFPlayer::Opening || m_state == FFPlayer::Stopping;
    const bool seekEnabled = active && m_canSeek && m_totalTime > 0;

    switch (m_state) {
    case FFPlayer::Playing:
        ui.pushButton_play->setText(QStringLiteral("暂停"));
        break;
    case FFPlayer::Paused:
        ui.pushButton_play->setText(QStringLiteral("继续"));
        break;
    case FFPlayer::Opening:
        ui.pushButton_play->setText(QStringLiteral("打开中…"));
        break;
    case FFPlayer::Stopping:
        ui.pushButton_play->setText(QStringLiteral("停止中…"));
        break;
    default:
        ui.pushButton_play->setText(QStringLiteral("播放"));
        break;
    }
    ui.pushButton_play->setEnabled(m_sourceAvailable && !busy);
    ui.pushButton_stop->setEnabled(active || m_state == FFPlayer::Opening);
    if (!seekEnabled && m_dragging) {
        m_dragging = false;
        updatePositionDisplay();
    }
    ui.horizontalSlider_player->setEnabled(seekEnabled);
    ui.horizontalSlider_player->setToolTip(seekEnabled
        ? QStringLiteral("拖动定位；左右方向键快退/快进 15 秒")
        : QStringLiteral("当前媒体不支持定位，或尚未取得时长"));
    ui.comboBox_rate->setEnabled(active && m_canRate);
    ui.comboBox_rate->setToolTip(active && m_canRate
        ? QStringLiteral("调整播放倍速") : QStringLiteral("当前媒体尚未确认支持倍速"));
}

void VPlayerSlider::updatePositionDisplay()
{
    ui.label_currenttime->setText(convertTime(m_currentTime));
    int value = 0;
    if (m_totalTime > 0 && m_currentTime > 0) {
        const qint64 position = qMin(m_currentTime, m_totalTime);
        value = static_cast<int>(static_cast<double>(position) / m_totalTime * SeekRange);
    }
    const QSignalBlocker blocker(ui.horizontalSlider_player);
    ui.horizontalSlider_player->setValue(qBound(0, value, SeekRange));
}

void VPlayerSlider::sltSliderMoved(int position)
{
    if (m_totalTime <= 0) {
        return;
    }
    const qint64 timeMs = static_cast<qint64>(
        static_cast<double>(m_totalTime) * qBound(0, position, SeekRange) / SeekRange);
    ui.label_currenttime->setText(convertTime(timeMs));
}

void VPlayerSlider::sltSliderPressed()
{
    if (!ui.horizontalSlider_player->isEnabled()) {
        return;
    }
    m_dragging = true;
    m_posPress = ui.horizontalSlider_player->value();
}

void VPlayerSlider::sltSliderReleased()
{
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    const int position = ui.horizontalSlider_player->value();
    if (ui.horizontalSlider_player->isEnabled() && position != m_posPress) {
        // 后端定位保留原暂停状态；拖动只提交定位请求，不发送播放/暂停请求。
        emit sigSeek(position, SeekRange);
    } else {
        updatePositionDisplay();
    }
}

QString VPlayerSlider::convertTime(qint64 timeMs)
{
    if (timeMs < 0) {
        return QStringLiteral("--:--");
    }
    const qint64 seconds = timeMs / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = seconds / 60 % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3").arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
