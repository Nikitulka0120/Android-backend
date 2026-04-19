#pragma once
#include <map>
#include <queue>
#include <mutex>
#include <vector>
#include <string>
#include <GL/glew.h>

struct TileJob {
    std::string id;
    int zoom;
    int x;
    int y;
};

struct TextureData {
    GLuint id = 0;
    bool isLoading = false;
    bool isLoaded = false;
    std::vector<uint8_t> rgbaBlob;
    int width = 0;
    int height = 0;
};

extern std::map<std::string, TextureData> g_TileCache;
extern std::queue<TileJob> g_JobQueue;
extern std::mutex g_JobMutex;
extern std::mutex g_CacheMutex;

void FetchWorker();
std::string GetTilePath(int zoom, int x, int y);
bool LoadTileFromDisk(int zoom, int x, int y, std::vector<uint8_t>& out_data);
void SaveTileToDisk(int zoom, int x, int y, const std::vector<uint8_t>& data);
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);