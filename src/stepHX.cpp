#include <iostream>
#include <map>
#include <chrono>
#include <coroutine>
#include <queue>
#include <string>
#include <thread>

#include "HX/Uninitialized.hpp"
#include "HX/RepeatAwaiter.hpp"
#include "HX/PreviousAwaiter.hpp"

template <class T>
struct Promise {
    auto initial_suspend() { 
        return std::suspend_always(); // 第一次创建, 直接挂起
    }

    auto final_suspend() noexcept {
        return HX::PreviousAwaiter(_previous);
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    void return_value(const T& res) {
        _res.putVal(res);
    }

    auto yield_value(T&& res) {
        _res.putVal(res);
        return std::suspend_always(); // 挂起协程
    }

    T result() {
        if (_exception) [[unlikely]] {
            std::rethrow_exception(_exception);
        }
        return _res.moveVal();
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise &operator=(Promise &&) = delete;

    HX::Uninitialized<T> _res;
    
    std::coroutine_handle<> _previous {}; // 上一个协程句柄
    std::exception_ptr _exception {}; // 异常信息
};

template <>
struct Promise<void> {
    auto initial_suspend() { 
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return HX::PreviousAwaiter(_previous);
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    void return_void() noexcept {
    }

    void result() {
        if (_exception) [[unlikely]] {
            std::rethrow_exception(_exception);
        }
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise &operator=(Promise &&) = delete;
    
    std::coroutine_handle<> _previous {}; // 上一个协程句柄
    std::exception_ptr _exception {}; // 异常信息
};

template <class T = void>
struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
        : _coroutine(coroutine) {}

    Task(Task &&) = delete;

    ~Task() {
        if (_coroutine)
            _coroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept { 
            return false; 
        }

        std::coroutine_handle<promise_type> await_suspend(
            std::coroutine_handle<> coroutine
        ) const noexcept {
            _coroutine.promise()._previous = coroutine;
            return _coroutine;
        }

        T await_resume() const {
            return _coroutine.promise().result();
        }

        std::coroutine_handle<promise_type> _coroutine;
    };

    auto operator co_await() const noexcept {
        return Awaiter(_coroutine);
    }

    operator std::coroutine_handle<>() const noexcept {
        return _coroutine;
    }

    std::coroutine_handle<promise_type> _coroutine; // 当前协程句柄
};

struct Loop {
    void addTask(std::coroutine_handle<> coroutine) {
        _taskQueue.emplace(coroutine);
    }

    void addTimer(
        std::chrono::system_clock::time_point expireTime, 
        std::coroutine_handle<> coroutine
    ) {
        _timer.insert({expireTime, coroutine});
    }

    void runAll() {
        while (_timer.size() || _taskQueue.size()) {
            while (_taskQueue.size()) {
                auto task = std::move(_taskQueue.front());
                _taskQueue.pop();
                task.resume();
            }

            if (_timer.size()) {
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
                    std::this_thread::sleep_until(it->first);
                }
            }
        }
    }

    static Loop& getLoop() {
        static Loop loop;
        return loop;
    }

private:
    explicit Loop() : _timer()
                    , _taskQueue()
    {}

    Loop& operator=(Loop&&) = delete;

    /// @brief 计时器红黑树
    std::multimap<std::chrono::system_clock::time_point, std::coroutine_handle<>> _timer;

    /// @brief 任务队列
    std::queue<std::coroutine_handle<>> _taskQueue;
};

/**
 * @brief 暂停者
 */
struct SleepAwaiter { // 使用 co_await 则需要定义这 3 个固定函数
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) const {
        Loop::getLoop().addTimer(_expireTime, coroutine);
    }

    void await_resume() const noexcept {
    }

    std::chrono::system_clock::time_point _expireTime; // 过期时间
};

/**
 * @brief 暂停指定时间点
 * @param expireTime 时间点, 如 2024-8-4 22:12:23
 */
Task<void> sleep_until(std::chrono::system_clock::time_point expireTime) {
    co_await SleepAwaiter(expireTime);
}

/**
 * @brief 暂停一段时间
 * @param duration 比如 3s
 */
Task<void> sleep_for(std::chrono::system_clock::duration  duration) {
    co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
}

using namespace std::chrono;

Task<int> taskFun01() {
    std::cout << "hello1开始睡1秒\n";
    co_await sleep_for(1s); // 1s 等价于 std::chrono::seconds(1);
    std::cout << "hello1睡醒了\n";
    std::cout << "hello1继续睡1秒\n";
    co_await sleep_for(1s); // 1s 等价于 std::chrono::seconds(1);
    std::cout << "hello1睡醒了\n";
    co_return 1;
}

Task<double> taskFun02() {
    std::cout << "hello2开始睡2秒\n";
    co_await sleep_for(2s);
    std::cout << "hello2睡醒了\n";
    co_return 11.4514;
}

Task<std::string> taskFun03() {
    std::cout << "hello3开始睡0.5秒\n";
    co_await sleep_for(500ms);
    std::cout << "hello3睡醒了\n";
    co_return "好难qwq";
}

#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <array>
#include <vector>

int main() {
    // 使用epoll监测stdin

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
#if 0
    /**
     * @brief 计划: 制作一个协程定时器
     *        功能: 比如暂停 1s 和 2s, 最终只会暂停 min(1s, 2s)
     *        目的: 深入理解协程
     */
    auto task_01 = taskFun01();
    auto task_02 = taskFun02();
    auto task_03 = taskFun03();
    Loop::getLoop().addTask(task_01);
    Loop::getLoop().addTask(task_02);
    Loop::getLoop().addTask(task_03);
    Loop::getLoop().runAll();
    std::cout << "看看01: " << task_01._coroutine.promise().result() << '\n';
    std::cout << "看看02: " << task_02._coroutine.promise().result() << '\n';
    std::cout << "看看03: " << task_03._coroutine.promise().result() << '\n';
#endif
    return 0;
}