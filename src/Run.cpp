#include "Run.hpp"

#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>

using namespace geode::prelude;

namespace fw {

    namespace {
        constexpr int AnchorEvery = 20;   // ticks between saved states while recording
        constexpr size_t RecentMax = 6;
        constexpr double Pi = 3.14159265358979323846;

        void addAnchor(PlayLayer* pl, int tick) {
            auto& S = state();
            CheckpointObject* cp = pl->createCheckpoint();
            if (!cp) return;
            S.recent.push_back(Anchor{ Ref<CheckpointObject>(cp), tick });
            while (S.recent.size() > RecentMax) S.recent.pop_front();
            S.lastAnchorTick = tick;
        }

        void truncateTo(int tick) {
            auto& S = state();
            std::erase_if(S.inputs, [&](Input const& i) { return i.tick >= tick; });
            std::erase_if(S.kept, [&](Anchor const& a) { return a.tick >= tick; });
            std::erase_if(S.recent, [&](Anchor const& a) { return a.tick >= tick; });
        }

        std::vector<int> parseHidden(std::string const& text) {
            std::vector<int> out;
            std::string token;
            for (char ch : text + ",") {
                if (ch >= '0' && ch <= '9') token += ch;
                else if (!token.empty()) {
                    out.push_back(std::atoi(token.c_str()));
                    token.clear();
                }
            }
            return out;
        }

        // A short, bright bell made from a few decaying sine waves. The playback speeds it up for
        // tight windows and slows it down for loose ones, so one file covers every window size.
        void makeDing() {
            auto path = Mod::get()->getSaveDir() / "ding.wav";
            if (std::filesystem::exists(path)) return;

            constexpr int rate = 44100;
            constexpr int samples = static_cast<int>(rate * 0.30);
            std::vector<uint8_t> wav;
            auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) wav.push_back((v >> (8 * i)) & 0xff); };
            auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; i++) wav.push_back((v >> (8 * i)) & 0xff); };
            auto tag = [&](char const* t) { for (int i = 0; i < 4; i++) wav.push_back(static_cast<uint8_t>(t[i])); };

            tag("RIFF"); u32(36 + samples * 2); tag("WAVE");
            tag("fmt "); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
            tag("data"); u32(samples * 2);

            for (int i = 0; i < samples; i++) {
                double t = static_cast<double>(i) / rate;
                double v = std::sin(2 * Pi * 1318.0 * t) * 0.60
                         + std::sin(2 * Pi * 1318.0 * 2.76 * t) * 0.25
                         + std::sin(2 * Pi * 1318.0 * 5.40 * t) * 0.10;
                double attack = std::min(1.0, t / 0.002);
                v *= std::exp(-t * 14.0) * attack * 0.6;
                int16_t s = static_cast<int16_t>(std::clamp(v, -1.0, 1.0) * 32767);
                u16(static_cast<uint16_t>(s));
            }
            (void)file::writeBinary(path, wav);
        }
    }

    State& state() {
        static State s;
        return s;
    }

    void reloadSettings() {
        auto* m = Mod::get();
        auto& s = state().settings;

        s.record = m->getSettingValue<bool>("record");
        s.maxWindow = static_cast<int>(m->getSettingValue<int64_t>("max-window"));
        s.lookahead = static_cast<int>(m->getSettingValue<int64_t>("lookahead"));
        s.autoPlayback = m->getSettingValue<bool>("auto-playback");
        s.showLevel = m->getSettingValue<bool>("show-level");
        s.scanFrom = static_cast<float>(m->getSettingValue<double>("scan-from"));
        s.scanTo = static_cast<float>(m->getSettingValue<double>("scan-to"));

        s.showLabels = m->getSettingValue<bool>("show-labels");
        s.labelSize = static_cast<float>(m->getSettingValue<double>("label-size"));
        auto pos = m->getSettingValue<std::string>("label-position");
        s.labelPos = pos == "Above player" ? 0 : pos == "Below player" ? 1 : 2;
        s.labelTime = static_cast<float>(m->getSettingValue<double>("label-time"));
        s.showRing = m->getSettingValue<bool>("show-ring");
        s.ringSize = static_cast<float>(m->getSettingValue<double>("ring-size"));
        auto colors = m->getSettingValue<std::string>("ring-colors");
        s.ringColors = colors == "Red to blue" ? 0 : colors == "Red or blue by side" ? 1 : 2;
        s.showOverview = m->getSettingValue<bool>("show-overview");
        s.overviewSize = static_cast<float>(m->getSettingValue<double>("overview-size"));
        s.showUpTo = static_cast<int>(m->getSettingValue<int64_t>("show-up-to"));
        s.hidden = parseHidden(m->getSettingValue<std::string>("hidden-windows"));

        s.sound = m->getSettingValue<bool>("sound-enabled");
        s.soundVolume = static_cast<float>(m->getSettingValue<double>("sound-volume"));
        s.soundUpTo = static_cast<int>(m->getSettingValue<int64_t>("sound-up-to"));
    }

    int tickOf(GJBaseGameLayer* layer) {
        return static_cast<int>(std::floor(layer->m_gameState.m_levelTime * TPS + 1e-3));
    }

    void clearRunData() {
        auto& S = state();
        S.inputs.clear();
        S.recent.clear();
        S.kept.clear();
        S.seq.clear();
        S.seqNext = 0;
        S.lastAnchorTick = -1000;
    }

    void clearAll() {
        auto& S = state();
        clearRunData();
        S.results.clear();
        S.hasResults = false;
        S.runStale = false;
        S.runEndTick = 0;
        S.runCompleted = false;
        S.runDied = false;
        S.macro.clear();
        S.macroName.clear();
        clearPlaybackUi();
    }

    // Called after every attempt start and every practice-mode respawn.
    // Anything recorded after this moment never happened, so it is dropped.
    void onReset(PlayLayer* pl) {
        auto& S = state();
        if (S.analyzing || S.suppressReset || S.playback) return;

        if (S.runStale) {
            // A playback just ended. The next attempt starts a fresh recording.
            clearRunData();
            clearPlaybackUi();
            S.runStale = false;
        }

        int tick = tickOf(pl);
        truncateTo(tick);
        S.currentTick = tick;
        S.lastAnchorTick = -1000;
        if (S.settings.record || S.importing) addAnchor(pl, tick);
    }

    void maybeAnchor(PlayLayer* pl, int tick) {
        auto& S = state();
        if (tick - S.lastAnchorTick >= AnchorEvery) addAnchor(pl, tick);
    }

    // A click is about to be recorded. Keep a saved state from far enough before it
    // that the click can later be moved earlier without leaving the saved range.
    void keepAnchorFor(int clickTick) {
        auto& S = state();
        int need = clickTick - S.settings.maxWindow - 1;

        Anchor const* best = nullptr;
        auto consider = [&](Anchor const& a) {
            if (a.tick <= need && (!best || a.tick > best->tick)) best = &a;
        };
        for (auto const& a : S.recent) consider(a);
        for (auto const& a : S.kept) consider(a);

        if (!best) {
            // Not enough history yet (the very start of a run): use the oldest state we have.
            auto consider2 = [&](Anchor const& a) {
                if (a.tick <= clickTick - 1 && (!best || a.tick < best->tick)) best = &a;
            };
            for (auto const& a : S.recent) consider2(a);
            for (auto const& a : S.kept) consider2(a);
        }
        if (!best) return;

        for (auto const& k : S.kept) if (k.tick == best->tick) return;

        Anchor copy = *best;
        auto it = std::upper_bound(S.kept.begin(), S.kept.end(), copy.tick,
            [](int t, Anchor const& a) { return t < a.tick; });
        S.kept.insert(it, copy);
    }

    void makeDingFile() { makeDing(); }
}

$on_mod(Loaded) {
    fw::reloadSettings();
    fw::makeDingFile();

    for (auto const* key : { "record", "auto-playback", "show-level", "show-labels", "show-ring", "show-overview", "sound-enabled" }) {
        listenForSettingChanges<bool>(key, [](bool) { fw::reloadSettings(); });
    }
    for (auto const* key : { "max-window", "lookahead", "show-up-to", "sound-up-to" }) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { fw::reloadSettings(); });
    }
    for (auto const* key : { "scan-from", "scan-to", "label-size", "label-time", "ring-size", "overview-size", "sound-volume" }) {
        listenForSettingChanges<double>(key, [](double) { fw::reloadSettings(); });
    }
    for (auto const* key : { "label-position", "ring-colors", "hidden-windows" }) {
        listenForSettingChanges<std::string>(key, [](std::string) { fw::reloadSettings(); });
    }
}
