// ---------------------------------------------------------------------------
// rmp::ads off Android: the stubs of thirdparty/raymob/admob.h, through the
// wrapper in src/rmp/ads.cpp. What a game that shows ads does on a desktop,
// a browser or a CI runner -- every call compiles, links and does nothing --
// is what this holds. The real ads are Android's, and only a phone shows one.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include <rmp/ads.h>

TEST_SUITE("ads") {
    TEST_CASE("off Android an interstitial never loads, and showing it does nothing") {
        CHECK_FALSE(rmp::ads::is_interstitial_loaded());
        rmp::ads::request_interstitial();
        CHECK_FALSE(rmp::ads::is_interstitial_loaded());
        rmp::ads::show_interstitial();
        CHECK_FALSE(rmp::ads::is_interstitial_loaded());
    }

    TEST_CASE("off Android a rewarded ad never loads and never earns anything") {
        rmp::ads::request_rewarded();
        CHECK_FALSE(rmp::ads::is_rewarded_loaded());
        rmp::ads::show_rewarded();
        CHECK_FALSE(rmp::ads::take_reward_earned());
        CHECK(rmp::ads::reward_amount() == 0);
        // The documented use: show it only when it has loaded, grant only
        // what was earned -- which off Android is never.
        if (rmp::ads::is_rewarded_loaded()) rmp::ads::show_rewarded();
        CHECK_FALSE(rmp::ads::take_reward_earned());
    }
}
