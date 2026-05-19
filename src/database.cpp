#include "database.h"
#include <iostream>
#include <string>
#include <cstdlib>

PGconn *ConnectToDatabase()
{
    const char *host = std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "localhost";
    const char *port = std::getenv("DB_PORT") ? std::getenv("DB_PORT") : "5433";
    const char *dbname = std::getenv("DB_NAME") ? std::getenv("DB_NAME") : "android_backend";
    const char *user = std::getenv("DB_USER") ? std::getenv("DB_USER") : "postgres";
    const char *password = std::getenv("DB_PASSWORD") ? std::getenv("DB_PASSWORD") : "somepassword";
    std::string conn_info =
        "host=" + std::string(host) +
        " port=" + std::string(port) +
        " dbname=" + std::string(dbname) +
        " user=" + std::string(user) +
        " password=" + std::string(password);

    PGconn *con = PQconnectdb(conn_info.c_str());

    if (PQstatus(con) != CONNECTION_OK)
    {
        std::cerr << "\033[31mОШИБКА\033[0m подключения к БД.\n"
                  << PQerrorMessage(con) << "\n";
        PQfinish(con);
        return nullptr;
    }

    std::cout << "Подключение \033[32mУСПЕШНО!\033[0m\n\n"
              << std::endl;
    return con;
}

void DisconnectFromDatabase(PGconn *conn)
{
    if (conn)
        PQfinish(conn);
}

bool SaveDataToDB(const char *req_data[], PGconn *con)
{
    std::string query =
        "INSERT INTO network_logs "
        "(latitude, longitude, altitude, accuracy, "
        "network_type, rsrp, rsrq, rssi) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)";

    PGresult *insert_res = PQexecParams(con, query.c_str(), 8, NULL,
                                        req_data, NULL, NULL, 0);

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

std::vector<DBRecord> LoadHistoryFromDB()
{
    std::vector<DBRecord> records;

    PGconn *conn = ConnectToDatabase();
    if (!conn)
    {
        std::cerr << "[Error] Failed to connect to database for loading history" << std::endl;
        return records;
    }

    const char *query =
        "SELECT latitude, longitude, altitude, accuracy, network_type, rsrp, rsrq, rssi,"
        "EXTRACT(EPOCH FROM timestamp) * 1000 as time_ms "
        "FROM network_logs WHERE rsrp BETWEEN -200 AND 0 "
        "ORDER BY timestamp ASC";

    PGresult *res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        std::cerr << "[Error] Failed to load history: " << PQerrorMessage(conn) << std::endl;
        PQclear(res);
        DisconnectFromDatabase(conn);
        return records;
    }

    int rows = PQntuples(res);
    if (rows == 0)
    {
        std::cout << "[Info] No records in database" << std::endl;
        PQclear(res);
        DisconnectFromDatabase(conn);
        return records;
    }

    records.reserve(rows);

    for (int i = 0; i < rows; i++)
    {
        DBRecord record;
        record.lat = atof(PQgetvalue(res, i, 0));
        record.lon = atof(PQgetvalue(res, i, 1));
        record.alt = PQgetisnull(res, i, 2) ? 0.0 : atof(PQgetvalue(res, i, 2));
        record.acc = PQgetisnull(res, i, 3) ? 0.0 : atof(PQgetvalue(res, i, 3));
        record.network_type = PQgetisnull(res, i, 4) ? "UNKNOWN" : PQgetvalue(res, i, 4);
        record.rsrp = PQgetisnull(res, i, 5)
                          ? -145.0
                          : atof(PQgetvalue(res, i, 5));
        record.rsrq = PQgetisnull(res, i, 6)
                          ? -20.0
                          : atof(PQgetvalue(res, i, 6));
        record.rssi = PQgetisnull(res, i, 7)
                          ? -120.0
                          : atof(PQgetvalue(res, i, 7));
        record.time_ms = PQgetisnull(res, i, 8)
                             ? 0.0
                             : atof(PQgetvalue(res, i, 8));
        records.push_back(std::move(record));
    }

    std::cout << "[Info] Loaded " << rows << " records from database" << std::endl;

    PQclear(res);
    DisconnectFromDatabase(conn);
    return records;
}