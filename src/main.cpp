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
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <GL/glew.h>
#include <SDL2/SDL.h>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include <libpq-fe.h>
#include <queue>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>


using namespace std;
namespace fs = filesystem;

#define HOST "localhost"
#define PORT "5433"
#define DB_NAME "android_backend"
#define DB_USER "postgres" // по умолчанию postgres
#define DB_USER_PASSWORD "somepassword"

using json = nlohmann::json;
int zoom = 1;
// математические приколы для преобразования долготы и широты в координатную плоскость
double MercatorXToTileX(double mercatorX, int zoom)
{
    return (0.5 + mercatorX / 360.0) * (1 << zoom);
}

double MercatorYToTileY(double mercatorY, int zoom)
{
    return (0.5 - mercatorY / 360.0) * (1 << zoom);
}

// обратный MercatorXToTileX
double TileXToMercatorX(int tileX, int zoom)
{
    return (tileX / static_cast<double>(1 << zoom) - 0.5) * 360.0;
}

// обратная функция MercatorYToTileY
double TileYToMercatorY(int tileY, int zoom)
{
    return (0.5 - tileY / static_cast<double>(1 << zoom)) * 360.0;
}

double LatToMercatorY(double lat)
{
    lat = max(-85.0511, min(85.0511, lat));
    double lat_rad = lat * M_PI / 180.0;
    double mercator_y = log(tan(M_PI / 4 + lat_rad / 2));
    return mercator_y * 180.0 / M_PI;
}

double MercatorYToLat(double mercator_y)
{
    double mercator_y_rad = mercator_y * M_PI / 180.0;
    double lat_rad = atan(sinh(mercator_y_rad));
    return lat_rad * 180.0 / M_PI;
}

struct TileJob
{
    string id; // строковый идентификатор "zoom/x/y"
    int zoom;
    int x;
    int y;
};

struct TextureData
{
    GLuint id = 0;                 // id текстуры в GPU
    bool isLoading = false;        // идет загрузка или нет
    bool isLoaded = false;         // полностью загружена
    vector<uint8_t> rgbaBlob; // пиксели в ОЗУ (до переноса в GPU)
    int width = 0;
    int height = 0;
};

map<string, TextureData> g_TileCache; // кэш текстур, мапа для сопоставления тайла и его индентификатора
queue<TileJob> g_JobQueue;                 // очередь на загрузку

// мьютексы
mutex g_JobMutex;
mutex g_CacheMutex;

struct SignalHistory // используется для отображения данных о вышках
{
    map<int, vector<float>> streams_y;
    vector<float> x;
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
        vector<int> present_pcis;
        for (auto &cell : cells_array)
        {
            int pci = cell["identity"].value("pci", -1);
            if (pci == -1 || pci == 2147483647)
                continue;
            present_pcis.push_back(pci);
            if (streams_y.find(pci) == streams_y.end())
            {
                streams_y[pci] = vector<float>(x.size() - 1, -145.0f);
            }
            float rsrp = -145.0f;
            string type = cell.value("type", "");
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
            if (find(present_pcis.begin(), present_pcis.end(), pci) == present_pcis.end())
            {
                vec.push_back(-145.0f);
            }
        }
    }
};

struct OfflineData // для отображения данных ищ файла
{
    vector<double> lats;
    vector<double> lons;
    vector<double> rsrps;
    vector<double> times;
    vector<double> indices;

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

struct Telemetry // основная структура данных
{
    string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
    vector<string> pending_records;
} data_store;

mutex mtx;
vector<string> log_messages;

// серверные переменные для будущих фильтров
bool running = true; // это вообще не трогаем
bool start_server = true;
bool get_location = true;
bool get_network = true;

// переменные по приколу
int session_data_counter = 0;

void flush_to_disk() // запись данных на диск
{
    if (data_store.pending_records.empty())
        return;
    ofstream file("log.json", ios::app);
    if (file.is_open())
    {
        for (const auto &r : data_store.pending_records)
            file << r << "\n";
        file.close();
    }
    data_store.pending_records.clear();
}

string GetTilePath(int zoom, int x, int y)
{
    stringstream strin;
    strin << "tile_cache/" << zoom << "/" << x;
    fs::create_directories(strin.str());

    strin << "/" << y << ".png";
    return strin.str();
}

bool LoadTileFromDisk(int zoom, int x, int y, vector<uint8_t> &out_data)
{
    string path = GetTilePath(zoom, x, y);

    ifstream file(path, ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    file.seekg(0, ios::end);
    size_t size = file.tellg();
    file.seekg(0, ios::beg);

    out_data.resize(size);
    file.read(reinterpret_cast<char *>(out_data.data()), size);
    return file.good();
}

// кидаем тайл на дискк
void SaveTileToDisk(int zoom, int x, int y, const vector<uint8_t> &data)
{
    string path = GetTilePath(zoom, x, y);

    ofstream file(path, ios::binary);
    if (file.is_open())
    {
        file.write(reinterpret_cast<const char *>(data.data()), data.size());
        file.close();
    }
}

// callback из примера
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    auto &buffer = *static_cast<vector<uint8_t> *>(userp);
    const auto *dataptr = static_cast<uint8_t *>(contents);
    buffer.insert(buffer.end(), dataptr, dataptr + realsize);
    return realsize;
}

void FetchWorker()
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);

    while (true)
    {
        TileJob job;
        bool foundJob = false;

        g_JobMutex.lock();
        if (!g_JobQueue.empty())
        {
            job = g_JobQueue.front();
            g_JobQueue.pop();
            foundJob = true;
        }
        g_JobMutex.unlock();

        if (!foundJob)
        {
            SDL_Delay(10);
            continue;
        }

        vector<uint8_t> rawBlob;
        bool loaded = false;

        // с диска
        if (LoadTileFromDisk(job.zoom, job.x, job.y, rawBlob))
        {
            cout << "[Cache] Loaded from disk: " << job.id << endl;
            loaded = true;
        }
        else
        {
            // интернет
            string url = "https://a.tile.openstreetmap.org/" + 
                             to_string(job.zoom) + "/" + 
                             to_string(job.x) + "/" + 
                             to_string(job.y) + ".png";

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rawBlob);

            if (curl_easy_perform(curl) == CURLE_OK)
            {
                cout << "[Download] Loaded from web: " << job.id << endl;
                SaveTileToDisk(job.zoom, job.x, job.y, rawBlob);
                loaded = true;
            }
            else
            {
                cerr << "[Error] Failed to load tile: " << job.id << endl;
            }
        }
        g_CacheMutex.lock();
        if (loaded && !rawBlob.empty())
        {
            int w, h, ch;
            unsigned char *data = stbi_load_from_memory(rawBlob.data(), (int)rawBlob.size(),
                                                        &w, &h, &ch, STBI_rgb_alpha);
            if (data)
            {
                g_TileCache[job.id].rgbaBlob.assign(data, data + (w * h * 4));
                g_TileCache[job.id].width = w;
                g_TileCache[job.id].height = h;
                g_TileCache[job.id].isLoading = false;
                g_TileCache[job.id].isLoaded = true;
                stbi_image_free(data);
            }
            else
            {
                g_TileCache[job.id].isLoading = false;
            }
        }
        else
        {
            g_TileCache[job.id].isLoading = false;
        }
        g_CacheMutex.unlock();
    }

    curl_easy_cleanup(curl);
}

bool save_data_to_db(const char *req_data[], PGconn *con) // сохранение данных в бд
{
    string query = "INSERT INTO network_logs "
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
        cerr << "\033[31mОШИБКА DB\033[0m: " << PQresultErrorMessage(insert_res) << endl;
        PQclear(insert_res);
        return false;
    }
    cout << "Данные вставлены \033[32mУСПЕШНО!\033[0m" << endl;
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
        cout << "[ZMQ Server] Started on port 7777. Waiting for Android..." << endl;
    }
    catch (const zmq::error_t &e)
    {
        cerr << "[ZMQ Error] Bind failed: " << e.what() << endl;
        return;
    }

    while (running)
    {
        if (!start_server)
        {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        zmq::message_t request;
        zmq::recv_result_t result = socket.recv(request, zmq::recv_flags::none);

        if (result)
        {
            string msg_str(static_cast<char *>(request.data()), request.size());
            cout << "[ZMQ] Received packet (" << msg_str.length() << " bytes)" << endl;

            try
            {
                auto j = json::parse(msg_str);
                lock_guard<mutex> lock(mtx);
                data_store.lat = to_string(j.value("lat", 0.0));
                data_store.lon = to_string(j.value("lon", 0.0));
                data_store.alt = to_string(j.value("alt", 0.0));
                data_store.acc = to_string(j.value("acc", 0.0));
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
                                string s_rsrp = to_string(rsrp_val);
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
            catch (const exception &e)
            {
                cerr << "[Data Error] Failed to process JSON: " << e.what() << endl;
            }
            socket.send(zmq::buffer(string("OK")), zmq::send_flags::none);
            session_data_counter++;
        }
    }
    lock_guard<mutex> lock(mtx);
    flush_to_disk();
    cout << "[ZMQ Server] Thread stopped." << endl;
}

void ColoredIndicator(const char *label, bool condition, const char *true_text = "ON", const char *false_text = "OFF") // просто прикольная штучка
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

void load_log_file() // загрузка данных из файла json
{
    lock_guard<mutex> lock(mtx);
    offline_store.clear();

    ifstream file("log.json");
    if (!file.is_open())
    {
        cerr << "[Error] Could not open log.json for reading." << endl;
        return;
    }

    string line;
    int line_count = 0;
    while (getline(file, line))
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
            cerr << "[JSON Error] Line " << line_count << ": " << e.what() << endl;
        }
        catch (const out_of_range &e)
        {
            cerr << "[Data Error] Required keys not found at line " << line_count << endl;
        }
    }

    if (line_count > 0)
    {
        offline_store.loaded = true;
        cout << "[Info] Loaded " << line_count << " records from log.json" << endl;
    }
    file.close();
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

            lock_guard<mutex> lock(mtx);
            for (auto &[pci, y_vec] : data_store.history.streams_y)
            {
                string label = "PCI: " + to_string(pci);
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

        // гео график по истории
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
                double diff = mercator_right - mercator_left;
                if (diff > 0 && diff < 360.0)
                {
                    zoom = (int)floor(log2(360.0 / diff)) + 2;
                    zoom = max(0, min(zoom, 19));
                }
                int minX = static_cast<int>(floor(MercatorXToTileX(mercator_left, zoom)));
                int minY = static_cast<int>(floor(MercatorYToTileY(mercator_top, zoom)));
                int maxX = static_cast<int>(floor(MercatorXToTileX(mercator_right, zoom)));
                int maxY = static_cast<int>(floor(MercatorYToTileY(mercator_bottom, zoom)));

                int maxTileCount = (1 << zoom) - 1;
                minX = max(0, min(minX, maxTileCount));
                maxX = max(0, min(maxX, maxTileCount));
                minY = max(0, min(minY, maxTileCount));
                maxY = max(0, min(maxY, maxTileCount));

                // запрашиваем тайлы
                for (int x = minX; x <= maxX; ++x)
                {
                    for (int y = minY; y <= maxY; ++y)
                    {
                        string tileId = to_string(zoom) + "/" + to_string(x) + "/" + to_string(y);

                        bool needLoad = false;
                        {
                            lock_guard<mutex> lock(g_CacheMutex);
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
                            lock_guard<mutex> lock(g_JobMutex);
                            TileJob job;
                            job.id = tileId;
                            job.zoom = zoom;
                            job.x = x;
                            job.y = y;
                            g_JobQueue.push(job);
                        }
                    }
                }

                // отрисовка тайлов
                for (int x = minX; x <= maxX; ++x)
                {
                    for (int y = minY; y <= maxY; ++y)
                    {
                        string tileId = to_string(zoom) + "/" + to_string(x) + "/" + to_string(y);

                        GLuint gpuId = 0;
                        {
                            lock_guard<mutex> lock(g_CacheMutex);
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

                            ImPlot::PlotImage(("##tile_" + tileId).c_str(), (ImTextureID)(intptr_t)gpuId, minPoint, maxPoint);
                        }
                    }
                }

                // отрисовка маршрута
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
                ImPlot::PlotScatter("route",
                                    offline_store.lons.data(),
                                    offline_store.lats.data(),
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
        cerr << "\033[31mОШИБКА\033[0m подключения к БД.\n"
                  << PQerrorMessage(con) << "\n";
        PQfinish(con); // рвём подключение перед выходом
        exit(1);
    }
    else
    {
        cout << "Подключение \033[32mУСПЕШНО!\033[0m\n\n"
                  << endl;
    }
    thread(FetchWorker).detach();
    thread server(run_server, con);
    run_gui();
    server.join();
    return 0;
}