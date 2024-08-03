#include <cstdio>
#include <iostream>

using ll = long long; // 为了突出重点, 就不使用模版了qwq

struct FibTask {
    int state = 0;
    ll a, b, c;

    ll fib() {
        switch (state) {
        case 0: // 第一次进入
            a = b = 1;
            state = 1;
            goto st_01;
        case 1: // 第二次进入
            state = 2;
            goto st_02;
        case 2: // 第三次进入
            state = 3;
            goto st_03;
        case 3: // 第4次及其以后的进入
            goto st_04;
        }

    st_01:
        return a;
    st_02:
        return b;
    st_03:
        while (1) {
            c = a + b; // 我们不能使用局部变量作为 c, 因为 c 是在恢复的时候还用到的
            return c;
            st_04:
            b = a;
            a = c;
        }
    }
};

int main() {
    FibTask f;
    while (1) {
        std::cout << f.fib() << '\n';
        getchar();
    }
    return 0;
}