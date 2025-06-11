#include "AmixTask.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
}

#include <iostream>
#include <fstream>
#include <memory>
#define CHECK_ERR(ret, msg)                           \
    if (ret < 0)                                      \
    {                                                 \
        char err[256];                                \
        av_strerror(ret, err, sizeof(err));           \
        std::cerr << msg << ": " << err << std::endl; \
        exit(1);                                      \
    }
AVFormatContext *open_input(const char *filename, AVCodecContext **dec_ctx)
{
    AVFormatContext *fmt_ctx = nullptr;
    std::string msg = "无法打开" + std::string(filename);
    CHECK_ERR(avformat_open_input(&fmt_ctx, filename, nullptr, nullptr), msg);
    avformat_find_stream_info(fmt_ctx, nullptr);
    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVStream *stream = fmt_ctx->streams[stream_index];
    const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
    *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(*dec_ctx, stream->codecpar);
    avcodec_open2(*dec_ctx, dec, nullptr);
    return fmt_ctx;
}

void init_filter_graph(AVFilterGraph **graph, AVFilterContext **src1_ctx, AVFilterContext **src2_ctx, AVFilterContext **sink_ctx, AVCodecContext *dec1, AVCodecContext *dec2)
{
    *graph = avfilter_graph_alloc();
    AVFilterContext *src1, *src2, *amix_ctx, *sink;

    auto create_abuffer = [&](AVCodecContext *dec, const char *name, AVFilterContext **out)
    {
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        char args[512];
        snprintf(args, sizeof(args),
                 "sample_rate=%d:sample_fmt=%s:channels=%d:channel_layout=0x%" PRIx64,
                 dec->sample_rate,
                 av_get_sample_fmt_name(dec->sample_fmt),
                 dec->channels,
                 dec->channel_layout);
        std::cout << args << "  " << av_get_default_channel_layout(dec->channels) << std::endl;
        avfilter_graph_create_filter(out, abuffer, name, args, nullptr, *graph);
    };

    create_abuffer(dec1, "src1", &src1);
    create_abuffer(dec2, "src2", &src2);
    avfilter_graph_create_filter(&amix_ctx, avfilter_get_by_name("amix"), "amix", "inputs=2", nullptr, *graph);
    avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "sink", nullptr, nullptr, *graph);

    avfilter_link(src1, 0, amix_ctx, 0);
    avfilter_link(src2, 0, amix_ctx, 1);
    avfilter_link(amix_ctx, 0, sink, 0);
    avfilter_graph_config(*graph, nullptr);

    *src1_ctx = src1;
    *src2_ctx = src2;
    *sink_ctx = sink;
}

void encode_output(const char *filename, AVCodecContext *enc_ctx, AVFrame *frame)
{
    AVFormatContext *out_fmt = nullptr;
    avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, filename);
    AVStream *out_stream = avformat_new_stream(out_fmt, nullptr);
    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
    avio_open(&out_fmt->pb, filename, AVIO_FLAG_WRITE);
    avformat_write_header(out_fmt, nullptr);

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    int ret = avcodec_send_frame(enc_ctx, frame);
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(enc_ctx, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        pkt.stream_index = out_stream->index;
        av_interleaved_write_frame(out_fmt, &pkt);
        av_packet_unref(&pkt);
    }
    av_write_trailer(out_fmt);
    avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
}

int Amix2FileTest()
{
    av_log_set_level(AV_LOG_ERROR);

    AVCodecContext *dec_ctx1 = nullptr;
    AVCodecContext *dec_ctx2 = nullptr;
    auto fmt1 = open_input("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", &dec_ctx1);
    auto fmt2 = open_input("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", &dec_ctx2);

    AVFilterGraph *graph;
    AVFilterContext *src1_ctx, *src2_ctx, *sink_ctx;
    init_filter_graph(&graph, &src1_ctx, &src2_ctx, &sink_ctx, dec_ctx1, dec_ctx2);

    AVPacket pkt1, pkt2;
    av_init_packet(&pkt1);
    av_init_packet(&pkt2);
    pkt1.data = pkt2.data = nullptr;
    pkt1.size = pkt2.size = 0;

    AVFrame *frame1 = av_frame_alloc();
    AVFrame *frame2 = av_frame_alloc();
    AVFrame *mixed = av_frame_alloc();

    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext *enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->sample_rate = dec_ctx1->sample_rate;
    enc_ctx->channel_layout = dec_ctx1->channel_layout;
    enc_ctx->channels = dec_ctx1->channels;
    enc_ctx->sample_fmt = dec_ctx1->sample_fmt;
    avcodec_open2(enc_ctx, enc, nullptr);

    while (av_read_frame(fmt1, &pkt1) >= 0 && av_read_frame(fmt2, &pkt2) >= 0)
    {
        avcodec_send_packet(dec_ctx1, &pkt1);
        avcodec_receive_frame(dec_ctx1, frame1);
        avcodec_send_packet(dec_ctx2, &pkt2);
        avcodec_receive_frame(dec_ctx2, frame2);

        av_buffersrc_add_frame_flags(src1_ctx, frame1, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_buffersrc_add_frame_flags(src2_ctx, frame2, AV_BUFFERSRC_FLAG_KEEP_REF);

        while (av_buffersink_get_frame(sink_ctx, mixed) >= 0)
        {
            encode_output("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/mixed_output.wav", enc_ctx, mixed);
            av_frame_unref(mixed);
        }

        av_frame_unref(frame1);
        av_frame_unref(frame2);
        av_packet_unref(&pkt1);
        av_packet_unref(&pkt2);
    }

    av_frame_free(&frame1);
    av_frame_free(&frame2);
    av_frame_free(&mixed);
    avfilter_graph_free(&graph);
    avcodec_free_context(&dec_ctx1);
    avcodec_free_context(&dec_ctx2);
    avcodec_free_context(&enc_ctx);
    avformat_close_input(&fmt1);
    avformat_close_input(&fmt2);

    std::cout << "混音完成，已保存到 mixed_output.wav" << std::endl;
    return 0;
}

void AmixTaskTest()
{
    std::vector<std::string> devs = {"/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4k2.mkv"};
    AmixTask task(devs);
    task.start();
    std::this_thread::sleep_for(std::chrono::seconds(20));
    task.stop();
}

// struct WAVHeader
// {
//     char riff[4] = {'R', 'I', 'F', 'F'};
//     uint32_t fileSize;
//     char wave[4] = {'W', 'A', 'V', 'E'};
//     char fmt[4] = {'f', 'm', 't', ' '};
//     uint32_t fmtSize = 16;
//     uint16_t audioFormat = 1; // PCM
//     uint16_t numChannels;
//     uint32_t sampleRate;
//     uint32_t byteRate;
//     uint16_t blockAlign;
//     uint16_t bitsPerSample;
//     char data[4] = {'d', 'a', 't', 'a'};
//     uint32_t dataSize;
// };

// void writeWAVHeader(std::ofstream &file, const WAVHeader &header)
// {
//     file.write(reinterpret_cast<const char *>(&header), sizeof(header));
// }

#define SAMPLE_FMT AV_SAMPLE_FMT_S16
#define SAMPLE_RATE 48000
int AmixMkvAudio()
{
    AVFormatContext *in_ctx1 = nullptr, *in_ctx2 = nullptr;
    AVFilterGraph *filter_graph = nullptr;
    std::ofstream wavFile("output.wav", std::ios::binary);
    std::vector<AVFilterContext *> _filters;
    AVFilterContext *_sink = nullptr;
    // 初始化FFmpeg
    avformat_network_init();

    // // 打开输入文件
    // avformat_open_input(&in_ctx1, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", nullptr, nullptr);
    // avformat_open_input(&in_ctx2, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K2.mkv", nullptr, nullptr);
    // avformat_find_stream_info(in_ctx1, nullptr);
    // avformat_find_stream_info(in_ctx2, nullptr);
    // 打开输入文件 1
    CHECK_ERR(avformat_open_input(&in_ctx1, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", nullptr, nullptr),
              "无法打开 4K1.mkv");
    CHECK_ERR(avformat_find_stream_info(in_ctx1, nullptr),
              "找不到输入流信息 1");
    // 打开输入文件 2
    CHECK_ERR(avformat_open_input(&in_ctx2, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4k2.mkv", nullptr, nullptr),
              "无法打开 4K2.mkv");
    CHECK_ERR(avformat_find_stream_info(in_ctx2, nullptr),
              "找不到输入流信息 2");

    // 创建滤镜图
    filter_graph = avfilter_graph_alloc();
    char args[512];
    for (size_t i = 0; i < 2; ++i)
    {
        snprintf(args, sizeof(args),
                 "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
                 SAMPLE_RATE, av_get_sample_fmt_name(SAMPLE_FMT), SAMPLE_RATE);

        AVFilterContext *buffersrc = nullptr;
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        std::string name = "src" + std::to_string(i);

        if (avfilter_graph_create_filter(&buffersrc, abuffer, name.c_str(), args, nullptr, filter_graph) < 0)
        {
            std::cerr << "Failed to create abuffer for input " << i << "\n";
            return false;
        }
        _filters.push_back(buffersrc);
    }

    AVFilterContext *amixCtx = nullptr;
    const AVFilter *amix = avfilter_get_by_name("amix");
    snprintf(args, sizeof(args), "inputs=%zu:duration=longest", _filters.size());
    if (avfilter_graph_create_filter(&amixCtx, amix, "amix", args, nullptr, filter_graph) < 0)
    {
        std::cerr << "Failed to create amix filter.\n";
        return false;
    }

    // 创建输出 sink
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (avfilter_graph_create_filter(&_sink, abuffersink, "sink", nullptr, nullptr, filter_graph) < 0)
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
    if (avfilter_graph_config(filter_graph, nullptr) < 0)
    {
        std::cerr << "Failed to configure filter graph.\n";
        return false;
    }
    // const char *filter_desc = "[0:a][1:a]amerge=inputs=2,aresample=48000,aformat=sample_fmts=s16p:channel_layouts=stereo[aout]";
    // avfilter_graph_parse2(filter_graph, filter_desc, nullptr, nullptr);
    // avfilter_graph_config(filter_graph, nullptr);

    // 准备WAV头
    // WAVHeader header;
    // header.numChannels = 2;
    // header.sampleRate = 48000;
    // header.bitsPerSample = 16;
    // header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    // header.blockAlign = header.numChannels * header.bitsPerSample / 8;
    // writeWAVHeader(wavFile, header);

    // 音频处理循环
    AVFrame *frame = av_frame_alloc();
    while (av_buffersink_get_frame(/*滤镜输出*/ _sink, frame) >= 0)
    {
        std::vector<int16_t> pcmData(frame->nb_samples * 2);
        for (int i = 0; i < frame->nb_samples; ++i)
        {
            pcmData[i * 2] = reinterpret_cast<int16_t *>(frame->data[0])[i];     // 左声道
            pcmData[i * 2 + 1] = reinterpret_cast<int16_t *>(frame->data[1])[i]; // 右声道
        }
        wavFile.write(reinterpret_cast<char *>(pcmData.data()), pcmData.size() * 2);
    }

    // 更新WAV头数据大小
    // uint32_t dataSize = wavFile.tellp() ;- sizeof(WAVHeader);
    // wavFile.seekp(4);
    // wavFile.write(reinterpret_cast<char *>(&dataSize), 4);
    // wavFile.seekp(sizeof(WAVHeader) - 4);
    // wavFile.write(reinterpret_cast<char *>(&dataSize), 4);

    // 资源清理
    av_frame_free(&frame);
    avfilter_graph_free(&filter_graph);
    avformat_close_input(&in_ctx1);
    avformat_close_input(&in_ctx2);
    wavFile.close();
    return 0;

    // AVFormatContext *in_fmt_ctx1 = nullptr;
    // AVFormatContext *in_fmt_ctx2 = nullptr;
    // AVFormatContext *out_fmt_ctx = nullptr;
    // AVFilterGraph *filter_graph = nullptr;
    // AVFilterContext *buffersrc_ctx1 = nullptr;
    // AVFilterContext *buffersrc_ctx2 = nullptr;
    // AVFilterContext *buffersink_ctx = nullptr;
    // AVStream *out_stream = nullptr;
    // AVCodecContext *dec_ctx1 = nullptr;
    // AVCodecContext *dec_ctx2 = nullptr;
    // AVCodecContext *enc_ctx = nullptr;
    // int audio_stream_idx1 = -1, audio_stream_idx2 = -1;
    // int ret = 0;
    // try
    // {
    //     // 初始化 FFmpeg
    //     avformat_network_init();
    //     // 打开输入文件 1
    //     CHECK_ERR(avformat_open_input(&in_fmt_ctx1, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", nullptr, nullptr),
    //               "无法打开 4K1.mkv");
    //     CHECK_ERR(avformat_find_stream_info(in_fmt_ctx1, nullptr),
    //                 "找不到输入流信息 1");
    //     // 打开输入文件 2
    //     CHECK_ERR(avformat_open_input(&in_fmt_ctx2, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K2.mkv", nullptr, nullptr),
    //               "无法打开 4K2.mkv");
    //     CHECK_ERR(avformat_find_stream_info(in_fmt_ctx2, nullptr),
    //                 "找不到输入流信息 2");
    //     // 查找音频流索引
    //     audio_stream_idx1 = av_find_best_stream(in_fmt_ctx1, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    //     audio_stream_idx2 = av_find_best_stream(in_fmt_ctx2, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    //     CHECK_ERR((audio_stream_idx1 < 0 || audio_stream_idx2 < 0),
    //                 "找不到音频流");
    //     // 创建输出上下文
    //     CHECK_ERR(avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, "mixed_output.wav"),
    //                 "无法创建输出上下文");
    //     // 创建输出流
    //     out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    //     CHECK_ERR(!out_stream, "无法创建输出流");
    //     // 初始化滤镜图
    //     filter_graph = avfilter_graph_alloc();
    //     CHECK_ERR(!filter_graph, "无法分配滤镜图");
    //     // 获取音频解码器上下文
    //     dec_ctx1 = in_fmt_ctx1->streams[audio_stream_idx1]->codecpar;
    //     dec_ctx2 = in_fmt_ctx2->streams[audio_stream_idx2]->codecpar;
    //     // 构建过滤器描述
    //     char args[512];
    //     snprintf(args, sizeof(args),
    //              "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
    //              1, dec_ctx1->sample_rate,
    //              dec_ctx1->sample_rate,
    //              av_get_sample_fmt_name(static_cast<AVSampleFormat>(dec_ctx1->format)),
    //              dec_ctx1->channel_layout);
    //     // 创建输入缓冲区源
    //     const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    //     CHECK_ERR(!abuffer, "找不到 abuffer 滤镜");
    //     CHECK_ERR(avfilter_graph_create_filter(&buffersrc_ctx1, abuffer, "in1", args, nullptr, filter_graph),
    //                 "无法创建输入缓存源 1");
    //     snprintf(args, sizeof(args),
    //              "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
    //              1, dec_ctx2->sample_rate,
    //              dec_ctx2->sample_rate,
    //              av_get_sample_fmt_name(static_cast<AVSampleFormat>(dec_ctx2->format)),
    //              dec_ctx2->channel_layout);
    //     CHECK_ERR(avfilter_graph_create_filter(&buffersrc_ctx2, abuffer, "in2", args, nullptr, filter_graph),
    //                 "无法创建输入缓存源 2");
    //     // 创建 amix 滤镜
    //     const AVFilter *amix = avfilter_get_by_name("amix");
    //     CHECK_ERR(!amix, "找不到 amix 滤镜");
    //     CHECK_ERR(avfilter_graph_create_filter(&buffersink_ctx, amix, "mix", "inputs=2:duration=longest", nullptr, filter_graph),
    //                 "无法创建 amix 滤镜");
    //     // 连接滤镜节点
    //     CHECK_ERR(avfilter_link(buffersrc_ctx1, 0, buffersink_ctx, 0),
    //                 "无法连接滤镜 1");
    //     CHECK_ERR(avfilter_link(buffersrc_ctx2, 0, buffersink_ctx, 1),
    //                 "无法连接滤镜 2");
    //     // 配置滤镜图
    //     CHECK_ERR(avfilter_graph_config(filter_graph, nullptr),
    //                 "无法配置滤镜图");
    //     // 打开输出文件
    //     CHECK_ERR(avio_open(&out_fmt_ctx->pb, "output.mkv", AVIO_FLAG_WRITE),
    //                 "无法打开输出文件");
    //     // 写入文件头
    //     CHECK_ERR(avformat_write_header(out_fmt_ctx, nullptr),
    //                 "无法写入文件头");
    //     AVFrame *frame = av_frame_alloc();
    //     AVPacket *packet = av_packet_alloc();
    //     CHECK_ERR(!frame || !packet, "内存分配失败");
    //     // 主处理循环
    //     while (true)
    //     {
    //         // 读取并处理 input1 的包
    //         while (av_read_frame(in_fmt_ctx1, packet) >= 0)
    //         {
    //             if (packet->stream_index == audio_stream_idx1)
    //             {
    //                 ret = av_buffersrc_add_frame(buffersrc_ctx1, frame);
    //                 if (ret < 0)
    //                     break;
    //             }
    //             av_packet_unref(packet);
    //         }
    //         // 读取并处理 input2 的包
    //         while (av_read_frame(in_fmt_ctx2, packet) >= 0)
    //         {
    //             if (packet->stream_index == audio_stream_idx2)
    //             {
    //                 ret = av_buffersrc_add_frame(buffersrc_ctx2, frame);
    //                 if (ret < 0)
    //                     break;
    //             }
    //             av_packet_unref(packet);
    //         }
    //         // 从滤镜图中获取混合后的帧
    //         while (ret >= 0)
    //         {
    //             ret = av_buffersink_get_frame(buffersink_ctx, frame);
    //             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    //                 break;
    //             CHECK_ERR(ret, "无法获取混合帧");
    //             // 写入混合后的音频（实际应用中需添加编码步骤）
    //             // ... 这里添加编码和写入逻辑 ...
    //             av_frame_unref(frame);
    //         }
    //         if (ret == AVERROR_EOF)
    //             break;
    //     }
    //     // 写入文件尾
    //     av_write_trailer(out_fmt_ctx);
    //     std::cout << "音频混合完成！输出文件：output.mkv" << std::endl;
    // }
    // catch (const std::exception &e)
    // {
    //     std::cerr << "错误: " << e.what() << std::endl;
    //     ret = -1;
    // }
    // // 释放资源
    // av_frame_free(&frame);
    // av_packet_free(&packet);
    // avfilter_graph_free(&filter_graph);
    // avformat_close_input(&in_fmt_ctx1);
    // avformat_close_input(&in_fmt_ctx2);
    // if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
    //     avio_closep(&out_fmt_ctx->pb);
    // avformat_free_context(out_fmt_ctx);
    // return ret;
}

// #include <libavcodec/avcodec.h>
// #include <libavformat/avformat.h>
// #include <libavfilter/avfilter.h>
// #include <libavfilter/buffersrc.h>
// #include <libavfilter/buffersink.h>
// #include <libavutil/opt.h>

#include <chrono>

#define INPUT_FILE2 "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4k2.mkv"
#define INPUT_FILE1 "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv"
#define OUTPUT_FILE "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/output.wav"
#define DURATION_SEC 60

int tst1()
{
    avformat_network_init();

    // 初始化输入上下文
    AVFormatContext *input_ctx1 = nullptr;
    AVFormatContext *input_ctx2 = nullptr;
    if (avformat_open_input(&input_ctx1, INPUT_FILE1, nullptr, nullptr) < 0 ||
        avformat_open_input(&input_ctx2, INPUT_FILE2, nullptr, nullptr) < 0)
    {
        fprintf(stderr, "无法打开输入文件\n");
        return -1;
    }

    // 查找音频流
    int audio_stream_idx1 = -1, audio_stream_idx2 = -1;
    for (int i = 0; i < input_ctx1->nb_streams; i++)
    {
        if (input_ctx1->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_idx1 = i;
            break;
        }
    }
    for (int i = 0; i < input_ctx2->nb_streams; i++)
    {
        if (input_ctx2->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_idx2 = i;
            break;
        }
    }

    // 创建过滤器图
    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    const AVFilter *abuffer1 = avfilter_get_by_name("abuffer");
    const AVFilter *abuffer2 = avfilter_get_by_name("abuffer");
    const AVFilter *amix = avfilter_get_by_name("amix");
    const AVFilter *equalizer = avfilter_get_by_name("equalizer");
    const AVFilter *afftdn = avfilter_get_by_name("afftdn");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    // 创建过滤器上下文
    AVFilterContext *buffer1_ctx = nullptr;
    AVFilterContext *buffer2_ctx = nullptr;
    AVFilterContext *amix_ctx = nullptr;
    AVFilterContext *equalizer_ctx = nullptr;
    AVFilterContext *afftdn_ctx = nullptr;
    AVFilterContext *sink_ctx = nullptr;

    // 配置输入源1
    char args1[512];
    int samplerate = input_ctx1->streams[audio_stream_idx1]->codecpar->sample_rate;
    snprintf(args1, sizeof(args1),
             "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
             samplerate, av_get_sample_fmt_name(SAMPLE_FMT), samplerate);
    // snprintf(args1, sizeof(args1), "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
    //          input_ctx1->streams[audio_stream_idx1]->codecpar->sample_rate,
    //          av_get_sample_fmt_name(static_cast<AVSampleFormat>(input_ctx1->streams[audio_stream_idx1]->codecpar->format)),
    //          input_ctx1->streams[audio_stream_idx1]->codecpar->channel_layout);
    avfilter_graph_create_filter(&buffer1_ctx, abuffer1, "in1", args1, nullptr, filter_graph);

    // 配置输入源2
    char args2[512];
    samplerate = input_ctx2->streams[audio_stream_idx2]->codecpar->sample_rate;
    snprintf(args2, sizeof(args2),
             "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
             samplerate, av_get_sample_fmt_name(SAMPLE_FMT), samplerate);
    // snprintf(args2, sizeof(args2), "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
    //          input_ctx2->streams[audio_stream_idx2]->codecpar->sample_rate,
    //          av_get_sample_fmt_name(static_cast<AVSampleFormat>(input_ctx2->streams[audio_stream_idx2]->codecpar->format)),
    //          input_ctx2->streams[audio_stream_idx2]->codecpar->channel_layout);
    avfilter_graph_create_filter(&buffer2_ctx, abuffer2, "in2", args2, nullptr, filter_graph);

    // 创建amix过滤器
    avfilter_graph_create_filter(&amix_ctx, amix, "amix", "inputs=2:duration=longest", nullptr, filter_graph);

    // 创建equalizer过滤器
    avfilter_graph_create_filter(&equalizer_ctx, equalizer, "equalizer",
                                 "f=240:width_type=q:width=15:g=-20:a=zdf", nullptr, filter_graph);

    // 创建afftdn过滤器
    avfilter_graph_create_filter(&afftdn_ctx, afftdn, "afftdn", nullptr, nullptr, filter_graph);

    // 创建输出sink
    avfilter_graph_create_filter(&sink_ctx, abuffersink, "out", nullptr, nullptr, filter_graph);

    // 连接过滤器
    avfilter_link(buffer1_ctx, 0, amix_ctx, 0);
    avfilter_link(buffer2_ctx, 0, amix_ctx, 1);
    avfilter_link(amix_ctx, 0, equalizer_ctx, 0);
    avfilter_link(equalizer_ctx, 0, afftdn_ctx, 0);
    avfilter_link(afftdn_ctx, 0, sink_ctx, 0);

    // 配置过滤器图
    avfilter_graph_config(filter_graph, nullptr);

    // 准备输出文件
    AVFormatContext *output_ctx = nullptr;
    avformat_alloc_output_context2(&output_ctx, nullptr, "wav", OUTPUT_FILE);
    AVStream *out_stream = avformat_new_stream(output_ctx, nullptr);

    // 设置输出参数
    out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = 44100;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16;
    out_stream->codecpar->bit_rate = 1411 * 1000;

    // 打开输出文件
    avio_open(&output_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);
    avformat_write_header(output_ctx, nullptr);

    // 处理音频帧
    auto start_time = std::chrono::steady_clock::now();
    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed >= DURATION_SEC)
            break;

        AVFrame *frame = av_frame_alloc();
        int ret = av_buffersink_get_frame(sink_ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        {
            break;
        }
        // 写入输出文件
        AVPacket *pkt = av_packet_alloc();
        avcodec_fill_audio_frame(frame, 2, AV_SAMPLE_FMT_S16, frame->data[0], frame->linesize[0], 0);
        av_packet_from_data(pkt, frame->data[0], frame->linesize[0]);
        av_packet_rescale_ts(pkt, (AVRational){1, 44100}, out_stream->time_base);
        av_interleaved_write_frame(output_ctx, pkt);

        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    // 清理资源
    av_write_trailer(output_ctx);
    avio_closep(&output_ctx->pb);
    avformat_free_context(output_ctx);
    avfilter_graph_free(&filter_graph);
    avformat_close_input(&input_ctx1);
    avformat_close_input(&input_ctx2);

    return 0;
}

int tst(int argc, char *argv[])
{
    AVFormatContext *fmt_ctx1 = NULL, *fmt_ctx2 = NULL;
    AVFilterGraph *filter_graph = NULL;
    AVFilterContext *src1_ctx = NULL, *src2_ctx = NULL;
    AVFilterContext *amix_ctx = NULL, *equalizer_ctx = NULL, *afftdn_ctx = NULL, *sink_ctx = NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int ret;

    // 打开输入文件1
    if ((ret = avformat_open_input(&fmt_ctx1, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4k2.mkv", NULL, NULL)) < 0)
    {
        fprintf(stderr, "无法打开输入文件1\n");
        return ret;
    }

    // 打开输入文件2
    if ((ret = avformat_open_input(&fmt_ctx2, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/4K1.mkv", NULL, NULL)) < 0)
    {
        fprintf(stderr, "无法打开输入文件2\n");
        return ret;
    }

    // 创建过滤器图
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
    {
        fprintf(stderr, "无法创建过滤器图\n");
        return AVERROR(ENOMEM);
    }

    // 创建输入源过滤器
    const AVFilter *abuffer1 = avfilter_get_by_name("abuffer");
    const AVFilter *abuffer2 = avfilter_get_by_name("abuffer");
    const AVFilter *amix = avfilter_get_by_name("amix");
    const AVFilter *equalizer = avfilter_get_by_name("equalizer");
    const AVFilter *afftdn = avfilter_get_by_name("afftdn");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    // 设置输入源1参数
    char args1[512];
    snprintf(args1, sizeof(args1),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             44100, av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), AV_CH_LAYOUT_STEREO);
    ret = avfilter_graph_create_filter(&src1_ctx, abuffer1, "in1", args1, NULL, filter_graph);

    // 设置输入源2参数
    char args2[512];
    snprintf(args2, sizeof(args2),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             48000, av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), AV_CH_LAYOUT_STEREO);
    ret = avfilter_graph_create_filter(&src2_ctx, abuffer2, "in2", args2, NULL, filter_graph);

    // 创建amix过滤器
    ret = avfilter_graph_create_filter(&amix_ctx, amix, "amix", "inputs=2", NULL, filter_graph);

    // 创建equalizer过滤器
    ret = avfilter_graph_create_filter(&equalizer_ctx, equalizer, "equalizer",
                                       "f=240:width_type=q:width=15:g=-20:a=zdf", NULL, filter_graph);

    // 创建afftdn过滤器
    ret = avfilter_graph_create_filter(&afftdn_ctx, afftdn, "afftdn", NULL, NULL, filter_graph);

    // 创建输出sink
    ret = avfilter_graph_create_filter(&sink_ctx, abuffersink, "out", NULL, NULL, filter_graph);

    // 连接过滤器
    ret = avfilter_link(src1_ctx, 0, amix_ctx, 0);
    ret = avfilter_link(src2_ctx, 0, amix_ctx, 1);
    ret = avfilter_link(amix_ctx, 0, equalizer_ctx, 0);
    ret = avfilter_link(equalizer_ctx, 0, afftdn_ctx, 0);
    ret = avfilter_link(afftdn_ctx, 0, sink_ctx, 0);

    // 配置过滤器图
    ret = avfilter_graph_config(filter_graph, NULL);

    // 创建输出文件上下文
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/tstOut.wav");
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);

    // 设置输出流参数
    out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = 44100;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16;
    out_stream->codecpar->bit_rate = 1411 * 1000;

    // 打开输出文件
    avio_open(&out_fmt_ctx->pb, "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/tstOut.wav", AVIO_FLAG_WRITE);
    avformat_write_header(out_fmt_ctx, NULL);

    // 处理音频帧
    while (1)
    {
        ret = av_buffersink_get_frame(sink_ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
            break;

        // 写入输出文件
        av_packet_rescale_ts(pkt, (AVRational){1, 44100}, out_stream->time_base);
        av_interleaved_write_frame(out_fmt_ctx, pkt);
    }

    // 清理资源
    av_write_trailer(out_fmt_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avfilter_graph_free(&filter_graph);
    avformat_close_input(&fmt_ctx1);
    avformat_close_input(&fmt_ctx2);
    avformat_free_context(out_fmt_ctx);

    return 0;
}

int tst2()
{
    AVFormatContext *fmt_ctx1 = NULL, *fmt_ctx2 = NULL;
    AVFilterGraph *filter_graph = NULL;
    AVFilterContext *src1_ctx, *src2_ctx, *sink_ctx;
    int ret;

    // 1. 初始化FFmpeg
    // av_register_all();
    // avfilter_register_all();

    // 2. 打开输入文件
    if ((ret = avformat_open_input(&fmt_ctx1, INPUT_FILE1, NULL, NULL)) < 0)
    {
        fprintf(stderr, "无法打开输入文件1\n");
        return ret;
    }
    if ((ret = avformat_open_input(&fmt_ctx2, INPUT_FILE2, NULL, NULL)) < 0)
    {
        fprintf(stderr, "无法打开输入文件2\n");
        return ret;
    }

    // 3. 创建滤镜图
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
    {
        fprintf(stderr, "无法分配滤镜图\n");
        return AVERROR(ENOMEM);
    }

    // 4. 创建输入滤镜
    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    char args[512];

    // 输入源1配置
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             44100, "fltp", (uint64_t)AV_CH_LAYOUT_STEREO);
    ret = avfilter_graph_create_filter(&src1_ctx, abuffer, "in1", args, NULL, filter_graph);

    // 输入源2配置
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             44100, "fltp", (uint64_t)AV_CH_LAYOUT_STEREO);
    ret = avfilter_graph_create_filter(&src2_ctx, abuffer, "in2", args, NULL, filter_graph);

    // 5. 创建amix+equalizer+afftdn滤镜链
    const AVFilter *amix = avfilter_get_by_name("amix");
    const AVFilter *equalizer = avfilter_get_by_name("equalizer");
    const AVFilter *afftdn = avfilter_get_by_name("afftdn");

    // 创建amix滤镜
    ret = avfilter_graph_create_filter(&sink_ctx, amix, "mix",
                                       "inputs=2:duration=longest", NULL, filter_graph);

    // 创建equalizer滤镜
    AVFilterContext *eq_ctx;
    ret = avfilter_graph_create_filter(&eq_ctx, equalizer, "eq",
                                       "f=240:width_type=q:width=15:g=-20:a=zdf",
                                       NULL, filter_graph);

    // 创建afftdn滤镜
    AVFilterContext *afftdn_ctx;
    ret = avfilter_graph_create_filter(&afftdn_ctx, afftdn, "noise_reduce",
                                       NULL, NULL, filter_graph);

    // 6. 连接滤镜节点
    avfilter_link(src1_ctx, 0, sink_ctx, 0);
    avfilter_link(src2_ctx, 0, sink_ctx, 1);
    avfilter_link(sink_ctx, 0, eq_ctx, 0);
    avfilter_link(eq_ctx, 0, afftdn_ctx, 0);

    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", OUTPUT_FILE);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = 44100;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16;
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);
    avformat_write_header(out_fmt_ctx, NULL);

    // 8. 音频帧处理循环 [处理数据包（需要添加音频帧处理循环）]
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    while (av_buffersink_get_frame(afftdn_ctx, frame) >= 0)
    {
        // 配置输出包参数
        pkt->stream_index = out_stream->index;
        pkt->duration = av_rescale_q(1, out_stream->time_base,
                                     (AVRational){1, frame->sample_rate});

        // 转换采样格式为S16LE
        struct SwrContext *swr = swr_alloc_set_opts(NULL,
                                                    AV_CH_LAYOUT_STEREO,
                                                    AV_SAMPLE_FMT_S16,
                                                    44100,
                                                    frame->channel_layout,
                                                    (AVSampleFormat)frame->format,
                                                    frame->sample_rate,
                                                    0, NULL);
        swr_init(swr);

        uint8_t **converted_data = NULL;
        av_samples_alloc(converted_data, NULL, 2, frame->nb_samples,
                         AV_SAMPLE_FMT_S16, 0);
        swr_convert(swr, converted_data, frame->nb_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);

        // 写入WAV文件
        pkt->data = converted_data[0];
        pkt->size = av_samples_get_buffer_size(NULL, 2, frame->nb_samples,
                                               AV_SAMPLE_FMT_S16, 0);
        av_write_frame(out_fmt_ctx, pkt);

        // 释放资源
        swr_free(&swr);
        av_freep(&converted_data[0]);
        av_frame_unref(frame);
    }
    // 收尾处理
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);

    // 9. 释放资源
    avformat_close_input(&fmt_ctx1);
    avformat_close_input(&fmt_ctx2);
    avfilter_graph_free(&filter_graph);
    return 0;
}

int main(int argc, char **argv)
{
    // 程序代码
    // tst(argc, argv);
    AmixTaskTest();
    return 0;
}
