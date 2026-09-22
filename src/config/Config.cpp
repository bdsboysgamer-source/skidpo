#include "Config.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace fb {

Config Config::Defaults() {
    return Config{};
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

    GetInt(kv, "timing.castingdetectdebouncems", cfg.timing.castingDetectDebounceMs);
    GetInt(kv, "timing.castmaxwaitms", cfg.timing.castMaxWaitMs);
    GetInt(kv, "timing.noobjecttimeoutms", cfg.timing.noObjectTimeoutMs);
    GetInt(kv, "timing.tholdms", cfg.timing.tHoldMs);
    GetInt(kv, "timing.clickpulsems", cfg.timing.clickPulseMs);

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
    file << "roi.flankPixels=" << roi.flankPixels << "\n\n";

    file << "[timing]\n";
    file << "timing.castingDetectDebounceMs=" << timing.castingDetectDebounceMs << "\n";
    file << "timing.castMaxWaitMs=" << timing.castMaxWaitMs << "\n";
    file << "timing.noObjectTimeoutMs=" << timing.noObjectTimeoutMs << "\n";
    file << "timing.tHoldMs=" << timing.tHoldMs << "\n";
    file << "timing.clickPulseMs=" << timing.clickPulseMs << "\n\n";

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

    return true;
}

} // namespace fb
