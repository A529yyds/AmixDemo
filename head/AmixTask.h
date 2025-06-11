/*************************************
 * AmixTask.h
 * 功能描述：通过ffmpeg的Amix过滤器把多个音频做实时融合
 * 创建时间：2025.06.03
 *************************************/
#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

class AVFormatContext;
class AVFilterContext;
class AVFilterGraph;
class AVFilter;
class AVCodecContext;
class AVFrame;
class AVRational;
class AVPacket;

struct FrameData
{
    AVFrame *frame;
    int stream_index;
    int64_t pts;
};

struct InputContext
{
    AVFormatContext *_fmtCtx = nullptr;
    AVCodecContext *_decCtx = nullptr;
    int _index = -1;

    // std::queue<FrameData> _input_que;
    // std::mutex _mtx;
    // std::condition_variable _cv;
    // bool _input_eof;
    // FrameData _fd;
    // bool _has_frame;
};

class AmixTask
{
public:
    AmixTask(const std::vector<std::string> &devices);
    ~AmixTask();
    void start();
    void stop();

private:
    void run();
    void run1();
    void run2();
    void run3();
    void run4();
    bool initial();

    void sender();
    void receiver();
    void receiver2();
    void receiver1();

    bool openInput(const std::string &name, InputContext &ictx, bool bFile);
    void normalizeStream(AVFrame *frame, const AVCodecContext *codec_ctx);
    void wrireFrame(AVFrame *frame, FILE *wavFile);
    void clearPkts();

    // 时间戳接口
    static int validate_timestamp(int64_t ts, int64_t prev_ts);
    int64_t safe_rescale_timestamp(AVFrame *frame, AVRational in_tb, AVRational out_tb);

    void write_fltp_frame(AVFrame *frame, FILE *outfile);
    // thread method
    // static int read_input(AVFormatContext *fmt_ctx, int stream_idx,
    //                       std::queue<FrameData> &queue, std::mutex &mtx,
    //                       std::condition_variable &cv, bool &eof_flag);

private:
    // std::thread _worker;
    std::thread _sWorker;
    std::thread _rWorker;
    std::atomic<bool> _running{false};
    std::vector<std::string> _devs;
    std::vector<InputContext> _inputCtxs;
    std::vector<AVFormatContext *> _inputs;
    AVFilterGraph *_filterGraph;
    std::vector<AVFilterContext *> _filters;
    AVFilterContext *_sink = nullptr;
    int _iPts = 0;
    std::queue<AVPacket> _pkts;
    std::mutex _mutex;
};