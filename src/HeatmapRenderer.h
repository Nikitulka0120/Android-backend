#pragma once

#include <vector>
#include <atomic>
#include <thread>
#include <string>
#include <GL/glew.h>

struct Color
{
    int r;
    int g;
    int b;
};

class HeatmapRenderer
{
public:
    HeatmapRenderer();
    bool IsGenerating() const { return m_isGenerating; }
    void UpdateData(
        const std::vector<double>& lats,
        const std::vector<double>& lons,
        const std::vector<double>& values,
        float maxRadius, float idwPower
    );

    void StartGeneration();

    void SaveToFile(const std::string& path);

    void ProcessGPUUpload();

    void UI_DrawOverlay();

    bool IsReady() const
    {
        return !m_isGenerating;
    }

private:
    void GenerateTask();
    float m_maxRadius = 30.0f;
    float m_idwPower = 2.0f;
    Color gradientColor(
        Color c1,
        Color c2,
        double ratio
    );

    double calculateDistance(
        double lat1,
        double lon1,
        double lat2,
        double lon2
    );

private:
    std::vector<double> m_lats;
    std::vector<double> m_lons;
    std::vector<double> m_vals;

    std::vector<unsigned char> image_data;

    GLuint m_textureID = 0;

    std::atomic<bool> m_isGenerating = false;
    std::atomic<bool> m_needsUpload = false;

    double m_minLat = 0.0;
    double m_maxLat = 0.0;

    double m_minLon = 0.0;
    double m_maxLon = 0.0;

    static constexpr int w = 1024;
    static constexpr int h = 1024;
    static constexpr int channels = 4;
};