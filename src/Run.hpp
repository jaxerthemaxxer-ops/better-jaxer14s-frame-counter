#pragma once

#include "Gdr2.hpp"

#include <Geode/Geode.hpp>

#include <deque>
#include <functional>
#include <string>
#include <vector>

// Everything the mod remembers about the current run, its analysis and its playback.
namespace fw {

    // GD 2.2 runs its physics at 240 steps per second.
    inline constexpr double TPS = 240.0;

    // Red = clicks that could have come earlier, blue = clicks that could have come later.
    inline const cocos2d::ccColor3B Red = { 255, 82, 82 };
    inline const cocos2d::ccColor3B Blue = { 82, 148, 255 };

    // One jump-button event. `arg` is the third argument the game passed to handleButton.
    // It is stored as-is, so it replays exactly the way it was recorded.
    struct Input {
        int tick;
        bool down;
        bool arg;
    };

    // A saved copy of the whole game state, taken a few frames before a click.
    struct Anchor {
        geode::Ref<CheckpointObject> cp;
        int tick = 0;
    };

    struct Click {
        bool arg = false;
        int down = 0;
        int up = -1;            // -1 when the button was never released
        int anchorIdx = -1;     // index into State::kept
        int anchorTick = 0;
        int maxEarly = 0;       // how far this click can move earlier before hitting a neighbour
        int maxLate = 0;
        int early = 0;          // measured: frames earlier that still survive
        int late = 0;           // measured: frames later that still survive
        bool reliable = true;   // false when the unchanged click could not be reproduced
        bool earlyCapped = false;
        bool lateCapped = false;
        int window() const { return early + late + 1; }
    };

    struct Settings {
        // Analysis
        bool record = true;
        int maxWindow = 12;
        int lookahead = 60;
        bool autoPlayback = true;
        bool showLevel = false;
        float scanFrom = 0.f;
        float scanTo = 100.f;

        // Playback visuals
        bool showLabels = true;
        float labelSize = .5f;
        int labelPos = 0;           // 0 above player, 1 below player, 2 top of screen
        float labelTime = 1.f;
        bool showRing = true;
        float ringSize = 1.f;
        int ringColors = 0;         // 0 red to blue, 1 by side, 2 white
        bool showOverview = true;
        float overviewSize = .5f;
        int showUpTo = 30;
        std::vector<int> hidden;

        // Sound
        bool sound = true;
        float soundVolume = .6f;
        int soundUpTo = 12;
    };

    struct State {
        Settings settings;

        // Recording
        std::vector<Input> inputs;
        std::deque<Anchor> recent;
        std::vector<Anchor> kept;       // sorted by tick
        int lastAnchorTick = -1000;
        int currentTick = 0;

        // Analysis and test runs
        bool analyzing = false;
        bool suppressReset = false;
        bool simDead = false;
        bool simFinished = false;
        std::vector<Input> seq;         // inputs for the test that is running right now
        size_t seqNext = 0;

        // Macro import: the macro is replayed while states are saved around its clicks
        bool importing = false;
        std::vector<Input> macro;
        size_t macroNext = 0;
        std::string macroName;

        // Playback
        bool playback = false;
        size_t pbNext = 0;
        bool runStale = false;          // a finished playback: the next attempt starts a fresh recording

        // Outcome of the run that was analyzed
        int runEndTick = 0;
        bool runCompleted = false;
        bool runDied = false;

        // Results
        std::vector<Click> results;
        bool hasResults = false;
        double analysisSeconds = 0.0;
        int analysisTests = 0;
    };

    State& state();
    void reloadSettings();
    int tickOf(GJBaseGameLayer* layer);

    // Recording (Run.cpp)
    void clearRunData();
    void clearAll();
    void onReset(PlayLayer* pl);
    void maybeAnchor(PlayLayer* pl, int tick);
    void keepAnchorFor(int clickTick);

    // Analysis (Analyzer.cpp)
    std::string beginAnalysis(PlayLayer* pl);
    std::string beginImport(PlayLayer* pl, macro::Replay const& replay, std::string const& name);
    void analyzerTick(PlayLayer* pl, std::function<void(float)> const& step);
    void abortAnalysis();

    // Playback (Playback.cpp)
    bool startPlayback(PlayLayer* pl);
    void endPlayback();
    void playbackClick(PlayLayer* pl, Input const& in);
    void clearPlaybackUi();
}
