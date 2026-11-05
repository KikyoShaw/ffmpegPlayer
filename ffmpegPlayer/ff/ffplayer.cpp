#include "ffplayer.h"

#include <QByteArray>
#include <QMetaObject>
#include <QMutexLocker>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavformat/avformat.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/samplefmt.h"
#include "libswscale/swscale.h"
}

namespace
{
const AVRational Microseconds = {1, AV_TIME_BASE};
const AVRational Milliseconds = {1, 1000};
const AVRational OutputSamples = {1, 48000};
const qint64 OpenTimeoutUs = 15000000;
const qint64 IoTimeoutUs = 10000000;
const int OutputRate = 48000;
const int OutputChannels = 2;
const int OutputSampleBytes = 4;
const size_t MaxVideoFrames = 12;
const qint64 MaxVideoBytes = 48LL * 1024 * 1024;

qint64 monotonicUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

QString ffError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}

qint64 samplesToMediaUs(qint64 samples, double rate)
{
    return static_cast<qint64>(std::llround(
        static_cast<double>(av_rescale_q(samples, OutputSamples, Microseconds)) * rate));
}

struct Interrupt
{
    std::atomic<bool>& cancelled;
    std::atomic<quint64>& serial;
    quint64 operationSerial = 0;
    qint64 deadlineUs = 0;
    bool watchRequests = false;
    bool timedOut = false;
    bool controlRequest = false;

    Interrupt(std::atomic<bool>& cancel, std::atomic<quint64>& requestSerial)
        : cancelled(cancel), serial(requestSerial) {}

    void begin(qint64 timeout, bool watch)
    {
        operationSerial = serial.load();
        deadlineUs = monotonicUs() + timeout;
        watchRequests = watch;
        timedOut = false;
        controlRequest = false;
    }

    void end()
    {
        deadlineUs = 0;
        watchRequests = false;
    }

    static int callback(void* opaque)
    {
        Interrupt& self = *static_cast<Interrupt*>(opaque);
        if (self.cancelled.load())
            return 1;
        if (self.watchRequests && self.serial.load() != self.operationSerial)
        {
            self.controlRequest = true;
            return 1;
        }
        if (self.deadlineUs && monotonicUs() >= self.deadlineUs)
        {
            self.timedOut = true;
            return 1;
        }
        return 0;
    }
};

struct FrameDeleter
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct AudioFormat
{
    int sampleRate = 0;
    int channels = 0;
    uint64_t layout = 0;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;

    bool valid() const
    {
        return sampleRate > 0 && channels > 0 && layout != 0
            && av_get_bytes_per_sample(format) > 0;
    }

    bool operator==(const AudioFormat& other) const
    {
        return sampleRate == other.sampleRate && channels == other.channels
            && layout == other.layout && format == other.format;
    }
};

AudioFormat audioFormat(const AVFrame* frame, const AVCodecContext* decoder)
{
    AudioFormat result;
    result.sampleRate = frame && frame->sample_rate > 0 ? frame->sample_rate : decoder->sample_rate;
    result.channels = frame && frame->channels > 0 ? frame->channels : decoder->channels;
    result.layout = frame && frame->channel_layout ? frame->channel_layout : decoder->channel_layout;
    if (!result.layout || av_get_channel_layout_nb_channels(result.layout) != result.channels)
        result.layout = av_get_default_channel_layout(result.channels);
    result.format = frame ? static_cast<AVSampleFormat>(frame->format) : decoder->sample_fmt;
    return result;
}

class AudioFilter
{
public:
    ~AudioFilter() { avfilter_graph_free(&graph); }

    bool open(const AudioFormat& input, double rate, QString& error)
    {
        if (!input.valid())
        {
            error = QStringLiteral("音频采样格式尚未就绪");
            return false;
        }
        graph = avfilter_graph_alloc();
        if (!graph)
        {
            error = QStringLiteral("无法分配音频滤镜");
            return false;
        }
        const char* formatName = av_get_sample_fmt_name(input.format);
        const QByteArray args = QStringLiteral("time_base=1/%1:sample_rate=%1:sample_fmt=%2:channel_layout=0x%3")
            .arg(input.sampleRate).arg(QString::fromLatin1(formatName))
            .arg(static_cast<qulonglong>(input.layout), 0, 16).toLatin1();
        int ret = create(&source, "abuffer", "input", args.constData());
        AVFilterContext* tail = source;
        const double factors[2] = {rate == 3.0 ? 1.5 : rate, 2.0};
        const int count = rate == 3.0 ? 2 : 1;
        for (int i = 0; ret >= 0 && i < count; ++i)
        {
            AVFilterContext* tempo = nullptr;
            const QByteArray name = QByteArray("tempo") + QByteArray::number(i);
            const QByteArray value = QByteArray::number(factors[i], 'g', 8);
            ret = create(&tempo, "atempo", name.constData(), value.constData());
            if (ret >= 0)
                ret = avfilter_link(tail, 0, tempo, 0);
            tail = tempo;
        }
        AVFilterContext* format = nullptr;
        if (ret >= 0)
            ret = create(&format, "aformat", "format",
                "sample_fmts=s16:sample_rates=48000:channel_layouts=stereo");
        if (ret >= 0)
            ret = avfilter_link(tail, 0, format, 0);
        if (ret >= 0)
            ret = create(&sink, "abuffersink", "output", nullptr);
        if (ret >= 0)
            ret = avfilter_link(format, 0, sink, 0);
        if (ret >= 0)
            ret = avfilter_graph_config(graph, nullptr);
        if (ret < 0)
        {
            error = QStringLiteral("无法创建音频倍速滤镜：%1").arg(ffError(ret));
            return false;
        }
        spec = input;
        return true;
    }

    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    AudioFormat spec;

private:
    int create(AVFilterContext** context, const char* filter, const char* name, const char* args)
    {
        const AVFilter* definition = avfilter_get_by_name(filter);
        return definition ? avfilter_graph_create_filter(context, definition, name, args, nullptr, graph)
                          : AVERROR_FILTER_NOT_FOUND;
    }
};

struct VideoFrame
{
    FramePtr frame;
    qint64 ptsUs = 0;
    qint64 endUs = 0;
    qint64 bytes = 0;
};

struct SessionResources
{
    explicit SessionResources(MyAudio& output) : audio(output)
    {
        packet = av_packet_alloc();
        decoded = av_frame_alloc();
        filtered = av_frame_alloc();
    }

    ~SessionResources()
    {
        audio.close();
        filter.reset();
        av_frame_free(&filtered);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        sws_freeContext(scaler);
        avcodec_free_context(&videoDecoder);
        avcodec_free_context(&audioDecoder);
        avformat_close_input(&format);
    }

    MyAudio& audio;
    AVFormatContext* format = nullptr;
    AVCodecContext* videoDecoder = nullptr;
    AVCodecContext* audioDecoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* filtered = nullptr;
    SwsContext* scaler = nullptr;
    std::unique_ptr<AudioFilter> filter;
};

// 输出时钟只统计设备实际消费的样本；音轨空档及尾部由可冻结的单调时钟接管。
class MediaClock
{
public:
    explicit MediaClock(MyAudio& audio) : m_audio(audio) {}

    qint64 position() const
    {
        if (paused)
            return m_frozenUs;
        if (audioMaster)
            return m_audioAnchorUs + static_cast<qint64>(
                std::max<qint64>(0, m_audio.playedUSecs() - m_audioBaseUs) * rate);
        return m_wallAnchorUs + static_cast<qint64>((monotonicUs() - m_wallAtUs) * rate);
    }

    void reset(qint64 mediaUs, double newRate, bool pause)
    {
        rate = newRate;
        audioMaster = false;
        paused = pause;
        m_wallAnchorUs = m_frozenUs = mediaUs;
        m_wallAtUs = monotonicUs();
    }

    void setPaused(bool pause)
    {
        if (paused == pause)
            return;
        const qint64 here = position();
        paused = pause;
        m_frozenUs = here;
        if (!pause)
        {
            m_wallAnchorUs = here;
            m_wallAtUs = monotonicUs();
        }
    }

    void useAudio(qint64 mediaUs, qint64 playedBaseUs)
    {
        audioMaster = true;
        m_audioAnchorUs = mediaUs;
        m_audioBaseUs = playedBaseUs;
    }

    void useWall()
    {
        const qint64 here = position();
        audioMaster = false;
        m_wallAnchorUs = here;
        m_wallAtUs = monotonicUs();
    }

    double rate = 1.0;
    bool paused = false;
    bool audioMaster = false;

private:
    MyAudio& m_audio;
    qint64 m_wallAnchorUs = 0;
    qint64 m_wallAtUs = monotonicUs();
    qint64 m_frozenUs = 0;
    qint64 m_audioAnchorUs = 0;
    qint64 m_audioBaseUs = 0;
};

bool openDecoder(AVFormatContext* format, int stream, AVCodecContext** output, QString& error)
{
    if (stream < 0)
        return true;
    AVStream* input = format->streams[stream];
    const AVCodec* codec = avcodec_find_decoder(input->codecpar->codec_id);
    if (!codec)
    {
        error = QStringLiteral("媒体使用了不支持的解码格式");
        return false;
    }
    *output = avcodec_alloc_context3(codec);
    if (!*output)
    {
        error = QStringLiteral("无法分配解码器");
        return false;
    }
    int ret = avcodec_parameters_to_context(*output, input->codecpar);
    if (ret >= 0)
    {
        (*output)->pkt_timebase = input->time_base;
        ret = avcodec_open2(*output, codec, nullptr);
    }
    if (ret < 0)
    {
        error = QStringLiteral("无法打开解码器：%1").arg(ffError(ret));
        return false;
    }
    return true;
}

bool hasSeekSupport(const AVFormatContext* format)
{
    if (format->ctx_flags & AVFMTCTX_UNSEEKABLE)
        return false;
    const QByteArray demuxer = format->iformat ? format->iformat->name : "";
    // 分段直播的播放列表本身可能可 seek，不能将它当作可定位的媒体时间轴。
    if ((demuxer.contains("hls") || demuxer.contains("applehttp") || demuxer == "dash")
        && (format->duration == AV_NOPTS_VALUE || format->duration <= 0))
        return false;
    if (format->pb && (format->pb->seekable & AVIO_SEEKABLE_NORMAL))
        return true;
    // HLS/RTSP 点播等输入可由 demuxer 提供时间定位，不能仅按 URL 或未知时长归类。
    return format->duration != AV_NOPTS_VALUE && format->duration > 0
        && format->iformat && (format->iformat->read_seek || format->iformat->read_seek2);
}

qint64 frameTimestamp(const AVFrame* frame, const AVStream* stream, qint64 originUs, qint64 fallback)
{
    const int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? frame->best_effort_timestamp : frame->pts;
    return timestamp == AV_NOPTS_VALUE ? fallback
        : av_rescale_q(timestamp, stream->time_base, Microseconds) - originUs;
}
}

FFPlayer::FFPlayer()
{
    connect(this, &QThread::finished, this, &FFPlayer::threadFinished, Qt::QueuedConnection);
}

FFPlayer::~FFPlayer()
{
    {
        QMutexLocker lock(&m_controlMutex);
        m_destroying = true;
        m_pendingStart = false;
        m_acceptingRequests = false;
        ++m_session;
        m_cancelled.store(true);
        ++m_controlSerial;
        m_controlChanged.wakeAll();
    }
    // 解码退出不依赖 GUI 事件循环；音频门面有独立事件循环，可在这里安全收尾。
    wait();
    m_audio.close();
}

void FFPlayer::post(quint64 session, quint64 generation, const std::function<void()>& action)
{
    QMetaObject::invokeMethod(this, [this, session, generation, action]() {
        {
            QMutexLocker lock(&m_controlMutex);
            if (m_destroying || session != m_session || generation != m_requests.generation)
                return;
        }
        action();
    }, Qt::QueuedConnection);
}

void FFPlayer::postFrame(quint64 session, quint64 generation, const QImage& image)
{
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying || session != m_session || generation != m_requests.generation)
            return;
        m_pendingImage = image;
        m_imageSession = session;
        m_imageGeneration = generation;
        if (m_imageQueued)
            return;
        m_imageQueued = true;
    }
    // GUI 阻塞时只保留最新画面，避免 queued 调用本身积压整段视频。
    QMetaObject::invokeMethod(this, [this]() {
        QImage image;
        {
            QMutexLocker lock(&m_controlMutex);
            m_imageQueued = false;
            if (m_destroying || m_imageSession != m_session
                || m_imageGeneration != m_requests.generation)
            {
                m_pendingImage = QImage();
                return;
            }
            image.swap(m_pendingImage);
        }
        emit sig_GetOneFrame(image);
    }, Qt::QueuedConnection);
}

void FFPlayer::postState(quint64 session, quint64 generation, PlaybackState state)
{
    post(session, generation, [this, state]() {
        m_state.store(state);
        emit sigStateChanged(state);
    });
}

void FFPlayer::postPosition(quint64 session, quint64 generation, qint64 value)
{
    value = std::max<qint64>(0, value);
    post(session, generation, [this, value]() {
        m_positionMs.store(value);
        emit sigSetCurrentTime(value);
    });
}

void FFPlayer::postError(quint64 session, quint64 generation, const QString& error)
{
    post(session, generation, [this, error]() { emit sigError(error); });
}

FFPlayer::Requests FFPlayer::requests() const
{
    QMutexLocker lock(&m_controlMutex);
    Requests copy = m_requests;
    copy.serial = m_controlSerial.load();
    return copy;
}

void FFPlayer::waitForRequest(unsigned long milliseconds)
{
    QMutexLocker lock(&m_controlMutex);
    if (!m_cancelled.load())
        m_controlChanged.wait(&m_controlMutex, milliseconds);
}

void FFPlayer::startPlay(const QString& source)
{
    quint64 session;
    quint64 generation;
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying)
            return;
        m_source = source.trimmed();
        session = ++m_session;
        generation = ++m_requests.generation;
        m_requests.paused = false;
        m_requests.seekMs = 0;
        m_pendingStart = true;
        m_stopPending = false;
        m_liveDisconnected = false;
        m_acceptingRequests = false;
        m_cancelled.store(true);
        ++m_controlSerial;
        m_controlChanged.wakeAll();
    }
    m_state.store(Opening);
    m_seekable.store(false);
    m_rateable.store(false);
    m_positionMs.store(0);
    m_durationMs.store(-1);
    postState(session, generation, Opening);
    postPosition(session, generation, 0);
    postFrame(session, generation, QImage());
    post(session, generation, [this]() { emit sigSetTotalTime(-1); });
    post(session, generation, [this]() { emit sigCapabilitiesChanged(false, false); });
    launchPending();
}

void FFPlayer::launchPending()
{
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying || m_workerActive || !m_pendingStart)
            return;
        m_pendingStart = false;
        m_workerActive = true;
        m_acceptingRequests = true;
        m_runSource = m_source;
        m_runSession = m_session;
        m_cancelled.store(false);
    }
    start();
}

void FFPlayer::threadFinished()
{
    bool restart;
    bool stopped;
    quint64 session;
    quint64 generation;
    {
        QMutexLocker lock(&m_controlMutex);
        m_workerActive = false;
        restart = m_pendingStart && !m_destroying;
        stopped = m_stopPending && !restart && !m_destroying;
        m_stopPending = false;
        session = m_session;
        generation = m_requests.generation;
    }
    if (restart)
        launchPending();
    else if (stopped)
    {
        postPosition(session, generation, 0);
        postFrame(session, generation, QImage());
        postState(session, generation, Stopped);
    }
}

void FFPlayer::stopPlay()
{
    quint64 session;
    quint64 generation;
    bool active;
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying)
            return;
        session = ++m_session;
        generation = ++m_requests.generation;
        active = m_workerActive;
        m_pendingStart = false;
        m_stopPending = active;
        m_acceptingRequests = false;
        m_liveDisconnected = false;
        m_requests.paused = false;
        m_cancelled.store(true);
        ++m_controlSerial;
        m_controlChanged.wakeAll();
    }
    m_seekable.store(false);
    m_rateable.store(false);
    m_state.store(active ? Stopping : Stopped);
    post(session, generation, [this]() { emit sigCapabilitiesChanged(false, false); });
    postState(session, generation, active ? Stopping : Stopped);
    if (!active)
    {
        postPosition(session, generation, 0);
        postFrame(session, generation, QImage());
    }
}

bool FFPlayer::isPlay() const
{
    const int state = m_state.load();
    return state == Opening || state == Playing || state == Paused;
}

bool FFPlayer::isSuspend() const { return m_state.load() == Paused; }
qint64 FFPlayer::position() const { return m_positionMs.load(); }
qint64 FFPlayer::duration() const { return m_durationMs.load(); }
double FFPlayer::rate() const { return m_actualRate.load(); }
bool FFPlayer::canSeek() const { return m_seekable.load(); }
bool FFPlayer::canRate() const { return m_rateable.load(); }

void FFPlayer::setVolume(int percent)
{
    m_audio.setVolume(std::max(0, std::min(100, percent)));
}

void FFPlayer::setMuted(bool muted) { m_audio.setMuted(muted); }

void FFPlayer::setRate(double value)
{
    const double choices[] = {0.5, 1.0, 1.5, 2.0, 3.0};
    double selected = 0;
    for (double choice : choices)
        if (std::abs(choice - value) < 0.0001)
            selected = choice;
    if (!selected)
        return;
    quint64 session;
    quint64 generation;
    bool effective;
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying)
            return;
        if (m_requests.rate == selected)
            return;
        m_requests.rate = selected;
        ++m_requests.rateRevision;
        effective = m_acceptingRequests && m_rateable.load();
        if (effective)
            ++m_requests.generation;
        session = m_session;
        generation = m_requests.generation;
        if (effective)
            ++m_controlSerial;
        m_controlChanged.wakeAll();
    }
    if (!effective && m_state.load() != Opening)
    {
        const double actual = m_actualRate.load();
        post(session, generation, [this, actual]() { emit sigRateChanged(actual); });
    }
}

void FFPlayer::sltPlay(bool play)
{
    QString reopen;
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying)
            return;
        if (play && ((!m_acceptingRequests && !m_pendingStart) || m_liveDisconnected))
            reopen = m_source;
        else if (m_acceptingRequests || m_pendingStart)
        {
            if (m_requests.paused == !play)
                return;
            m_requests.paused = !play;
            ++m_controlSerial;
            m_controlChanged.wakeAll();
        }
    }
    if (!reopen.isEmpty())
        startPlay(reopen);
}

void FFPlayer::sltSeek(int pos, int base)
{
    const qint64 total = duration();
    if (base <= 0 || total < 0)
        return;
    const qint64 clamped = std::max(0, std::min(base, pos));
    seekTo(av_rescale(clamped, total, base));
}

void FFPlayer::seekTo(qint64 value)
{
    QMutexLocker lock(&m_controlMutex);
    if (m_destroying || !m_acceptingRequests || !m_seekable.load())
        return;
    const qint64 total = m_durationMs.load();
    value = std::max<qint64>(0, value);
    if (total >= 0)
        value = std::min(value, total);
    m_requests.seekMs = std::min(value, (std::numeric_limits<qint64>::max)() / 1000);
    ++m_requests.seekRevision;
    ++m_requests.generation;
    ++m_controlSerial;
    m_controlChanged.wakeAll();
}

void FFPlayer::run()
{
    QString source;
    quint64 session;
    {
        QMutexLocker lock(&m_controlMutex);
        source = m_runSource;
        session = m_runSession;
    }
    quint64 generation = requests().generation;
    QString failure;
    PlaybackState finalState = Ended;
    qint64 finalPositionUs = 0;
    bool livePause = false;

    // 所有 FFmpeg 对象都只存在于 run 的作用域，退出通知在资源清理完成后发送。
    const auto execute = [&]() -> bool {
        Interrupt interrupt(m_cancelled, m_controlSerial);
        SessionResources r(m_audio);
        if (!r.packet || !r.decoded || !r.filtered)
        {
            failure = QStringLiteral("无法分配解码缓冲");
            return false;
        }
        if (source.isEmpty())
        {
            failure = QStringLiteral("媒体地址为空");
            return false;
        }
        static std::once_flag networkInit;
        std::call_once(networkInit, []() { avformat_network_init(); });
        r.format = avformat_alloc_context();
        if (!r.format)
        {
            failure = QStringLiteral("无法分配媒体上下文");
            return false;
        }
        r.format->interrupt_callback.callback = &Interrupt::callback;
        r.format->interrupt_callback.opaque = &interrupt;
        QByteArray encodedSource = source.toUtf8();
        const QUrl url(source);
        if (url.isLocalFile())
            encodedSource = url.toLocalFile().toUtf8();
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", "10000000", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);
        interrupt.begin(OpenTimeoutUs, false);
        int ret = avformat_open_input(&r.format, encodedSource.constData(), nullptr, &options);
        interrupt.end();
        av_dict_free(&options);
        if (ret < 0)
        {
            failure = interrupt.timedOut ? QStringLiteral("打开媒体超时")
                : QStringLiteral("无法打开媒体：%1").arg(ffError(ret));
            return false;
        }
        if (m_cancelled.load())
            return false;
        interrupt.begin(OpenTimeoutUs, false);
        ret = avformat_find_stream_info(r.format, nullptr);
        interrupt.end();
        if (ret < 0)
        {
            failure = interrupt.timedOut ? QStringLiteral("探测媒体超时")
                : QStringLiteral("无法读取媒体信息：%1").arg(ffError(ret));
            return false;
        }
        if (m_cancelled.load())
            return false;

        int videoStream = av_find_best_stream(r.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream >= 0 && (r.format->streams[videoStream]->disposition & AV_DISPOSITION_ATTACHED_PIC))
            videoStream = -1;
        const int audioStream = av_find_best_stream(r.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (videoStream < 0 && audioStream < 0)
        {
            failure = QStringLiteral("媒体不包含可解码的音频或视频");
            return false;
        }
        if (!openDecoder(r.format, videoStream, &r.videoDecoder, failure)
            || !openDecoder(r.format, audioStream, &r.audioDecoder, failure))
            return false;
        if (audioStream >= 0 && !m_audio.open(OutputRate, OutputChannels, &failure))
        {
            failure = QStringLiteral("无法打开 48000 Hz 双声道 S16 音频输出：%1").arg(failure);
            return false;
        }

        const bool seekable = hasSeekSupport(r.format);
        const qint64 totalMs = r.format->duration == AV_NOPTS_VALUE || r.format->duration < 0
            ? -1 : av_rescale_q(r.format->duration, Microseconds, Milliseconds);
        qint64 originUs = r.format->start_time;
        if (originUs == AV_NOPTS_VALUE)
        {
            originUs = AV_NOPTS_VALUE;
            for (unsigned int i = 0; i < r.format->nb_streams; ++i)
            {
                if (static_cast<int>(i) != videoStream && static_cast<int>(i) != audioStream)
                    continue;
                const AVStream* stream = r.format->streams[i];
                if (stream->start_time != AV_NOPTS_VALUE)
                {
                    const qint64 start = av_rescale_q(stream->start_time, stream->time_base, Microseconds);
                    originUs = originUs == AV_NOPTS_VALUE ? start : std::min(originUs, start);
                }
            }
            if (originUs == AV_NOPTS_VALUE)
                originUs = 0;
        }

        qint64 declaredAudioEndUs = -1;
        if (audioStream >= 0)
        {
            const AVStream* stream = r.format->streams[audioStream];
            if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
            {
                const qint64 start = stream->start_time == AV_NOPTS_VALUE ? 0
                    : av_rescale_q(stream->start_time, stream->time_base, Microseconds) - originUs;
                declaredAudioEndUs = start + av_rescale_q(stream->duration, stream->time_base, Microseconds);
            }
        }
        Requests control = requests();
        quint64 seekRevision = control.seekRevision;
        quint64 rateRevision = control.rateRevision;
        generation = control.generation;
        double actualRate = seekable ? control.rate : 1.0;
        bool paused = control.paused;
        bool previewNeeded = paused && videoStream >= 0;
        bool resumeNeedsSeek = false;
        qint64 discardBeforeUs = 0;
        qint64 nextVideoUs = 0;
        qint64 nextAudioUs = 0;
        qint64 lastVideoEndUs = 0;
        qint64 lastAudioEndUs = 0;
        qint64 videoBytes = 0;
        qint64 filterAnchorUs = 0;
        qint64 filteredSamples = 0;
        bool filterHasAnchor = false;
        bool filteredReady = false;
        int filteredOffset = 0;
        qint64 filteredStartUs = 0;
        FramePtr audioInput;
        bool havePacket = false;
        bool demuxEof = false;
        bool videoReceive = false;
        bool audioReceive = false;
        bool videoFlushSent = false;
        bool audioFlushSent = false;
        bool videoEof = videoStream < 0;
        bool audioEof = audioStream < 0;
        bool filterFlushSent = false;
        bool filterEof = audioStream < 0;
        bool filterWouldBlock = true;
        bool interruptedRead = false;
        qint64 readIdleSince = 0;
        qint64 lastPositionAt = 0;
        std::deque<VideoFrame> video;
        MediaClock clock(m_audio);
        clock.reset(0, actualRate, paused);
        m_audio.setPaused(paused);

        const auto disconnectLive = [&]() -> bool {
            QMutexLocker lock(&m_controlMutex);
            if (m_cancelled.load() || session != m_session || !m_requests.paused)
                return false;
            // 在关闭连接前发布重连意图，关闭期间到达的恢复请求也会开启新会话。
            m_liveDisconnected = true;
            m_acceptingRequests = false;
            return true;
        };
        const auto publishCapabilities = [&]() {
            post(session, generation, [this, seekable]() {
                m_seekable.store(seekable);
                m_rateable.store(seekable);
                emit sigCapabilitiesChanged(seekable, seekable);
            });
        };
        const auto publishRate = [&]() {
            const double value = actualRate;
            post(session, generation, [this, value]() {
                m_actualRate.store(value);
                emit sigRateChanged(value);
            });
        };
        const auto publishDuration = [&]() {
            post(session, generation, [this, totalMs]() {
                m_durationMs.store(totalMs);
                emit sigSetTotalTime(totalMs);
            });
        };
        publishCapabilities();
        publishDuration();
        publishRate();
        postState(session, generation, paused ? Paused : Playing);
        if (paused && !seekable && disconnectLive())
        {
            livePause = true;
            return true;
        }

        const auto present = [&](VideoFrame& item) -> bool {
            const AVFrame* frame = item.frame.get();
            r.scaler = sws_getCachedContext(r.scaler, frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!r.scaler)
            {
                failure = QStringLiteral("无法创建视频像素转换器");
                return false;
            }
            QImage image(frame->width, frame->height, QImage::Format_RGB32);
            if (image.isNull())
            {
                failure = QStringLiteral("无法分配视频画面");
                return false;
            }
            uint8_t* data[4] = {image.bits(), nullptr, nullptr, nullptr};
            int strides[4] = {image.bytesPerLine(), 0, 0, 0};
            if (sws_scale(r.scaler, frame->data, frame->linesize, 0, frame->height, data, strides) <= 0)
            {
                failure = QStringLiteral("视频像素转换失败");
                return false;
            }
            // QImage 自有内存，解码器复用 AVFrame/缓冲不影响排队给 GUI 的画面。
            postFrame(session, generation, image);
            return true;
        };

        const auto serviceVideo = [&]() -> bool {
            if (paused || video.empty())
                return true;
            const qint64 now = clock.position();
            while (video.size() > 1 && video[1].ptsUs <= now + 1000)
            {
                videoBytes -= video.front().bytes;
                video.pop_front();
            }
            if (!video.empty() && video.front().ptsUs <= now + 1000)
            {
                if (!present(video.front()))
                    return false;
                videoBytes -= video.front().bytes;
                video.pop_front();
            }
            return true;
        };

        const auto resetDecode = [&](qint64 targetUs, double newRate,
                                     std::unique_ptr<AudioFilter> replacement) {
            avformat_flush(r.format);
            if (r.format->pb)
            {
                r.format->pb->error = 0;
                r.format->pb->eof_reached = 0;
            }
            if (r.videoDecoder)
                avcodec_flush_buffers(r.videoDecoder);
            if (r.audioDecoder)
                avcodec_flush_buffers(r.audioDecoder);
            r.filter = std::move(replacement);
            av_packet_unref(r.packet);
            av_frame_unref(r.decoded);
            av_frame_unref(r.filtered);
            audioInput.reset();
            video.clear();
            videoBytes = 0;
            havePacket = demuxEof = videoReceive = audioReceive = false;
            videoFlushSent = audioFlushSent = filterFlushSent = false;
            videoEof = videoStream < 0;
            audioEof = filterEof = audioStream < 0;
            filteredReady = filterHasAnchor = false;
            filterWouldBlock = true;
            filteredOffset = 0;
            filteredSamples = 0;
            actualRate = newRate;
            discardBeforeUs = targetUs;
            nextVideoUs = nextAudioUs = targetUs;
            lastVideoEndUs = lastAudioEndUs = targetUs;
            m_audio.clear();
            m_audio.setPaused(paused);
            clock.reset(targetUs, actualRate, paused);
            previewNeeded = paused && videoStream >= 0;
            resumeNeedsSeek = previewNeeded;
            interruptedRead = false;
            readIdleSince = 0;
            lastPositionAt = 0;
        };

        const auto relocate = [&](qint64 targetUs, double newRate) -> bool {
            std::unique_ptr<AudioFilter> candidate;
            if (r.audioDecoder)
            {
                const AudioFormat spec = r.filter ? r.filter->spec : audioFormat(nullptr, r.audioDecoder);
                // 切速先准备候选滤镜；失败不碰当前定位、音频队列和实际倍率。
                if (spec.valid())
                {
                    candidate.reset(new AudioFilter);
                    if (!candidate->open(spec, newRate, failure))
                        return false;
                }
                else if (newRate != actualRate)
                {
                    failure = QStringLiteral("音频格式尚未就绪，暂时无法切换倍速");
                    return false;
                }
            }
            const bool outputWasPaused = paused;
            const bool clockWasPaused = clock.paused;
            m_audio.setPaused(true);
            clock.setPaused(true);
            interrupt.begin(IoTimeoutUs, false);
            // 一次 demux 定位同时影响音视频，不能对两个 stream 分别 seek。
            const int result = av_seek_frame(r.format, -1, targetUs + originUs, AVSEEK_FLAG_BACKWARD);
            interrupt.end();
            if (result < 0)
            {
                m_audio.setPaused(outputWasPaused);
                clock.setPaused(clockWasPaused);
                failure = interrupt.timedOut ? QStringLiteral("定位媒体超时")
                    : QStringLiteral("定位失败：%1").arg(ffError(result));
                return false;
            }
            resetDecode(targetUs, newRate, std::move(candidate));
            return true;
        };

        const auto queueVideo = [&](const AVFrame* frame) -> bool {
            const AVStream* stream = r.format->streams[videoStream];
            const qint64 pts = frameTimestamp(frame, stream, originUs, nextVideoUs);
            qint64 frameDuration = frame->pkt_duration > 0
                ? av_rescale_q(frame->pkt_duration, stream->time_base, Microseconds) : 0;
            if (frameDuration <= 0)
            {
                const AVRational fps = av_guess_frame_rate(r.format, r.format->streams[videoStream], nullptr);
                frameDuration = fps.num > 0 && fps.den > 0
                    ? av_rescale_q(1, av_inv_q(fps), Microseconds) : 40000;
            }
            frameDuration = std::max<qint64>(1, frameDuration);
            nextVideoUs = pts + frameDuration;
            lastVideoEndUs = std::max(lastVideoEndUs, nextVideoUs);
            if (pts < discardBeforeUs)
                return true;
            FramePtr copy(av_frame_clone(frame));
            if (!copy)
            {
                failure = QStringLiteral("无法保留视频解码帧");
                return false;
            }
            VideoFrame item;
            item.frame = std::move(copy);
            item.ptsUs = pts;
            item.endUs = nextVideoUs;
            const int bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, 1);
            item.bytes = std::max(1, bytes);
            if (paused && previewNeeded)
            {
                if (!present(item))
                    return false;
                previewNeeded = false;
                resumeNeedsSeek = true;
                return true;
            }
            // 保留最近待播帧和最新尾帧；视频队列满时仍继续寻找音频，不等待画面 PTS。
            while (!video.empty() && (video.size() >= MaxVideoFrames
                || videoBytes + item.bytes > MaxVideoBytes))
            {
                videoBytes -= video.back().bytes;
                video.pop_back();
            }
            const auto at = std::upper_bound(video.begin(), video.end(), pts,
                [](qint64 time, const VideoFrame& value) { return time < value.ptsUs; });
            videoBytes += item.bytes;
            video.insert(at, std::move(item));
            return true;
        };

        const auto prepareAudio = [&](const AVFrame* frame) -> bool {
            const AudioFormat spec = audioFormat(frame, r.audioDecoder);
            if (!spec.valid())
            {
                failure = QStringLiteral("解码器返回了无效音频格式");
                return false;
            }
            const qint64 start = frameTimestamp(frame, r.format->streams[audioStream], originUs, nextAudioUs);
            const AVRational samples = {1, spec.sampleRate};
            const qint64 end = start + av_rescale_q(frame->nb_samples, samples, Microseconds);
            nextAudioUs = end;
            lastAudioEndUs = std::max(lastAudioEndUs, end);
            if (end <= discardBeforeUs || frame->nb_samples <= 0)
                return true;
            audioInput.reset(av_frame_clone(frame));
            if (!audioInput)
            {
                failure = QStringLiteral("无法保留音频解码帧");
                return false;
            }
            int skip = 0;
            if (start < discardBeforeUs)
                skip = static_cast<int>(std::min<qint64>(frame->nb_samples,
                    av_rescale_rnd(discardBeforeUs - start, spec.sampleRate, AV_TIME_BASE, AV_ROUND_UP)));
            const bool planar = av_sample_fmt_is_planar(spec.format) != 0;
            const int planes = planar ? spec.channels : 1;
            const int step = av_get_bytes_per_sample(spec.format) * (planar ? 1 : spec.channels);
            for (int channel = 0; channel < planes; ++channel)
            {
                audioInput->extended_data[channel] += static_cast<qint64>(skip) * step;
                if (channel < AV_NUM_DATA_POINTERS)
                    audioInput->data[channel] = audioInput->extended_data[channel];
            }
            audioInput->nb_samples -= skip;
            audioInput->sample_rate = spec.sampleRate;
            audioInput->channels = spec.channels;
            audioInput->channel_layout = spec.layout;
            const qint64 trimmedStart = start + av_rescale_q(skip, samples, Microseconds);
            audioInput->pts = av_rescale_q(trimmedStart, Microseconds, samples);
            if (audioInput->nb_samples <= 0)
                audioInput.reset();
            return true;
        };

        while (!m_cancelled.load())
        {
            control = requests();
            const bool newSeek = control.seekRevision != seekRevision;
            const bool newRate = control.rateRevision != rateRevision;
            if ((newSeek || newRate) && seekable)
            {
                const qint64 target = newSeek ? control.seekMs * 1000 : std::max<qint64>(0, clock.position());
                const double requestedRate = newRate ? control.rate : actualRate;
                const double oldRate = actualRate;
                paused = control.paused;
                generation = control.generation;
                const bool changed = relocate(target, requestedRate);
                if (!changed && !m_cancelled.load())
                {
                    postError(session, generation, failure);
                    failure.clear();
                    if (newRate)
                    {
                        QMutexLocker lock(&m_controlMutex);
                        if (session == m_session && m_requests.rateRevision == control.rateRevision)
                            m_requests.rate = oldRate;
                    }
                    // 失败的定位仍恢复请求的暂停状态，时钟不能从 UI 请求时提前跳走。
                    m_audio.setPaused(paused);
                    clock.setPaused(paused);
                }
                seekRevision = control.seekRevision;
                rateRevision = control.rateRevision;
                publishCapabilities();
                publishDuration();
                publishRate();
                postState(session, generation, paused ? Paused : Playing);
                postPosition(session, generation, clock.position() / 1000);
            }
            else if (newRate)
            {
                rateRevision = control.rateRevision;
                publishRate();
            }

            if (control.paused != paused)
            {
                paused = control.paused;
                if (paused && !seekable && disconnectLive())
                {
                    m_audio.setPaused(true);
                    clock.setPaused(true);
                    finalPositionUs = clock.position();
                    livePause = true;
                    return true;
                }
                m_audio.setPaused(paused);
                clock.setPaused(paused);
                if (!paused && (resumeNeedsSeek || interruptedRead))
                {
                    const qint64 target = std::max<qint64>(0, clock.position());
                    // 暂停预览不投递音频；恢复时在同一时间重新装填，避免跳过预览期间的音频。
                    if (!relocate(target, actualRate))
                        return false;
                    previewNeeded = false;
                    resumeNeedsSeek = false;
                }
                readIdleSince = 0;
                postState(session, generation, paused ? Paused : Playing);
            }
            if (!paused && interruptedRead)
            {
                if (seekable)
                {
                    if (!relocate(std::max<qint64>(0, clock.position()), actualRate))
                        return false;
                }
                else
                {
                    quint64 reconnectSession;
                    quint64 reconnectGeneration;
                    {
                        QMutexLocker lock(&m_controlMutex);
                        if (session != m_session || m_cancelled.load())
                            return false;
                        if (m_requests.paused)
                            continue;
                        // 暂停与恢复可能在一次阻塞读取中合并；被中断的直播连接仍需重开。
                        m_pendingStart = true;
                        m_acceptingRequests = false;
                        m_liveDisconnected = false;
                        reconnectSession = ++m_session;
                        reconnectGeneration = ++m_requests.generation;
                        m_cancelled.store(true);
                    }
                    postState(reconnectSession, reconnectGeneration, Opening);
                    return false;
                }
            }
            if (paused && !previewNeeded)
            {
                waitForRequest(20);
                continue;
            }
            if (!serviceVideo())
                return false;
            if (!paused && monotonicUs() - lastPositionAt >= 100000)
            {
                qint64 pos = clock.position() / 1000;
                if (totalMs >= 0)
                    pos = std::min(pos, totalMs);
                postPosition(session, generation, pos);
                lastPositionAt = monotonicUs();
            }
            if (audioStream >= 0)
            {
                const QString audioError = m_audio.errorString();
                if (!audioError.isEmpty())
                {
                    failure = QStringLiteral("音频输出失败：%1").arg(audioError);
                    return false;
                }
            }

            if (filteredReady)
            {
                const qint64 sampleTime = filteredStartUs + samplesToMediaUs(filteredOffset, actualRate);
                // 音轨可能晚于视频开始；在首批音频到达前维持墙钟，避免视频永远等音频。
                if (!clock.audioMaster && sampleTime > clock.position() + 2000)
                {
                    waitForRequest(5);
                    continue;
                }
                if (!clock.audioMaster && clock.position() - sampleTime > 100000)
                {
                    const qint64 late = clock.position() - sampleTime;
                    const qint64 skip = static_cast<qint64>(late * OutputRate / (AV_TIME_BASE * actualRate));
                    filteredOffset += static_cast<int>(std::min<qint64>(skip,
                        r.filtered->nb_samples - filteredOffset));
                }
                const int count = std::min(960, r.filtered->nb_samples - filteredOffset);
                if (count > 0)
                {
                    const QByteArray pcm(reinterpret_cast<const char*>(r.filtered->data[0])
                        + static_cast<qint64>(filteredOffset) * OutputSampleBytes, count * OutputSampleBytes);
                    const qint64 playedBefore = m_audio.playedUSecs();
                    const int accepted = m_audio.enqueue(pcm);
                    if (accepted < 0 || accepted > pcm.size() || accepted % OutputSampleBytes)
                    {
                        failure = QStringLiteral("音频输出队列返回了无效的写入长度");
                        return false;
                    }
                    if (accepted > 0)
                    {
                        if (!clock.audioMaster)
                            clock.useAudio(filteredStartUs + samplesToMediaUs(filteredOffset, actualRate), playedBefore);
                        filteredOffset += accepted / OutputSampleBytes;
                    }
                    else
                        waitForRequest(5);
                }
                if (filteredOffset >= r.filtered->nb_samples)
                {
                    av_frame_unref(r.filtered);
                    filteredReady = false;
                }
                continue;
            }

            if (r.filter && !filterEof)
            {
                ret = av_buffersink_get_frame(r.filter->sink, r.filtered);
                filterWouldBlock = ret == AVERROR(EAGAIN);
                if (ret == 0)
                {
                    if (r.filtered->format != AV_SAMPLE_FMT_S16 || r.filtered->sample_rate != OutputRate
                        || r.filtered->channels != OutputChannels || r.filtered->nb_samples < 0
                        || (r.filtered->nb_samples > 0 && !r.filtered->data[0]))
                    {
                        failure = QStringLiteral("音频滤镜未返回 48000 Hz 双声道 S16 数据");
                        return false;
                    }
                    filteredStartUs = filterAnchorUs + samplesToMediaUs(filteredSamples, actualRate);
                    filteredSamples += r.filtered->nb_samples;
                    filteredOffset = 0;
                    filteredReady = true;
                    continue;
                }
                if (ret == AVERROR_EOF)
                    filterEof = true;
                else if (ret != AVERROR(EAGAIN))
                {
                    failure = QStringLiteral("读取音频滤镜失败：%1").arg(ffError(ret));
                    return false;
                }
            }
            if (!paused && clock.audioMaster && !filteredReady
                && (filterWouldBlock || filterEof) && m_audio.isDrained())
                clock.useWall();

            if (audioInput)
            {
                const AudioFormat spec = audioFormat(audioInput.get(), r.audioDecoder);
                if (r.filter && !(r.filter->spec == spec))
                {
                    if (!filterFlushSent)
                    {
                        ret = av_buffersrc_add_frame_flags(r.filter->source, nullptr, 0);
                        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
                        {
                            failure = QStringLiteral("排空旧音频格式失败：%1").arg(ffError(ret));
                            return false;
                        }
                        filterFlushSent = ret != AVERROR(EAGAIN);
                        continue;
                    }
                    if (!filterEof)
                    {
                        waitForRequest(1);
                        continue;
                    }
                    r.filter.reset();
                }
                if (!r.filter)
                {
                    r.filter.reset(new AudioFilter);
                    if (!r.filter->open(spec, actualRate, failure))
                        return false;
                    filterEof = filterFlushSent = false;
                    filterHasAnchor = false;
                    filteredSamples = 0;
                }
                if (!filterHasAnchor)
                {
                    const AVRational inputSamples = {1, spec.sampleRate};
                    filterAnchorUs = av_rescale_q(audioInput->pts, inputSamples, Microseconds);
                    filterHasAnchor = true;
                }
                ret = av_buffersrc_add_frame_flags(r.filter->source, audioInput.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
                if (ret == AVERROR(EAGAIN))
                {
                    waitForRequest(1);
                    continue;
                }
                if (ret < 0)
                {
                    failure = QStringLiteral("提交音频滤镜失败：%1").arg(ffError(ret));
                    return false;
                }
                audioInput.reset();
                continue;
            }

            if (audioReceive)
            {
                ret = avcodec_receive_frame(r.audioDecoder, r.decoded);
                if (ret == 0)
                {
                    const bool prepared = prepareAudio(r.decoded);
                    av_frame_unref(r.decoded);
                    if (!prepared)
                        return false;
                    continue;
                }
                if (ret == AVERROR_EOF)
                    audioEof = true;
                else if (ret != AVERROR(EAGAIN))
                {
                    failure = QStringLiteral("音频解码失败：%1").arg(ffError(ret));
                    return false;
                }
                audioReceive = false;
            }
            if (videoReceive)
            {
                ret = avcodec_receive_frame(r.videoDecoder, r.decoded);
                if (ret == 0)
                {
                    const bool queued = queueVideo(r.decoded);
                    av_frame_unref(r.decoded);
                    if (!queued)
                        return false;
                    continue;
                }
                if (ret == AVERROR_EOF)
                    videoEof = true;
                else if (ret != AVERROR(EAGAIN))
                {
                    failure = QStringLiteral("视频解码失败：%1").arg(ffError(ret));
                    return false;
                }
                videoReceive = false;
            }

            if (havePacket)
            {
                AVCodecContext* decoder = nullptr;
                bool* receive = nullptr;
                if (r.packet->stream_index == videoStream)
                {
                    decoder = r.videoDecoder;
                    receive = &videoReceive;
                }
                else if (r.packet->stream_index == audioStream && !(paused && previewNeeded))
                {
                    decoder = r.audioDecoder;
                    receive = &audioReceive;
                }
                if (decoder)
                {
                    ret = avcodec_send_packet(decoder, r.packet);
                    if (ret == AVERROR(EAGAIN))
                    {
                        *receive = true;
                        continue;
                    }
                    if (ret < 0)
                    {
                        failure = QStringLiteral("提交媒体数据失败：%1").arg(ffError(ret));
                        return false;
                    }
                    *receive = true;
                }
                else if (paused && previewNeeded && r.packet->stream_index == audioStream)
                    resumeNeedsSeek = true;
                av_packet_unref(r.packet);
                havePacket = false;
                continue;
            }

            if (demuxEof)
            {
                if (!videoEof && !videoFlushSent)
                {
                    ret = avcodec_send_packet(r.videoDecoder, nullptr);
                    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                    {
                        failure = QStringLiteral("排空视频解码器失败：%1").arg(ffError(ret));
                        return false;
                    }
                    videoFlushSent = ret != AVERROR(EAGAIN);
                    videoEof = ret == AVERROR_EOF;
                    videoReceive = !videoEof;
                    continue;
                }
                if (!audioEof && !audioFlushSent)
                {
                    ret = avcodec_send_packet(r.audioDecoder, nullptr);
                    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                    {
                        failure = QStringLiteral("排空音频解码器失败：%1").arg(ffError(ret));
                        return false;
                    }
                    audioFlushSent = ret != AVERROR(EAGAIN);
                    audioEof = ret == AVERROR_EOF;
                    audioReceive = !audioEof;
                    continue;
                }
                if (audioEof && r.filter && !filterFlushSent)
                {
                    ret = av_buffersrc_add_frame_flags(r.filter->source, nullptr, 0);
                    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
                    {
                        failure = QStringLiteral("排空音频滤镜失败：%1").arg(ffError(ret));
                        return false;
                    }
                    filterFlushSent = ret != AVERROR(EAGAIN);
                    continue;
                }
                if (audioEof && !r.filter)
                    filterEof = true;
                if (paused && previewNeeded && videoEof)
                {
                    previewNeeded = false;
                    resumeNeedsSeek = true;
                    continue;
                }
                if (videoEof && audioEof && filterEof && video.empty() && m_audio.isDrained()
                    && clock.position() >= lastVideoEndUs)
                {
                    {
                        QMutexLocker lock(&m_controlMutex);
                        if (m_requests.seekRevision != seekRevision || m_requests.rateRevision != rateRevision
                            || m_requests.paused != paused)
                            continue;
                        m_acceptingRequests = false;
                    }
                    finalPositionUs = std::max(clock.position(), std::max(lastVideoEndUs, lastAudioEndUs));
                    if (totalMs >= 0)
                        finalPositionUs = totalMs * 1000;
                    return true;
                }
                waitForRequest(5);
                continue;
            }

            // 正常音轨仍可能有后续包时，视频积压不阻塞寻找音频；已确认的音轨尾部按墙钟节流。
            const bool audioTail = audioStream >= 0 && declaredAudioEndUs >= 0
                && lastAudioEndUs >= declaredAudioEndUs - 100000
                && nextVideoUs >= declaredAudioEndUs;
            const bool audioGap = audioStream >= 0 && !clock.audioMaster && lastAudioEndUs > 0
                && nextVideoUs - lastAudioEndUs > 500000 && !audioInput && !filteredReady;
            if ((audioStream < 0 || audioTail || audioGap) && !paused && !video.empty()
                && (video.size() >= MaxVideoFrames || video.back().ptsUs - clock.position() >= 300000))
            {
                waitForRequest(5);
                continue;
            }
            interrupt.begin(IoTimeoutUs, true);
            interrupt.operationSerial = control.serial;
            ret = av_read_frame(r.format, r.packet);
            interrupt.end();
            if (m_cancelled.load())
                return false;
            if (ret >= 0)
            {
                havePacket = true;
                readIdleSince = 0;
                continue;
            }
            av_packet_unref(r.packet);
            if (interrupt.controlRequest)
            {
                interruptedRead = true;
                if (r.format->pb)
                {
                    r.format->pb->error = 0;
                    r.format->pb->eof_reached = 0;
                }
                continue;
            }
            if (interrupt.timedOut)
            {
                failure = QStringLiteral("读取媒体超时");
                return false;
            }
            if (ret == AVERROR(EAGAIN))
            {
                if (r.format->pb)
                {
                    r.format->pb->error = 0;
                    r.format->pb->eof_reached = 0;
                }
                if (!readIdleSince)
                    readIdleSince = monotonicUs();
                if (monotonicUs() - readIdleSince >= IoTimeoutUs)
                {
                    failure = QStringLiteral("媒体持续无数据，读取超时");
                    return false;
                }
                waitForRequest(10);
                continue;
            }
            if (ret == AVERROR_EOF)
            {
                demuxEof = true;
                continue;
            }
            failure = QStringLiteral("读取媒体失败：%1").arg(ffError(ret));
            return false;
        }
        return false;
    };

    const bool succeeded = execute();
    if (m_cancelled.load())
        return;
    {
        QMutexLocker lock(&m_controlMutex);
        if (m_destroying || session != m_session)
            return;
        m_acceptingRequests = false;
        m_liveDisconnected = livePause;
        generation = m_requests.generation;
    }
    if (livePause)
    {
        postPosition(session, generation, finalPositionUs / 1000);
        postState(session, generation, Paused);
        return;
    }
    finalState = succeeded ? Ended : Error;
    post(session, generation, [this]() {
        m_seekable.store(false);
        m_rateable.store(false);
        emit sigCapabilitiesChanged(false, false);
    });
    if (!succeeded)
        postError(session, generation, failure.isEmpty() ? QStringLiteral("播放失败") : failure);
    else
        postPosition(session, generation, finalPositionUs / 1000);
    postState(session, generation, finalState);
}
