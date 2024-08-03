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

template<class T>
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

    auto yield_value(T ret) {
        mRetValue = ret;
        return RepeatAwaiter();
    }

    void return_void() {
        mRetValue = 0;
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    T mRetValue;
};

template<class T>
struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine)
        : mCoroutine(coroutine) {}

    std::coroutine_handle<promise_type> mCoroutine;
};

template<class T>
Task<T> fib() {
    T a = 1, b = 1;
    co_yield a;
    co_yield b;
    while (1) {
        T c = a + b;
        co_yield c;
        a = b;
        b = c;
    }
}

int main() {
    // 实现一个 生成器
    using ll = long long;
    auto task = fib<ll>();
    while (1) {
        task.mCoroutine.resume();
        std::cout << task.mCoroutine.promise().mRetValue << '\n';
        getchar();
    }
    return 0;
}
