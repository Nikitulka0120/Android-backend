#include "server.h"
#include "data_structures.h"
#include "database.h"
#include <zmq.hpp>
#include <thread>
#include <mutex>
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <algorithm>

bool running = true;
bool start_server = true;
bool get_location = true;
bool get_network = true;

void FlushToDisk()
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

void RunServer(PGconn *db_con)
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
                            float rsrq_val = -30.0f;
                            float rssi_val = -120.0f;
                            if (data_store.type == "LTE")
                            {
                                rsrp_val = cell["signal"].value("rsrp", -145.0f);
                                rsrq_val = cell["signal"].value("rsrq", -30.0f);
                                rssi_val = cell["signal"].value("rssi", -120.0f);
                            }
                            else if (data_store.type == "NR")
                            {
                                rsrp_val = cell["signal"].value("ssRsrp", -145.0f);
                                rsrq_val = cell["signal"].value("ssRsrq", -30.0f);
                                rssi_val = cell["signal"].value("ssRssi", -120.0f);
                            }
                            else if (data_store.type == "GSM")
                            {
                                rssi_val = cell["signal"].value("dbm", -120.0f);
                            }

                            data_store.current_rsrp = rsrp_val;
                            data_store.current_rsrq = rsrq_val;
                            data_store.current_rssi = rssi_val;
                            std::string s_rsrp = std::to_string(rsrp_val);
                            std::string s_rsrq = std::to_string(rsrq_val);
                            std::string s_rssi = std::to_string(rssi_val);
                            const char *params[8] = {
                                data_store.lat.c_str(),
                                data_store.lon.c_str(),
                                data_store.alt.c_str(),
                                data_store.acc.c_str(),
                                data_store.type.c_str(),
                                s_rsrp.c_str(),
                                s_rsrq.c_str(),
                                s_rssi.c_str()};
                            SaveDataToDB(params, db_con);

                            log_messages.push_back(
                                "Recv: " + data_store.type +
                                " | RSRP: " + s_rsrp +
                                " | RSRQ: " + s_rsrq +
                                " | RSSI: " + s_rssi);
                            break;
                        }
                    }
                }

                data_store.pending_records.push_back(j.dump());
                if (log_messages.size() > 50)
                    log_messages.erase(log_messages.begin());

                if (data_store.pending_records.size() >= 10)
                    FlushToDisk();
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
    FlushToDisk();
    std::cout << "[ZMQ Server] Thread stopped." << std::endl;
}