#include <zmq.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <fstream>

int main()
{
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    std::cout << "Starting server on port 7777..." << std::endl;
    socket.bind("tcp://*:7777");

    std::ofstream file("data.json", std::ios::app);

    while (true)
    {
        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);

        if (res)
        {
            std::string message(static_cast<char *>(request.data()), request.size());
            std::cout << "Received data: " << message << std::endl;
            file  << message << "\n";
            file.flush();
            std::string reply_str = "OK: Data received";
            zmq::message_t reply(reply_str.size());
            memcpy(reply.data(), reply_str.data(), reply_str.size());

            socket.send(reply, zmq::send_flags::none);
            std::cout << "Reply sent" << std::endl;
        }
    }

    return 0;
}