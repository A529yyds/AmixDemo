#include "AmixTask.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}
#include <iostream>
#define SAMPLE_FMT AV_SAMPLE_FMT_S16
#define SAMPLE_RATE 48000
#define CHANNEL_LAYOUT AV_CH_LAYOUT_STEREO
AmixTask::AmixTask(const std::vector<std::string> &devices) : _devs(devices)
{
    _inputs.clear();
    _filterGraph = nullptr;
}

AmixTask::~AmixTask()
{
    stop();
}

void AmixTask::start()
{
    _running = initial();
    _worker = std::thread(&AmixTask::run, this);
}

void AmixTask::stop()
{
    _running = false;
    if (_worker.joinable())
    {
        _worker.join();
    }
}

void AmixTask::run()
{
    // AVFrame *frame = av_frame_alloc();
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    AVFrame *out_frame = av_frame_alloc();
    while (_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // 应该在这里调用 av_read_frame + av_buffersrc_add_frame + av_buffersink_get_frame
        for (size_t i = 0; i < _inputCtxs.size(); ++i)
        {
            if (av_read_frame(_inputCtxs[i]._fmtCtx, &pkt) < 0)
                continue;
            if (pkt.stream_index != _inputCtxs[i]._index)
                continue;

            if (avcodec_send_packet(_inputCtxs[i]._decCtx, &pkt) >= 0)
            {
                while (avcodec_receive_frame(_inputCtxs[i]._decCtx, frame) >= 0)
                {
                    av_buffersrc_add_frame(_filters[i], frame);
                }
            }
            av_packet_unref(&pkt);
        }

        int ret = av_buffersink_get_frame(_sink, out_frame);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            break;

        std::cout << "Got mixed frame, samples: " << out_frame->nb_samples << "\n";
        av_frame_unref(out_frame);
    }
    // av_frame_free(&frame);

    avfilter_graph_free(&_filterGraph);
    for (auto ctx : _inputs)
    {
        avformat_close_input(&ctx);
    }

    std::cout << "Audio mixing stopped.\n";
}

bool AmixTask::initial()
{
    avdevice_register_all();
    _inputCtxs.resize(_devs.size());
    for (size_t i = 0; i < _devs.size(); ++i)
    {
        if (!openInput(_devs[i], _inputCtxs[i]))
            return -1;
    }

    char args[512];
    _filterGraph = avfilter_graph_alloc();
    for (size_t i = 0; i < _inputCtxs.size(); ++i)
    {
        snprintf(args, sizeof(args),
                 "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
                 SAMPLE_RATE, av_get_sample_fmt_name(SAMPLE_FMT), SAMPLE_RATE);

        AVFilterContext *buffersrc = nullptr;
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        std::string name = "src" + std::to_string(i);

        if (avfilter_graph_create_filter(&buffersrc, abuffer, name.c_str(), args, nullptr, _filterGraph) < 0)
        {
            std::cerr << "Failed to create abuffer for input " << i << "\n";
            return false;
        }
        _filters.push_back(buffersrc);
    }

    AVFilterContext *amixCtx = nullptr;
    const AVFilter *amix = avfilter_get_by_name("amix");
    snprintf(args, sizeof(args), "inputs=%zu:duration=longest", _filters.size());
    if (avfilter_graph_create_filter(&amixCtx, amix, "amix", args, nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create amix filter.\n";
        return false;
    }

    // 创建输出 sink
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (avfilter_graph_create_filter(&_sink, abuffersink, "sink", nullptr, nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create sink filter.\n";
        return false;
    }

    // 连接 src -> amix -> sink
    for (size_t i = 0; i < _filters.size(); ++i)
    {
        avfilter_link(_filters[i], 0, amixCtx, i);
    }
    avfilter_link(amixCtx, 0, _sink, 0);
    // 初始化图
    if (avfilter_graph_config(_filterGraph, nullptr) < 0)
    {
        std::cerr << "Failed to configure filter graph.\n";
        return false;
    }
    std::cout << "Audio mixing started...\n";
    return true;
}

bool AmixTask::initial1()
{
    avdevice_register_all();
    // avfilter_register_all();
    std::vector<AVFilterContext *> filters;
    for (const auto &dev : _devs)
    {
        const AVInputFormat *input_fmt = av_find_input_format("alsa");
        AVFormatContext *fmt_ctx = nullptr;
        if (avformat_open_input(&fmt_ctx, dev.c_str(), input_fmt, nullptr) != 0)
        {
            std::cerr << "Failed to open " << dev << "\n";
            continue;
        }
        _inputs.push_back(fmt_ctx);
    }
    if (_inputs.size() < 2)
    {
        std::cerr << "Need at least 2 audio devices to mix.\n";
        return false;
    }
    char args[512];
    _filterGraph = avfilter_graph_alloc();
    for (size_t i = 0; i < _inputs.size(); ++i)
    {
        snprintf(args, sizeof(args),
                 "time_base=1/44100:sample_rate=44100:sample_fmt=s16:channel_layout=stereo");

        AVFilterContext *buffersrc = nullptr;
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        std::string name = "src" + std::to_string(i);

        if (avfilter_graph_create_filter(&buffersrc, abuffer, name.c_str(), args, nullptr, _filterGraph) < 0)
        {
            std::cerr << "Failed to create abuffer for input " << i << "\n";
            return false;
        }
        filters.push_back(buffersrc);
    }
    AVFilterContext *amixCtx = nullptr;
    const AVFilter *amix = avfilter_get_by_name("amix");

    snprintf(args, sizeof(args), "inputs=%zu:duration=longest", filters.size());
    if (avfilter_graph_create_filter(&amixCtx, amix, "amix", args, nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create amix filter.\n";
        return false;
    }
    // 创建输出 sink
    // AVFilterContext *sink = nullptr;
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    if (avfilter_graph_create_filter(&_sink, abuffersink, "sink", nullptr, nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create sink filter.\n";
        return false;
    }

    // 连接 src -> amix -> sink
    for (size_t i = 0; i < filters.size(); ++i)
    {
        avfilter_link(filters[i], 0, amixCtx, i);
    }
    avfilter_link(amixCtx, 0, _sink, 0);
    // 初始化图
    if (avfilter_graph_config(_filterGraph, nullptr) < 0)
    {
        std::cerr << "Failed to configure filter graph.\n";
        return false;
    }
    std::cout << "Audio mixing started...\n";
    return true;
}

bool AmixTask::openInput(const std::string &dev, InputContext &ictx)
{
    const AVInputFormat *input_fmt = av_find_input_format("alsa");
    if (!input_fmt)
        return false;

    if (avformat_open_input(&ictx._fmtCtx, dev.c_str(), input_fmt, nullptr) < 0)
    {
        std::cerr << "Failed to open " << dev << "\n";
        return false;
    }
    if (avformat_find_stream_info(ictx._fmtCtx, nullptr) < 0)
    {
        std::cerr << "Stream info failed\n";
        return false;
    }
    for (unsigned i = 0; i < ictx._fmtCtx->nb_streams; ++i)
    {
        if (ictx._fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            ictx._index = i;
            break;
        }
    }
    const AVCodec *dec = avcodec_find_decoder(ictx._fmtCtx->streams[ictx._index]->codecpar->codec_id);
    ictx._decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ictx._decCtx, ictx._fmtCtx->streams[ictx._index]->codecpar);
    avcodec_open2(ictx._decCtx, dec, nullptr);
    return true;
}
