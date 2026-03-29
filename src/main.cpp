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

#include <GL/glew.h>
#include <SDL2/SDL.h>

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

        // гео график по истории
        ImGui::Begin("Movement Route");
        if (offline_store.loaded && !offline_store.lats.empty())
        {
            if (ImPlot::BeginPlot("Map", ImVec2(-1, -1)))
            {
                // Lon - по X, Lat - по Y
                ImPlot::SetupAxes("Longitude", "Latitude");
                ImPlot::SetupAxisLimits(ImAxis_X1, 82.900, 82.960, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 55.000, 55.040, ImGuiCond_Once);

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