#define _USE_MATH_DEFINES
#include <zmq.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <fstream>
#include <mutex>
#include <vector>
#include <ctime>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <curl/curl.h>
#include <numbers>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include <libpq-fe.h>

#define HOST "localhost"
#define PORT "5433"
#define DB_NAME "android_backend"
#define DB_USER "postgres" // по умолчанию postgres
#define DB_USER_PASSWORD "somepassword"

using json = nlohmann::json;

struct SignalHistory
{
    std::map<int, std::vector<float>> streams_y;
    std::vector<float> x;
    float current_step = 0;
    const int max_points = 200;
    void add_points(const json &cells_array)
    {
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
                streams_y[pci] = std::vector<float>(x.size() - 1, -145.0f);
            }
            float rsrp = -145.0f;
            std::string type = cell.value("type", "");
            if (type == "LTE")
                rsrp = cell["signal"].value("rsrp", -145.0f);
            else if (type == "NR")
                rsrp = cell["signal"].value("ssRsrp", -145.0f);
            else if (type == "GSM")
                rsrp = cell["signal"].value("dbm", -145.0f);
            streams_y[pci].push_back(rsrp);
        }
        for (auto &[pci, vec] : streams_y)
        {
            if (std::find(present_pcis.begin(), present_pcis.end(), pci) == present_pcis.end())
            {
                vec.push_back(-145.0f);
            }
        }
    }
};

struct OfflineData
{
    std::vector<double> lats;
    std::vector<double> lons;
    std::vector<double> rsrps;
    std::vector<double> times;
    std::vector<double> indices;

    bool loaded = false;

    void clear()
    {
        lats.clear();
        lons.clear();
        rsrps.clear();
        times.clear();
        indices.clear();
        loaded = false;
    }
} offline_store;

struct Telemetry
{
    std::string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
    std::vector<std::string> pending_records;
} data_store;

static const ImPlotAxisFlags _xFlags{
    ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines |
    ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels |
    ImPlotAxisFlags_NoInitialFit | ImPlotAxisFlags_NoMenus |
    ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoHighlight};

static const ImPlotAxisFlags _yFlags{_xFlags |
                                     ImPlotAxisFlags_Invert};

double PI = M_PI;
double PI2 = M_PI * 2;
double RAD = M_PI / 180.0;
double DEG = 180.0 / M_PI;

int POW2[]{
    (1 << 0), (1 << 1), (1 << 2), (1 << 3), (1 << 4), (1 << 5), (1 << 6),
    (1 << 7), (1 << 8), (1 << 9), (1 << 10), (1 << 11), (1 << 12), (1 << 13),
    (1 << 14), (1 << 15), (1 << 16), (1 << 17), (1 << 18)};

double lon2x(const double lon, int z = 0)
{
    return (lon + 180.0) / 360.0 * double(POW2[z]);
}

double lat2y(const double lat, int z = 0)
{
    return (1.0 - asinh(tan(lat * RAD)) / PI) / 2.0 * double(POW2[z]);
}

double x2lon(const double x, int z = 0)
{
    return x / double(POW2[z]) * 360.0 - 180.0;
}

double y2lat(const double y, const int z = 0)
{
    const double n{PI - PI2 * y / double(POW2[z])};
    return DEG * atan(0.5 * (exp(n) - exp(-n)));
}

void getTileCoords(double lon, double lat, int zoom, int &x, int &y)
{
    x = (int)lon2x(lon, zoom);
    y = (int)lat2y(lat, zoom);
}

double _minLat{-85.0};
double _maxLat{+85.0};
double _minLon{-179.9};
double _maxLon{+179.9};
int MinZoom{0};
int MaxZoom{18};

double _minX = lon2x(_minLon, 0);
double _maxX = lon2x(_maxLon, 0);
double _minY = lat2y(_minLat, 0);
double _maxY = lat2y(_maxLat, 0);

ImPlotPoint _mousePos{};
ImPlotRect _plotLims{};
ImVec2 _plotSize{};

struct TileInfo
{
    int width, height, channels;
    unsigned char *data;
    std::vector<unsigned char> blob;
    GLuint texture_id;
    bool loaded;
    bool loading;
    int x, y, z;
};

std::vector<TileInfo> tiles;
struct MapState
{
    int current_zoom = 16;
    double center_lon = 82.92;
    double center_lat = 55.03;
    bool need_reload = false;
} map_state;

void clearTiles()
{
    for (auto &tile : tiles)
    {
        if (tile.texture_id != 0)
            glDeleteTextures(1, &tile.texture_id);
        if (tile.data != nullptr)
            stbi_image_free(tile.data);
    }
    tiles.clear();
}

void glLoad(int index)
{
    glGenTextures(1, &tiles[index].texture_id);
    glBindTexture(GL_TEXTURE_2D, tiles[index].texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 tiles[index].width,
                 tiles[index].height,
                 0, GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 tiles[index].data);
    tiles[index].loaded = true;
}

void stbLoad(int index)
{
    tiles[index].data = stbi_load_from_memory(
        tiles[index].blob.data(),
        tiles[index].blob.size(),
        &tiles[index].width,
        &tiles[index].height,
        &tiles[index].channels,
        STBI_rgb_alpha);
}

int findTile(int z, int x, int y)
{
    for (size_t i = 0; i < tiles.size(); i++)
    {
        if (tiles[i].z == z && tiles[i].x == x && tiles[i].y == y)
            return i;
    }
    return -1;
}

std::string makeUrl(int z, int x, int y)
{
    std::ostringstream urlmaker;
    urlmaker << "https://a.tile.openstreetmap.org";
    urlmaker << '/' << z << '/' << x << '/' << y << ".png";
    return urlmaker.str();
}

size_t onPullResponse(void *data, size_t size, size_t nmemb,
                      void *userp)
{
    size_t realsize{size * nmemb};
    auto &blob{*static_cast<std::vector<unsigned char> *>(userp)};
    auto const *const dataptr{static_cast<unsigned char *>(data)};
    blob.insert(blob.cend(), dataptr, dataptr + realsize);
    std::cout << "tile size = " << realsize << std::endl;
    return realsize;
}

bool receiveTile(int z, int x, int y,
                 std::vector<unsigned char> &blob)
{
    CURL *curl{curl_easy_init()};
    curl_easy_setopt(curl, CURLOPT_URL, makeUrl(z, x, y).c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&blob);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onPullResponse);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);

    if (res == CURLE_OK && http_code == 200 && !blob.empty())
    {
        return true;
    }

    std::cout << "tile failed: " << z << "/" << x << "/" << y << " HTTP: " << http_code << std::endl;
    blob.clear();
    return false;
}

std::vector<unsigned char> tileRequest(int z, int x, int y)
{
    if (x < 0 || x >= POW2[z] || y < 0 || y >= POW2[z])
    {
        std::cout << "invalid tile coordinates: " << z << "/" << x << "/" << y << std::endl;
        return std::vector<unsigned char>();
    }

    int idx = findTile(z, x, y);
    if (idx != -1)
    {
        if (tiles[idx].loading)
        {
            return std::vector<unsigned char>();
        }
        if (tiles[idx].loaded)
        {
            return std::vector<unsigned char>();
        }
    }

    if (idx == -1)
    {
        TileInfo new_tile;
        new_tile.z = z;
        new_tile.x = x;
        new_tile.y = y;
        new_tile.loaded = false;
        new_tile.loading = true;
        new_tile.texture_id = 0;
        new_tile.data = nullptr;
        tiles.push_back(new_tile);
        idx = tiles.size() - 1;
    }
    else
    {
        tiles[idx].loading = true;
    }

    std::vector<unsigned char> blob;
    if (receiveTile(z, x, y, blob) && !blob.empty())
    {
        std::cout << "tile is received: " << z << "/" << x << "/" << y << " size: " << blob.size() << std::endl;
        tiles[idx].blob = blob;
        tiles[idx].loading = false;
        return blob;
    }
    else
    {
        std::cout << "tile is not received: " << z << "/" << x << "/" << y << std::endl;
        tiles[idx].loading = false;
        tiles.erase(tiles.begin() + idx);
        return std::vector<unsigned char>();
    }
}

std::mutex mtx;
std::vector<std::string> log_messages;

// серверные переменные для будущих фильтров
bool running = true; // это вообще не трогаем
bool start_server = true;
bool get_location = true;
bool get_network = true;

// переменные по приколу
int session_data_counter = 0;

void flush_to_disk()
{
    if (data_store.pending_records.empty())
        return;
    std::ofstream file("log.json", std::ios::app);
    if (file.is_open())
    {
        for (const auto &r : data_store.pending_records)
            file << r << "\n";
        file.close();
    }
    data_store.pending_records.clear();
}

bool save_data_to_db(const char *req_data[], PGconn *con)
{
    std::string query = "INSERT INTO network_logs "
                        "(latitude, longitude, altitude, accuracy, network_type, rsrp) "
                        "VALUES ($1, $2, $3, $4, $5, $6)";
    PGresult *insert_res = PQexecParams(
        con,
        query.c_str(),
        6,
        NULL,
        req_data,
        NULL,
        NULL,
        0);

    if (PQresultStatus(insert_res) != PGRES_COMMAND_OK)
    {
        std::cerr << "\033[31mОШИБКА DB\033[0m: " << PQresultErrorMessage(insert_res) << std::endl;
        PQclear(insert_res);
        return false;
    }
    std::cout << "Данные вставлены \033[32mУСПЕШНО!\033[0m" << std::endl;
    PQclear(insert_res);
    return true;
}

// Сервер
void run_server(PGconn *db_con)
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::rcvtimeo, 1000);

    try
    {
        socket.bind("tcp://*:7777");
        std::cout << "[ZMQ Server] Started on port 7777. Waiting for Android..." << std::endl;
    }
    catch (const zmq::error_t &e)
    {
        std::cerr << "[ZMQ Error] Bind failed: " << e.what() << std::endl;
        return;
    }

    while (running)
    {
        if (!start_server)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        zmq::message_t request;
        zmq::recv_result_t result = socket.recv(request, zmq::recv_flags::none);

        if (result)
        {
            std::string msg_str(static_cast<char *>(request.data()), request.size());
            std::cout << "[ZMQ] Received packet (" << msg_str.length() << " bytes)" << std::endl;

            try
            {
                auto j = json::parse(msg_str);
                std::lock_guard<std::mutex> lock(mtx);
                data_store.lat = std::to_string(j.value("lat", 0.0));
                data_store.lon = std::to_string(j.value("lon", 0.0));
                data_store.alt = std::to_string(j.value("alt", 0.0));
                data_store.acc = std::to_string(j.value("acc", 0.0));
                {
                    if (j.contains("cell_data") && j["cell_data"].contains("cells"))
                    {
                        auto &cells = j["cell_data"]["cells"];
                        data_store.history.add_points(cells);
                        for (auto &cell : cells)
                        {
                            if (cell.value("registered", false))
                            {
                                data_store.type = cell.value("type", "N/A");

                                float rsrp_val = -145.0f;
                                if (data_store.type == "LTE")
                                    rsrp_val = cell["signal"].value("rsrp", -145.0f);
                                else if (data_store.type == "NR")
                                    rsrp_val = cell["signal"].value("ssRsrp", -145.0f);
                                else if (data_store.type == "GSM")
                                    rsrp_val = cell["signal"].value("dbm", -145.0f);

                                data_store.current_rsrp = rsrp_val;
                                std::string s_rsrp = std::to_string(rsrp_val);
                                const char *params[6] = {
                                    data_store.lat.c_str(),
                                    data_store.lon.c_str(),
                                    data_store.alt.c_str(),
                                    data_store.acc.c_str(),
                                    data_store.type.c_str(),
                                    s_rsrp.c_str()};
                                save_data_to_db(params, db_con);

                                log_messages.push_back("Recv: " + data_store.type + " | RSRP: " + s_rsrp);
                                break;
                            }
                        }
                    }
                    data_store.pending_records.push_back(j.dump());
                    if (log_messages.size() > 50)
                        log_messages.erase(log_messages.begin());
                }
                if (data_store.pending_records.size() >= 10)
                {
                    flush_to_disk();
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[Data Error] Failed to process JSON: " << e.what() << std::endl;
            }
            socket.send(zmq::buffer(std::string("OK")), zmq::send_flags::none);
            session_data_counter++;
        }
    }
    std::lock_guard<std::mutex> lock(mtx);
    flush_to_disk();
    std::cout << "[ZMQ Server] Thread stopped." << std::endl;
}

void ColoredIndicator(const char *label, bool condition, const char *true_text = "ON", const char *false_text = "OFF")
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

void load_log_file()
{
    std::lock_guard<std::mutex> lock(mtx);
    offline_store.clear();

    std::ifstream file("log.json");
    if (!file.is_open())
    {
        std::cerr << "[Error] Could not open log.json for reading." << std::endl;
        return;
    }

    std::string line;
    int line_count = 0;
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;

        try
        {
            auto j = json::parse(line);

            offline_store.lats.push_back(j.at("lat").get<double>());
            offline_store.lons.push_back(j.at("lon").get<double>());
            offline_store.times.push_back(j.at("time").get<double>());
            offline_store.indices.push_back(line_count);
            if (j.contains("cell_data") &&
                j["cell_data"].contains("signal") &&
                j["cell_data"]["signal"].contains("rsrp"))
            {
                double rsrp = j["cell_data"]["signal"]["rsrp"].get<double>();
                offline_store.rsrps.push_back(rsrp);
            }
            else
            {
                offline_store.rsrps.push_back(-145.0);
            }
            line_count++;
        }
        catch (const json::parse_error &e)
        {
            std::cerr << "[JSON Error] Line " << line_count << ": " << e.what() << std::endl;
        }
        catch (const std::out_of_range &e)
        {
            std::cerr << "[Data Error] Required keys not found at line " << line_count << std::endl;
        }
    }

    if (line_count > 0)
    {
        offline_store.loaded = true;
        std::cout << "[Info] Loaded " << line_count << " records from log.json" << std::endl;
    }
    file.close();
}

void loadTilesForCurrentView(ImPlotRect &plot_lims, ImVec2 plot_size)
{
    const int TILE_SIZE_PX = 256;
    int tiles_x_needed = ceil(plot_size.x / TILE_SIZE_PX) + 2;
    int tiles_y_needed = ceil(plot_size.y / TILE_SIZE_PX) + 2;
    double center_lon = (plot_lims.X.Min + plot_lims.X.Max) / 2.0;
    double center_lat = (plot_lims.Y.Min + plot_lims.Y.Max) / 2.0;

    int center_tile_x = (int)lon2x(center_lon, map_state.current_zoom);
    int center_tile_y = (int)lat2y(center_lat, map_state.current_zoom);

    int min_x = center_tile_x - tiles_x_needed / 2;
    int max_x = center_tile_x + tiles_x_needed / 2;
    int min_y = center_tile_y - tiles_y_needed / 2;
    int max_y = center_tile_y + tiles_y_needed / 2;

    if (min_x < 0)
        min_x = 0;
    if (max_x > POW2[map_state.current_zoom])
        max_x = POW2[map_state.current_zoom];
    if (min_y < 0)
        min_y = 0;
    if (max_y > POW2[map_state.current_zoom])
        max_y = POW2[map_state.current_zoom];

    std::cout << "Window size: " << plot_size.x << "x" << plot_size.y << " px" << std::endl;
    std::cout << "Tiles needed: " << tiles_x_needed << "x" << tiles_y_needed << std::endl;
    std::cout << "Tile range X: " << min_x << " - " << max_x << std::endl;
    std::cout << "Tile range Y: " << min_y << " - " << max_y << std::endl;

    int loaded = 0;
    for (int y = min_y; y <= max_y; y++)
    {
        for (int x = min_x; x <= max_x; x++)
        {
            int idx = findTile(map_state.current_zoom, x, y);
            if (idx == -1)
            {
                std::vector<unsigned char> blob = tileRequest(map_state.current_zoom, x, y);
                if (!blob.empty())
                {
                    int current_idx = findTile(map_state.current_zoom, x, y);
                    if (current_idx != -1)
                    {
                        stbLoad(current_idx);
                        glLoad(current_idx);
                        loaded++;
                    }
                }
            }
        }
    }
    std::cout << "Loaded " << loaded << " new tiles" << std::endl;
}

// GUI
void run_gui()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        return;
    SDL_Window *window = SDL_CreateWindow("Network Analyzer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 700, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Включить Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Включить Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Окно последних полученных данных
        ImGui::Begin("Current stats");
        ImGui::Text("Network type: %s", data_store.type.c_str());
        ImGui::Text("RSRP: %.1f dBm", data_store.current_rsrp);
        ImGui::Separator();
        ImGui::Text("Lat: %s", data_store.lat.c_str());
        ImGui::Text("Lon: %s", data_store.lon.c_str());
        ImGui::Text("Alt: %s", data_store.alt.c_str());
        ImGui::Text("Acc: %s", data_store.acc.c_str());
        ImGui::Separator();
        ImGui::Text("Data counter\n in this session: %d", session_data_counter);
        ImGui::End();

        // Окно checkboxov
        ImGui::Begin("Filters");
        ImGui::Checkbox("Is running?", &start_server);
        ImGui::Checkbox("Is location?", &get_location);
        ImGui::Checkbox("Is network?", &get_network);
        ImGui::End();

        // Окно ststus
        ImGui::Begin("Status");
        ColoredIndicator("Server", start_server, "RUNNING", "STOPPED");
        ColoredIndicator("Location", get_location);
        ColoredIndicator("Network", get_network);
        ImGui::End();

        // Окно графика realtime
        ImGui::Begin("Signal Graph");
        if (ImPlot::BeginPlot("History", ImVec2(-1, -1)))
        {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_X1, data_store.history.current_step - 100, data_store.history.current_step, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);

            std::lock_guard<std::mutex> lock(mtx);
            for (auto &[pci, y_vec] : data_store.history.streams_y)
            {
                std::string label = "PCI: " + std::to_string(pci);
                ImPlot::PlotLine(label.c_str(), data_store.history.x.data(), y_vec.data(), (int)y_vec.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        // загрузка истории из файла на графикеи
        ImGui::Begin("Log Control");
        if (ImGui::Button("Load log.json", ImVec2(-1, 40)))
        {
            load_log_file();
        }
        if (offline_store.loaded)
        {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Total: %d records", (int)offline_store.lats.size());
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Records not loaded");
        }
        ImGui::End();

        // история из файла
        ImGui::Begin("Full Signal History");
        if (offline_store.loaded && !offline_store.rsrps.empty())
        {
            if (ImPlot::BeginPlot("RSRP History", ImVec2(-1, -1)))
            {
                ImPlot::SetupAxes("Record Index", "dBm");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, offline_store.indices.size(), ImGuiCond_Always);
                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 1), 1.5f);
                ImPlot::PlotLine("RSRP",
                                 offline_store.indices.data(),
                                 offline_store.rsrps.data(),
                                 (int)offline_store.rsrps.size());

                ImPlot::EndPlot();
            }
        }
        else
        {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Begin("Map");
        if (ImPlot::BeginPlot("##ImOsmMapPlot", ImVec2(-1, -1)))
        {
            ImVec2 _uv0{0, 0}, _uv1{1, 1};
            ImVec4 _tint{1, 1, 1, 1};

            ImPlot::SetupAxes("Lon", "Lat");

            static bool initial_zoom_set = false;
            if (!initial_zoom_set)
            {
                ImPlot::SetupAxisLimits(ImAxis_X1, 82.92, 82.93, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 55.02, 55.03, ImGuiCond_Once);
                initial_zoom_set = true;
            }

            ImVec2 mouse_pos = ImGui::GetMousePos();
            ImVec2 plot_pos = ImPlot::GetPlotPos();
            ImVec2 plot_size = ImPlot::GetPlotSize();

            if (mouse_pos.x >= plot_pos.x && mouse_pos.x <= plot_pos.x + plot_size.x &&
                mouse_pos.y >= plot_pos.y && mouse_pos.y <= plot_pos.y + plot_size.y)
            {
                float mouse_wheel = ImGui::GetIO().MouseWheel;
                if (mouse_wheel != 0)
                {
                    int new_zoom = map_state.current_zoom + (mouse_wheel > 0 ? 1 : -1);
                    if (new_zoom >= MinZoom && new_zoom <= MaxZoom && new_zoom != map_state.current_zoom)
                    {
                        map_state.current_zoom = new_zoom;
                        map_state.need_reload = true;
                        clearTiles();
                    }
                }
            }

            ImPlotRect plot_lims = ImPlot::GetPlotLimits();
            ImVec2 plot_size_px = ImPlot::GetPlotSize();
            if (tiles.empty() || map_state.need_reload)
            {
                loadTilesForCurrentView(plot_lims, plot_size_px);
                map_state.need_reload = false;
            }

            for (size_t i = 0; i < tiles.size(); i++)
            {
                if (tiles[i].loaded && tiles[i].z == map_state.current_zoom)
                {
                    double tile_min_lon = x2lon(tiles[i].x, tiles[i].z);
                    double tile_max_lon = x2lon(tiles[i].x + 1, tiles[i].z);
                    double tile_max_lat = y2lat(tiles[i].y, tiles[i].z);
                    double tile_min_lat = y2lat(tiles[i].y + 1, tiles[i].z);

                    ImVec2 tile_bmin(tile_min_lon, tile_min_lat);
                    ImVec2 tile_bmax(tile_max_lon, tile_max_lat);

                    ImPlot::PlotImage(("##tile_" + std::to_string(i)).c_str(),
                                      tiles[i].texture_id,
                                      tile_bmin,
                                      tile_bmax,
                                      _uv0,
                                      _uv1,
                                      _tint);
                }
            }

            ImPlot::EndPlot();
        }
        ImGui::End();

        // гео график по истории
        ImGui::Begin("Movement Route");
        if (offline_store.loaded && !offline_store.lats.empty())
        {
            if (ImPlot::BeginPlot("Map", ImVec2(-1, -1)))
            {
                // Lon - по X, Lat - по Y
                ImPlot::SetupAxes("Longitude", "Latitude");
                ImPlot::SetupAxisLimits(ImAxis_X1, 82.915, 82.925, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 55.025, 55.035, ImGuiCond_Once);

                // Рисуем линию перемещения
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
                ImPlot::PlotScatter("route",
                                    offline_store.lons.data(), // X
                                    offline_store.lats.data(), // Y
                                    (int)offline_store.lats.size());
                ImPlot::EndPlot();
            }
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

int main()
{
    PGconn *con;   // обьект подключения
    PGresult *res; // результат запроса к базе
    const char *info = "host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD;
    con = PQconnectdb(info); // Выполняем SQL-запрос из переменной info
    if (PQstatus(con) != CONNECTION_OK)
    { // если подключение не удалось пишем ошибку
        std::cerr << "\033[31mОШИБКА\033[0m подключения к БД.\n"
                  << PQerrorMessage(con) << "\n";
        PQfinish(con); // рвём подключение перед выходом
        exit(1);
    }
    else
    {
        std::cout << "Подключение \033[32mУСПЕШНО!\033[0m\n\n"
                  << std::endl;
    }
    std::thread server(run_server, con);
    run_gui();
    server.join();
    return 0;
}