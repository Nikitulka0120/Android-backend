#include "data_structures.h"
#include <algorithm>

void SignalHistory::AddPoints(const json &cells_array)
{
    /* данный метод используется для построения графика,
    который отображает данные полученные в реальном времени,
    для графика истории я использую запрросы к бд*/
    if (x.size() >= max_points)
    {
        x.erase(x.begin());
        for (auto &[pci, vec] : streams_y)
            if (!vec.empty())
                vec.erase(vec.begin());
    }
    x.push_back(current_step++);
    std::vector<int> present_pcis;

    for (auto &cell : cells_array)
    {
        int pci = cell["identity"].value("pci", -1);
        if (pci == -1 || pci == 2147483647)
            continue;
        present_pcis.push_back(pci);
        if (streams_y.find(pci) == streams_y.end())
        {
            streams_y[pci] = std::vector<float>(x.size() - 1, -120.0f);
        }

        float rsrp = -120.0f;
        std::string type = cell.value("type", "");
        if (type == "LTE")
            rsrp = cell["signal"].value("rsrp", -120.0f);
        else if (type == "NR")
            rsrp = cell["signal"].value("ssRsrp", -120.0f);
        else if (type == "GSM")
            rsrp = cell["signal"].value("dbm", -120.0f);

        streams_y[pci].push_back(rsrp);
    }

    for (auto &[pci, vec] : streams_y)
    {
        if (std::find(present_pcis.begin(), present_pcis.end(), pci) == present_pcis.end())
        {
            vec.push_back(-120.0f);
        }
    }
}

void OfflineData::clear() // метод отчищает все накопленные данные
{
    lats.clear();
    lons.clear();
    rsrps.clear();
    rsrqs.clear();
    rssis.clear();
    alts.clear();
    times.clear();
    indices.clear();
    loaded = false;
}
OfflineData offline_store;
Telemetry data_store;
std::mutex mtx;
std::vector<std::string> log_messages;
int session_data_counter = 0;