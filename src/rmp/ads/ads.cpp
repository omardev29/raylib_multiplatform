// ---------------------------------------------------------------------------
// rmp::ads: the one translation unit that includes <admob.h>.
//
// The public header declares eight functions and includes nothing of raymob's,
// so a game that never shows an ad never compiles the JNI bridge's header --
// and rmp/ads.h stands alone like every other public header (tools/header_check.sh).
// admob.h is a no-op off Android, which is what makes these safe everywhere.
// ---------------------------------------------------------------------------

#include <rmp/ads.h>

#include <admob.h>

namespace rmp::ads {

void request_interstitial() { ::RequestInterstitialAd(); }
bool is_interstitial_loaded() { return ::IsInterstitialAdLoaded(); }
void show_interstitial() { ::ShowInterstitialAd(); }

void request_rewarded() { ::RequestRewardedAd(); }
bool is_rewarded_loaded() { return ::IsRewardedAdLoaded(); }
void show_rewarded() { ::ShowRewardedAd(); }

bool take_reward_earned() { return ::TakeRewardEarned(); }
int reward_amount() { return ::GetRewardAmount(); }

} // namespace rmp::ads
