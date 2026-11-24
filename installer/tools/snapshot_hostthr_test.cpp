// Host test of jsp_snapshot_hostthr.h: which host threads the snapshot freeze leaves running (GPU driver / graphics runtime threads) and the skip
// list the freeze reads. No game, no GPU, no Windows: the classification inputs are the strings the Windows glue reads.
//   bash installer/tools/snapshot_hostthr_test.sh
#include "jsp_snapshot_hostthr.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int main()
{
    // the module names the bundles showed, and the other vendors' user-mode drivers
    CHECK(jsh::driver_module("nvwgf2umx.dll"));
    CHECK(jsh::driver_module("NVWGF2UMX.DLL"));
    CHECK(jsh::driver_module("nvwgf2um.dll"));
    CHECK(jsh::driver_module("nvgpucomp64.dll"));
    CHECK(jsh::driver_module("amdxc64.dll"));
    CHECK(jsh::driver_module("atidxx64.dll"));
    CHECK(jsh::driver_module("igd12umd64.dll"));
    CHECK(jsh::driver_module("igc64.dll"));
    CHECK(jsh::driver_module("D3D12Core.dll"));
    CHECK(jsh::driver_module("d3d12.dll"));
    CHECK(jsh::driver_module("dxgi.dll"));
    // never: the exe, the CRT trampoline, ntdll's pool, the audio stack, the C runtime, a name that only contains a driver name
    CHECK(!jsh::driver_module("jsp.exe"));
    CHECK(!jsh::driver_module("ucrtbase.dll"));
    CHECK(!jsh::driver_module("ntdll.dll"));
    CHECK(!jsh::driver_module("kernel32.dll"));
    CHECK(!jsh::driver_module("AudioSes.dll"));
    CHECK(!jsh::driver_module("xinput1_4.dll"));
    CHECK(!jsh::driver_module("my_nvwgf2umx.dll"));
    CHECK(!jsh::driver_module(""));
    CHECK(!jsh::driver_module(nullptr));

    // thread descriptions
    CHECK(jsh::driver_name("D3D Background Thread 0"));
    CHECK(jsh::driver_name("D3D Background Thread 12"));
    CHECK(!jsh::driver_name("ppu CRI FS File Access 3"));
    CHECK(!jsh::driver_name("spurs jc 4"));
    CHECK(!jsh::driver_name("D3D"));
    CHECK(!jsh::driver_name(""));
    CHECK(!jsh::driver_name(nullptr));

    // start module -> class: the exe wins over any name, a driver module is final, the rest waits for its description
    CHECK(jsh::from_start("jsp.exe", true) == jsh::Other);
    CHECK(jsh::from_start("nvwgf2umx.dll", true) == jsh::Other);
    CHECK(jsh::from_start("nvwgf2umx.dll", false) == jsh::Driver);
    CHECK(jsh::from_start("ucrtbase.dll", false) == jsh::Maybe);
    CHECK(jsh::from_start(nullptr, false) == jsh::Maybe);

    // a Maybe thread: its description is read on the first pass, then every kNamePeriod passes, kNameChecks times at most, then it is Other
    {
        jsh::Ent e; e.cls = jsh::Maybe;
        CHECK(jsh::want_name_check(e, 1));
        jsh::name_checked(e, false);
        CHECK(e.cls == jsh::Maybe);
        CHECK(!jsh::want_name_check(e, 2));
        CHECK(jsh::want_name_check(e, jsh::kNamePeriod));
        unsigned reads = 1;
        for (uint32_t pass = 2; pass < 100000 && e.cls == jsh::Maybe; pass++)
            if (jsh::want_name_check(e, pass)) { reads++; jsh::name_checked(e, false); }
        CHECK(e.cls == jsh::Other);
        CHECK(reads == jsh::kNameChecks);
        CHECK(!jsh::want_name_check(e, jsh::kNamePeriod * 3));
    }
    {   // the name appears on a later read (the thread named itself after it started)
        jsh::Ent e; e.cls = jsh::Maybe;
        jsh::name_checked(e, false);
        jsh::name_checked(e, true);
        CHECK(e.cls == jsh::Driver);
        CHECK(!jsh::want_name_check(e, jsh::kNamePeriod));   // final
    }
    {   // Other and Driver are never read again
        jsh::Ent a; a.cls = jsh::Other; jsh::Ent b; b.cls = jsh::Driver;
        CHECK(!jsh::want_name_check(a, 1) && !jsh::want_name_check(b, 1));
    }

    // the skip list: tid, handle AND creation time must match; a failed read (0) or a reused handle / tid is frozen
    {
        jsh::SkipList s;
        int h1 = 0, h2 = 0;
        CHECK(s.add(100, &h1, 555));
        CHECK(!s.add(101, &h2, 0));          // no creation time: never listed
        CHECK(s.skip(100, &h1, 555));
        CHECK(!s.skip(100, &h1, 556));       // the handle now names another thread
        CHECK(!s.skip(100, &h1, 0));         // the read failed at the freeze
        CHECK(!s.skip(100, &h2, 555));       // another handle
        CHECK(!s.skip(99, &h1, 555));        // another tid
        CHECK(!s.skip(101, &h2, 0));
        s.clear();
        CHECK(!s.skip(100, &h1, 555));       // rebuilt every prepare()
        for (int i = 0; i < jsh::kSkip; i++) CHECK(s.add((uint32_t)(1000 + i), &h1, 7));
        CHECK(!s.add(5000, &h2, 7));          // full: frozen as before, counted
        CHECK(s.overflow == 1);
        CHECK(!s.skip(5000, &h2, 7));
    }

    if (g_fail) { std::printf("snapshot_hostthr_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("snapshot_hostthr_test: all passed\n");
    return 0;
}
