#include <chrono>             // 引入时间库，用于时间相关操作
#include <coroutine>          // 引入协程库，用于协程支持
#include <deque>              // 引入双端队列容器，用于任务队列
#include <queue>              // 引入优先队列容器，用于定时器
#include <span>               // 引入 span，用于处理不拥有的数组视图
#include <thread>             // 引入线程库，用于线程相关操作
#include "debug.hpp"          // 引入自定义调试宏

using namespace std::chrono_literals; // 使用 chrono 库的时间字面量

// 用于处理 void 类型的辅助结构体
template <class T = void> struct NonVoidHelper {
    using Type = T;
};

// 特化处理 void 类型
template <> struct NonVoidHelper<void> {
    using Type = NonVoidHelper;

    explicit NonVoidHelper() = default;
};

// 未初始化的值存储结构
template <class T> struct Uninitialized {
    union {
        T mValue;
    };

    Uninitialized() noexcept {}  // 默认构造函数
    Uninitialized(Uninitialized &&) = delete;  // 禁止移动构造
    ~Uninitialized() noexcept {}  // 默认析构函数

    // 移动值并销毁原值
    T moveValue() {
        T ret(std::move(mValue));
        mValue.~T();
        return ret;
    }

    // 初始化值
    template <class... Ts> void putValue(Ts &&...args) {
        new (std::addressof(mValue)) T(std::forward<Ts>(args)...);
    }
};

// 特化处理 void 类型
template <> struct Uninitialized<void> {
    auto moveValue() {
        return NonVoidHelper<>{};
    }

    void putValue(NonVoidHelper<>) {}
};

// 处理 const 类型和引用类型的特化
template <class T> struct Uninitialized<T const> : Uninitialized<T> {};
template <class T> struct Uninitialized<T &> : Uninitialized<std::reference_wrapper<T>> {};
template <class T> struct Uninitialized<T &&> : Uninitialized<T> {};

// 协程等待器概念检查
template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h) {
    { a.await_ready() };
    { a.await_suspend(h) };
    { a.await_resume() };
};

// 可等待的概念检查
template <class A>
concept Awaitable = Awaiter<A> || requires(A a) {
    { a.operator co_await() } -> Awaiter;
};

// 可等待的特征提取
template <class A> struct AwaitableTraits;

template <Awaiter A> struct AwaitableTraits<A> {
    using RetType = decltype(std::declval<A>().await_resume());
    using NonVoidRetType = NonVoidHelper<RetType>::Type;
};

template <class A>
    requires(!Awaiter<A> && Awaitable<A>)
struct AwaitableTraits<A>
    : AwaitableTraits<decltype(std::declval<A>().operator co_await())> {};

// 一些自定义的协程等待器
struct RepeatAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (coroutine.done())
            return std::noop_coroutine();
        else
            return coroutine;
    }

    void await_resume() const noexcept {}
};

struct PreviousAwaiter {
    std::coroutine_handle<> mPrevious;

    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (mPrevious)
            return mPrevious;
        else
            return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

// 协程承诺（Promise）类型
template <class T> struct Promise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_value(T &&ret) {
        mResult.putValue(std::move(ret));
    }

    void return_value(T const &ret) {
        mResult.putValue(ret);
    }

    T result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
        return mResult.moveValue();
    }

    auto get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};
    Uninitialized<T> mResult;

    Promise &operator=(Promise &&) = delete;
};

// void 类型的 Promise 特化
template <> struct Promise<void> {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_void() noexcept {}

    void result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
    }

    auto get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};

    Promise &operator=(Promise &&) = delete;
};

// 协程任务（Task）类型
template <class T = void> struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
        : mCoroutine(coroutine) {}

    Task(Task &&) = delete;

    ~Task() {
        mCoroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept {
            return false;
        }

        std::coroutine_handle<promise_type>
        await_suspend(std::coroutine_handle<> coroutine) const noexcept {
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

// 协程循环调度器
struct Loop {
    std::deque<std::coroutine_handle<>> mReadyQueue; // 就绪队列

    struct TimerEntry {
        std::chrono::system_clock::time_point expireTime; // 超时时间
        std::coroutine_handle<> coroutine; // 协程句柄

        bool operator<(TimerEntry const &that) const noexcept {
            return expireTime > that.expireTime; // 定时器优先级比较
        }
    };

    std::priority_queue<TimerEntry> mTimerHeap; // 定时器堆

    void addTask(std::coroutine_handle<> coroutine) {
        mReadyQueue.push_front(coroutine);
    }

    void addTimer(std::chrono::system_clock::time_point expireTime,
                  std::coroutine_handle<> coroutine) {
        mTimerHeap.push({expireTime, coroutine});
    }

    void runAll() {
        while (!mTimerHeap.empty() || !mReadyQueue.empty()) {
            while (!mReadyQueue.empty()) {
                auto coroutine = mReadyQueue.front();
                debug(), "pop";
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
                    std::this_thread::sleep_until(timer.expireTime);
                }
            }
        }
    }

    Loop &operator=(Loop &&) = delete;
};

// 获取全局 Loop 实例
Loop &getLoop() {
    static Loop loop;
    return loop;
}

// 用于处理 sleep 的协程等待器
struct SleepAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) const {
        getLoop().addTimer(mExpireTime, coroutine);
    }

    void await_resume() const noexcept {}

    std::chrono::system_clock::time_point mExpireTime;
};

// 协程任务：sleep_until
Task<void> sleep_until(std::chrono::system_clock::time_point expireTime) {
    co_await SleepAwaiter(expireTime);
}

// 协程任务：sleep_for
Task<void> sleep_for(std::chrono::system_clock::duration duration) {
    co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
}

// 获取当前协程的协程句柄
struct CurrentCoroutineAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) noexcept {
        mCurrent = coroutine;
        return coroutine;
    }

    auto await_resume() const noexcept {
        return mCurrent;
    }

    std::coroutine_handle<> mCurrent;
};

// 返回之前协程的承诺（Promise）
struct ReturnPreviousPromise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() {
        throw;
    }

    void return_value(std::coroutine_handle<> previous) noexcept {
        mPrevious = previous;
    }

    auto get_return_object() {
        return std::coroutine_handle<ReturnPreviousPromise>::from_promise(
            *this);
    }

    std::coroutine_handle<> mPrevious{};

    ReturnPreviousPromise &operator=(ReturnPreviousPromise &&) = delete;
};

// 任务类型：返回之前的协程
struct ReturnPreviousTask {
    using promise_type = ReturnPreviousPromise;

    ReturnPreviousTask(std::coroutine_handle<promise_type> coroutine) noexcept
        : mCoroutine(coroutine) {}

    ReturnPreviousTask(ReturnPreviousTask &&) = delete;

    ~ReturnPreviousTask() {
        mCoroutine.destroy();
    }

    std::coroutine_handle<promise_type> mCoroutine;
};

// 当所有协程完成时的控制块
struct WhenAllCtlBlock {
    std::size_t mCount; // 协程数量
    std::coroutine_handle<> mPrevious{}; // 上一个协程
    std::exception_ptr mException{}; // 异常指针
};

// 当所有协程完成时的等待器
struct WhenAllAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const {
        if (mTasks.empty()) return coroutine;
        mControl.mPrevious = coroutine;
        for (auto const &t: mTasks.subspan(1))
            getLoop().addTask(t.mCoroutine);
        return mTasks.front().mCoroutine;
    }

    void await_resume() const {
        if (mControl.mException) [[unlikely]] {
            std::rethrow_exception(mControl.mException);
        }
    }

    WhenAllCtlBlock &mControl;
    std::span<ReturnPreviousTask const> mTasks;
};

// 辅助函数：当所有协程完成时
template <class T>
ReturnPreviousTask whenAllHelper(auto const &t, WhenAllCtlBlock &control,
                                 Uninitialized<T> &result) {
    try {
        result.putValue(co_await t);
    } catch (...) {
        control.mException = std::current_exception();
        co_return control.mPrevious;
    }
    --control.mCount;
    if (control.mCount == 0) {
        co_return control.mPrevious;
    }
    co_return nullptr;
}

// 当所有协程完成时的实现
template <std::size_t... Is, class... Ts>
Task<std::tuple<typename AwaitableTraits<Ts>::NonVoidRetType...>>
whenAllImpl(std::index_sequence<Is...>, Ts &&...ts) {
    WhenAllCtlBlock control{sizeof...(Ts)};
    std::tuple<Uninitialized<typename AwaitableTraits<Ts>::RetType>...> result;
    ReturnPreviousTask taskArray[]{whenAllHelper(ts, control, std::get<Is>(result))...};
    co_await WhenAllAwaiter(control, taskArray);
    co_return std::tuple<typename AwaitableTraits<Ts>::NonVoidRetType...>(
        std::get<Is>(result).moveValue()...);
}

// 当所有协程完成时的公共接口
template <Awaitable... Ts>
    requires(sizeof...(Ts) != 0)
auto when_all(Ts &&...ts) {
    return whenAllImpl(std::make_index_sequence<sizeof...(Ts)>{},
                       std::forward<Ts>(ts)...);
}

// 当任何协程完成时的控制块
struct WhenAnyCtlBlock {
    static constexpr std::size_t kNullIndex = std::size_t(-1);

    std::size_t mIndex{kNullIndex}; // 完成的协程索引
    std::coroutine_handle<> mPrevious{}; // 上一个协程
    std::exception_ptr mException{}; // 异常指针
};

// 当任何协程完成时的等待器
struct WhenAnyAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const {
        if (mTasks.empty()) return coroutine;
        mControl.mPrevious = coroutine;
        for (auto const &t: mTasks.subspan(1))
            getLoop().addTask(t.mCoroutine);
        return mTasks.front().mCoroutine;
    }

    void await_resume() const {
        if (mControl.mException) [[unlikely]] {
            std::rethrow_exception(mControl.mException);
        }
    }

    WhenAnyCtlBlock &mControl;
    std::span<ReturnPreviousTask const> mTasks;
};

// 辅助函数：当任何协程完成时
template <class T>
ReturnPreviousTask whenAnyHelper(auto const &t, WhenAnyCtlBlock &control,
                                 Uninitialized<T> &result, std::size_t index) {
    try {
        result.putValue(co_await t);
    } catch (...) {
        control.mException = std::current_exception();
        co_return control.mPrevious;
    }
    --control.mIndex = index;
    co_return control.mPrevious;
}

// 当任何协程完成时的实现
template <std::size_t... Is, class... Ts>
Task<std::variant<typename AwaitableTraits<Ts>::NonVoidRetType...>>
whenAnyImpl(std::index_sequence<Is...>, Ts &&...ts) {
    WhenAnyCtlBlock control{};
    std::tuple<Uninitialized<typename AwaitableTraits<Ts>::RetType>...> result;
    ReturnPreviousTask taskArray[]{whenAnyHelper(ts, control, std::get<Is>(result), Is)...};
    co_await WhenAnyAwaiter(control, taskArray);
    Uninitialized<std::variant<typename AwaitableTraits<Ts>::NonVoidRetType...>> varResult;
    ((control.mIndex == Is && (varResult.putValue(
        std::in_place_index<Is>, std::get<Is>(result).moveValue()), 0)), ...);
    co_return varResult.moveValue();
}

// 当任何协程完成时的公共接口
template <Awaitable... Ts>
    requires(sizeof...(Ts) != 0)
auto when_any(Ts &&...ts) {
    return whenAnyImpl(std::make_index_sequence<sizeof...(Ts)>{},
                       std::forward<Ts>(ts)...);
}

// 示例协程：hello1
Task<int> hello1() {
    debug(), "hello1开始睡1秒";
    co_await sleep_for(1s); // 1s 等价于 std::chrono::seconds(1)
    debug(), "hello1睡醒了";
    co_return 1;
}

// 示例协程：hello2
Task<int> hello2() {
    debug(), "hello2开始睡2秒";
    co_await sleep_for(2s); // 2s 等价于 std::chrono::seconds(2)
    debug(), "hello2睡醒了";
    co_return 2;
}

// 示例协程：hello
Task<int> hello() {
    debug(), "hello开始等1和2";
    auto v = co_await when_any(hello1(), hello2(), hello2());
    debug(), "hello看到", (int)v.index() + 1, "睡醒了";
    co_return std::get<0>(v);
}

// 主函数
int main() {
    auto t = hello();
    getLoop().addTask(t);
    getLoop().runAll();
    debug(), "主函数中得到hello结果:", t.mCoroutine.promise().result();
    return 0;
}
