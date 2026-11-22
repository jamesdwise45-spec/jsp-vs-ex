// jsp_unit_names.h -- ENHANCEMENT: the English names of the fighters (ids 0..41) and the Support characters (ids 200..213), for the
// port's own text (the Discord presence, jsp_discord_state.h; the rollback pick's picker, jsp_net_pick_flow.h). Printable ASCII only (the
// menu font of the overlay draws 0x20..0x7E). Header only, no game state.
#pragma once

namespace jnames {

inline const char* fighter_name(int id)
{
    static const char* const k[42] = {
        "Luffy", "Ace", "Hancock", "Akainu", "Naruto", "Sasuke", "Madara", "Goku", "Frieza", "Vegeta",
        "Yusuke", "Toguro", "Hiei", "Toriko", "Zebra", "Ichigo", "Aizen", "Gon", "Killua", "Jotaro",
        "Dio", "Dai", "Kenshin", "Shishio", "Seiya", "Kenshiro", "Raoh", "Oga & Baby Beel", "Gintoki Sakata", "Medaka",
        "Ryotsu", "Arale & Gatchan", "Koro-sensei", "Saiki", "Momotaro", "Bo-bobo", "Tsuna & Reborn", "Luckyman", "Taro", "Jonathan",
        "Joseph", "Nube"};
    return id >= 0 && id < 42 ? k[id] : nullptr;
}
// The Support characters, ids 200..213 (docs/balance/Support.md; their CharacterName text is id - 200 + 42). 208 (Yoh) is a stub the
// game never offers (13 Supports in the shop for 14 files); named anyway in case a save or a mod puts it in.
inline constexpr int kSupportFirst = 200, kSupportEnd = 214, kSupportStub = 208;
inline const char* support_name(int id)
{
    static const char* const k[14] = {
        "Neuro", "Kumagawa", "Rukia", "Chitoge", "Hisoka", "Edajima", "Kagura & Sadaharu", "Jaguar", "Yoh", "Kuroko",
        "SKET Dance", "Lala", "Allen", "Hinata"};
    return id >= kSupportFirst && id < kSupportEnd ? k[id - kSupportFirst] : nullptr;
}

}  // namespace jnames
