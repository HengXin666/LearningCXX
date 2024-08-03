#include <chrono>
#include <coroutine>
#include <thread>
#include "rbtree.hpp"
#include "debug.hpp"

using namespace std::chrono_literals;

struct RepeatAwaiter {
    bool await_ready() const noexcept { 
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

struct Promise {
    auto initial_suspend() {
        return std::suspend_always();
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

struct Task {
    using promise_type = Promise;

    Task(std::coroutine_handle<promise_type> coroutine)
        : mCoroutine(coroutine) {}

    std::coroutine_handle<promise_type> mCoroutine;
};

Task itoa(int n) {
    while (1) {
        co_yield n++;
    }
}

int main() {
    // 实现一个 生成器
    auto task = itoa(10);
    while (1) {
        task.mCoroutine.resume();
        std::cout << task.mCoroutine.promise().mRetValue << '\n';
        getchar();
    }
    return 0;
}
