#include "gui.h"
#include "data_structures.h"
#include "tile_cache.h"
#include "mercator.h"
#include "database.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <iostream>
#include <string>
#include <mutex>
#include <algorithm>
#include "HeatmapRenderer.h"

extern bool running;
extern bool start_server;
extern bool get_location;
extern bool get_network;
extern int zoom;
extern std::mutex g_CacheMutex;
extern std::mutex g_JobMutex;

HeatmapRenderer g_HeatmapRenderer;
constexpr int MODE_RSRP = 0;
constexpr int MODE_RSRQ = 1;
constexpr int MODE_RSSI = 2;
constexpr int MODE_ALT = 3;
int mode = MODE_RSRP;

static float maxRadius = 60.0f;
static float idwPower = 3.0f;

const std::vector<double> &GetCurrentModeData()
{
    switch (mode)
    {
    case MODE_RSRQ:
        return offline_store.rsrqs;

    case MODE_RSSI:
        return offline_store.rssis;

    case MODE_ALT:
        return offline_store.alts;

    default:
        return offline_store.rsrps;
    }
}

void ColoredIndicator(const char *label, bool condition,
                      const char *true_text, const char *false_text)
{
    ImGui::Text("%s: ", label);
    ImGui::SameLine();

    if (condition)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
        ImGui::Text("* %s", true_text);
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("* %s", false_text);
        ImGui::PopStyleColor();
    }
}

bool LoadDataFromDB()
{
    std::lock_guard<std::mutex> lock(mtx);
    offline_store.clear();

    auto records = LoadHistoryFromDB();
    if (records.empty())
        return false;

    for (size_t i = 0; i < records.size(); i++)
    {
        offline_store.lats.push_back(records[i].lat);
        offline_store.lons.push_back(records[i].lon);
        offline_store.times.push_back(records[i].time_ms);
        offline_store.indices.push_back(i);
        offline_store.rsrps.push_back(records[i].rsrp);
        offline_store.rsrqs.push_back(records[i].rsrq);
        offline_store.rssis.push_back(records[i].rssi);
        offline_store.alts.push_back(records[i].alt);
    }

    offline_store.loaded = true;

    g_HeatmapRenderer.UpdateData(
        offline_store.lats,
        offline_store.lons,
        GetCurrentModeData(),
        maxRadius, idwPower);
    g_HeatmapRenderer.StartGeneration();

    return true;
}

void RunGUI()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        return;

    SDL_Window *window = SDL_CreateWindow("Network Analyzer", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 1100, 700,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    LoadDataFromDB();

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
        }
        g_HeatmapRenderer.ProcessGPUUpload();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Current stats");
        ImGui::Text("Network type: %s", data_store.type.c_str());
        ImGui::Text("RSRP: %.1f dBm", data_store.current_rsrp);
        ImGui::Separator();
        ImGui::Text("Lat: %s", data_store.lat.c_str());
        ImGui::Text("Lon: %s", data_store.lon.c_str());
        ImGui::Text("Alt: %s", data_store.alt.c_str());
        ImGui::Text("Acc: %s", data_store.acc.c_str());
        ImGui::Separator();
        ImGui::Text("Data counter in this session: %d", session_data_counter);
        ImGui::End();

        ImGui::Begin("Filters");
        ImGui::Checkbox("Is running?", &start_server);
        ImGui::Checkbox("Is location?", &get_location);
        ImGui::Checkbox("Is network?", &get_network);
        ImGui::End();

        ImGui::Begin("Status");
        ColoredIndicator("Server", start_server, "RUNNING", "STOPPED");
        ColoredIndicator("Location", get_location);
        ColoredIndicator("Network", get_network);
        ImGui::End();

        ImGui::Begin("Signal Graph");
        if (ImPlot::BeginPlot("History", ImVec2(-1, -1)))
        {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_X1, data_store.history.current_step - 100,
                                    data_store.history.current_step, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);

            std::lock_guard<std::mutex> lock(mtx);
            for (auto &[pci, y_vec] : data_store.history.streams_y)
            {
                std::string label = "PCI: " + std::to_string(pci);
                ImPlot::PlotLine(label.c_str(), data_store.history.x.data(),
                                 y_vec.data(), (int)y_vec.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("Log Control");
        if (ImGui::Button("Load from Database", ImVec2(-1, 40)))
        {
            LoadDataFromDB();
        }
        if (offline_store.loaded)
        {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Total: %d records from DB",
                               (int)offline_store.lats.size());
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Records not loaded");
        }
        ImGui::End();

        ImGui::Begin("Full Signal History");
        if (offline_store.loaded && !offline_store.rsrps.empty())
        {
            float window_width = ImGui::GetContentRegionAvail().x;
            float plot_height = (ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y * 2) / 3.0f;

            int num_records = (int)offline_store.indices.size();

            if (ImPlot::BeginPlot("RSRP History", ImVec2(window_width, plot_height)))
            {
                ImPlot::SetupAxes("Record Index", "dBm");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, num_records, ImGuiCond_Always);
                ImPlot::PlotLine("RSRP",
                                 offline_store.indices.data(),
                                 offline_store.rsrps.data(),
                                 num_records);
                ImPlot::EndPlot();
            }

            if (ImPlot::BeginPlot("RSRQ History", ImVec2(window_width, plot_height)))
            {
                ImPlot::SetupAxes("Record Index", "dB");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -30, 0);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, num_records, ImGuiCond_Always);
                ImPlot::PlotLine("RSRQ",
                                 offline_store.indices.data(),
                                 offline_store.rsrqs.data(),
                                 num_records);
                ImPlot::EndPlot();
            }

            if (ImPlot::BeginPlot("RSSI History", ImVec2(window_width, plot_height)))
            {
                ImPlot::SetupAxes("Record Index", "dBm");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -120, -30);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, num_records, ImGuiCond_Always);
                ImPlot::PlotLine("RSSI",
                                 offline_store.indices.data(),
                                 offline_store.rssis.data(),
                                 num_records);
                ImPlot::EndPlot();
            }
        }
        else
        {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Begin("Heatmap Settings");

        ImGui::Text("Status: ");
        ImGui::SameLine();
        if (g_HeatmapRenderer.IsGenerating())
        {
            double time = ImGui::GetTime();
            int dots = (int)(time * 3.0) % 4;
            const char* dotStr = (dots == 0) ? "" : (dots == 1) ? "." : (dots == 2) ? ".." : "...";
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Generating%s", dotStr);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Ready");
        }
        ImGui::Separator();
        int oldMode = mode;
        ImGui::RadioButton("RSRP", &mode, MODE_RSRP);
        ImGui::SameLine();
        ImGui::RadioButton("RSRQ", &mode, MODE_RSRQ);
        ImGui::SameLine();
        ImGui::RadioButton("RSSI", &mode, MODE_RSSI);
        ImGui::SameLine();
        ImGui::RadioButton("Altitude", &mode, MODE_ALT);

        if (oldMode != mode && offline_store.loaded)
        {
            g_HeatmapRenderer.UpdateData(
                offline_store.lats,
                offline_store.lons,
                GetCurrentModeData(),
                maxRadius, 
                idwPower);

            g_HeatmapRenderer.StartGeneration();
        }

        ImGui::End();

        ImGui::Begin("Movement Route");
        if (offline_store.loaded && !offline_store.lats.empty())
        {
            if (ImPlot::BeginPlot("Map", ImVec2(-1, -1)))
            {
                ImPlot::SetupAxes("Longitude", "Latitude");
                ImPlot::SetupAxisLimits(ImAxis_X1, 82.900, 82.960, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 55.000, 55.040, ImGuiCond_Once);

                ImPlotRect limits = ImPlot::GetPlotLimits();
                double mercator_left = limits.X.Min;
                double mercator_right = limits.X.Max;
                double mercator_bottom = LatToMercatorY(limits.Y.Min);
                double mercator_top = LatToMercatorY(limits.Y.Max);

                zoom = CalculateZoom(mercator_left, mercator_right);

                int minX = (int)floor(MercatorXToTileX(mercator_left, zoom));
                int minY = (int)floor(MercatorYToTileY(mercator_top, zoom));
                int maxX = (int)floor(MercatorXToTileX(mercator_right, zoom));
                int maxY = (int)floor(MercatorYToTileY(mercator_bottom, zoom));

                int maxTileCount = (1 << zoom) - 1;
                minX = std::max(0, std::min(minX, maxTileCount));
                maxX = std::max(0, std::min(maxX, maxTileCount));
                minY = std::max(0, std::min(minY, maxTileCount));
                maxY = std::max(0, std::min(maxY, maxTileCount));

                for (int x = minX; x <= maxX; ++x)
                {
                    for (int y = minY; y <= maxY; ++y)
                    {
                        std::string tileId = std::to_string(zoom) + "/" +
                                             std::to_string(x) + "/" + std::to_string(y);

                        bool needLoad = false;
                        {
                            std::lock_guard<std::mutex> lock(g_CacheMutex);
                            if (g_TileCache.find(tileId) == g_TileCache.end())
                            {
                                TextureData newTex;
                                newTex.isLoading = true;
                                g_TileCache[tileId] = newTex;
                                needLoad = true;
                            }
                        }

                        if (needLoad)
                        {
                            std::lock_guard<std::mutex> lock(g_JobMutex);
                            TileJob job;
                            job.id = tileId;
                            job.zoom = zoom;
                            job.x = x;
                            job.y = y;
                            g_JobQueue.push(job);
                        }
                    }
                }

                for (int x = minX; x <= maxX; ++x)
                {
                    for (int y = minY; y <= maxY; ++y)
                    {
                        std::string tileId = std::to_string(zoom) + "/" +
                                             std::to_string(x) + "/" + std::to_string(y);

                        GLuint gpuId = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_CacheMutex);
                            auto it = g_TileCache.find(tileId);
                            if (it != g_TileCache.end())
                            {
                                auto &tex = it->second;
                                if (!tex.rgbaBlob.empty() && tex.id == 0)
                                {
                                    glGenTextures(1, &tex.id);
                                    glBindTexture(GL_TEXTURE_2D, tex.id);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height, 0,
                                                 GL_RGBA, GL_UNSIGNED_BYTE, tex.rgbaBlob.data());
                                    tex.rgbaBlob.clear();
                                }
                                gpuId = tex.id;
                            }
                        }

                        if (gpuId != 0)
                        {
                            double left = TileXToMercatorX(x, zoom);
                            double right = TileXToMercatorX(x + 1, zoom);
                            double top_mercator = TileYToMercatorY(y, zoom);
                            double bottom_mercator = TileYToMercatorY(y + 1, zoom);
                            double top_lat = MercatorYToLat(top_mercator);
                            double bottom_lat = MercatorYToLat(bottom_mercator);

                            ImPlotPoint minPoint{left, bottom_lat};
                            ImPlotPoint maxPoint{right, top_lat};
                            ImPlot::PlotImage(("##tile_" + tileId).c_str(),
                                              (ImTextureID)(intptr_t)gpuId, minPoint, maxPoint);
                        }
                    }
                }
                g_HeatmapRenderer.UI_DrawOverlay();

                const auto &currentValues = GetCurrentModeData();

                if (!currentValues.empty())
                {
                    float minValue = *std::min_element(currentValues.begin(), currentValues.end());
                    float maxValue = *std::max_element(currentValues.begin(), currentValues.end());
                    float range = maxValue - minValue + 0.0001f;

                    const char *unit = "";
                    switch (mode)
                    {
                    case MODE_RSRP:
                        unit = "dBm";
                        break;
                    case MODE_RSRQ:
                        unit = "dB";
                        break;
                    case MODE_RSSI:
                        unit = "dBm";
                        break;
                    case MODE_ALT:
                        unit = "m";
                        break;
                    }

                    std::vector<double> group_excellent_lats, group_excellent_lons;
                    std::vector<double> group_good_lats, group_good_lons;
                    std::vector<double> group_fair_lats, group_fair_lons;
                    std::vector<double> group_poor_lats, group_poor_lons;
                    std::vector<double> group_no_sig_lats, group_no_sig_lons;

                    for (size_t i = 0; i < currentValues.size(); i++)
                    {
                        float value = currentValues[i];
                        double lat = offline_store.lats[i];
                        double lon = offline_store.lons[i];

                        if (mode == MODE_RSRP)
                        {
                            if (value >= -80.0f)
                            {
                                group_excellent_lats.push_back(lat);
                                group_excellent_lons.push_back(lon);
                            }
                            else if (value >= -90.0f)
                            {
                                group_good_lats.push_back(lat);
                                group_good_lons.push_back(lon);
                            }
                            else if (value >= -100.0f)
                            {
                                group_fair_lats.push_back(lat);
                                group_fair_lons.push_back(lon);
                            }
                            else if (value >= -110.0f)
                            {
                                group_poor_lats.push_back(lat);
                                group_poor_lons.push_back(lon);
                            }
                            else
                            {
                                group_no_sig_lats.push_back(lat);
                                group_no_sig_lons.push_back(lon);
                            }
                        }
                        else
                        {
                            float normalized = (value - minValue) / range;
                            if (normalized > 0.8f)
                            {
                                group_excellent_lats.push_back(lat);
                                group_excellent_lons.push_back(lon);
                            }
                            else if (normalized > 0.6f)
                            {
                                group_good_lats.push_back(lat);
                                group_good_lons.push_back(lon);
                            }
                            else if (normalized > 0.4f)
                            {
                                group_fair_lats.push_back(lat);
                                group_fair_lons.push_back(lon);
                            }
                            else if (normalized > 0.2f)
                            {
                                group_poor_lats.push_back(lat);
                                group_poor_lons.push_back(lon);
                            }
                            else
                            {
                                group_no_sig_lats.push_back(lat);
                                group_no_sig_lons.push_back(lon);
                            }
                        }
                    }
                    char label_excellent[128];
                    char label_good[128];
                    char label_fair[128];
                    char label_poor[128];
                    char label_no_sig[128];

                    if (mode == MODE_RSRP)
                    {
                        snprintf(label_excellent, sizeof(label_excellent), "Excellent (>= -80.0 %s)", unit);
                        snprintf(label_good, sizeof(label_good), "Good ([-90.0, -80.0) %s)", unit);
                        snprintf(label_fair, sizeof(label_fair), "Fair/Marginal ([-100.0, -90.0) %s)", unit);
                        snprintf(label_poor, sizeof(label_poor), "Poor/Weak ([-110.0, -100.0) %s)", unit);
                        snprintf(label_no_sig, sizeof(label_no_sig), "No Signal (< -110.0 %s)", unit);
                    }
                    else
                    {
                        snprintf(label_excellent, sizeof(label_excellent), "Excellent (>= %.1f %s)", minValue + 0.8f * range, unit);
                        snprintf(label_good, sizeof(label_good), "Good (>= %.1f %s)", minValue + 0.6f * range, unit);
                        snprintf(label_fair, sizeof(label_fair), "Fair (>= %.1f %s)", minValue + 0.4f * range, unit);
                        snprintf(label_poor, sizeof(label_poor), "Weak (>= %.1f %s)", minValue + 0.2f * range, unit);
                        snprintf(label_no_sig, sizeof(label_no_sig), "Very Bad (< %.1f %s)", minValue + 0.2f * range, unit);
                    }
                    if (!group_excellent_lats.empty())
                        ImPlot::PlotScatter(label_excellent, group_excellent_lons.data(), group_excellent_lats.data(), (int)group_excellent_lats.size());

                    if (!group_good_lats.empty())
                        ImPlot::PlotScatter(label_good, group_good_lons.data(), group_good_lats.data(), (int)group_good_lats.size());

                    if (!group_fair_lats.empty())
                        ImPlot::PlotScatter(label_fair, group_fair_lons.data(), group_fair_lats.data(), (int)group_fair_lats.size());

                    if (!group_poor_lats.empty())
                        ImPlot::PlotScatter(label_poor, group_poor_lons.data(), group_poor_lats.data(), (int)group_poor_lats.size());

                    if (!group_no_sig_lats.empty())
                        ImPlot::PlotScatter(label_no_sig, group_no_sig_lons.data(), group_no_sig_lats.empty() ? nullptr : group_no_sig_lats.data(), (int)group_no_sig_lats.size());
                }
                else
                {
                    ImPlot::PlotScatter("route", offline_store.lons.data(), offline_store.lats.data(), (int)offline_store.lats.size());
                }

                ImPlot::EndPlot();
            }
        }
        else
        {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Begin("Signal Legend");
        if (offline_store.loaded && !offline_store.lats.empty())
        {
            const auto &currentValues = GetCurrentModeData();

            float minValue = *std::min_element(currentValues.begin(), currentValues.end());
            float maxValue = *std::max_element(currentValues.begin(), currentValues.end());

            const char *modeName = "Value";
            const char *unit = "";
            switch (mode)
            {
            case MODE_RSRP:
                modeName = "RSRP";
                unit = "dBm";
                break;
            case MODE_RSRQ:
                modeName = "RSRQ";
                unit = "dB";
                break;
            case MODE_RSSI:
                modeName = "RSSI";
                unit = "dBm";
                break;
            case MODE_ALT:
                modeName = "Altitude";
                unit = "m";
                break;
            }

            ImGui::Text("Current Mode: %s", modeName);
            ImGui::Separator();
            ImDrawList *draw_list = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float bar_width = ImGui::GetContentRegionAvail().x;
            float bar_height = 20.0f;

            if (bar_width > 50.0f)
            {
                ImU32 col_blue = IM_COL32(0, 0, 255, 255);
                ImU32 col_red = IM_COL32(255, 0, 0, 255);

                draw_list->AddRectFilledMultiColor(
                    pos,
                    ImVec2(pos.x + bar_width, pos.y + bar_height),
                    col_blue, col_red, col_red, col_blue);

                ImGui::Dummy(ImVec2(bar_width, bar_height + 5.0f));
                ImGui::Text("%.1f %s (Min)", minValue, unit);
                ImGui::SameLine(bar_width - ImGui::CalcTextSize("000.0 m (Max)").x);
                ImGui::Text("%.1f %s (Max)", maxValue, unit);
            }
            else
            {
                ImGui::Text("Min: %.1f %s", minValue, unit);
                ImGui::Text("Max: %.1f %s", maxValue, unit);
            }

            ImGui::Separator();
            for (int i = 0; i <= 4; ++i)
            {
                float t = i / 4.0f;
                float val = minValue + t * (maxValue - minValue);
                int r = (int)(0 + t * (255 - 0));
                int g = 0;
                int b = (int)(255 + t * (0 - 255));

                ImGui::ColorButton(std::to_string(i).c_str(), ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip);
                ImGui::SameLine();
                ImGui::Text("%.1f %s", val, unit);
            }

            ImGui::Separator();
            ImGui::Text("Total points: %d", (int)currentValues.size());
        }
        else
        {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, 1100, 700);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
}