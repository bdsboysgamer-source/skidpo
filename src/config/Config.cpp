#include "Config.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace fb {

namespace {

constexpr uint8_t kVkT = 0x54;
constexpr uint8_t kVkD = 0x44;
constexpr uint8_t kVkA = 0x41;
constexpr uint8_t kVkW = 0x57;
constexpr uint8_t kVkS = 0x53;

// ---- Default macro sequences ------------------------------------------
// Every run of clicks on the same spot (previously "click N times") is now
// a single RepeatClick step: an autoclicker burst at a fixed 150ms cadence
// (see App::kRepeatClickIntervalMs) for durationSec seconds - durationSec
// and delayAfterSec below are just starting points; both are user-editable
// per step from the in-app macro config tabs.
MacroConfig DefaultMacroA() {
    MacroConfig mc;
    mc.name = "Sell Runo";
    mc.steps = {
        { MacroStepKind::Tap, kVkT, 0, 0, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 3.0f, 1.0f },
        { MacroStepKind::Click, 0, 1270, 885, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 4.0f, 1.0f },
        { MacroStepKind::Click, 0, 1270, 925, 0.0f, 1.0f },
        { MacroStepKind::Hold, kVkD, 0, 0, 0.5f, 1.0f },
        { MacroStepKind::Hold, kVkT, 0, 0, 3.0f, 1.0f },
        { MacroStepKind::Hold, kVkA, 0, 0, 0.5f, 1.0f },
        { MacroStepKind::Tap, kVkT, 0, 0, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 1270, 905, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 3.0f, 1.0f },
    };
    return mc;
}

MacroConfig DefaultMacroB() {
    MacroConfig mc;
    mc.name = "Buy Fish Head";
    mc.steps = {
        { MacroStepKind::Tap, kVkT, 0, 0, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 3.0f, 1.0f },
        { MacroStepKind::Click, 0, 775, 705, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 775, 705, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 1280, 890, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 1280, 905, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 1280, 945, 0.0f, 1.0f },
    };
    return mc;
}

// Sell Shiro started as a duplicate of Sell Runo (DefaultMacroA) with Hold
// D/A swapped for Hold W/S, then had its Click(1270,885) and the
// RepeatClick(960,921) after it removed, and its next click retargeted
// from (1270,925) to (1270,905).
MacroConfig DefaultMacroC() {
    MacroConfig mc;
    mc.name = "Sell Shiro";
    mc.steps = {
        { MacroStepKind::Tap, kVkT, 0, 0, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 3.0f, 1.0f },
        { MacroStepKind::Click, 0, 1270, 905, 0.0f, 1.0f },
        { MacroStepKind::Hold, kVkW, 0, 0, 0.5f, 1.0f },
        { MacroStepKind::Hold, kVkT, 0, 0, 3.0f, 1.0f },
        { MacroStepKind::Hold, kVkS, 0, 0, 0.5f, 1.0f },
        { MacroStepKind::Tap, kVkT, 0, 0, 0.0f, 1.0f },
        { MacroStepKind::Click, 0, 1270, 905, 0.0f, 1.0f },
        { MacroStepKind::RepeatClick, 0, 960, 921, 3.0f, 1.0f },
    };
    return mc;
}

const char* MacroStepKindName(MacroStepKind kind) {
    switch (kind) {
        case MacroStepKind::Tap: return "Tap";
        case MacroStepKind::Hold: return "Hold";
        case MacroStepKind::Click: return "Click";
        case MacroStepKind::RepeatClick: return "RepeatClick";
    }
    return "Tap";
}

} // namespace

Config Config::Defaults() {
    Config cfg{};
    cfg.macroA = DefaultMacroA();
    cfg.macroB = DefaultMacroB();
    cfg.macroC = DefaultMacroC();
    return cfg;
}

namespace {

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::unordered_map<std::string, std::string> ParseIni(const std::string& path, bool& ok) {
    std::unordered_map<std::string, std::string> kv;
    std::ifstream file(path);
    ok = file.is_open();
    if (!ok) return kv;

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string value = Trim(trimmed.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        kv[key] = value;
    }
    return kv;
}

bool GetInt(const std::unordered_map<std::string, std::string>& kv, const std::string& key, int& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    try {
        out = std::stoi(it->second);
        return true;
    } catch (...) {
        return false;
    }
}

bool GetFloat(const std::unordered_map<std::string, std::string>& kv, const std::string& key, float& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    try {
        out = std::stof(it->second);
        return true;
    } catch (...) {
        return false;
    }
}

bool GetBool(const std::unordered_map<std::string, std::string>& kv, const std::string& key, bool& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
    return false;
}

} // namespace

Config Config::LoadFromFile(const std::string& path) {
    Config cfg = Config::Defaults();
    bool opened = false;
    auto kv = ParseIni(path, opened);
    if (!opened) return cfg;

    GetInt(kv, "roi.screenx", cfg.roi.screenX);
    GetInt(kv, "roi.screeny", cfg.roi.screenY);
    GetInt(kv, "roi.width", cfg.roi.width);
    GetInt(kv, "roi.height", cfg.roi.height);
    GetInt(kv, "roi.flankpixels", cfg.roi.flankPixels);
    GetInt(kv, "roi.secondaryscreenoffsetxpx", cfg.roi.secondaryScreenOffsetXPx);

    GetInt(kv, "timing.castingdetectdebouncems", cfg.timing.castingDetectDebounceMs);
    GetInt(kv, "timing.castmaxwaitms", cfg.timing.castMaxWaitMs);
    GetInt(kv, "timing.noobjecttimeoutms", cfg.timing.noObjectTimeoutMs);
    GetInt(kv, "timing.postfishdelayms", cfg.timing.postFishDelayMs);
    GetInt(kv, "timing.tholdms", cfg.timing.tHoldMs);
    GetInt(kv, "timing.clickpulsems", cfg.timing.clickPulseMs);
    GetInt(kv, "timing.macromovedurationms", cfg.timing.macroMoveDurationMs);
    GetFloat(kv, "timing.detectionmaxhz", cfg.timing.detectionMaxHz);

    GetFloat(kv, "detection.markerminpixelscore", cfg.detection.markerMinPixelScore);
    GetInt(kv, "detection.targetbordercolumnroilocalx", cfg.detection.targetBorderColumnRoiLocalX);
    GetInt(kv, "detection.targetbordercolumnsearchwidth", cfg.detection.targetBorderColumnSearchWidth);
    GetInt(kv, "detection.targetborderrunheightpx", cfg.detection.targetBorderRunHeightPx);
    GetInt(kv, "detection.targetborderrunheighttolerancepx", cfg.detection.targetBorderRunHeightTolerancePx);
    GetFloat(kv, "detection.targetborderuniformitytolerance", cfg.detection.targetBorderUniformityTolerance);
    GetFloat(kv, "detection.targetbordercoloraxistolerance", cfg.detection.targetBorderColorAxisTolerance);
    GetFloat(kv, "detection.targetbordercoloraxismargin", cfg.detection.targetBorderColorAxisMargin);
    GetFloat(kv, "detection.targetpriorsearchminradiuspx", cfg.detection.targetPriorSearchMinRadiusPx);
    GetInt(kv, "detection.markerminheightpx", cfg.detection.markerMinHeightPx);
    GetInt(kv, "detection.markermincolumns", cfg.detection.markerMinColumns);
    GetFloat(kv, "detection.markerminfillratio", cfg.detection.markerMinFillRatio);
    GetInt(kv, "detection.markergapbridgepx", cfg.detection.markerGapBridgePx);
    GetFloat(kv, "detection.minconfidencetoreport", cfg.detection.minConfidenceToReport);

    GetFloat(kv, "tracking.maxjumppxpersec", cfg.tracking.maxJumpPxPerSec);
    GetInt(kv, "tracking.reacquireframes", cfg.tracking.reacquireFrames);
    GetInt(kv, "tracking.misstimeoutms", cfg.tracking.missTimeoutMs);
    GetFloat(kv, "tracking.alpha", cfg.tracking.alpha);
    GetFloat(kv, "tracking.beta", cfg.tracking.beta);
    GetFloat(kv, "tracking.coastconfidencedecaypersec", cfg.tracking.coastConfidenceDecayPerSec);

    GetBool(kv, "controller.holdmovesmarkerup", cfg.controller.holdMovesMarkerUp);
    GetFloat(kv, "controller.deadbandpx", cfg.controller.deadbandPx);
    GetInt(kv, "controller.mintoggleintervalms", cfg.controller.minToggleIntervalMs);
    GetFloat(kv, "controller.minconfidenceforcontrol", cfg.controller.minConfidenceForControl);
    GetInt(kv, "controller.controllercoastms", cfg.controller.controllerCoastMs);
    GetFloat(kv, "controller.predictionhorizonsec", cfg.controller.predictionHorizonSec);

    GetBool(kv, "ui.debugmodedefault", cfg.ui.debugModeDefault);
    GetInt(kv, "ui.refreshintervalms", cfg.ui.refreshIntervalMs);
    GetFloat(kv, "ui.overlayrenderhz", cfg.ui.overlayRenderHz);

    // Macro steps: kind/vk/x/y come from Config::Defaults() (code, not the
    // file) and are never overridden here - only the two user-editable
    // fields are read back, by step index, so a code change to the default
    // sequence itself is never silently overridden by a stale file.
    auto loadMacroOverrides = [&kv](MacroConfig& mc, const std::string& prefix) {
        for (size_t i = 0; i < mc.steps.size(); ++i) {
            std::string p = prefix + ".step" + std::to_string(i) + ".";
            GetFloat(kv, p + "durationsec", mc.steps[i].durationSec);
            GetFloat(kv, p + "delayaftersec", mc.steps[i].delayAfterSec);
        }
    };
    loadMacroOverrides(cfg.macroA, "macroa");
    loadMacroOverrides(cfg.macroB, "macrob");
    loadMacroOverrides(cfg.macroC, "macroc");

    return cfg;
}

bool Config::SaveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;

    file << "# Fishing bot configuration\n\n";
    file << "[roi]\n";
    file << "roi.screenX=" << roi.screenX << "\n";
    file << "roi.screenY=" << roi.screenY << "\n";
    file << "roi.width=" << roi.width << "\n";
    file << "roi.height=" << roi.height << "\n";
    file << "roi.flankPixels=" << roi.flankPixels << "\n";
    file << "roi.secondaryScreenOffsetXPx=" << roi.secondaryScreenOffsetXPx << "\n\n";

    file << "[timing]\n";
    file << "timing.castingDetectDebounceMs=" << timing.castingDetectDebounceMs << "\n";
    file << "timing.castMaxWaitMs=" << timing.castMaxWaitMs << "\n";
    file << "timing.noObjectTimeoutMs=" << timing.noObjectTimeoutMs << "\n";
    file << "timing.postFishDelayMs=" << timing.postFishDelayMs << "\n";
    file << "timing.tHoldMs=" << timing.tHoldMs << "\n";
    file << "timing.clickPulseMs=" << timing.clickPulseMs << "\n";
    file << "timing.macroMoveDurationMs=" << timing.macroMoveDurationMs << "\n";
    file << "timing.detectionMaxHz=" << timing.detectionMaxHz << "\n\n";

    file << "[detection]\n";
    file << "detection.markerMinPixelScore=" << detection.markerMinPixelScore << "\n";
    file << "detection.targetBorderColumnRoiLocalX=" << detection.targetBorderColumnRoiLocalX << "\n";
    file << "detection.targetBorderColumnSearchWidth=" << detection.targetBorderColumnSearchWidth << "\n";
    file << "detection.targetBorderRunHeightPx=" << detection.targetBorderRunHeightPx << "\n";
    file << "detection.targetBorderRunHeightTolerancePx=" << detection.targetBorderRunHeightTolerancePx << "\n";
    file << "detection.targetBorderUniformityTolerance=" << detection.targetBorderUniformityTolerance << "\n";
    file << "detection.targetBorderColorAxisTolerance=" << detection.targetBorderColorAxisTolerance << "\n";
    file << "detection.targetBorderColorAxisMargin=" << detection.targetBorderColorAxisMargin << "\n";
    file << "detection.targetPriorSearchMinRadiusPx=" << detection.targetPriorSearchMinRadiusPx << "\n";
    file << "detection.markerMinHeightPx=" << detection.markerMinHeightPx << "\n";
    file << "detection.markerMinColumns=" << detection.markerMinColumns << "\n";
    file << "detection.markerMinFillRatio=" << detection.markerMinFillRatio << "\n";
    file << "detection.markerGapBridgePx=" << detection.markerGapBridgePx << "\n";
    file << "detection.minConfidenceToReport=" << detection.minConfidenceToReport << "\n\n";

    file << "[tracking]\n";
    file << "tracking.maxJumpPxPerSec=" << tracking.maxJumpPxPerSec << "\n";
    file << "tracking.reacquireFrames=" << tracking.reacquireFrames << "\n";
    file << "tracking.missTimeoutMs=" << tracking.missTimeoutMs << "\n";
    file << "tracking.alpha=" << tracking.alpha << "\n";
    file << "tracking.beta=" << tracking.beta << "\n";
    file << "tracking.coastConfidenceDecayPerSec=" << tracking.coastConfidenceDecayPerSec << "\n\n";

    file << "[controller]\n";
    file << "controller.holdMovesMarkerUp=" << (controller.holdMovesMarkerUp ? "true" : "false") << "\n";
    file << "controller.deadbandPx=" << controller.deadbandPx << "\n";
    file << "controller.minToggleIntervalMs=" << controller.minToggleIntervalMs << "\n";
    file << "controller.minConfidenceForControl=" << controller.minConfidenceForControl << "\n";
    file << "controller.controllerCoastMs=" << controller.controllerCoastMs << "\n";
    file << "controller.predictionHorizonSec=" << controller.predictionHorizonSec << "\n\n";

    file << "[ui]\n";
    file << "ui.debugModeDefault=" << (ui.debugModeDefault ? "true" : "false") << "\n";
    file << "ui.refreshIntervalMs=" << ui.refreshIntervalMs << "\n";
    file << "ui.overlayRenderHz=" << ui.overlayRenderHz << "\n\n";

    auto writeMacro = [&file](const MacroConfig& mc, const std::string& prefix) {
        file << "# " << prefix << ".stepN.kind/vk/x/y are fixed in code (Config::Defaults) and\n";
        file << "# written here only for reference - editing them has no effect. Only\n";
        file << "# durationSec/delayAfterSec are read back on load; prefer the in-app\n";
        file << "# macro config tabs over editing these directly.\n";
        file << "[" << prefix << "]\n";
        file << prefix << ".name=" << mc.name << "\n";
        for (size_t i = 0; i < mc.steps.size(); ++i) {
            const MacroStepConfig& s = mc.steps[i];
            std::string p = prefix + ".step" + std::to_string(i) + ".";
            file << p << "kind=" << MacroStepKindName(s.kind) << "\n";
            file << p << "vk=" << static_cast<int>(s.vk) << "\n";
            file << p << "x=" << s.clickX << "\n";
            file << p << "y=" << s.clickY << "\n";
            file << p << "durationSec=" << s.durationSec << "\n";
            file << p << "delayAfterSec=" << s.delayAfterSec << "\n";
        }
        file << "\n";
    };
    writeMacro(macroA, "macroA");
    writeMacro(macroB, "macroB");
    writeMacro(macroC, "macroC");

    return true;
}

} // namespace fb
