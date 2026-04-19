#pragma once
#include <libpq-fe.h>

extern bool running;
extern bool start_server;

void RunServer(PGconn* db_con);
void FlushToDisk();