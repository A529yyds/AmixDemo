extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
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
#include <thread>
#include "stdio.h"
#include "AmixTask.h"

#define OUTPUT_FILE "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/taskOut.wav"
#define OUTPUT_FILE_FLAC "/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/output.flac"

#define TARGET_SAMPLE_RATE 48000
#define CHANNEL_LAYOUT AV_CH_LAYOUT_STEREO
AmixTask::AmixTask(const std::vector<std::string> &devices) : _devs(devices)
{
    _inputs.clear();
    clearPkts();
    _filterGraph = nullptr;
}

AmixTask::~AmixTask()
{
    stop();
}

void AmixTask::start()
{
    _running = initial();
    // _worker = std::thread(&AmixTask::run, this);
    _sWorker = std::thread(&AmixTask::sender, this);
    _rWorker = std::thread(&AmixTask::receiver, this);
}

void AmixTask::stop()
{
    std::mutex mutex_;
    std::lock_guard<std::mutex> lock(mutex_);
    {
        _running = false;
    }
    // if (_worker.joinable())
    // {
    //     _worker.join();
    // }
    if (_sWorker.joinable())
    {
        _sWorker.join();
    }
    if (_rWorker.joinable())
    {
        _rWorker.join();
    }
}

struct WAVHeader
{
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize;
};

void writeWAVHeader(FILE *file, const WAVHeader &header)
{
    size_t size = fwrite(reinterpret_cast<const char *>(&header), sizeof(header), sizeof(header) + 1, file);
}

void AmixTask::run2()
{
    // AVFrame *frame = av_frame_alloc();
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    AVFrame *out_frame = av_frame_alloc();
    FILE *wavFile = fopen("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/output.wav", "wb");

    // 准备WAV头
    WAVHeader header;
    header.numChannels = 2;
    header.sampleRate = 48000;
    header.bitsPerSample = 16;
    header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    header.blockAlign = header.numChannels * header.bitsPerSample / 8;
    writeWAVHeader(wavFile, header);
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
                    // normalizeStream(frame, _inputCtxs[i]._decCtx);
                    av_buffersrc_add_frame(_filters[i], frame);
                }
            }
            av_packet_unref(&pkt);
        }

        int ret = av_buffersink_get_frame(_sink, out_frame);
        if (ret == AVERROR(EAGAIN))
            continue;
        else if (ret < 0)
            break;
        else if (ret >= 0)
        {
            std::vector<int16_t> pcmData(out_frame->nb_samples * 2);
            for (int i = 0; i < out_frame->nb_samples; ++i)
            {
                pcmData[i * 2] = reinterpret_cast<int16_t *>(out_frame->data[0])[i];     // 左声道
                pcmData[i * 2 + 1] = reinterpret_cast<int16_t *>(out_frame->data[1])[i]; // 右声道
            }
            fwrite(reinterpret_cast<char *>(pcmData.data()), 1, pcmData.size() * 2, wavFile);
            // wrireFrame(out_frame, wavFile);
        }

        std::cout << "Got mixed frame, samples: " << out_frame->nb_samples << "\n";
        av_frame_unref(out_frame);
    }

    // 更新WAV头数据大小
    // uint32_t dataSize = wavFile.tellp();
    // -sizeof(WAVHeader);
    // wavFile.seekp(4);
    // wavFile.write(reinterpret_cast<char *>(&dataSize), 4);
    // wavFile.seekp(sizeof(WAVHeader) - 4);
    // wavFile.write(reinterpret_cast<char *>(&dataSize), 4);
    av_frame_free(&frame);
    av_frame_free(&out_frame);
    avfilter_graph_free(&_filterGraph);
    for (auto ctx : _inputs)
    {
        avformat_close_input(&ctx);
    }
    fclose(wavFile);

    std::cout << "Audio mixing stopped.\n";
}

// FLTP不可以用作wav写入数据
void AmixTask::run()
{
    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, OUTPUT_FILE_FLAC);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_FLAC; // AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = TARGET_SAMPLE_RATE;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_FLTP; // AV_SAMPLE_FMT_S16;
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE_FLAC, AVIO_FLAG_WRITE);
    avformat_write_header(out_fmt_ctx, NULL);
    // 8. 音频帧处理循环
    AVPacket tmpPkt;
    AVPacket *pkt = av_packet_alloc();
    pkt->size = 0;
    AVFrame *frame = av_frame_alloc();
    int64_t last_dts = AV_NOPTS_VALUE;
    // // 初始化重采样器：转换采样格式为S16LE
    uint8_t *converted_data = NULL;
    // AVPacket outPkt;
    double retio = (double)44100 / (double)48000;

    while (_running)
    {
        // 应该在这里调用 av_read_frame + av_buffersrc_add_frame + av_buffersink_get_frame
        for (size_t i = 0; i < _inputCtxs.size(); ++i)
        {
            if (av_read_frame(_inputCtxs[i]._fmtCtx, &tmpPkt) < 0)
                continue;
            if (tmpPkt.stream_index != _inputCtxs[i]._index)
                continue;
            if (avcodec_send_packet(_inputCtxs[i]._decCtx, &tmpPkt) >= 0)
            {
                while (avcodec_receive_frame(_inputCtxs[i]._decCtx, frame) >= 0)
                {
                    std::cout << "amix before pts is " << tmpPkt.pts << " dts is " << tmpPkt.dts << std::endl;
                    // 将帧推送到过滤器图
                    av_buffersrc_add_frame(_filters[i], frame);
                }
                av_packet_unref(&tmpPkt);
                av_frame_unref(frame);
            }
            AVFrame *out_frame = av_frame_alloc();
            AVRational itm_base = {1, _inputCtxs[i]._decCtx->sample_rate};
            int tstI = 0;
            // std::cout << "out tstI is " << tstI << std::endl;
            while (true)
            {
                if (tstI > 0 && out_frame == nullptr)
                {
                    out_frame = av_frame_alloc();
                }
                tstI++;
                // std::cout << "in tstI is " << tstI << std::endl;
                int bRet = av_buffersink_get_frame(_sink, out_frame);
                if (bRet < 0)
                {
                    // std::cout << "bRet is " << bRet << " i is " << i << std::endl;
                    break;
                }
                // av_new_packet(pkt, out_frame->nb_samples * av_get_bytes_per_sample((AVSampleFormat)out_frame->format));

                AVRational otm_base = out_stream->time_base;
                if (out_frame->pts == AV_NOPTS_VALUE)
                {
                    out_frame->pts = last_dts != AV_NOPTS_VALUE ? last_dts + out_frame->nb_samples : 0;
                }
                // 强制单调递增检查
                int64_t current_dts = av_rescale_q(out_frame->pts, itm_base, otm_base);
                if (last_dts != AV_NOPTS_VALUE && current_dts <= last_dts)
                {
                    current_dts = last_dts + 1;
                }
                last_dts = current_dts;
                pkt->pts = pkt->dts = current_dts;
                pkt->stream_index = out_stream->index;
                std::cout << "amix after pts is " << pkt->pts << " dts is " << pkt->dts << std::endl;
                // 平面转打包处理
                if (!converted_data)
                    av_samples_alloc(&converted_data, out_frame->linesize, out_frame->channels, out_frame->nb_samples,
                                     (AVSampleFormat)out_frame->format, 0);
                if (pkt->size == 0)
                    pkt->size = av_samples_get_buffer_size(out_frame->linesize, 2, out_frame->nb_samples,
                                                           AV_SAMPLE_FMT_FLTP, 0);
                int size = out_frame->linesize[0] - 1;
                if (pkt->size != 0)
                {
                    for (int i = 0; i < size; ++i)
                    {
                        converted_data[i * 2] = reinterpret_cast<int16_t *>(out_frame->data[0])[i];     // 左声道
                        converted_data[i * 2 + 1] = reinterpret_cast<int16_t *>(out_frame->data[1])[i]; // 右声道
                    }
                }
                pkt->data = converted_data;
                // 写入WAV文件
                int writeFrameRet = av_write_frame(out_fmt_ctx, pkt);

                // int fillAudioRet = avcodec_fill_audio_frame(out_frame, out_frame->nb_samples,
                //                                             (AVSampleFormat)out_frame->format, pkt->data, pkt->size, 0);

                // int writeFrameRet = av_interleaved_write_frame(out_fmt_ctx, pkt);
                // std::cout << "fillAudioRet is " << fillAudioRet << " writeFrameRet is " << writeFrameRet << std::endl;
                std::cout << " writeFrameRet is " << writeFrameRet << std::endl;
                // 释放资源
                if (out_frame)
                {
                    av_frame_unref(out_frame);
                    out_frame = nullptr;
                    std::cout << " out_frame = nullptr " << std::endl;
                }
            }
        }
    }
    // 收尾处理
    av_freep(&converted_data);
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    // if (pkt != nullptr)
    av_packet_free(&pkt);
    // av_frame_free(&frame);
}

void AmixTask::run4()
{
    // std::thread t1(read_input, _inputCtxs[0]._fmtCtx, _inputCtxs[0]._index,
    //                std::ref(_inputCtxs[0]._input_que), std::ref(_inputCtxs[0]._mtx), std::ref(_inputCtxs[0]._cv), std::ref(_inputCtxs[0]._input_eof));
    // std::thread t2(read_input, _inputCtxs[1]._fmtCtx, _inputCtxs[1]._index,
    //                std::ref(_inputCtxs[1]._input_que), std::ref(_inputCtxs[1]._mtx), std::ref(_inputCtxs[1]._cv), std::ref(_inputCtxs[1]._input_eof));
    // // 准备输出
    // AVFormatContext *out_fmt_ctx = NULL;
    // avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, OUTPUT_FILE);
    // AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    // out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    // out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    // out_stream->codecpar->format = AV_SAMPLE_FMT_S16;
    // out_stream->codecpar->sample_rate = 44100;
    // out_stream->codecpar->channels = 2;
    // out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    // avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);
    // avformat_write_header(out_fmt_ctx, NULL);
    // // 主处理循环
    // AVFrame *filt_frame = av_frame_alloc();
    // int64_t last_pts = AV_NOPTS_VALUE;
    // int64_t pts_offset = 0;

    // for (int i = 0; i < _inputCtxs.size(); i++)
    // {
    //     while (!_inputCtxs[i]._input_eof || !_inputCtxs[i]._input_que.empty())
    //     {
    //         // 获取输入帧
    //         {
    //             bool ret = (!_inputCtxs[i]._input_que.empty() || _inputCtxs[i]._input_eof);
    //             std::unique_lock<std::mutex>
    //                 lock(_inputCtxs[i]._mtx);
    //             _inputCtxs[i]._cv.wait(lock, [](bool ret)
    //                                    { return ret; });
    //             if (!_inputCtxs[i]._input_que.empty())
    //             {
    //                 _inputCtxs[i]._fd = _inputCtxs[i]._input_que.front();
    //                 _inputCtxs[i]._input_que.pop();
    //                 _inputCtxs[i]._has_frame = true;
    //             }
    //         }
    //     }
    // }
    // // 处理PTS同步
    // if (_inputCtxs[0]._has_frame && _inputCtxs[1]._has_frame)
    // {
    //     if (_inputCtxs[0]._fd.pts != AV_NOPTS_VALUE)
    //     {
    //         int64_t pts_diff = _inputCtxs[1]._fd.pts - _inputCtxs[0]._fd.pts;
    //         if (pts_diff > 0)
    //         {
    //             // 输入1比输入2快，等待输入2
    //             _inputCtxs[1]._input_que.push(_inputCtxs[1]._fd);
    //             _inputCtxs[1]._has_frame = false;
    //         }
    //         else if (pts_diff < 0)
    //         {
    //             // 输入2比输入1快，等待输入1
    //             _inputCtxs[0]._input_que.push(_inputCtxs[0]._fd);
    //             _inputCtxs[0]._has_frame = false;
    //         }
    //     }
    // }
    // for (int i = 0; i < _inputCtxs.size(); i++)
    // {
    //     while (!_inputCtxs[i]._input_eof || !_inputCtxs[i]._input_que.empty())
    //     {
    //         // 向滤镜图添加帧
    //         if (_inputCtxs[0]._has_frame)
    //         {
    //             av_buffersrc_add_frame_flags(_filters[i], _inputCtxs[i]._fd.frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    //             av_frame_free(&_inputCtxs[i]._fd.frame);
    //         } // 从滤镜图获取处理后的帧
    //         while (true)
    //         {
    //             int ret = av_buffersink_get_frame(_sink, filt_frame);
    //             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    //                 break;

    //             // 处理PTS连续性
    //             if (filt_frame->pts != AV_NOPTS_VALUE)
    //             {
    //                 if (last_pts == AV_NOPTS_VALUE)
    //                 {
    //                     last_pts = filt_frame->pts;
    //                 }
    //                 else
    //                 {
    //                     if (filt_frame->pts <= last_pts)
    //                     {
    //                         pts_offset += last_pts - filt_frame->pts + 1;
    //                     }
    //                     last_pts = filt_frame->pts;
    //                 }
    //                 filt_frame->pts += pts_offset;
    //             }

    //             // 写入输出文件
    //             AVPacket pkt;
    //             av_init_packet(&pkt);
    //             pkt.data = filt_frame->data[0];
    //             pkt.size = av_samples_get_buffer_size(NULL, filt_frame->channels,
    //                                                   filt_frame->nb_samples,
    //                                                   (AVSampleFormat)filt_frame->format, 1);
    //             pkt.pts = filt_frame->pts;
    //             pkt.dts = filt_frame->pts;
    //             av_write_frame(out_fmt_ctx, &pkt);
    //             av_packet_unref(&pkt);
    //             av_frame_unref(filt_frame);
    //         }
    //     }
    // }

    // // 清理资源
    // av_write_trailer(out_fmt_ctx);
    // avio_closep(&out_fmt_ctx->pb);
    // avformat_free_context(out_fmt_ctx);
    // avfilter_graph_free(&_filterGraph);
    // av_frame_free(&filt_frame);
    // t1.join();
    // t2.join();
    // avformat_close_input(&_inputCtxs[0]._fmtCtx);
    // avformat_close_input(&_inputCtxs[1]._fmtCtx);
}

void AmixTask::run1()
{
    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", OUTPUT_FILE);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_AAC; // AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = TARGET_SAMPLE_RATE;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16; // AV_SAMPLE_FMT_S16;
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);
    avformat_write_header(out_fmt_ctx, NULL);
    // 8. 音频帧处理循环
    AVPacket tmpPkt;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *out_frame = av_frame_alloc();
    int64_t last_dts = AV_NOPTS_VALUE;
    while (_running)
    {
        // 应该在这里调用 av_read_frame + av_buffersrc_add_frame + av_buffersink_get_frame
        for (size_t i = 0; i < _inputCtxs.size(); ++i)
        {
            if (av_read_frame(_inputCtxs[i]._fmtCtx, &tmpPkt) < 0)
                continue;
            if (tmpPkt.stream_index != _inputCtxs[i]._index)
                continue;
            if (avcodec_send_packet(_inputCtxs[i]._decCtx, &tmpPkt) >= 0)
            {
                // 初始化重采样器：转换采样格式为S16LE
                struct SwrContext *swr = swr_alloc_set_opts(NULL,
                                                            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, TARGET_SAMPLE_RATE,
                                                            _inputCtxs[i]._decCtx->channel_layout, _inputCtxs[i]._decCtx->sample_fmt, _inputCtxs[i]._decCtx->sample_rate, 0, NULL);
                swr_init(swr);
                while (avcodec_receive_frame(_inputCtxs[i]._decCtx, frame) >= 0)
                {
                    // 将帧推送到过滤器图
                    av_buffersrc_add_frame(_filters[i], frame);
                }
                av_packet_unref(&tmpPkt);
                av_frame_unref(frame);
                while (av_buffersink_get_frame(_sink, out_frame) >= 0)
                {
                    // 优化后的时间戳转换
                    out_frame->pts = av_rescale_q_rnd(
                        out_frame->pts,
                        _sink->inputs[0]->time_base, // 输入时间基准
                        out_stream->time_base,       // 输出时间基准
                        (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    // 处理DTS确保连续性但不强制单调
                    if (last_dts != AV_NOPTS_VALUE)
                    {
                        out_frame->pkt_dts = av_rescale_q_rnd(
                            std::max(last_dts + 1, out_frame->pts),
                            _sink->inputs[0]->time_base,
                            out_stream->time_base,
                            AV_ROUND_NEAR_INF);
                    }
                    else
                    {
                        out_frame->pkt_dts = out_frame->pts;
                    }
                    last_dts = out_frame->pkt_dts;
                    pkt->pts = pkt->dts = last_dts;
                    // 配置输出包参数
                    pkt->stream_index = out_stream->index;
                    // 平面转打包处理
                    uint8_t *converted_data = NULL;
                    av_samples_alloc(&converted_data, NULL, out_frame->channels, out_frame->nb_samples,
                                     AV_SAMPLE_FMT_S16, 0);
                    swr_convert(swr, &converted_data, out_frame->nb_samples,
                                (const uint8_t **)out_frame->data, out_frame->nb_samples);
                    // 写入WAV文件
                    pkt->data = converted_data;
                    pkt->size = av_samples_get_buffer_size(NULL, 2, out_frame->nb_samples,
                                                           AV_SAMPLE_FMT_S16, 0);
                    av_write_frame(out_fmt_ctx, pkt);
                    // 释放资源
                    av_freep(&converted_data);
                    av_frame_unref(out_frame);
                }
                swr_free(&swr);
            }
        }
        // 从过滤器图获取处理后的帧
        // while (1)
        // {
        //     int ret = av_buffersink_get_frame(_sink, out_frame);
        //     if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        //     {
        //         break;
        //     }
        //     else if (ret < 0)
        //     {
        //         break;
        //     }
        //     // 写入输出文件
        //     AVPacket out_pkt;
        //     av_init_packet(&out_pkt);
        //     out_pkt.data = nullptr;
        //     out_pkt.size = 0;
        //     ret = avcodec_send_frame(out_codec_ctx, out_frame);
        //     if (ret < 0)
        //     {
        //         av_frame_unref(out_frame);
        //         break;
        //     }
        //     while (ret >= 0)
        //     {
        //         ret = avcodec_receive_packet(out_codec_ctx, &out_pkt);
        //         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        //         {
        //             break;
        //         }
        //         else if (ret < 0)
        //         {
        //             break;
        //         }
        //         av_packet_rescale_ts(&out_pkt, _sink->inputs[0]->time_base, out_stream->time_base);
        //         out_pkt.stream_index = out_stream->index;
        //         ret = av_interleaved_write_frame(out_fmt_ctx, &out_pkt);
        //         if (ret < 0)
        //         {
        //             av_packet_unref(&out_pkt);
        //             break;
        //         }
        //         av_packet_unref(&out_pkt);
        //     }
        //     av_frame_unref(out_frame);
        // }

        // while (av_buffersink_get_frame(_sink, out_frame) >= 0)
        // {
        //     // 转换采样格式为S16LE
        //     struct SwrContext *swr = swr_alloc_set_opts(NULL,
        //                                                 AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, TARGET_SAMPLE_RATE,
        //                                                 out_frame->channel_layout, (AVSampleFormat)out_frame->format, out_frame->sample_rate, 0, NULL);
        //     swr_init(swr);
        //     AVRational itm_base = {1, out_stream->codecpar->sample_rate};
        //     AVRational otm_base = out_stream->time_base;
        //     if (out_frame->pts == AV_NOPTS_VALUE)
        //     {
        //         out_frame->pts = last_dts != AV_NOPTS_VALUE ? last_dts + out_frame->nb_samples : 0;
        //     }
        //     // 强制单调递增检查
        //     int64_t current_dts = av_rescale_q(out_frame->pts, itm_base, otm_base);
        //     if (last_dts != AV_NOPTS_VALUE && current_dts <= last_dts)
        //     {
        //         current_dts = last_dts + 1;
        //     }
        //     last_dts = current_dts;
        //     // pkt->pts = av_rescale_q(out_frame->pts, itm_base, otm_base);
        //     // pkt->dts = pkt->pts;
        //     pkt->pts = pkt->dts = current_dts;
        //     // 配置输出包参数
        //     pkt->duration = av_rescale_q(out_frame->nb_samples,
        //                                  itm_base, otm_base);
        //     pkt->stream_index = out_stream->index;
        //     // pkt->stream_index = out_stream->index;
        //     // pkt->duration = av_rescale_q(1, out_stream->time_base,
        //     //                              (AVRational){1, out_frame->sample_rate});
        //     // pkt->time_base = (AVRational){1, 44100};
        //     uint8_t *converted_data = NULL;
        //     av_samples_alloc(&converted_data, NULL, 2, out_frame->nb_samples,
        //                      AV_SAMPLE_FMT_S16, 0);
        //     swr_convert(swr, &converted_data, out_frame->nb_samples,
        //                 (const uint8_t **)out_frame->data, out_frame->nb_samples);
        //     // 写入WAV文件
        //     pkt->data = converted_data;
        //     pkt->size = av_samples_get_buffer_size(NULL, 2, out_frame->nb_samples,
        //                                            AV_SAMPLE_FMT_S16, 0);
        //     av_write_frame(out_fmt_ctx, pkt);
        //     // 释放资源
        //     swr_free(&swr);
        //     av_freep(&converted_data);
        //     av_frame_unref(out_frame);
        // }
    }
    // 收尾处理
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// 可以直接使用的线程
void AmixTask::run3()
{
    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", OUTPUT_FILE);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE; // AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = TARGET_SAMPLE_RATE;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16; // AV_SAMPLE_FMT_S16;
    out_stream->codecpar->bits_per_coded_sample = 16; // 设置位深度
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);

    // 显式设置扩展信息
    avformat_write_header(out_fmt_ctx, NULL);
    // 8. 音频帧处理循环
    AVPacket tmpPkt;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *out_frame = av_frame_alloc();
    int64_t last_dts = AV_NOPTS_VALUE;
    uint8_t *converted_data = NULL;
    _iPts = 0;
    pkt->pts = pkt->dts = pkt->size = 0;
    double retio = (double)44100 / (double)48000;
    // 初始化重采样器：转换采样格式为S16LE
    struct SwrContext *swr = swr_alloc_set_opts(NULL,                                  // 空上下文
                                                AV_CH_LAYOUT_STEREO,                   // 输出声道布局
                                                AV_SAMPLE_FMT_S16,                     // 输出采样格式
                                                TARGET_SAMPLE_RATE,                    // 输出采样率
                                                _inputCtxs[0]._decCtx->channel_layout, // 输入升到布局
                                                _inputCtxs[0]._decCtx->sample_fmt,     // 输入采样格式
                                                44100,                                 // 输入采样率
                                                0,                                     // 日志偏移
                                                NULL);                                 // 日志上下文
    swr_init(swr);
    while (_running)
    {
        // 应该在这里调用 av_read_frame + av_buffersrc_add_frame + av_buffersink_get_frame
        for (size_t i = 0; i < _inputCtxs.size(); ++i)
        {
            if (av_read_frame(_inputCtxs[i]._fmtCtx, &tmpPkt) < 0)
                continue;
            if (tmpPkt.stream_index != _inputCtxs[i]._index)
                continue;
            if (tmpPkt.pos == 76)
            {
                tmpPkt.pos += 76;
            }
            if (avcodec_send_packet(_inputCtxs[i]._decCtx, &tmpPkt) >= 0)
            {
                while (avcodec_receive_frame(_inputCtxs[i]._decCtx, frame) >= 0)
                {
                    // 将帧推送到过滤器图
                    av_buffersrc_add_frame(_filters[i], frame);
                }
                av_packet_unref(&tmpPkt);
                av_frame_unref(frame);
                // _iPts = 0;
                while (av_buffersink_get_frame(_sink, out_frame) >= 0)
                {
                    // write_wav(out_frame, OUTPUT_FILE);
                    // 优化后的时间戳转换
                    // out_frame->pts = av_rescale_q_rnd(
                    //     out_frame->pts,
                    //     _sink->inputs[i]->time_base, // 目标时间基
                    //     out_stream->time_base,       // 原时间基
                    //     (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    // out_frame->pts = av_rescale_q(
                    //     out_frame->pts,
                    //     _sink->inputs[i]->time_base, // 目标时间基
                    //     out_stream->time_base        // 原时间基
                    // );
                    // // 处理DTS确保连续性但不强制单调
                    // if (last_dts != AV_NOPTS_VALUE)
                    // {
                    //     out_frame->pkt_dts = av_rescale_q(
                    //         std::max(last_dts + 1, out_frame->pts),
                    //         out_stream->time_base,
                    //         _sink->inputs[i]->time_base);
                    // }
                    // else
                    // {
                    //     out_frame->pkt_dts = out_frame->pts;
                    // }
                    // last_dts = out_frame->pkt_dts;
                    // retio = 1 / retio;
                    // int pts = out_frame->pts * re;
                    // int pts1 = out_frame->nb_samples * retio;
                    // std::cout << "pts is " << pkt->pts << std::endl;

                    // pkt->pts = pkt->dts += 512; // last_dts;
                    int pts1 = out_frame->pts * retio;
                    pkt->pts = pkt->dts += pts1; // out_frame->pts;
                    // 配置输出包参数
                    pkt->stream_index = out_stream->index;
                    // 平面转打包处理
                    if (pkt->size == 0)
                        pkt->size = av_samples_get_buffer_size(NULL, 2, out_frame->nb_samples,
                                                               AV_SAMPLE_FMT_S16, 0);

                    if (!converted_data)
                        av_samples_alloc(&converted_data, NULL, out_frame->channels, out_frame->nb_samples,
                                         AV_SAMPLE_FMT_S16, 0);
                    // 将FLTP转化成S16
                    swr_convert(swr, &converted_data, out_frame->nb_samples,
                                (const uint8_t **)out_frame->data, out_frame->nb_samples);
                    // 写入WAV文件
                    pkt->data = converted_data;

                    av_write_frame(out_fmt_ctx, pkt);

                    // pushPkts(pkt);
                    // AVPacket dstPkt;
                    // av_packet_ref(&dstPkt, pkt);
                    // _pkts.push(dstPkt);
                    // 释放资源
                    av_frame_unref(out_frame);
                }
            }
        }
    }
    // 收尾处理
    // int iFrame = 0;
    // while (!_pkts.empty())
    // {
    //     int tstRet = av_write_frame(out_fmt_ctx, &_pkts.front());
    //     if (tstRet < 0)
    //     {
    //         // std::cout << "frame " << iFrame << " wrote error" << std::endl;
    //     }
    //     _pkts.pop(); // 移除已处理元素
    // }

    swr_free(&swr);
    av_freep(&converted_data);
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

bool AmixTask::initial()
{
    // 1. 初始化FFmoeg
    avdevice_register_all();
    // 2. 打开输入文件
    _inputCtxs.resize(_devs.size());
    for (size_t i = 0; i < _devs.size(); ++i)
    {
        if (!openInput(_devs[i], _inputCtxs[i], true))
            return -1;
    }

    // 3. 创建滤镜图
    char args[512];
    _filterGraph = avfilter_graph_alloc();
    for (size_t i = 0; i < _inputCtxs.size(); ++i)
    {
        // 4. 创建输入滤镜，并配置输入源
        int samplerate = _inputCtxs[i]._decCtx->sample_rate;
        const char *fmtName = av_get_sample_fmt_name(_inputCtxs[i]._decCtx->sample_fmt);
        AVFilterContext *buffersrc = nullptr;
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        std::string name = "src" + std::to_string(i);

        snprintf(args, sizeof(args),
                 "sample_rate=%d:sample_fmt=%s:channel_layout=stereo:time_base=1/%d",
                 samplerate, fmtName, samplerate);
        if (avfilter_graph_create_filter(&buffersrc, abuffer, name.c_str(), args, nullptr, _filterGraph) < 0)
        {
            std::cerr << "Failed to create abuffer for input " << i << "\n";
            return false;
        }
        _filters.push_back(buffersrc);
    }

    // 5. 创建amix、equalizer以及afftdn滤镜链
    AVFilterContext *amixCtx = nullptr, *eq_ctx = nullptr, *afftdn_ctx = nullptr;
    const AVFilter *amix = avfilter_get_by_name("amix");
    const AVFilter *equalizer = avfilter_get_by_name("equalizer");
    const AVFilter *afftdn = avfilter_get_by_name("afftdn");
    if (avfilter_graph_create_filter(&amixCtx, amix, "amix", "inputs=2", nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create amix filter.\n";
        return false;
    }
    // // 构建均衡器器 "f=240:width_type=q:width=15:g=-20:a=zdf"
    if (avfilter_graph_create_filter(&eq_ctx, equalizer, "equalizer", "f=240:width_type=q:width=15:g=-20:a=zdf", nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create equalizer filter.\n";
        return false;
    }
    // 构建降噪器
    if (avfilter_graph_create_filter(&afftdn_ctx, afftdn, "afftdn", nullptr, nullptr, _filterGraph) < 0)
    {
        std::cerr << "Failed to create noise_reduce filter.\n";
        return false;
    }
    // 创建 aformat 滤镜实例
    // AVFilterContext *aformat_ctx;
    // const AVFilter *aformat = avfilter_get_by_name("aformat");
    // snprintf(args, sizeof(args),
    //          "sample_rates=%d:sample_fmts=%s:channel_layouts=stereo",
    //          44100, av_get_sample_fmt_name(AV_SAMPLE_FMT_S16));
    // if (avfilter_graph_create_filter(&aformat_ctx, aformat, "aformat", args, NULL, _filterGraph))
    // {
    //     std::cerr << "Failed to create aformat filter.\n";
    //     return false;
    // }
    // 创建输出 sink接收源
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
    avfilter_link(amixCtx, 0, eq_ctx, 0);
    avfilter_link(eq_ctx, 0, afftdn_ctx, 0);
    // avfilter_link(afftdn_ctx, 0, aformat_ctx, 0);
    avfilter_link(afftdn_ctx, 0, _sink, 0);
    // 初始化图
    if (avfilter_graph_config(_filterGraph, nullptr) < 0)
    {
        std::cerr << "Failed to configure filter graph.\n";
        return false;
    }
    return true;
}

bool AmixTask::openInput(const std::string &name, InputContext &ictx, bool bFile)
{
    if (!bFile)
    {
        const AVInputFormat *input_fmt = av_find_input_format("alsa");
        if (!input_fmt)
            return false;

        if (avformat_open_input(&ictx._fmtCtx, name.c_str(), input_fmt, nullptr) < 0)
        {
            std::cerr << "Failed to open " << name << "\n";
            return false;
        }
    }
    else
    {
        if (avformat_open_input(&ictx._fmtCtx, name.c_str(), nullptr, nullptr) < 0)
        {
            std::cerr << "Failed to open " << name << "\n";
            return false;
        }
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
    ictx._decCtx->pkt_timebase = ictx._decCtx->time_base;
    return true;
}

void AmixTask::sender()
{
    AVPacket tmpPkt;
    AVFrame *frame = av_frame_alloc();
    while (_running)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 应该在这里调用 av_read_frame + av_buffersrc_add_frame + av_buffersink_get_frame
        for (size_t i = 0; i < _inputCtxs.size(); ++i)
        {
            if (av_read_frame(_inputCtxs[i]._fmtCtx, &tmpPkt) < 0)
                continue;
            if (tmpPkt.stream_index != _inputCtxs[i]._index)
                continue;
            if (tmpPkt.pos == 76)
            {
                tmpPkt.pos += 76;
            }
            if (avcodec_send_packet(_inputCtxs[i]._decCtx, &tmpPkt) >= 0)
            {
                while (avcodec_receive_frame(_inputCtxs[i]._decCtx, frame) >= 0)
                {
                    // 将帧推送到过滤器图
                    // std::cout << " sender sample_rate is " << frame->sample_rate << std::endl;
                    int ret = av_buffersrc_add_frame(_filters[i], frame);
                    // usleep(5000);
                }
                av_packet_unref(&tmpPkt);
                av_frame_unref(frame);
            }
        }
    }
}

void AmixTask::receiver1()
{
    uint8_t *converted_data = NULL;
    AVFrame *out_frame = av_frame_alloc();
    AVPacket pkt;
    pkt.pts = pkt.dts = pkt.size = 0;
    double retio = (double)44100 / (double)48000;

    while (_running)
    {
        while (av_buffersink_get_frame(_sink, out_frame) >= 0)
        {
            if (!_running)
                break;
            std::lock_guard<std::mutex> lock(_mutex);
            std::cout << "_pkts size is " << _pkts.size() << std::endl;
            int pts = out_frame->pts * retio;
            pkt.pts = pkt.dts += pts;
            // 配置输出包参数
            // 平面转打包处理
            if (!converted_data)
                av_samples_alloc(&converted_data, out_frame->linesize, out_frame->channels, out_frame->nb_samples,
                                 (AVSampleFormat)out_frame->format, 0);
            if (pkt.size == 0)
                pkt.size = av_samples_get_buffer_size(out_frame->linesize, 2, out_frame->nb_samples,
                                                      AV_SAMPLE_FMT_FLTP, 0);
            int size = out_frame->linesize[0] - 1;
            if (pkt.size != 0)
            {
                for (int i = 0; i < size; ++i)
                {
                    converted_data[i * 2] = reinterpret_cast<int16_t *>(out_frame->data[0])[i];     // 左声道
                    converted_data[i * 2 + 1] = reinterpret_cast<int16_t *>(out_frame->data[1])[i]; // 右声道
                }
            }
            pkt.stream_index = 0; // out_stream->index;
            pkt.data = converted_data;
            _pkts.push(pkt);
            // 释放资源
            av_frame_unref(out_frame);
            // usleep(2000);
        }
    }
    std::cout << "av_buffersink_get_frame end " << std::endl;
    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, OUTPUT_FILE_FLAC);
    // 添加标题元数据
    av_dict_set(&out_fmt_ctx->metadata, "title",
                "Abandoned Beauty Video 4K HDR Dolby Vision - 8K / 4K Video ULTRA HD", 0);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_FLAC; // AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = 44100;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_FLTP; // AV_SAMPLE_FMT_S16;
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);

    // 显式设置扩展信息
    avformat_write_header(out_fmt_ctx, NULL);
    while (!_pkts.empty())
    {
        bool bret = av_write_frame(out_fmt_ctx, &_pkts.front());
        std::cout << "av_write_frame bret = " << bret << std::endl;
        _pkts.pop();
    }
    std::cout << "av_write_frame end " << std::endl;
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    // if (&converted_data != nullptr)
    //     av_freep(&converted_data);
}
void AmixTask::receiver2()
{
    // 7. 配置输出格式
    AVFormatContext *out_fmt_ctx = NULL;
    avformat_alloc_output_context2(&out_fmt_ctx, NULL, "wav", OUTPUT_FILE);
    // 添加标题元数据
    av_dict_set(&out_fmt_ctx->metadata, "title",
                "Abandoned Beauty Video 4K HDR Dolby Vision - 8K / 4K Video ULTRA HD", 0);
    AVStream *out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    out_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE; // AV_CODEC_ID_PCM_S16LE;
    out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    out_stream->codecpar->sample_rate = 44100;
    out_stream->codecpar->channels = 2;
    out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    out_stream->codecpar->format = AV_SAMPLE_FMT_S16; // AV_SAMPLE_FMT_S16;
    avio_open(&out_fmt_ctx->pb, OUTPUT_FILE, AVIO_FLAG_WRITE);

    // 显式设置扩展信息
    avformat_write_header(out_fmt_ctx, NULL); // 日志上下文
    // SwrContext *swr = swr_alloc_set_opts(NULL,                // 空上下文
    //                                      AV_CH_LAYOUT_STEREO, // 输出声道布局
    //                                      AV_SAMPLE_FMT_S16,   // 输出采样格式
    //                                      44100,               // 输出采样率
    //                                      AV_CH_LAYOUT_STEREO, // 输入升到布局
    //                                      AV_SAMPLE_FMT_FLTP,  // 输入采样格式
    //                                      48000,               // 输入采样率
    //                                      0,                   // 日志偏移
    //                                      NULL);               // 日志上下文
    // int iRet = swr_init(swr);

    /* create resampler context */
    SwrContext *swr = swr_alloc();
    if (!swr)
    {
        fprintf(stderr, "Could not allocate resampler context\n");
    }

    /* set options */
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(swr, "in_chlayout", &ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", 48000, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    av_opt_set_chlayout(swr, "out_chlayout", &ch_layout, 0);
    av_opt_set_int(swr, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_int(swr, "dither_method", SWR_DITHER_TRIANGULAR, 0); // 添加三角形抖动抑制量化噪声
    av_opt_set_double(swr, "rematrix_maxval", 0.99, 0);             // 设置压缩阈值避免削波
    av_opt_set_int(swr, "filter_size", 32, 0);                      // 加长滤波器:启用高质量重采样滤波器

    /* initialize the resampling context */
    if ((swr_init(swr)) < 0)
    {
        fprintf(stderr, "Failed to initialize the resampling context\n");
    }

    // std::cout << "swr_init ret is " << iRet << std::endl;

    uint8_t *converted_data = NULL;
    AVFrame *out_frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    pkt->pts = pkt->dts = pkt->size = 0;
    double retio = (double)44100 / (double)48000;

    while (_running)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        while (av_buffersink_get_frame(_sink, out_frame) >= 0)
        {
            if (!_running)
                break;
            int pts = out_frame->pts * retio;
            {
                pkt->pts = pkt->dts += pts;
                // 配置输出包参数
                pkt->stream_index = out_stream->index;
                // 平面转打包处理
                if (pkt->size == 0)
                    pkt->size = av_samples_get_buffer_size(NULL, 2, out_frame->nb_samples,
                                                           AV_SAMPLE_FMT_S16, 0);

                if (!converted_data)
                    av_samples_alloc(&converted_data, NULL, out_frame->channels, out_frame->nb_samples,
                                     AV_SAMPLE_FMT_S16, 0);
                // 将FLTP转化成S16
                int out_samples = swr_get_out_samples(swr, out_frame->nb_samples);
                int iRet = swr_convert(swr, &converted_data, out_samples,
                                       (const uint8_t **)out_frame->data, out_frame->nb_samples);
                // std::cout << "out_samples is " << out_samples << " iRet is " << iRet << " out_frame format is " << out_frame->format << std::endl;
                // 写入WAV文件
                pkt->data = converted_data;
                AVPacket dst_pkt;
                av_packet_ref(&dst_pkt, pkt); // 深拷贝数据
                _pkts.push(dst_pkt);
            }
            // av_write_frame(out_fmt_ctx, pkt);
            // 释放资源
            av_frame_unref(out_frame);
            // usleep(12800);
        }
    }
    std::cout << "av_buffersink_get_frame end " << std::endl;
    while (!_pkts.empty())
    {
        av_write_frame(out_fmt_ctx, &_pkts.front());
        _pkts.pop();
    }
    std::cout << "av_write_frame end " << std::endl;
    swr_free(&swr);
    av_freep(&converted_data);
    av_write_trailer(out_fmt_ctx);
    avio_closep(&out_fmt_ctx->pb);
    avformat_free_context(out_fmt_ctx);
    av_packet_free(&pkt);
}
void AmixTask::receiver()
{
    FILE *pcm_file = fopen("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/output.fltp", "wb");
    // FILE *wavfile = fopen("/home/chentiancan/Projects_lcy/AmixDemo/audioFiles/output.wav", "wb");
    AVFrame *out_frame = av_frame_alloc();
    while (_running)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        while (av_buffersink_get_frame(_sink, out_frame) >= 0)
        {
            write_fltp_frame(out_frame, pcm_file);
            // wrireFrame(out_frame, wavfile);
            // 释放资源
            av_frame_unref(out_frame);
            // usleep(12800);
        }
    }
    fclose(pcm_file);
}

void AmixTask::normalizeStream(AVFrame *frame, const AVCodecContext *codec_ctx)
{
    static SwrContext *swr_ctx = nullptr;
    const AVSampleFormat target_fmt = AV_SAMPLE_FMT_S16;
    const int target_rate = 48000;
    const uint64_t target_layout = AV_CH_LAYOUT_STEREO;

    // 初始化重采样器（单例模式）
    if (!swr_ctx)
    {
        swr_ctx = swr_alloc_set_opts(
            nullptr,
            target_layout,
            target_fmt,
            target_rate,
            codec_ctx->channel_layout,
            codec_ctx->sample_fmt,
            codec_ctx->sample_rate,
            0, nullptr);
        swr_init(swr_ctx);
    }

    // 执行格式转换
    AVFrame *converted_frame = av_frame_alloc();
    converted_frame->sample_rate = target_rate;
    converted_frame->format = target_fmt;
    converted_frame->channel_layout = target_layout;
    swr_convert_frame(swr_ctx, converted_frame, frame);
    av_frame_unref(frame);
    av_frame_move_ref(frame, converted_frame);
    av_frame_free(&converted_frame);
}

void AmixTask::wrireFrame(AVFrame *frame, FILE *wavFile)
{
    // 初始化重采样上下文（FLTP→S16）
    SwrContext *swr = swr_alloc_set_opts(NULL,
                                         av_get_default_channel_layout(frame->channels), // 输出布局
                                         AV_SAMPLE_FMT_S16,                              // 输出格式
                                         frame->sample_rate,                             // 输出采样率
                                         av_get_default_channel_layout(frame->channels), // 输入布局
                                         AV_SAMPLE_FMT_FLTP,                             // 输入格式
                                         frame->sample_rate,                             // 输入采样率
                                         0, NULL);
    swr_init(swr);

    // 分配转换缓冲区
    uint8_t *converted_data = NULL;
    av_samples_alloc(&converted_data, NULL,
                     frame->channels,
                     frame->nb_samples,
                     (AVSampleFormat)frame->format, 0);

    // 执行格式转换
    swr_convert(swr, &converted_data, frame->nb_samples,
                (const uint8_t **)frame->data, frame->nb_samples);

    // 写入文件（交错格式数据在converted_data[0]）
    fwrite(&converted_data[0], 1,
           frame->nb_samples * frame->channels * 2, // S16每样本2字节
           wavFile);

    // 释放资源
    av_freep(&converted_data);
    swr_free(&swr);
}

void AmixTask::clearPkts()
{
    while (!_pkts.empty())
    {
        av_packet_unref(&_pkts.front());
        _pkts.pop();
    }
}

int AmixTask::validate_timestamp(int64_t ts, int64_t prev_ts)
{
    const int MAX_JUMP = 90000 * 2; // 允许2秒的跳跃
    return (ts == AV_NOPTS_VALUE) ||
           (prev_ts == AV_NOPTS_VALUE) ||
           (llabs(ts - prev_ts) < MAX_JUMP);
}

int64_t AmixTask::safe_rescale_timestamp(AVFrame *frame, AVRational in_tb, AVRational out_tb)
{
    if (frame->pts == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;

    int64_t rescaled = av_rescale_q_rnd(
        frame->pts, in_tb, out_tb,
        (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

    // 防止整数溢出
    if (rescaled < INT64_MIN / 2 || rescaled > INT64_MAX / 2)
    {
        return av_rescale(frame->pts, in_tb.num, in_tb.den) /
               av_rescale(1, out_tb.num, out_tb.den);
    }
    return rescaled;
}

void AmixTask::write_fltp_frame(AVFrame *frame, FILE *outfile)
{ // 校验格式参数
    if (frame->format != AV_SAMPLE_FMT_FLTP)
    {
        fprintf(stderr, "非FLTP格式帧\n");
        return;
    }

    // 计算每通道样本数
    int samples = frame->nb_samples;
    int channels = frame->ch_layout.nb_channels;

    // 交错写入各通道数据
    for (int s = 0; s < samples; ++s)
    {
        for (int c = 0; c < channels; ++c)
        {
            float *channel_data = (float *)frame->extended_data[c];
            fwrite(&channel_data[s], sizeof(float), 1, outfile);
        }
    }
}

// int AmixTask::read_input(AVFormatContext *fmt_ctx, int stream_idx, std::queue<FrameData> &queue, std::mutex &mtx, std::condition_variable &cv, bool &eof_flag
// {
//     AVCodecContext *codec_ctx = NULL;
//     const AVCodec *codec = NULL;
//     codec = avcodec_find_decoder(fmt_ctx->streams[stream_idx]->codecpar->codec_id);
//     codec_ctx = avcodec_alloc_context3(codec);
//     avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_idx]->codecpar);
//     avcodec_open2(codec_ctx, codec, NULL);
//     while (true)
//     {
//         AVPacket packet;
//         int ret = av_read_frame(fmt_ctx, &packet);
//         if (ret < 0)
//         {
//             eof_flag = true;
//             cv.notify_all();
//             break;
//         }
//         if (packet.stream_index != stream_idx)
//         {
//             av_packet_unref(&packet);
//             continue;
//         }
//         AVFrame *frame = av_frame_alloc();
//         ret = avcodec_send_packet(codec_ctx, &packet);
//         if (ret < 0)
//         {
//             av_frame_free(&frame);
//             av_packet_unref(&packet);
//             continue;
//         }
//         ret = avcodec_receive_frame(codec_ctx, frame);
//         if (ret < 0)
//         {
//             av_frame_free(&frame);
//             av_packet_unref(&packet);
//             continue;
//         }
//         std::unique_lock<std::mutex> lock(mtx);
//         queue.push({frame, packet.stream_index, packet.pts});
//         cv.notify_one();
//         av_packet_unref(&packet);
//     }
//     return 0;
// }
