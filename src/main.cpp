#include "database.h"
#include "server.h"
#include "gui.h"
#include "tile_cache.h"
#include <thread>

int main() {
    PGconn* con = ConnectToDatabase();
    if (!con) return 1;
    
    std::thread(FetchWorker).detach();
    std::thread server(RunServer, con);
    RunGUI();
    server.join();
    
    DisconnectFromDatabase(con);
    return 0;
}