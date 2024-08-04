#include <iostream>
#include <map>
#include <chrono>
#include <coroutine>
#include <queue>
#include <string>
#include <thread>

/**
 * @brief 协程模式: 不暂停
 */
struct RepeatAwaiter {
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (coroutine.done())
            return std::noop_coroutine();
        else
            return coroutine;
    }

    void await_resume() const noexcept {}
};

/**
 * @brief 协程模式: 暂停, 会运行之前的协程
 */
struct PreviousAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (_previous)
            return _previous;
        else
            return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

    std::coroutine_handle<> _previous; // 之前的协程
};

template <class T>
struct Promise {
    auto initial_suspend() { 
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(_previous);
    }

    void unhandled_exception() noexcept {}

    void return_value(T res) {
        new (&_previous) T(std::move(res)); // 设置协程的返回值
    }

    auto yield_value(T res) {
        new (&_previous) T(std::move(res)); // 设置协程的返回值
        return std::suspend_always();       // 挂起协程
    }

    T result() {
        T res = std::move(_res);
        _res.~T();
        return res;
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise() = default;
    Promise(Promise &&) = delete;
    ~Promise() = default;


    union {
        T _res;
    };
    
    std::coroutine_handle<> _previous {}; // 上一个协程句柄
};

template <>
struct Promise<void> {
    auto initial_suspend() { 
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(_previous);
    }

    void unhandled_exception() noexcept {}

    void return_void() noexcept {
    }

    void result() {
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise() = default;
    Promise(Promise &&) = delete;
    ~Promise() = default;
    
    std::coroutine_handle<> _previous {}; // 上一个协程句柄
};

template <class T = void>
struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
        : _coroutine(coroutine) {}

    Task(Task &&) = delete;

    ~Task() {
        _coroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
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
    co_return 1;
}

Task<double> taskFun02() {
    std::cout << "hello2开始睡2秒\n";
    co_await sleep_for(2s); // 1s 等价于 std::chrono::seconds(1);
    std::cout << "hello2睡醒了\n";
    co_return 11.4514;
}

int main() {
    /**
     * @brief 计划: 制作一个协程定时器
     *        功能: 比如暂停 1s 和 2s, 最终只会暂停 min(1s, 2s)
     *        目的: 深入理解协程
     */
    auto task_01 = taskFun01();
    auto task_02 = taskFun02();
    Loop::getLoop().addTask(task_01);
    Loop::getLoop().addTask(task_02);
    Loop::getLoop().runAll();
    return 0;
}