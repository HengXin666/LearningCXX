#include <chrono>
#include <coroutine>
#include <deque>
#include <queue>
#include <thread>
#include "debug.hpp"

using namespace std::chrono_literals;

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
    std::coroutine_handle<> mPrevious;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (mPrevious)
            return mPrevious;
        else
            return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

/**
 * @brief 主模版
 */
template <class T>
struct Promise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    auto yield_value(T ret) noexcept {
        new (&mResult) T(std::move(ret)); // 生成协程的中间结果
        return std::suspend_always();     // 挂起协程
    }

    void return_value(T ret) noexcept {
        new (&mResult) T(std::move(ret)); // 设置协程的返回值
    }

    T result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
        T ret = std::move(mResult);
        mResult.~T();
        return ret;
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this); // 获取协程句柄
    }

    std::coroutine_handle<> mPrevious{}; // 上一个协程句柄
    std::exception_ptr mException{};
    union {
        T mResult;
    };

    Promise() noexcept {}
    Promise(Promise &&) = delete;
    ~Promise() {}
};

template <>
struct Promise<void> {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious); // 协程结束时使用 PreviousAwaiter 进行处理
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_void() noexcept {
    }

    void result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException); // 重新抛出异常
        }
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious{}; // 上一个协程句柄
    std::exception_ptr mException{};

    Promise() = default;
    Promise(Promise &&) = delete;
    ~Promise() = default;
};

template <class T = void>
struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
        : mCoroutine(coroutine) {}

    Task(Task &&) = delete;

    ~Task() {
        mCoroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
            mCoroutine.promise().mPrevious = coroutine;
            return mCoroutine;
        }

        T await_resume() const {
            return mCoroutine.promise().result();
        }

        std::coroutine_handle<promise_type> mCoroutine;
    };

    auto operator co_await() const noexcept {
        return Awaiter(mCoroutine);
    }

    operator std::coroutine_handle<>() const noexcept {
        return mCoroutine;
    }

    std::coroutine_handle<promise_type> mCoroutine;
};

/**
 * @brief 基于小根堆的计时器系统
 */
struct Loop {
    /// @brief 协程句柄队列
    std::deque<std::coroutine_handle<>> mReadyQueue;

    /// @brief 计时器块
    struct TimerEntry {
        std::chrono::system_clock::time_point expireTime; // 结束时间点
        std::coroutine_handle<> coroutine; // 协程句柄

        bool operator<(TimerEntry const &that) const noexcept {
            return expireTime > that.expireTime;
        }
    };

    // 堆
    std::priority_queue<TimerEntry> mTimerHeap;

    /**
     * @brief 添加任务
     * @param coroutine 
     */
    void addTask(std::coroutine_handle<> coroutine) {
        mReadyQueue.push_front(coroutine);
    }

    /**
     * @brief 添加计时器
     * @param expireTime 
     * @param coroutine 
     */
    void addTimer(std::chrono::system_clock::time_point expireTime, std::coroutine_handle<> coroutine) {
        mTimerHeap.push({expireTime, coroutine});
    }

    void runAll() { // 协程调度器?
        while (!mTimerHeap.empty() || !mReadyQueue.empty()) {
            while (!mReadyQueue.empty()) {
                auto coroutine = mReadyQueue.front();
                mReadyQueue.pop_front();
                coroutine.resume();
            }
            if (!mTimerHeap.empty()) {
                auto nowTime = std::chrono::system_clock::now();
                auto timer = std::move(mTimerHeap.top());
                if (timer.expireTime < nowTime) {
                    mTimerHeap.pop();
                    timer.coroutine.resume();
                } else {
                    std::this_thread::sleep_until(timer.expireTime); // 继续等待
                }
            }
        }
    }

    Loop &operator=(Loop &&) = delete;
};

/**
 * @brief 全局懒汉单例
 * @return Loop& 
 */
Loop &getLoop() {
    static Loop loop;
    return loop;
}

struct SleepAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) const {
        getLoop().addTimer(mExpireTime, coroutine);
    }

    void await_resume() const noexcept {
    }

    std::chrono::system_clock::time_point mExpireTime; // 过期时间
};

// 这个才是更加底层的东西
Task<void> sleep_until(std::chrono::system_clock::time_point expireTime) {
    co_await SleepAwaiter(expireTime);
    co_return;
}

// 值得注意的是 sleep_for 实际上是基于 sleep_until 实现的
Task<void> sleep_for(std::chrono::system_clock::duration duration) {
    co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
    co_return;
}

Task<int> hello1() {
    debug(), "hello1开始睡1秒";
    co_await sleep_for(1s); // 1s 等价于 std::chrono::seconds(1)
    debug(), "hello1睡醒了";
    co_return 1;
}

Task<int> hello2() {
    debug(), "hello2开始睡2秒";
    co_await sleep_for(2s); // 2s 等价于 std::chrono::seconds(2)
    debug(), "hello2睡醒了";
    co_return 2;
}

int main() {
    auto t1 = hello1();
    auto t2 = hello2();
    getLoop().addTask(t1);
    getLoop().addTask(t2);
    getLoop().runAll();
    // 单线程这样玩, 会休眠 sum(1, 2) 秒, 而协程定时器, 会休眠 max(1, 2) 秒
    debug(), "主函数中得到hello1结果:", t1.mCoroutine.promise().result();
    debug(), "主函数中得到hello2结果:", t2.mCoroutine.promise().result();
    return 0;
}
