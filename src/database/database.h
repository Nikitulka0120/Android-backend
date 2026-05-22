#pragma once
#include <libpq-fe.h>
#include <vector>
#include <string>

PGconn *ConnectToDatabase();
void DisconnectFromDatabase(PGconn *conn);
bool SaveDataToDB(const char *req_data[], PGconn *con);

struct DBRecord  // структура используемая как буфер между базой и offlie_store
{
    double lat;
    double lon;
    double alt;
    double acc;
    double rsrp;
    double rsrq;
    double rssi;
    double time_ms;
    std::string network_type;
};

std::vector<DBRecord> LoadHistoryFromDB();