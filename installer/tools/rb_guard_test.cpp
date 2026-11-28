// Host test of installer/port/jsp/jsp_rb_guard.h (FIX test13, the rollback guards of jsp_net_game.cpp): the NULL-call loop counter (once per boundary epoch, per
// thread, reset by a new epoch), the desync-first order of one batch of session events, the reasons a helper-thread register write-back is skipped, and the loader
// boundary wait's off switch after timeouts in a row. Run: installer/tools/rb_guard_test.sh
#include "../port/jsp/jsp_rb_guard.h"
#include <cstdio>
#include <string>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void test_nullcall()
{
    using namespace jrg;
    NcThread a, b;
    int fired = 0;
    for (uint32_t i = 0; i < kNullCallsPerTick - 1; i++) fired += nc_count(a, 7);
    CHECK(fired == 0 && a.n == kNullCallsPerTick - 1);
    CHECK(nc_count(a, 7));                                   // the 32nd NULL call of the tick raises the flag
    for (int i = 0; i < 1000; i++) fired += nc_count(a, 7);  // a loop goes on: no second report in the same epoch
    CHECK(fired == 0);
    for (uint32_t i = 0; i < kNullCallsPerTick - 1; i++) fired += nc_count(b, 7);   // another thread counts on its own
    CHECK(fired == 0);
    // a new boundary epoch starts the count again: a few NULL calls per tick never reach the limit (the healthy case: none at all in 12 field logs)
    NcThread c;
    for (uint32_t ep = 0; ep < 500; ep++) for (int i = 0; i < 5; i++) fired += nc_count(c, ep);
    CHECK(fired == 0);
    // a loop that spans epochs (the main thread is stuck in a gate: no boundary, no new epoch) still fires within its epoch
    NcThread d; uint32_t calls = 0; bool hit = false;
    while (!hit && calls < 10000) { hit = nc_count(d, 42); calls++; }
    CHECK(hit && calls == kNullCallsPerTick);
    CHECK(nc_count(d, 43, 1));   // a custom limit of 1 fires on the first call of an epoch
    std::printf("null-call counter: ok\n");
}

struct Ev { int kind; int id; };   // kind 1 = desync, 2 = disconnected, 0 = other
static void test_desync_first()
{
    std::vector<Ev> v = {{2, 0}, {0, 1}, {2, 2}, {1, 3}, {0, 4}, {1, 5}};
    jrg::desync_first(v, [](const Ev& e) { return e.kind == 1; });
    CHECK(v[0].id == 3 && v[1].id == 5);                             // desyncs first, in their order
    CHECK(v[2].id == 0 && v[3].id == 1 && v[4].id == 2 && v[5].id == 4);   // the rest keep theirs
    std::vector<Ev> w = {{2, 0}, {0, 1}};
    jrg::desync_first(w, [](const Ev& e) { return e.kind == 1; });
    CHECK(w[0].id == 0 && w[1].id == 1);   // no desync: unchanged
    std::vector<Ev> none;
    jrg::desync_first(none, [](const Ev& e) { return e.kind == 1; });
    CHECK(none.empty());
    std::printf("desync first: ok\n");
}

static void test_regs_skip()
{
    using jrg::RegSkip;
    CHECK(jrg::regs_skip_kind(false, true, 2, 0x4D3D4C, 2, 0x4D3D4C) == RegSkip::NoSave);
    CHECK(jrg::regs_skip_kind(true, false, 2, 0x4D3D4C, 0, 0) == RegSkip::Ended);
    CHECK(jrg::regs_skip_kind(true, true, 2, 0x4D3D4C, 2, 0x4D3D84) == RegSkip::Moved);   // a CRI thread caught away from its idle wait (room 38 field lines)
    CHECK(jrg::regs_skip_kind(true, true, 2, 0x4D3D4C, 0, 0x4D3D4C) == RegSkip::Moved);   // running guest code now
    CHECK(jrg::regs_skip_kind(true, true, 1, 0x4D4F30, 1, 0x4D4F30) == RegSkip::None);
    CHECK(std::string(jrg::regs_skip_name(RegSkip::Moved)) == "not parked in the same wait" && std::string(jrg::regs_skip_name(RegSkip::NoSave)) == "no save of that tick");
    std::printf("register write-back skips: ok\n");
}

static void test_loader_gate()
{
    using namespace jrg;
    CHECK(loader_idle(true, 2, kLoaderIdleLr) && !loader_idle(true, 0, kLoaderIdleLr) && !loader_idle(true, 2, 0x00538404u) && loader_idle(false, 0, 0));
    LoaderGate g;
    CHECK(!g.result(false) && !g.result(false) && !g.off);
    CHECK(!g.result(true) && g.misses == 0);                   // an idle boundary resets the run of timeouts
    CHECK(!g.result(false) && !g.result(false));
    CHECK(g.result(false) && g.off);                          // the third timeout in a row turns it off (reported once)
    CHECK(!g.result(false) && g.off);
    CHECK(g.waits == 7 && g.timeouts == 6);
    std::printf("loader gate: ok\n");
}

int main()
{
    test_nullcall();
    test_desync_first();
    test_regs_skip();
    test_loader_gate();
    std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
