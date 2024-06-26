#include <src/library/testing/benchmark/bench.h>
#include <ydb-cpp-sdk/library/threading/future/future.h>

#include <string>
#include <ydb-cpp-sdk/util/generic/xrange.h>

using namespace NThreading;

template <typename T>
void TestAllocPromise(const NBench::NCpu::TParams& iface) {
    for (const auto it : xrange(iface.Iterations())) {
        Y_UNUSED(it);
        Y_DO_NOT_OPTIMIZE_AWAY(NewPromise<T>());
    }
}

template <typename T>
TPromise<T> SetPromise(T value) {
    auto promise = NewPromise<T>();
    promise.SetValue(value);
    return promise;
}

template <typename T>
void TestSetPromise(const NBench::NCpu::TParams& iface, T value) {
    for (const auto it : xrange(iface.Iterations())) {
        Y_UNUSED(it);
        Y_DO_NOT_OPTIMIZE_AWAY(SetPromise(value));
    }
}

Y_CPU_BENCHMARK(AllocPromiseVoid, iface) {
    TestAllocPromise<void>(iface);
}

Y_CPU_BENCHMARK(AllocPromiseUI64, iface) {
    TestAllocPromise<ui64>(iface);
}

Y_CPU_BENCHMARK(AllocPromiseStroka, iface) {
    TestAllocPromise<std::string>(iface);
}

Y_CPU_BENCHMARK(SetPromiseUI64, iface) {
    TestSetPromise<ui64>(iface, 1234567890ull);
}

Y_CPU_BENCHMARK(SetPromiseStroka, iface) {
    TestSetPromise<std::string>(iface, "test test test");
}
