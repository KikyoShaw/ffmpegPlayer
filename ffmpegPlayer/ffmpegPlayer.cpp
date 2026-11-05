#include "ffmpegPlayer.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QFileDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>

ffmpegPlayer::ffmpegPlayer(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);
    ui.verticalLayout->addWidget(&m_playerSlider);

    connect(ui.pushButton_open, &QPushButton::clicked, this, &ffmpegPlayer::openFile);
    connect(ui.pushButton_network, &QPushButton::clicked, this, &ffmpegPlayer::playSourceFromEditor);
    connect(ui.lineEdit_source, &QLineEdit::returnPressed, this, &ffmpegPlayer::playSourceFromEditor);

    connect(&m_ffplayer, &FFPlayer::sig_GetOneFrame, this, [this](const QImage &image) {
        if (image.isNull() || m_state == FFPlayer::Stopped || m_state == FFPlayer::Stopping
            || m_state == FFPlayer::Ended || m_state == FFPlayer::Error) {
            return;
        }
        m_frame = image;
        ui.label->update();
    });
    connect(&m_ffplayer, &FFPlayer::sigSetTotalTime, this, [this](qint64 duration) {
        m_duration = duration;
        m_playerSlider.sltSetTotalTime(duration);
    });
    connect(&m_ffplayer, &FFPlayer::sigSetCurrentTime, this, [this](qint64 position) {
        m_position = qMax<qint64>(0, position);
        m_playerSlider.sltSetCurrentTime(position);
    });
    connect(&m_ffplayer, &FFPlayer::sigStateChanged, this, &ffmpegPlayer::setPlaybackState);
    connect(&m_ffplayer, &FFPlayer::sigCapabilitiesChanged, this, [this](bool canSeek, bool canRate) {
        m_canSeek = canSeek;
        m_canRate = canRate;
        m_capabilitiesKnown = true;
        m_playerSlider.sltSetCapabilities(canSeek, canRate);
        updateStatus();
    });
    connect(&m_ffplayer, &FFPlayer::sigRateChanged, &m_playerSlider, &VPlayerSlider::sltSetRate);
    connect(&m_ffplayer, &FFPlayer::sigError, this, [this](const QString &error) {
        m_error = error;
        m_playerSlider.sltSetCurrentTime(m_position);
        updateStatus();
    });

    connect(&m_playerSlider, &VPlayerSlider::sigPlay, &m_ffplayer, &FFPlayer::sltPlay);
    connect(&m_playerSlider, &VPlayerSlider::sigStop, this, &ffmpegPlayer::stopStream);
    connect(&m_playerSlider, &VPlayerSlider::sigSeek, &m_ffplayer, &FFPlayer::sltSeek);
    connect(&m_playerSlider, &VPlayerSlider::sigRateChanged, &m_ffplayer, &FFPlayer::setRate);
    connect(&m_playerSlider, &VPlayerSlider::sigVolumeChanged, &m_ffplayer, &FFPlayer::setVolume);
    connect(&m_playerSlider, &VPlayerSlider::sigMutedChanged, &m_ffplayer, &FFPlayer::setMuted);

    m_ffplayer.setVolume(m_playerSlider.volume());
    m_ffplayer.setMuted(m_playerSlider.isMuted());
    m_playerSlider.setSourceAvailable(false);
    updateStatus();
    qApp->installEventFilter(this);
}

ffmpegPlayer::~ffmpegPlayer()
{
    qApp->removeEventFilter(this);
}

void ffmpegPlayer::playStream(const QString &source)
{
    if (source.trimmed().isEmpty()) {
        m_error = QStringLiteral("请选择文件或输入网络地址。");
        updateStatus();
        return;
    }

    m_lastSource = source;
    ui.lineEdit_source->setText(source);
    m_error.clear();
    m_position = 0;
    m_duration = -1;
    m_canSeek = false;
    m_canRate = false;
    m_capabilitiesKnown = false;
    m_frame = QImage();
    ui.label->update();
    m_playerSlider.clear();
    m_playerSlider.setSourceAvailable(true);
    m_playerSlider.sltSetCurrentTime(0);
    updateStatus();
    m_ffplayer.startPlay(source);

    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
}

void ffmpegPlayer::stopStream()
{
    m_ffplayer.stopPlay();
}

void ffmpegPlayer::openFile()
{
    const QString source = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开媒体文件"), QString(),
        QStringLiteral("媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.webm *.ts *.m3u8 *.mp3 *.aac *.wav *.flac *.ogg);;所有文件 (*)"));
    if (!source.isEmpty()) {
        playStream(source);
    }
}

void ffmpegPlayer::playSourceFromEditor()
{
    playStream(ui.lineEdit_source->text().trimmed());
}

void ffmpegPlayer::setPlaybackState(int state)
{
    if (state < FFPlayer::Stopped || state > FFPlayer::Error) {
        return;
    }
    m_state = static_cast<FFPlayer::PlaybackState>(state);
    if (m_state == FFPlayer::Opening) {
        m_error.clear();
    }
    m_playerSlider.sltSetPlaybackState(state);
    if (m_state == FFPlayer::Stopped) {
        m_position = 0;
        m_playerSlider.sltSetCurrentTime(m_lastSource.isEmpty() ? -1 : 0);
        m_frame = QImage();
        ui.label->update();
    }
    updateStatus();
}

void ffmpegPlayer::updateStatus()
{
    QString text;
    switch (m_state) {
    case FFPlayer::Stopped:
        text = m_lastSource.isEmpty()
            ? QStringLiteral("请选择文件或输入网络地址开始播放。")
            : QStringLiteral("已停止，点击播放可重新播放。");
        break;
    case FFPlayer::Opening:
        text = QStringLiteral("正在打开媒体…");
        break;
    case FFPlayer::Playing:
        text = QStringLiteral("正在播放");
        break;
    case FFPlayer::Paused:
        text = QStringLiteral("已暂停");
        break;
    case FFPlayer::Stopping:
        text = QStringLiteral("正在停止…");
        break;
    case FFPlayer::Ended:
        text = QStringLiteral("播放结束，点击播放可重新播放。");
        break;
    case FFPlayer::Error:
        text = QStringLiteral("播放出错，可重播或打开其他媒体。");
        break;
    }
    if (m_capabilitiesKnown && !m_canSeek && !m_canRate
        && (m_state == FFPlayer::Playing || m_state == FFPlayer::Paused)) {
        text += QStringLiteral("  实时输入：固定 1×，不支持定位；暂停后继续会重新连接到最新画面。");
    }
    if (!m_error.isEmpty()) {
        text += QStringLiteral("  错误：") + m_error;
    }
    ui.label_status->setText(text);
    ui.label_status->setStyleSheet(m_error.isEmpty() ? QString() : QStringLiteral("color: #b42318;"));
}

void ffmpegPlayer::seekBy(qint64 offsetMs)
{
    if (!m_canSeek || m_playerSlider.isDragging()
        || (m_state != FFPlayer::Playing && m_state != FFPlayer::Paused)) {
        return;
    }
    qint64 target = qMax<qint64>(0, m_position + offsetMs);
    if (m_duration > 0) {
        target = qMin(target, m_duration);
    }
    m_ffplayer.seekTo(target);
}

void ffmpegPlayer::toggleFullScreen()
{
    if (isFullScreen()) {
        if (m_wasMaximized) {
            showMaximized();
        } else {
            showNormal();
        }
    } else {
        m_wasMaximized = isMaximized();
        showFullScreen();
    }
}

bool ffmpegPlayer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui.label) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(ui.label);
            painter.fillRect(ui.label->rect(), Qt::black);
            if (!m_frame.isNull()) {
                // 目标区域使用 Qt 逻辑坐标；直接绘制原图，让绘图设备处理 DPR，避免重复缩放和 pixmap 撑大布局。
                const QRectF area(ui.label->contentsRect());
                QSizeF size(m_frame.size());
                size.scale(area.size(), Qt::KeepAspectRatio);
                QRectF target(QPointF(0, 0), size);
                target.moveCenter(area.center());
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(target, m_frame, QRectF(m_frame.rect()));
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                toggleFullScreen();
                mouseEvent->accept();
                return true;
            }
        }
    }

    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) {
        return QWidget::eventFilter(watched, event);
    }
    QWidget *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != this || !isActiveWindow()) {
        return QWidget::eventFilter(watched, event);
    }

    QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
    const int key = keyEvent->key();
    const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
    const bool open = key == Qt::Key_O && (modifiers & Qt::ControlModifier)
        && !(modifiers & (Qt::AltModifier | Qt::MetaModifier));
    const bool editing = qobject_cast<QLineEdit *>(QApplication::focusWidget()) != Q_NULLPTR;
    const bool plain = !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    const bool escape = key == Qt::Key_Escape && isFullScreen();
    const bool playbackKey = plain && !editing && (key == Qt::Key_Space || key == Qt::Key_Left
        || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_M);
    if (!open && !escape && !playbackKey) {
        return QWidget::eventFilter(watched, event);
    }
    keyEvent->accept();
    if (event->type() == QEvent::ShortcutOverride) {
        return true;
    }
    if (open) {
        if (!keyEvent->isAutoRepeat()) {
            openFile();
        }
        return true;
    }
    if (escape) {
        toggleFullScreen();
        return true;
    }
    switch (key) {
    case Qt::Key_Space:
        if (!keyEvent->isAutoRepeat()) {
            m_playerSlider.togglePlayback();
        }
        break;
    case Qt::Key_Left:
        seekBy(-15000);
        break;
    case Qt::Key_Right:
        seekBy(15000);
        break;
    case Qt::Key_Up:
        m_playerSlider.changeVolume(5);
        break;
    case Qt::Key_Down:
        m_playerSlider.changeVolume(-5);
        break;
    case Qt::Key_M:
        if (!keyEvent->isAutoRepeat()) {
            m_playerSlider.toggleMuted();
        }
        break;
    default:
        break;
    }
    return true;
}

void ffmpegPlayer::closeEvent(QCloseEvent *event)
{
    m_ffplayer.stopPlay();
    QWidget::closeEvent(event);
}

void ffmpegPlayer::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        toggleFullScreen();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
