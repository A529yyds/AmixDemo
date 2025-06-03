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

class AVFormatContext;
class AVFilterContext;
class AVFilterGraph;
class AVFilter;
class AVCodecContext;

struct InputContext
{
    AVFormatContext *_fmtCtx = nullptr;
    AVCodecContext *_decCtx = nullptr;
    int _index = -1;
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
    bool initial();
    bool initial1();
    bool openInput(const std::string &dev, InputContext &ictx);

private:
    std::thread _worker;
    std::atomic<bool> _running{false};
    std::vector<std::string> _devs;
    std::vector<InputContext> _inputCtxs;
    std::vector<AVFormatContext *> _inputs;
    AVFilterGraph *_filterGraph;
    std::vector<AVFilterContext *> _filters;
    AVFilterContext *_sink = nullptr;
};