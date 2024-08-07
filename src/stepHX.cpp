#include <iostream>
#include <map>
#include <chrono>
#include <coroutine>
#include <queue>
#include <string>
#include <thread>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <array>
#include <vector>

#include "HX/Task.hpp"

using namespace std::chrono;

class TimerLoop {
    void addTimer(
        std::chrono::system_clock::time_point expireTime, 
        std::coroutine_handle<> coroutine
    ) {
        _timer.insert({expireTime, coroutine});
    }

public:
    void addTask(std::coroutine_handle<> coroutine) {
        _taskQueue.emplace(coroutine);
    }

    /**
     * @brief 执行全部任务
     */
    void runAll() {
        while (_timer.size() || _taskQueue.size()) {
            while (_taskQueue.size()) { // 执行协程任务
                auto task = std::move(_taskQueue.front());
                _taskQueue.pop();
                task.resume();
            }

            if (_timer.size()) { // 执行计时器任务
                auto now = std::chrono::system_clock::now();
                auto it = _timer.begin();
                if (now >= it->first) {
                    do {
                        it->second.resume();
                        _timer.erase(it);
                        if (_timer.empty())
                            break;
                        it = _timer.begin();
                    } while (now >= it->first);
                } else {
                    std::this_thread::sleep_until(it->first); // 全场睡大觉 [阻塞]
                }
            }
        }
    }

    static TimerLoop& getLoop() {
        static TimerLoop loop;
        return loop;
    }

private:
    /**
     * @brief 暂停者
     */
    struct SleepAwaiter { // 使用 co_await 则需要定义这 3 个固定函数
        bool await_ready() const noexcept { // 暂停
            return false;
        }

        void await_suspend(std::coroutine_handle<> coroutine) const { // `await_ready`后执行: 添加计时器
            TimerLoop::getLoop().addTimer(_expireTime, coroutine);
        }

        void await_resume() const noexcept { // 计时结束
        }

        std::chrono::system_clock::time_point _expireTime; // 过期时间
    };

public:
    /**
     * @brief 暂停指定时间点
     * @param expireTime 时间点, 如 2024-8-4 22:12:23
     */
    HX::Task<void> static sleep_until(std::chrono::system_clock::time_point expireTime) {
        co_await SleepAwaiter(expireTime);
    }

    /**
     * @brief 暂停一段时间
     * @param duration 比如 3s
     */
    HX::Task<void> static sleep_for(std::chrono::system_clock::duration duration) {
        co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
    }

private:
    explicit TimerLoop() : _timer()
                         , _taskQueue()
    {}

    TimerLoop& operator=(TimerLoop&&) = delete;

    /// @brief 计时器红黑树
    std::multimap<std::chrono::system_clock::time_point, std::coroutine_handle<>> _timer;

    /// @brief 任务队列
    std::queue<std::coroutine_handle<>> _taskQueue;
};


class EpollLoop {
    
};

HX::Task<int> taskFun01() {
    std::cout << "hello1开始睡1秒\n";
    co_await TimerLoop::sleep_for(1s); // 1s 等价于 std::chrono::seconds(1);
    std::cout << "hello1睡醒了\n";
    std::cout << "hello1继续睡1秒\n";
    co_await TimerLoop::sleep_for(1s); // 1s 等价于 std::chrono::seconds(1);
    std::cout << "hello1睡醒了\n";
    co_return 1;
}

HX::Task<double> taskFun02() {
    std::cout << "hello2开始睡2秒\n";
    co_await TimerLoop::sleep_for(2s);
    std::cout << "hello2睡醒了\n";
    co_return 11.4514;
}

HX::Task<std::string> taskFun03() {
    std::cout << "hello3开始睡0.5秒\n";
    co_await TimerLoop::sleep_for(500ms);
    std::cout << "hello3睡醒了\n";
    co_return "好难qwq";
}

int main() {
    // 使用epoll监测stdin
#if 0
    // 设置非阻塞
    int ioFd = 1;
    int flags = fcntl(ioFd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(ioFd, F_SETFL, flags);

    int epfd = epoll_create1(0);
    struct ::epoll_event ev;
    ev.data.fd = ioFd;
    ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_ADD, ioFd, &ev);

    while(1) {
        std::array<struct ::epoll_event, 16> evs;
        int len = epoll_wait(epfd, evs.data(), evs.size(), -1);
        for (int i = 0; i < len; ++i) {
            std::string buf;
            char c;
            while (int len = ::read(evs[i].data.fd, &c, 1) > 0) {
                buf.push_back(c);
            }
            std::cout << buf << '\n';
        }
    }
#elif 0
    /**
     * @brief 计划: 制作一个协程定时器
     *        功能: 比如暂停 1s 和 2s, 最终只会暂停 min(1s, 2s)
     *        目的: 深入理解协程
     */
    auto task_01 = taskFun01();
    auto task_02 = taskFun02();
    auto task_03 = taskFun03();
    TimerLoop::getLoop().addTask(task_01);
    TimerLoop::getLoop().addTask(task_02);
    TimerLoop::getLoop().addTask(task_03);
    TimerLoop::getLoop().runAll();
    std::cout << "看看01: " << task_01._coroutine.promise().result() << '\n';
    std::cout << "看看02: " << task_02._coroutine.promise().result() << '\n';
    std::cout << "看看03: " << task_03._coroutine.promise().result() << '\n';
#elif 1

#endif
    return 0;
}