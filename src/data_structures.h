#pragma once
#include <vector>
#include <map>
#include <string>
#include <nlohmann/json.hpp>
#include <mutex>

using json = nlohmann::json;

struct SignalHistory {
    std::map<int, std::vector<float>> streams_y;
    std::vector<float> x;
    float current_step = 0;
    const int max_points = 200;
    void add_points(const json& cells_array);
};

struct OfflineData {
    std::vector<double> lats;
    std::vector<double> lons;
    std::vector<double> rsrps;
    std::vector<double> times;
    std::vector<double> indices;
    bool loaded = false;
    void clear();
};

struct Telemetry {
    std::string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
    std::vector<std::string> pending_records;
};

extern OfflineData offline_store;
extern Telemetry data_store;
extern std::mutex mtx;
extern std::vector<std::string> log_messages;
extern int session_data_counter;