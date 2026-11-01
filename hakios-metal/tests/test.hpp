// The smallest test runner that does the job: CHECK keeps going and counts, TEST registers a function.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

// the chip the tests are built for: HAKIOS_METAL_A618 (make CHIP=618) is ios-sunfish's Adreno 618, whose values the
// tests below give beside the A635's, each worked out from Mesa 25.0.7 as the A635's were
#if defined(HAKIOS_METAL_A618)
inline constexpr bool kA618 = true;
#else
inline constexpr bool kA618 = false;
#endif
// fibers an SP times SPs: 4096 x 2 on the A635, 2048 x 1 on the A618 (freedreno_devices.py fibers_per_sp, num_ccu)
inline constexpr unsigned kTestFibers = kA618 ? 2048u : 4096u, kTestSpCores = kA618 ? 1u : 2u;

struct Case {
    const char* name;
    std::function<void()> run;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> run) { cases().push_back(Case{name, std::move(run)}); }
};

}  // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                         \
    static void name();                                                    \
    static test::Register TEST_CAT(register_, name)(#name, name);          \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            test::failures()++;                                                            \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        auto va_ = (a);                                                                                    \
        auto vb_ = (b);                                                                                    \
        if (!(va_ == vb_)) {                                                                               \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", __FILE__, __LINE__, #a, #b, \
                         (unsigned long long)va_, (unsigned long long)vb_);                                \
            test::failures()++;                                                                            \
        }                                                                                                  \
    } while (0)
