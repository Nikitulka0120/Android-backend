#include "database.h"
#include <iostream>

#define HOST "localhost"
#define PORT "5433"
#define DB_NAME "android_backend"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "somepassword"

PGconn* ConnectToDatabase() {
    const char* info = "host=" HOST " port=" PORT " dbname=" DB_NAME 
                       " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn* con = PQconnectdb(info);
    
    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА\033[0m подключения к БД.\n"
                  << PQerrorMessage(con) << "\n";
        PQfinish(con);
        return nullptr;
    }
    
    std::cout << "Подключение \033[32mУСПЕШНО!\033[0m\n\n" << std::endl;
    return con;
}

void DisconnectFromDatabase(PGconn* conn) {
    if (conn) PQfinish(conn);
}

bool SaveDataToDB(const char* req_data[], PGconn* con) {
    std::string query = "INSERT INTO network_logs "
                        "(latitude, longitude, altitude, accuracy, network_type, rsrp) "
                        "VALUES ($1, $2, $3, $4, $5, $6)";
    
    PGresult* insert_res = PQexecParams(con, query.c_str(), 6, NULL, 
                                        req_data, NULL, NULL, 0);
    
    if (PQresultStatus(insert_res) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА DB\033[0m: " << PQresultErrorMessage(insert_res) << std::endl;
        PQclear(insert_res);
        return false;
    }
    
    std::cout << "Данные вставлены \033[32mУСПЕШНО!\033[0m" << std::endl;
    PQclear(insert_res);
    return true;
}