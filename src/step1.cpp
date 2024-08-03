#include <chrono>
#include <coroutine>
#include "debug.hpp"
#include <thread>

struct RepeatAwaiter { // awaiter(原始指针) / awaitable(operator->)
    bool await_ready() const noexcept { 
        return false;
    }

    /**
     * @brief 它的返回值决定了协程在挂起后是否以及如何恢复。这个返回值类型是灵活的!
     * @param coroutine 
     * @return 可以返回指向同一个协程、另一个协程、或 std::noop_coroutine() 的句柄。
     * 
     * std::noop_coroutine():
     * 返回这个特殊的协程句柄表示没有实际的工作需要做。
     * 这意味着当前协程被挂起后, 不需要恢复任何协程的执行。这通常用于表示协程已经完成了其目的, 没有后续操作。
     * 
     * std::coroutine_handle<>::from_address(nullptr) 或等效值:
     * 指示协程已完成, 其协程帧应该被销毁。这种情况下, 控制权返回给调度器或协程的调用者。
     * 
     * coroutine (传入的协程句柄):
     * 返回传入的协程句柄表示恢复执行这个协程。这通常用于协程之间的切换或控制流的转移。
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        // (每次co_yield)停止执行
        // return std::noop_coroutine();

        // 如果 coroutine 可以执行就继续执行
        if (coroutine.done())
            return std::noop_coroutine();
        else
            return coroutine; // 继续执行本协程
    }

    void await_resume() const noexcept {}
};

struct RepeatAwaitable { // awaitable(operator->)
    RepeatAwaiter operator co_await() {
        return RepeatAwaiter();
    }
};

struct Promise {
    auto initial_suspend() {
        return std::suspend_always(); // 返回协程控制权
    }

    auto final_suspend() noexcept {
        return std::suspend_always();
    }

    void unhandled_exception() {
        throw;
    }

    auto yield_value(int ret) {
        mRetValue = ret;
        return RepeatAwaiter();
    }

    void return_void() {
        mRetValue = 0;
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    int mRetValue;
};

/// @brief 定义协程任务句柄
struct Task {
    using promise_type = Promise; // 协程任务 必需要包含 promise_type

    Task(std::coroutine_handle<promise_type> coroutine) // 会自行选择合适的构造函数 (可以理解成 [](){} 的捕获)
        : mCoroutine(coroutine) {}

    std::coroutine_handle<promise_type> mCoroutine; // 协程句柄
};

Task hello() {
    debug(), "hello 42";
    co_yield 42;
    debug(), "hello 12";
    co_yield 12;
    debug(), "hello 6";
    co_yield 6;
    debug(), "hello 结束";
    co_return;
}

int main() {
    debug(), "main即将调用hello";
    Task t = hello();
    debug(), "main调用完了hello";
    // 实际上上面什么也没有做(并没有调用hello)

    while (!t.mCoroutine.done()) {
        t.mCoroutine.resume();
        debug(), "main得到hello结果为",
            t.mCoroutine.promise().mRetValue;
    }
    return 0;
}
