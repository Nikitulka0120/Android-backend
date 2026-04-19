#include "gui.h"
#include "data_structures.h"
#include "tile_cache.h"
#include "mercator.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <algorithm>

extern bool running;
extern bool start_server;
extern bool get_location;
extern bool get_network;
extern int zoom;
extern std::mutex g_CacheMutex;
extern std::mutex g_JobMutex;

void ColoredIndicator(const char* label, bool condition, 
                      const char* true_text, const char* false_text) {
    ImGui::Text("%s: ", label);
    ImGui::SameLine();
    
    if (condition) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
        ImGui::Text("* %s", true_text);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("* %s", false_text);
        ImGui::PopStyleColor();
    }
}

void LoadLogFile() {
    std::lock_guard<std::mutex> lock(mtx);
    offline_store.clear();
    
    std::ifstream file("log.json");
    if (!file.is_open()) {
        std::cerr << "[Error] Could not open log.json for reading." << std::endl;
        return;
    }
    
    std::string line;
    int line_count = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        try {
            auto j = json::parse(line);
            offline_store.lats.push_back(j.at("lat").get<double>());
            offline_store.lons.push_back(j.at("lon").get<double>());
            offline_store.times.push_back(j.at("time").get<double>());
            offline_store.indices.push_back(line_count);
            
            if (j.contains("cell_data") && j["cell_data"].contains("signal") &&
                j["cell_data"]["signal"].contains("rsrp")) {
                offline_store.rsrps.push_back(j["cell_data"]["signal"]["rsrp"].get<double>());
            } else {
                offline_store.rsrps.push_back(-145.0);
            }
            line_count++;
        } catch (const std::exception& e) {
            std::cerr << "[Error] Line " << line_count << ": " << e.what() << std::endl;
        }
    }
    
    if (line_count > 0) {
        offline_store.loaded = true;
        std::cout << "[Info] Loaded " << line_count << " records from log.json" << std::endl;
    }
    file.close();
}

void RunGUI() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;
    
    SDL_Window* window = SDL_CreateWindow("Network Analyzer", SDL_WINDOWPOS_CENTERED,
                        SDL_WINDOWPOS_CENTERED, 1100, 700,
                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();
    
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");
    
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }
        
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
        if (ImPlot::BeginPlot("History", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_X1, data_store.history.current_step - 100,
                                   data_store.history.current_step, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);
            
            std::lock_guard<std::mutex> lock(mtx);
            for (auto& [pci, y_vec] : data_store.history.streams_y) {
                std::string label = "PCI: " + std::to_string(pci);
                ImPlot::PlotLine(label.c_str(), data_store.history.x.data(),
                               y_vec.data(), (int)y_vec.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("Log Control");
        if (ImGui::Button("Load log.json", ImVec2(-1, 40))) {
            LoadLogFile();
        }
        if (offline_store.loaded) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Total: %d records",
                             (int)offline_store.lats.size());
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Records not loaded");
        }
        ImGui::End();

        ImGui::Begin("Full Signal History");
        if (offline_store.loaded && !offline_store.rsrps.empty()) {
            if (ImPlot::BeginPlot("RSRP History", ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Record Index", "dBm");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, offline_store.indices.size(), ImGuiCond_Always);
                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 1), 1.5f);
                ImPlot::PlotLine("RSRP", offline_store.indices.data(),
                               offline_store.rsrps.data(), (int)offline_store.rsrps.size());
                ImPlot::EndPlot();
            }
        } else {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Begin("Movement Route");
        if (offline_store.loaded && !offline_store.lats.empty()) {
            if (ImPlot::BeginPlot("Map", ImVec2(-1, -1))) {
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

                for (int x = minX; x <= maxX; ++x) {
                    for (int y = minY; y <= maxY; ++y) {
                        std::string tileId = std::to_string(zoom) + "/" + 
                                           std::to_string(x) + "/" + std::to_string(y);
                        
                        bool needLoad = false;
                        {
                            std::lock_guard<std::mutex> lock(g_CacheMutex);
                            if (g_TileCache.find(tileId) == g_TileCache.end()) {
                                TextureData newTex;
                                newTex.isLoading = true;
                                g_TileCache[tileId] = newTex;
                                needLoad = true;
                            }
                        }
                        
                        if (needLoad) {
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

                for (int x = minX; x <= maxX; ++x) {
                    for (int y = minY; y <= maxY; ++y) {
                        std::string tileId = std::to_string(zoom) + "/" + 
                                           std::to_string(x) + "/" + std::to_string(y);
                        
                        GLuint gpuId = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_CacheMutex);
                            auto it = g_TileCache.find(tileId);
                            if (it != g_TileCache.end()) {
                                auto& tex = it->second;
                                if (!tex.rgbaBlob.empty() && tex.id == 0) {
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
                        
                        if (gpuId != 0) {
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

                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
                ImPlot::PlotScatter("route", offline_store.lons.data(),
                                  offline_store.lats.data(), (int)offline_store.lats.size());
                ImPlot::EndPlot();
            }
        } else {
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