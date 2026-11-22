#include "Run.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

class $modify(FWBaseLayer, GJBaseGameLayer) {
    // Records every jump press and release during normal play.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& S = fw::state();
        auto* pl = PlayLayer::get();
        bool inLevel = pl && static_cast<GJBaseGameLayer*>(pl) == this;

        // Real input is ignored while the mod drives the level.
        if (inLevel && (S.analyzing || S.importing || S.playback)) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!inLevel || button != 1 || !S.settings.record) return;
        if (m_player1 && m_player1->m_isDead) return;

        S.inputs.push_back({ S.currentTick, down, isPlayer1 });
        if (down) fw::keepAnchorFor(S.currentTick);
    }

    // The game's per-step entry point. Recording tags every input with the step it happened on,
    // and test runs, macro replays and playback feed their inputs in at exactly the same place.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& S = fw::state();
        auto* pl = PlayLayer::get();

        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        int tick = fw::tickOf(this);
        S.currentTick = tick;

        // A short test run during the analysis.
        if (S.analyzing) {
            while (S.seqNext < S.seq.size() && S.seq[S.seqNext].tick <= tick) {
                auto const& in = S.seq[S.seqNext++];
                GJBaseGameLayer::handleButton(in.down, 1, in.arg);
            }
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        // An imported macro being replayed. The clicks are recorded as they are applied, and a
        // saved state is taken every few frames, just like during normal play.
        if (S.importing) {
            while (S.macroNext < S.macro.size() && S.macro[S.macroNext].tick <= tick) {
                auto const& in = S.macro[S.macroNext++];
                GJBaseGameLayer::handleButton(in.down, 1, in.arg);
                S.inputs.push_back({ tick, in.down, in.arg });
                if (in.down) fw::keepAnchorFor(tick);
            }
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            if (m_player1 && !m_player1->m_isDead) fw::maybeAnchor(pl, fw::tickOf(this));
            return;
        }

        // The analyzed run playing back with rings, numbers and sounds.
        if (S.playback) {
            while (S.pbNext < S.inputs.size() && S.inputs[S.pbNext].tick <= tick) {
                auto const in = S.inputs[S.pbNext++];
                GJBaseGameLayer::handleButton(in.down, 1, in.arg);
                fw::playbackClick(pl, in);
            }
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (S.settings.record && m_player1 && !m_player1->m_isDead) {
            fw::maybeAnchor(pl, fw::tickOf(this));
        }
    }

    void stepOnce(float dt) {
        GJBaseGameLayer::update(dt);
    }

    // While analyzing, the level is not played at real speed. Each game frame instead runs as
    // many test steps as fit in a small time budget.
    void update(float dt) {
        auto& S = fw::state();
        auto* pl = PlayLayer::get();

        if (!(S.analyzing || S.importing) || !pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            GJBaseGameLayer::update(dt);
            return;
        }

        fw::analyzerTick(pl, [this](float d) { this->stepOnce(d); });
    }
};

class $modify(FWPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        fw::clearAll();
        PlayLayer::setupHasCompleted();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        fw::onReset(this);
    }

    // Test simulations must not kill the player or end the level for real.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& S = fw::state();
        if (S.analyzing || S.importing) {
            S.simDead = true;
            return;
        }
        if (S.playback) fw::endPlayback();
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& S = fw::state();
        if (S.analyzing || S.importing) {
            S.simFinished = true;
            return;
        }
        if (S.playback) fw::endPlayback();
        PlayLayer::levelComplete();
    }

    void onQuit() {
        fw::abortAnalysis();
        fw::endPlayback();
        fw::clearAll();
        PlayLayer::onQuit();
    }
};
