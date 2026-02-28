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
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

struct TelemetryData {
    std::string lat = "-";
    std::string lon = "-";
    std::string alt = "-";
    std::string acc = "-";
    std::string mobile_data = "-";
} current_telemetry;

std::mutex mtx;
std::vector<std::string> log_messages;
bool running = true;

std::string find_value(const std::string& msg, const std::string& key) {
    std::string search_str = "\"" + key + "\":";
    size_t start_pos = msg.find(search_str);
    if (start_pos == std::string::npos) return "";
    
    start_pos += search_str.length();
    size_t val_start = msg.find_first_not_of(" \"", start_pos);
    size_t val_end = msg.find_first_of("\",}", val_start);
    
    if (val_start != std::string::npos && val_end != std::string::npos) {
        return msg.substr(val_start, val_end - val_start);
    }
    return "";
}

void run_server() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::rcvtimeo, 500); 

    try {
        socket.bind("tcp://*:7777");
        std::cout << "[Server] Started on port 7777" << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Server] Bind error: " << e.what() << std::endl;
        return;
    }

    std::ofstream file("data.json", std::ios::app);

    while (running) {
        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);

        if (res) {
            std::string message(static_cast<char *>(request.data()), request.size());
            file << message << "\n";
            file.flush();
            
            {
                std::lock_guard<std::mutex> lock(mtx);
                std::string t_lat = find_value(message, "lat");
                std::string t_lon = find_value(message, "lon");
                std::string t_alt = find_value(message, "alt");
                std::string t_acc = find_value(message, "acc");
                std::string net_type = find_value(message, "type");

                if (!t_lat.empty()) current_telemetry.lat = t_lat;
                if (!t_lon.empty()) current_telemetry.lon = t_lon;
                if (!t_alt.empty()) current_telemetry.alt = t_alt;
                if (!t_acc.empty()) current_telemetry.acc = t_acc;
                if (!net_type.empty()) current_telemetry.mobile_data = net_type;

                log_messages.push_back(message);
                if(log_messages.size() > 500) log_messages.erase(log_messages.begin());
            }

            std::string reply_str = "OK";
            socket.send(zmq::buffer(reply_str), zmq::send_flags::none);
        }
    }
}

void run_gui() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;
    SDL_Window* window = SDL_CreateWindow(
        "ZMQ Log Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1100, 700, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);
    glewInit();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

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
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        ImGui::SetNextWindowPos(ImVec2(10, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);
        ImGui::Begin("Current Telemetry");
            ImGui::Columns(2, "telemetry_cols", true);
            ImGui::Text("Latitude:"); ImGui::NextColumn(); ImGui::TextColored(ImVec4(1,1,0,1), "%s", current_telemetry.lat.c_str()); ImGui::NextColumn();
            ImGui::Text("Longitude:"); ImGui::NextColumn(); ImGui::TextColored(ImVec4(1,1,0,1), "%s", current_telemetry.lon.c_str()); ImGui::NextColumn();
            ImGui::Text("Altitude:"); ImGui::NextColumn(); ImGui::Text("%s", current_telemetry.alt.c_str()); ImGui::NextColumn();
            ImGui::Text("Accuracy:"); ImGui::NextColumn(); ImGui::Text("%s", current_telemetry.acc.c_str()); ImGui::NextColumn();
            ImGui::Separator();
            ImGui::Text("Network Type:"); ImGui::NextColumn(); ImGui::TextColored(ImVec4(0,1,1,1), "%s", current_telemetry.mobile_data.c_str());
            ImGui::Columns(1);
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(340, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(740, 660), ImGuiCond_FirstUseEver);
        ImGui::Begin("ZMQ Server Log");
            if (ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
                std::lock_guard<std::mutex> lock(mtx);
                ImGui::PushTextWrapPos(0.0f); 
                for (const auto& msg : log_messages) {
                    ImGui::TextUnformatted(msg.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    std::thread server_thread(run_server);
    run_gui();
    if (server_thread.joinable()) server_thread.join();
    return 0;
}