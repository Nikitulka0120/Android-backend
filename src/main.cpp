#include <zmq.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <fstream>
#include <mutex>
#include <vector>

#include <GL/glew.h>
#include <SDL2/SDL.h>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

struct SignalHistory
{
    std::vector<float> x;
    std::vector<float> y;
    float current_step = 0;
    const int max_points = 200;

    void add_point(float value)
    {
        if (x.size() >= max_points)
        {
            x.erase(x.begin());
            y.erase(y.begin());
        }
        x.push_back(current_step++);
        y.push_back(value);
    }
};

struct Telemetry
{
    std::string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
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


std::string get_json_value(const std::string &json, const std::string &key)
{
    std::string search_key = "\"" + key + "\"";
    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos)
        return "";

    size_t colon_pos = json.find(":", key_pos);
    if (colon_pos == std::string::npos)
        return "";

    size_t start = json.find_first_not_of(" \"", colon_pos + 1);
    size_t end = json.find_first_of("\",}]", start);

    if (start != std::string::npos && end != std::string::npos)
    {
        return json.substr(start, end - start);
    }
    return "";
}

// Сервер
void run_server()
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::rcvtimeo, 1000);

    try
    {
        socket.bind("tcp://*:7777");
        std::cout << "[Server] Listening on 7777..." << std::endl;
    }
    catch (const zmq::error_t &e)
    {
        return;
    }

    while (start_server)
    {
        zmq::message_t request;
        if (socket.recv(request, zmq::recv_flags::none))
        {
            std::string msg(static_cast<char *>(request.data()), request.size());

            {
                std::lock_guard<std::mutex> lock(mtx);

                data_store.lat = get_json_value(msg, "lat");
                data_store.lon = get_json_value(msg, "lon");
                data_store.alt = get_json_value(msg, "alt");
                data_store.acc = get_json_value(msg, "acc");
                data_store.type = get_json_value(msg, "type");

                std::string s_val = "";
                if (data_store.type == "LTE")
                    s_val = get_json_value(msg, "rsrp");
                else if (data_store.type == "NR")
                    s_val = get_json_value(msg, "ssRsrp");
                else if (data_store.type == "GSM")
                    s_val = get_json_value(msg, "dbm");

                if (!s_val.empty())
                {
                    try
                    {
                        data_store.current_rsrp = std::stof(s_val);
                        data_store.history.add_point(data_store.current_rsrp);
                    }
                    catch (...)
                    {
                    }
                }

                log_messages.push_back("[" + data_store.type + "] RSRP: " + s_val);
                if (log_messages.size() > 50)
                    log_messages.erase(log_messages.begin());
            }
            session_data_counter++;
            socket.send(zmq::buffer(std::string("OK")), zmq::send_flags::none);
        }
    }
    session_data_counter = 0;
}

void ColoredIndicator(const char* label, bool condition, const char* true_text = "ON", const char* false_text = "OFF") {
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

//GUI 
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

        // Окно графика
        ImGui::Begin("Signal Graph");
        if (ImPlot::BeginPlot("History", ImVec2(-1, -1)))
        {
            ImPlot::SetupAxes("Ticks", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_X1, data_store.history.current_step - 100, data_store.history.current_step, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);

            std::lock_guard<std::mutex> lock(mtx);
            if (!data_store.history.x.empty())
            {
                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 1), 2.0f);
                ImPlot::PlotLine("RSRP", data_store.history.x.data(), data_store.history.y.data(), (int)data_store.history.x.size());
            }
            ImPlot::EndPlot();
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
    std::thread server(run_server);
    run_gui();
    server.join();
    return 0;
}