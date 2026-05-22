#include "database.h"
#include "server.h"
#include "gui.h"
#include "tile_cache.h"
#include <thread>

int main()              // въодная точка приложения
{
    PGconn *con = ConnectToDatabase();  // пытаемся подключиться к бд
    if (!con)
        return 1;

    std::thread(TileWorker).detach();  // загрузка тайлов
    std::thread server(RunServer, con); // получение данных от android
    RunGUI();                           // интерфейс приложения
    server.join();

    DisconnectFromDatabase(con);
    return 0;
}