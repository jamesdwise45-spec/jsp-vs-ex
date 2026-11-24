// Host test of the sound archive preload: the rules (installer/port/jsp/jsp_sound_preload_rule.h) and, built with -DSOUND_PRELOAD_GLUE and the
// toolkit's include folder, the glue itself (installer/port/jsp/jsp_sound_preload.cpp, its POSIX branch) on a file the test writes: the provider is
// chained in front of the previous one and hands it every open, read, size and close; only the first open of the sound archive starts a preload.
// No game data. Run by sound_preload_test.sh.
#include "jsp_sound_preload_rule.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void rules()
{
    using namespace jspre;
    CHECK(is_sound_archive("/dev_hdd0/game/BLUS31519-INSTALL/USRDIR/data/PS3/partition_op_sound_ps3.cpk"));
    CHECK(is_sound_archive("/dev_bdvd/PS3_GAME/USRDIR/data/PS3/PARTITION_OP_SOUND_PS3.CPK"));
    CHECK(is_sound_archive("partition_op_sound_ps3.cpk"));
    CHECK(is_sound_archive("C:\\x\\partition_op_sound_ps3.cpk"));
    CHECK(!is_sound_archive("/dev_bdvd/x/partition_op_sound_ps3.cpk.tmp"));
    CHECK(!is_sound_archive("/dev_bdvd/x/xpartition_op_sound_ps3.cpk"));
    CHECK(!is_sound_archive("/dev_bdvd/x/partition_op_battle_ps3.cpk"));
    CHECK(!is_sound_archive(nullptr));
    CHECK(!is_sound_archive(""));

    CHECK(parse_budget_mb(nullptr) == kDefaultMb);
    CHECK(parse_budget_mb("") == kDefaultMb);
    CHECK(parse_budget_mb("  ") == kDefaultMb);
    CHECK(parse_budget_mb("0") == 0);
    CHECK(parse_budget_mb(" 256 ") == 256);
    CHECK(parse_budget_mb("256\r\n") == 256);
    CHECK(parse_budget_mb("12ab") == kDefaultMb);
    CHECK(parse_budget_mb("-5") == kDefaultMb);
    CHECK(parse_budget_mb("999999999999999999999") == kMaxMb);

    const uint64_t MB = 1ull << 20, GB = 1ull << 30, sound = 399929344ull;   // a file of the sound archive's size (381.4 MB)
    CHECK(preload_bytes(sound, 512, 0) == sound);                  // the whole file, RAM unknown
    CHECK(preload_bytes(sound, 512, 8 * GB) == sound);             // plenty of RAM
    CHECK(preload_bytes(sound, 0, 8 * GB) == 0);                   // off
    CHECK(preload_bytes(sound, -1, 8 * GB) == 0);
    CHECK(preload_bytes(0, 512, 8 * GB) == 0);
    CHECK(preload_bytes(sound, 100, 8 * GB) == 100 * MB);          // the budget
    CHECK(preload_bytes(sound, 512, GB + 300 * MB) == 150 * MB);   // half of the free RAM above the 1 GB reserve
    CHECK(preload_bytes(sound, 512, GB) == 0);                     // no room
    CHECK(preload_bytes(sound, 512, 512 * MB) == 0);
    const uint64_t odd = preload_bytes(sound, 512, GB + 2 * (100 * MB + 12345));
    CHECK(odd < sound && odd % kAlign == 0 && odd <= 100 * MB + 12345 && odd + kAlign > 100 * MB + 12345);   // a partial preload ends on a block
    CHECK(preload_bytes(1000, 512, 8 * GB) == 1000);               // a small whole file keeps its odd length
}

#ifdef SOUND_PRELOAD_GLUE
#include "ps3emu/fs_vfile.h"
extern "C" { const ps3_fs_vfile_ops* volatile g_ps3_fs_vfile_ops = nullptr; }   // defined by ppu_fs.cpp in the game
extern "C" void jsp_sound_preload_install(void);
extern "C" void jsp_sound_preload_fight_start(void);
extern "C" int jsp_sound_preload_state(uint64_t* bytes);

// A stand-in for the mods folder's provider: claims "*.mod" paths only and counts the calls.
static int f_opens = 0, f_sizes = 0, f_reads = 0, f_closes = 0;
static ps3_fs_vfile* f_open(const char* g, const char*) { ++f_opens; return std::strstr(g, ".mod") ? reinterpret_cast<ps3_fs_vfile*>(new int(7)) : nullptr; }
static uint64_t f_size(ps3_fs_vfile* vf) { ++f_sizes; return (uint64_t)*reinterpret_cast<int*>(vf); }
static size_t f_read(ps3_fs_vfile*, FILE*, uint64_t, void* d, size_t n) { ++f_reads; std::memset(d, 'm', n); return n; }
static void f_close(ps3_fs_vfile* vf) { ++f_closes; delete reinterpret_cast<int*>(vf); }
static const ps3_fs_vfile_ops f_ops = { f_open, f_size, f_read, f_close };

static int wait_state(uint64_t* bytes)
{
    for (int i = 0; i < 500; ++i) {
        const int s = jsp_sound_preload_state(bytes);
        if (s == 2 || s == -1) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return jsp_sound_preload_state(bytes);
}

static void glue(const char* dir)
{
    const std::string path = std::string(dir) + "/partition_op_sound_ps3.cpk";
    const size_t size = 3 * 1024 * 1024 + 777;   // synthetic bytes, not game data
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f);
        if (!f) return;
        for (size_t i = 0; i < size; ++i) std::fputc((int)(i * 31u & 0xFF), f);
        std::fclose(f);
    }
    g_ps3_fs_vfile_ops = &f_ops;
    jsp_sound_preload_install();
    const ps3_fs_vfile_ops* ops = g_ps3_fs_vfile_ops;
    CHECK(ops != &f_ops && ops != nullptr);   // chained in front
    if (!ops || ops == &f_ops) return;
    jsp_sound_preload_install();              // once per process
    CHECK(g_ps3_fs_vfile_ops == ops);

    uint64_t bytes = 1;
    CHECK(jsp_sound_preload_state(&bytes) == 0 && bytes == 0);
    jsp_sound_preload_fight_start();          // nothing to refresh yet: no crash, no state change
    CHECK(jsp_sound_preload_state(nullptr) == 0);

    // Another file: handed to the previous provider, no preload.
    CHECK(ops->open("/dev_bdvd/x/partition_op_map_ps3.cpk", "/nonexistent") == nullptr);
    CHECK(f_opens == 1 && jsp_sound_preload_state(nullptr) == 0);
    // A file the previous provider claims: its view, and every later call reaches it.
    ps3_fs_vfile* vf = ops->open("/dev_bdvd/x/a.mod", "/nonexistent");
    CHECK(vf != nullptr && f_opens == 2);
    if (vf) {
        char b[4] = {};
        CHECK(ops->size(vf) == 7 && f_sizes == 1);
        CHECK(ops->read(vf, nullptr, 0, b, 4) == 4 && b[0] == 'm' && f_reads == 1);
        ops->close(vf);
        CHECK(f_closes == 1);
    }
    // The sound archive: not claimed (the game reads its own handle), the preload starts and holds the whole file.
    CHECK(ops->open("/dev_hdd0/game/X/USRDIR/data/PS3/partition_op_sound_ps3.cpk", path.c_str()) == nullptr);
    CHECK(f_opens == 3);
    CHECK(wait_state(&bytes) == 2);
    CHECK(bytes == size);
    // A second open (another path) starts nothing new.
    CHECK(ops->open("/dev_bdvd/y/partition_op_sound_ps3.cpk", "/nonexistent") == nullptr);
    CHECK(jsp_sound_preload_state(&bytes) == 2 && bytes == size);
    jsp_sound_preload_fight_start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the refresh thread logs its line
    CHECK(jsp_sound_preload_state(&bytes) == 2 && bytes == size);
    std::remove(path.c_str());
}
#endif

int main(int argc, char** argv)
{
    rules();
#ifdef SOUND_PRELOAD_GLUE
    if (argc < 2) { std::fprintf(stderr, "usage: sound_preload_test DIR (an empty scratch folder)\n"); return 2; }
    glue(argv[1]);
#else
    (void)argc; (void)argv;
#endif
    if (g_fail) { std::fprintf(stderr, "sound preload test: %d failure(s)\n", g_fail); return 1; }
    std::printf("sound preload test: ok\n");
    return 0;
}
