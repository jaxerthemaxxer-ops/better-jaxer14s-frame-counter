#include "Run.hpp"
#include "Ui.hpp"

#include <algorithm>
#include <chrono>
#include <climits>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace fw {

    namespace {
        // Slightly more than one physics step, so every call to the game's update runs exactly one.
        constexpr float StepDt = (1.f / 240.f) * 1.0005f;
        constexpr double BudgetMs = 18.0;   // time spent per game frame, keeps the screen alive

        class CancelHandler : public CCNode {
        public:
            static bool requested;
            static CancelHandler* create() {
                auto* ret = new CancelHandler();
                if (ret->init()) { ret->autorelease(); return ret; }
                delete ret;
                return nullptr;
            }
            void onCancel(CCObject*) { requested = true; }
        };
        bool CancelHandler::requested = false;

        enum class Mode { Idle, Calibrate, Record, Analyze };

        struct Engine {
            Mode mode = Mode::Idle;
            bool active = false;
            Clock::time_point start;
            float savedSfx = 1.f;

            // Macro import
            std::vector<Input> baseMacro;
            int candidates[3] = { 0, 1, -1 };   // timing offsets to try
            int candIdx = 0;
            int bestOffset = 0;
            int bestReach = -1;
            bool runStarted = false;
            int endTick = 0;
            int runSteps = 0;
            int runLimit = 0;
            int progressTick = 0;

            // Analysis
            std::vector<Click> clicks;
            size_t index = 0;
            int tests = 0;
            int unreliable = 0;

            // Overlay
            CCLayerColor* overlay = nullptr;
            CCLayerColor* fill = nullptr;
            CCLabelBMFont* title = nullptr;
            CCLabelBMFont* status = nullptr;
            float barWidth = 300.f;
        };
        Engine E;

        double msSince(Clock::time_point t) {
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        }

        // ---------- Overlay ----------

        void makeOverlay() {
            auto* scene = CCDirector::get()->getRunningScene();
            if (!scene) return;
            auto win = CCDirector::get()->getWinSize();
            bool compact = state().settings.showLevel;   // level stays visible, small panel at the bottom

            float cx = win.width / 2;
            float titleY = compact ? 62.f : win.height / 2 + 50.f;
            float barY = compact ? 40.f : win.height / 2;
            float statusY = compact ? 22.f : win.height / 2 - 24.f;
            CCPoint cancelPos = compact ? ccp(win.width - 70.f, 40.f) : ccp(cx, win.height / 2 - 70.f);

            E.overlay = compact
                ? CCLayerColor::create({ 16, 12, 28, 215 }, win.width, 80.f)
                : CCLayerColor::create({ 16, 12, 28, 245 }, win.width, win.height);

            float top = compact ? 80.f : win.height;
            auto* redStripe = CCLayerColor::create({ 255, 82, 82, 255 }, win.width / 2, 5.f);
            redStripe->setPosition({ 0.f, top - 5.f });
            E.overlay->addChild(redStripe);
            auto* blueStripe = CCLayerColor::create({ 82, 148, 255, 255 }, win.width / 2, 5.f);
            blueStripe->setPosition({ win.width / 2, top - 5.f });
            E.overlay->addChild(blueStripe);

            E.title = CCLabelBMFont::create("Starting...", "bigFont.fnt");
            E.title->setScale(compact ? .5f : .8f);
            E.title->setPosition({ cx, titleY });
            E.overlay->addChild(E.title);

            auto* track = CCLayerColor::create({ 120, 40, 50, 255 }, E.barWidth, 12.f);
            track->setPosition({ cx - E.barWidth / 2, barY });
            E.overlay->addChild(track);

            E.fill = CCLayerColor::create({ 82, 148, 255, 255 }, 1.f, 12.f);
            E.fill->setPosition({ cx - E.barWidth / 2, barY });
            E.overlay->addChild(E.fill);

            E.status = CCLabelBMFont::create("", "chatFont.fnt");
            E.status->setPosition({ cx, statusY });
            E.overlay->addChild(E.status);

            auto* handler = CancelHandler::create();
            E.overlay->addChild(handler);

            auto* spr = ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", .8f);
            spr->setScale(compact ? .7f : 1.f);
            auto* btn = CCMenuItemSpriteExtra::create(spr, handler, menu_selector(CancelHandler::onCancel));
            auto* menu = CCMenu::create();
            menu->addChild(btn);
            menu->setPosition(cancelPos);
            E.overlay->addChild(menu);

            scene->addChild(E.overlay, 10000);
        }

        void updateOverlay() {
            if (!E.overlay) return;

            float frac = 0.f;
            std::string title, status;

            switch (E.mode) {
                case Mode::Calibrate:
                    title = "Matching the macro's timing";
                    frac = (E.candIdx + std::min(1.f, E.progressTick / std::max(1.f, static_cast<float>(E.endTick)))) / 3.f;
                    status = fmt::format("Timing option {} of 3", std::min(3, E.candIdx + 1));
                    break;
                case Mode::Record:
                    title = "Replaying the macro";
                    frac = std::min(1.f, E.progressTick / std::max(1.f, static_cast<float>(E.endTick)));
                    status = fmt::format("Frame {} of {}", E.progressTick, E.endTick);
                    break;
                default:
                    title = "Measuring frame windows";
                    frac = E.clicks.empty() ? 1.f : static_cast<float>(E.index) / static_cast<float>(E.clicks.size());
                    status = fmt::format("{} / {} clicks   |   {:.1f} s   |   {} tests",
                        E.index, E.clicks.size(), msSince(E.start) / 1000.0, E.tests);
                    break;
            }

            E.title->setString(title.c_str());
            E.fill->setContentSize({ std::max(1.f, E.barWidth * frac), 12.f });
            E.status->setString(status.c_str());
        }

        void removeOverlay() {
            if (E.overlay) E.overlay->removeFromParent();
            E.overlay = nullptr;
            E.fill = nullptr;
            E.title = nullptr;
            E.status = nullptr;
        }

        void setMuted(bool mute) {
            auto* fmod = FMODAudioEngine::get();
            if (!fmod) return;
            if (mute) {
                E.savedSfx = fmod->m_sfxVolume;
                fmod->setEffectsVolume(0.f);
            }
            else {
                fmod->setEffectsVolume(E.savedSfx);
            }
        }

        // ---------- Building the list of clicks ----------

        std::string buildClicks() {
            auto& S = state();
            E.clicks.clear();
            E.index = 0;
            E.tests = 0;
            E.unreliable = 0;

            if (S.inputs.empty() || S.kept.empty()) {
                return "Nothing is recorded yet. Play part of a level first (turn on \"Record runs\" in the settings), or import a macro.";
            }

            for (int a = 0; a < 2; a++) {
                bool arg = a == 1;
                bool held = false;
                std::vector<Click> list;

                for (auto const& in : S.inputs) {
                    if (in.arg != arg) continue;
                    if (in.down && !held) {
                        held = true;
                        Click c;
                        c.arg = arg;
                        c.down = in.tick;
                        list.push_back(c);
                    }
                    else if (!in.down && held) {
                        held = false;
                        list.back().up = in.tick;
                    }
                }

                for (size_t k = 0; k < list.size(); k++) {
                    Click& c = list[k];

                    // Prefer the latest saved state that still leaves room to move the click earlier.
                    int room = c.down - S.settings.maxWindow - 1;
                    int pick = -1;
                    for (int i = 0; i < static_cast<int>(S.kept.size()); i++) {
                        if (S.kept[i].tick <= room) pick = i;
                    }
                    if (pick < 0) {
                        for (int i = 0; i < static_cast<int>(S.kept.size()); i++) {
                            if (S.kept[i].tick <= c.down - 1) { pick = i; break; }
                        }
                    }
                    if (pick < 0) {
                        c.reliable = false;
                        E.clicks.push_back(c);
                        continue;
                    }
                    c.anchorIdx = pick;
                    c.anchorTick = S.kept[pick].tick;

                    int prev = k > 0 ? (list[k - 1].up >= 0 ? list[k - 1].up : list[k - 1].down) : INT_MIN / 2;
                    int next = k + 1 < list.size() ? list[k + 1].down : INT_MAX / 2;

                    c.maxEarly = std::max(0, std::min({ S.settings.maxWindow, c.down - c.anchorTick - 1, c.down - prev - 1 }));
                    int latest = c.up >= 0 ? c.up : c.down;
                    c.maxLate = std::max(0, std::min(S.settings.maxWindow, next - latest - 1));

                    E.clicks.push_back(c);
                }
            }

            std::sort(E.clicks.begin(), E.clicks.end(), [](Click const& x, Click const& y) { return x.down < y.down; });

            // Only the part of the run chosen in the settings.
            int last = std::max(S.runEndTick, S.inputs.empty() ? 0 : S.inputs.back().tick);
            float from = last * S.settings.scanFrom / 100.f;
            float to = last * S.settings.scanTo / 100.f;
            std::erase_if(E.clicks, [&](Click const& c) { return c.down < from || c.down > to; });

            if (E.clicks.empty()) return "No complete clicks were recorded in that part of the run.";
            return "";
        }

        // ---------- One test simulation ----------

        // Moves one click `shift` frames, then runs the level from a saved state a few frames before it.
        // Returns true when the player is still alive after the look-ahead.
        bool survives(PlayLayer* pl, std::function<void(float)> const& step, Click const& c, int shift) {
            auto& S = state();
            if (c.anchorIdx < 0 || c.anchorIdx >= static_cast<int>(S.kept.size())) return false;

            Anchor const& anchor = S.kept[c.anchorIdx];
            int endTick = std::max(c.down + shift, (c.up >= 0 ? c.up : c.down) + shift) + S.settings.lookahead;

            // Which buttons were already held when the saved state was taken.
            bool held[2] = { false, false };
            for (auto const& in : S.inputs) {
                if (in.tick > anchor.tick) break;
                held[in.arg ? 1 : 0] = in.down;
            }

            S.seq.clear();
            S.seqNext = 0;
            for (int a = 0; a < 2; a++) {
                if (held[a]) S.seq.push_back({ anchor.tick, true, a == 1 });
            }
            for (auto const& in : S.inputs) {
                if (in.tick <= anchor.tick) continue;
                if (in.tick > endTick) break;

                Input x = in;
                bool isThisDown = x.arg == c.arg && x.down && x.tick == c.down;
                bool isThisUp = x.arg == c.arg && !x.down && c.up >= 0 && x.tick == c.up;
                if (isThisDown || isThisUp) x.tick += shift;
                S.seq.push_back(x);
            }
            std::stable_sort(S.seq.begin(), S.seq.end(), [](Input const& a, Input const& b) { return a.tick < b.tick; });

            S.simDead = false;
            S.simFinished = false;

            pl->loadFromCheckpoint(anchor.cp.data());
            pl->m_extraDelta = 0.0;
            if (pl->m_player1) pl->m_player1->releaseAllButtons();
            if (pl->m_player2) pl->m_player2->releaseAllButtons();

            int guard = 0;
            int limit = (endTick - anchor.tick) + 200;
            while (!S.simDead && !S.simFinished && tickOf(pl) < endTick && guard++ < limit) {
                step(StepDt);
            }

            E.tests++;
            return !S.simDead;
        }

        void measure(PlayLayer* pl, std::function<void(float)> const& step, Click& c) {
            if (c.anchorIdx < 0) {
                c.early = c.late = 0;
                E.unreliable++;
                return;
            }

            // The click as it was played must survive, otherwise the saved state or the physics
            // did not reproduce the run and nothing measured from it can be trusted.
            if (!survives(pl, step, c, 0)) {
                c.reliable = false;
                c.early = c.late = 0;
                E.unreliable++;
                return;
            }

            // The window is assumed to be one unbroken stretch around the original frame, so each
            // side can be found by halving instead of trying every shift one by one.
            int lo = 0, hi = c.maxEarly + 1;
            while (hi - lo > 1) {
                int mid = (lo + hi) / 2;
                if (survives(pl, step, c, -mid)) lo = mid; else hi = mid;
            }
            c.early = lo;
            c.earlyCapped = lo == c.maxEarly;

            lo = 0;
            hi = c.maxLate + 1;
            while (hi - lo > 1) {
                int mid = (lo + hi) / 2;
                if (survives(pl, step, c, mid)) lo = mid; else hi = mid;
            }
            c.late = lo;
            c.lateCapped = lo == c.maxLate;
        }

        // ---------- Fast full-level runs (macro import) ----------

        void startRun(PlayLayer* pl) {
            auto& S = state();
            S.simDead = false;
            S.simFinished = false;
            pl->resetLevel();
            pl->m_extraDelta = 0.0;
            E.runSteps = 0;
            E.runLimit = E.endTick + 600;
        }

        // Steps the level until it ends, dies or reaches the end tick. Returns false when the time
        // budget for this frame ran out first.
        bool stepRun(PlayLayer* pl, std::function<void(float)> const& step) {
            auto& S = state();
            auto t0 = Clock::now();
            while (!S.simDead && !S.simFinished && tickOf(pl) < E.endTick) {
                step(StepDt);
                E.progressTick = tickOf(pl);
                if (++E.runSteps > E.runLimit) return true;
                if (E.runSteps % 16 == 0 && msSince(t0) > BudgetMs) return false;
            }
            return true;
        }

        std::vector<Input> shifted(int offset) {
            std::vector<Input> out = E.baseMacro;
            for (auto& in : out) in.tick = std::max(0, in.tick + offset);
            std::stable_sort(out.begin(), out.end(), [](Input const& a, Input const& b) { return a.tick < b.tick; });
            return out;
        }

        void finish(PlayLayer* pl, bool ok, std::string const& error = "");

        void tickCalibrate(PlayLayer* pl, std::function<void(float)> const& step) {
            auto& S = state();

            if (!E.runStarted) {
                S.seq = shifted(E.candidates[E.candIdx]);
                S.seqNext = 0;
                E.runStarted = true;
                startRun(pl);
                return;
            }
            if (!stepRun(pl, step)) return;

            bool perfect = S.simFinished || (!S.simDead && tickOf(pl) >= E.endTick);
            int reach = perfect ? INT_MAX : tickOf(pl);
            if (reach > E.bestReach) {
                E.bestReach = reach;
                E.bestOffset = E.candidates[E.candIdx];
            }
            E.runStarted = false;

            if (perfect || ++E.candIdx >= 3) {
                // Timing chosen. Replay the macro once more, this time saving states around every click.
                S.analyzing = false;
                S.importing = true;
                S.macro = shifted(E.bestOffset);
                S.macroNext = 0;
                S.seq.clear();
                clearRunData();
                E.mode = Mode::Record;
                E.progressTick = 0;
                startRun(pl);
            }
        }

        void tickRecord(PlayLayer* pl, std::function<void(float)> const& step) {
            auto& S = state();
            if (!stepRun(pl, step)) return;

            S.runEndTick = tickOf(pl);
            S.runCompleted = S.simFinished;
            S.runDied = S.simDead;
            S.importing = false;

            // Straight on to measuring the recorded clicks.
            std::string err = buildClicks();
            if (!err.empty()) {
                finish(pl, false, err);
                return;
            }
            S.analyzing = true;
            E.mode = Mode::Analyze;
        }

        void tickAnalyze(PlayLayer* pl, std::function<void(float)> const& step) {
            auto t0 = Clock::now();
            while (E.index < E.clicks.size() && !CancelHandler::requested) {
                measure(pl, step, E.clicks[E.index]);
                E.index++;
                if (msSince(t0) > BudgetMs) break;
            }
            if (E.index >= E.clicks.size()) finish(pl, true);
        }

        void finish(PlayLayer* pl, bool ok, std::string const& error) {
            auto& S = state();

            S.analyzing = false;
            S.importing = false;
            S.seq.clear();
            S.seqNext = 0;
            S.macroNext = 0;
            E.active = false;
            E.mode = Mode::Idle;
            CancelHandler::requested = false;

            setMuted(false);
            removeOverlay();

            if (ok) {
                S.results = E.clicks;
                S.hasResults = true;
                S.analysisSeconds = msSince(E.start) / 1000.0;
                S.analysisTests = E.tests;
                if (S.runEndTick == 0 && !S.inputs.empty()) S.runEndTick = S.inputs.back().tick;
                log::info("Frame windows: {} clicks, {} tests, {:.2f} s, {} not reproducible",
                    E.clicks.size(), E.tests, S.analysisSeconds, E.unreliable);

                // Straight into playback, the way the original did.
                if (S.settings.autoPlayback && startPlayback(pl)) return;
            }

            // Put the level back at the start without throwing the recorded run away.
            S.suppressReset = true;
            pl->resetLevel();
            S.suppressReset = false;
            pl->pauseGame(false);

            if (!error.empty()) {
                FLAlertLayer::create("Frame Windows", error, "OK")->show();
                return;
            }
            if (ok) {
                PauseLayer* pause = nullptr;
                if (auto* scene = CCDirector::get()->getRunningScene()) pause = scene->getChildByType<PauseLayer>(0);
                ResultsPopup::create(pause)->show();
            }
        }

        void beginCommon() {
            E.start = Clock::now();
            E.active = true;
            CancelHandler::requested = false;
            setMuted(true);
            makeOverlay();
        }
    }

    // Measure a run that was recorded during normal play.
    std::string beginAnalysis(PlayLayer* pl) {
        auto& S = state();
        if (S.analyzing || S.importing) return "An analysis is already running.";
        if (!pl) return "Open a level first.";

        E = Engine{};
        std::string err = buildClicks();
        if (!err.empty()) return err;

        E.mode = Mode::Analyze;
        beginCommon();
        S.analyzing = true;
        return "";
    }

    // Replay a .gdr2 macro in the level, saving states around its clicks, then measure it.
    std::string beginImport(PlayLayer* pl, macro::Replay const& replay, std::string const& name) {
        auto& S = state();
        if (S.analyzing || S.importing) return "An analysis is already running.";
        if (!pl) return "Open a level first.";

        E = Engine{};

        double tps = replay.framerate > 0.0 ? replay.framerate : 240.0;
        bool anyJump = false;
        for (size_t i = 0; i < replay.frame.size(); i++) {
            if (replay.button[i] != 1) continue;
            int tick = static_cast<int>(std::floor(static_cast<double>(replay.frame[i]) * (TPS / tps) + 1e-6));
            E.baseMacro.push_back({ tick, replay.down[i] != 0, replay.player2[i] != 0 });
            if (replay.down[i]) anyJump = true;
        }
        if (!anyJump) return "That macro has no jump inputs.";

        std::stable_sort(E.baseMacro.begin(), E.baseMacro.end(), [](Input const& a, Input const& b) { return a.tick < b.tick; });
        E.endTick = E.baseMacro.back().tick + 30;

        S.macroName = name;
        S.results.clear();
        S.hasResults = false;
        S.runEndTick = 0;
        S.runCompleted = false;
        S.runDied = false;
        S.runStale = false;
        clearRunData();

        E.mode = Mode::Calibrate;
        beginCommon();
        S.analyzing = true;
        return "";
    }

    void analyzerTick(PlayLayer* pl, std::function<void(float)> const& step) {
        auto& S = state();
        if (!E.active) {
            S.analyzing = false;
            S.importing = false;
            return;
        }
        if (CancelHandler::requested) {
            finish(pl, false);
            return;
        }

        switch (E.mode) {
            case Mode::Calibrate: tickCalibrate(pl, step); break;
            case Mode::Record: tickRecord(pl, step); break;
            case Mode::Analyze: tickAnalyze(pl, step); break;
            default: break;
        }

        if (E.active) updateOverlay();
    }

    void abortAnalysis() {
        auto& S = state();
        if (!S.analyzing && !S.importing) return;
        S.analyzing = false;
        S.importing = false;
        S.seq.clear();
        E.active = false;
        E.mode = Mode::Idle;
        CancelHandler::requested = false;
        setMuted(false);
        removeOverlay();
    }
}
