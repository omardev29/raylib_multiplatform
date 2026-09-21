#pragma once
// State that has to survive a scene change lives in exactly one place. A scene
// owns its state and loses it on the way out, which is right for the enemies
// and wrong for the score. Every scene reaches this through
// rmp::global<Progress>(), and there is exactly one for the life of the app.
struct Progress {
    int best_score = 0;
    int runs = 0;
};
