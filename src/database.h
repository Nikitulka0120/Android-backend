#pragma once
#include <libpq-fe.h>

PGconn* ConnectToDatabase();
void DisconnectFromDatabase(PGconn* conn);
bool SaveDataToDB(const char* req_data[], PGconn* con);