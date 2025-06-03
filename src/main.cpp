#include "AmixTask.h"

int main(int argc, char **argv)
{
    // 程序代码
    std::vector<std::string> devs = {"hw:0,0", "hw:0,2"};
    AmixTask task(devs);
    task.start();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    task.stop();
    return 0;
}