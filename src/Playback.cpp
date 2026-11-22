#include "Run.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

using namespace geode::prelude;

namespace fw {

    namespace {
        std::unordered_map<long long, int> clickIndex;   // (tick, player) -> index in State::results
        CCNode* overview = nullptr;

        long long keyOf(int tick, bool arg) {
            return static_cast<long long>(tick) * 2 + (arg ? 1 : 0);
        }

        bool shown(int window) {
            auto const& s = state().settings;
            if (window > s.showUpTo) return false;
            return std::find(s.hidden.begin(), s.hidden.end(), window) == s.hidden.end();
        }

        ccColor3B mix(ccColor3B a, ccColor3B b, float t) {
            return ccc3(
                static_cast<GLubyte>(a.r + (b.r - a.r) * t),
                static_cast<GLubyte>(a.g + (b.g - a.g) * t),
                static_cast<GLubyte>(a.b + (b.b - a.b) * t)
            );
        }

        // Tight windows are red, roomy ones are blue.
        ccColor3B colorFor(Click const& c) {
            auto const& s = state().settings;
            if (s.ringColors == 2) return ccc3(245, 245, 250);
            if (s.ringColors == 1) {
                if (c.early > c.late) return Red;
                if (c.late > c.early) return Blue;
                return ccc3(245, 245, 250);
            }
            float t = std::clamp((c.window() - 1) / static_cast<float>(std::max(1, s.maxWindow)), 0.f, 1.f);
            return mix(Red, Blue, t);
        }

        void buildOverview(PlayLayer* pl) {
            auto& S = state();
            auto const& s = S.settings;

            if (overview) {
                overview->removeFromParent();
                overview = nullptr;
            }
            if (!s.showOverview || !pl->m_uiLayer) return;

            std::map<int, int> counts;
            int reliable = 0;
            double sum = 0;
            for (auto const& c : S.results) {
                if (!c.reliable) continue;
                reliable++;
                sum += c.window();
                if (shown(c.window())) counts[c.window()]++;
            }

            overview = CCNode::create();
            auto win = CCDirector::get()->getWinSize();
            overview->setPosition({ 8.f, win.height - 8.f });

            float y = 0.f;
            float lineH = 34.f * s.overviewSize;
            auto addLine = [&](std::string const& text, ccColor3B color) {
                auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
                label->setScale(s.overviewSize);
                label->setColor(color);
                label->setAnchorPoint({ 0.f, 1.f });
                label->setPosition({ 0.f, y });
                overview->addChild(label);
                y -= lineH;
            };

            addLine(reliable > 0 ? fmt::format("Avg window {:.1f}", sum / reliable) : std::string("Frame windows"), ccc3(245, 245, 250));
            for (auto const& [window, n] : counts) {
                Click fake;
                fake.early = window - 1;
                addLine(fmt::format("{}f: {}", window, n), colorFor(fake));
            }

            pl->m_uiLayer->addChild(overview, 500);
        }

        std::string const& dingPath() {
            static std::string path = utils::string::pathToString(Mod::get()->getSaveDir() / "ding.wav");
            return path;
        }
    }

    // Replays the analyzed run in the level with a ring, a number and a sound on every click.
    bool startPlayback(PlayLayer* pl) {
        auto& S = state();
        if (!pl || S.inputs.empty() || !S.hasResults) return false;

        clickIndex.clear();
        for (size_t i = 0; i < S.results.size(); i++) {
            clickIndex[keyOf(S.results[i].down, S.results[i].arg)] = static_cast<int>(i);
        }

        S.playback = true;
        S.pbNext = 0;
        S.runStale = false;

        pl->resetLevel();   // onReset ignores this one while a playback is active
        buildOverview(pl);
        return true;
    }

    void endPlayback() {
        auto& S = state();
        if (!S.playback) return;
        S.playback = false;
        S.runStale = true;   // the next attempt starts a fresh recording
    }

    void clearPlaybackUi() {
        if (overview) {
            overview->removeFromParent();
            overview = nullptr;
        }
        clickIndex.clear();
    }

    // Called at the moment a recorded click is pressed during playback.
    void playbackClick(PlayLayer* pl, Input const& in) {
        auto& S = state();
        auto const& s = S.settings;
        if (!in.down) return;

        auto it = clickIndex.find(keyOf(in.tick, in.arg));
        if (it == clickIndex.end()) return;
        Click const& c = S.results[it->second];

        int window = c.window();
        if (c.reliable && !shown(window)) return;

        ccColor3B color = c.reliable ? colorFor(c) : ccc3(140, 140, 150);

        PlayerObject* player = pl->m_player1;
        if (pl->m_gameState.m_isDualMode && pl->m_player2 && player
            && !player->m_holdingButtons[1] && pl->m_player2->m_holdingButtons[1]) {
            player = pl->m_player2;
        }
        if (!player || !pl->m_objectLayer) return;
        CCPoint pos = player->getPosition();

        if (s.showRing && c.reliable) {
            auto* wave = CCCircleWave::create(8.f * s.ringSize, 46.f * s.ringSize, .45f, false, true);
            wave->m_color = color;
            wave->setPosition(pos);
            pl->m_objectLayer->addChild(wave, 200);
        }

        if (s.showLabels) {
            auto* label = CCLabelBMFont::create(c.reliable ? std::to_string(window).c_str() : "?", "bigFont.fnt");
            label->setColor(color);
            label->setScale(s.labelSize);

            CCNode* parent = pl->m_objectLayer;
            CCPoint at = pos;
            if (s.labelPos == 0) {
                at = { pos.x, pos.y + 34.f };
            }
            else if (s.labelPos == 1) {
                at = { pos.x, pos.y - 34.f };
            }
            else if (pl->m_uiLayer) {
                auto win = CCDirector::get()->getWinSize();
                parent = pl->m_uiLayer;
                at = { win.width / 2, win.height - 42.f };
            }
            label->setPosition(at);
            parent->addChild(label, 201);

            float t = std::max(.2f, s.labelTime);
            label->runAction(CCSequence::create(
                CCSpawn::create(CCMoveBy::create(t, { 0.f, 14.f }), CCFadeOut::create(t), nullptr),
                CCRemoveSelf::create(),
                nullptr
            ));
        }

        // Tight windows ring higher, roomy ones lower, so you can hear the difference.
        if (s.sound && c.reliable && window <= s.soundUpTo) {
            if (auto* fmod = FMODAudioEngine::get()) {
                float speed = std::clamp(1.7f - 0.08f * static_cast<float>(window - 1), 0.7f, 1.7f);
                fmod->playEffect(dingPath(), speed, 0.f, s.soundVolume);
            }
        }
    }
}
