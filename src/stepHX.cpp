#include <string.h>
#include <cstring>
#include <cstdlib>
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
#include <sys/un.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string_view>
#include <source_location>

#include "HX/Task.hpp"

/**
 * @brief 并没有错误处理哦!
 */

using namespace std::chrono;

auto checkError(
    auto res, 
    std::source_location const &loc = std::source_location::current()
) {
    if (res == -1) [[unlikely]] {
        throw std::system_error(
            errno, 
            std::system_category(),
            (std::string)loc.file_name() + ":" + std::to_string(loc.line())
        );
    }
    return res;
}

/**
 * @brief Epoll 事件掩码
 */
using EpollEventMask = uint32_t;

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
    EpollLoop& operator=(EpollLoop&&) = delete;

    explicit EpollLoop() : _epfd(::epoll_create1(0))
                         , _evs()
    {
        _evs.resize(64);
    }

    ~EpollLoop() {
        ::close(_epfd);
    }

    std::vector<struct ::epoll_event> _evs;
public:
    static EpollLoop& get() {
        static EpollLoop loop;
        return loop;
    }

    void removeListener(int fd) {
        ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
        --_count;
    }

    bool addListener(class EpollFilePromise &promise, EpollEventMask mask, int ctl);

    bool run(std::optional<std::chrono::system_clock::duration> timeout);

    int _epfd = -1;
    int _count = 0;
};

struct EpollFilePromise : HX::Promise<EpollEventMask> {
    auto get_return_object() {
        return std::coroutine_handle<EpollFilePromise>::from_promise(*this);
    }

    EpollFilePromise &operator=(EpollFilePromise &&) = delete;

    inline ~EpollFilePromise() {
        if (_fd != -1) {
            EpollLoop::get().removeListener(_fd);
        }
    }

    int _fd = -1;
};

bool EpollLoop::addListener(EpollFilePromise &promise, EpollEventMask mask, int ctl) {
    struct ::epoll_event event;
    event.events = mask;
    event.data.ptr = &promise;
    int res = ::epoll_ctl(_epfd, ctl, promise._fd, &event); // 这里返回了 -1 !!!
    try {
        checkError(res);
    } catch(const std::exception& e) {
        std::cerr << e.what() << " Erron: " << errno << '\n';
    }
    if (res == -1)
        return false;
    if (ctl == EPOLL_CTL_ADD)
        ++_count;
    return true;
}

bool EpollLoop::run(std::optional<std::chrono::system_clock::duration> timeout) {
    if (!_count)
        return false;
    int epollTimeOut = -1;
    if (timeout) {
        epollTimeOut = timeout->count();
    }
    int len = epoll_wait(_epfd, _evs.data(), _evs.size(), epollTimeOut);
    for (int i = 0; i < len; ++i) {
        auto& event = _evs[i];
        // ((EpollFilePromise *)event.data.ptr)->_previous.resume();
        auto& promise = *(EpollFilePromise *)event.data.ptr;
        std::coroutine_handle<EpollFilePromise>::from_promise(promise).resume();
    }
    return true;
}

struct EpollFileAwaiter {
    explicit EpollFileAwaiter(int fd, EpollEventMask mask, EpollEventMask ctl = EPOLL_CTL_ADD) 
        : _fd(fd)
        , _mask(mask)
        , _ctl(ctl)
    {} 

    bool await_ready() const noexcept {
        return false;
    }

    bool await_suspend(std::coroutine_handle<EpollFilePromise> coroutine) {
        auto &promise = coroutine.promise();
        promise._fd = _fd;
        if (!EpollLoop::get().addListener(promise, _mask, _ctl)) {
            promise._fd = -1;
            coroutine.resume();
            return true;
        }
        return false;
    }

    EpollEventMask await_resume() const noexcept {
        return _mask;
    }

    int _fd = -1;
    EpollEventMask _mask = 0;
    int _ctl = EPOLL_CTL_ADD;
};

HX::Task<EpollEventMask, EpollFilePromise> waitFileEvent(int fd, EpollEventMask mask) {
    co_return co_await EpollFileAwaiter(fd, mask);
}

class AsyncFile {
protected:
    int _fd = -1;
public:
    AsyncFile() = default;
    
    explicit AsyncFile(int fd) noexcept : _fd(fd) {
        int flags = ::fcntl(_fd, F_GETFL);
        flags |= O_NONBLOCK;
        ::fcntl(_fd, F_SETFL, flags);

        struct epoll_event event;
        event.events = EPOLLET;
        event.data.ptr = nullptr;
        ::epoll_ctl(EpollLoop::get()._epfd, EPOLL_CTL_ADD, _fd, &event);
    }

    HX::Task<ssize_t, EpollFilePromise> writeFile(std::string_view str) {
        co_await waitFileEvent(_fd, EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT); // 为什么不再这里 co_await ?
        ssize_t writeLen = ::write(_fd, str.data(), str.size());
        if (writeLen == -1 && errno != 11) {
            try {
                checkError(writeLen);
            } catch(const std::exception& e) {
                std::cerr << e.what() << " Erron: " << errno << '\n';
            }
            throw;
        }
        co_return writeLen;
    }

    HX::Task<ssize_t, EpollFilePromise> readFile(std::span<char> buf) {
        co_await waitFileEvent(_fd, EPOLLIN | EPOLLET | EPOLLERR | EPOLLONESHOT);
        ssize_t readLen = ::read(_fd, buf.data(), buf.size());
        if (readLen == -1 && errno != 11) {
            try {
                checkError(readLen);
            } catch(const std::exception& e) {
                std::cerr << e.what() << " Erron: " << errno << '\n';
            }
            throw;
        }
        co_return readLen;
    }

    AsyncFile(AsyncFile &&that) noexcept : _fd(that._fd) {
        that._fd = -1;
    }

    AsyncFile &operator=(AsyncFile &&that) noexcept {
        std::swap(_fd, that._fd);
        return *this;
    }

    int getFd() const {
        return _fd;
    }

    ~AsyncFile() {
        if (_fd == -1) {
            return;
        }
        ::epoll_ctl(EpollLoop::get()._epfd, EPOLL_CTL_DEL, _fd, nullptr);
    }
};

HX::Task<void> socketConnect(
    AsyncFile& fd,
    const struct sockaddr_in& sockaddr
) {
    int res = ::connect(fd.getFd(), (struct sockaddr *)&sockaddr, sizeof(sockaddr));
    if (res == -1) [[unlikely]] { // 这里是干什么??
        co_await waitFileEvent(fd.getFd(), EPOLLOUT | EPOLLET | EPOLLONESHOT);
    }
}

/**
 * @brief 创建tcp ipv4 连接
 * @param ip 
 * @param post 
 * @return HX::Task<AsyncFile> 
 */
HX::Task<AsyncFile> createTcpClientByIpV4(const char *ip, int port) {
    AsyncFile res {::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};

    struct sockaddr_in sockaddr;
    std::memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = inet_addr(ip);
    sockaddr.sin_port = htons(port);

    co_await socketConnect(res, sockaddr);

    co_return std::move(res);
}

HX::Task<void> co_main() {
    auto client = co_await createTcpClientByIpV4("127.0.0.1", 28205);
    co_await client.writeFile("GET / HTTP/1.1\r\n\r\n");
    std::vector<char> buf(4096);
    auto dataSize = co_await client.readFile(buf);
    std::cout << "收到消息长度: " << dataSize << "\n内容是: " << std::string {buf.data(), (std::size_t)dataSize} << '\n';
}

int main() {
    co_main()._coroutine.resume();
    printf("awa\n");
    EpollLoop::get().run(std::nullopt);
    return 0;
}


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

int _main() {
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