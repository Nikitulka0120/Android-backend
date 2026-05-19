#include "HeatmapRenderer.h"
#include <cmath>
#include <algorithm>
#include "implot.h"
#include <iostream>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

HeatmapRenderer::HeatmapRenderer()
{
    image_data.resize(w * h * channels);
}

Color HeatmapRenderer::gradientColor(Color c1, Color c2, double ratio)
{
    return {
        static_cast<int>(c1.r + (c2.r - c1.r) * ratio),
        static_cast<int>(c1.g + (c2.g - c1.g) * ratio),
        static_cast<int>(c1.b + (c2.b - c1.b) * ratio)};
}

double HeatmapRenderer::calculateDistance(double lat1,
                                          double lon1,
                                          double lat2,
                                          double lon2)
{
    constexpr double R = 6371000.0;

    double phi1 = lat1 * M_PI / 180.0;
    double phi2 = lat2 * M_PI / 180.0;

    double dPhi = (lat2 - lat1) * M_PI / 180.0;
    double dLambda = (lon2 - lon1) * M_PI / 180.0;

    double x = dLambda * std::cos((phi1 + phi2) * 0.5);
    double y = dPhi;

    return R * std::sqrt(x * x + y * y);
}

void HeatmapRenderer::UpdateData(const std::vector<double> &lats,
                                 const std::vector<double> &lons,
                                 const std::vector<double> &values,
                                 float maxRadius, float idwPower)
{
    if (m_isGenerating)
        return;
    m_lats = lats;
    m_lons = lons;
    m_vals = values;
    m_maxRadius = maxRadius;
    m_idwPower = idwPower;
}

void HeatmapRenderer::StartGeneration()
{
    if (m_isGenerating || m_lats.empty())
        return;
    m_isGenerating = true;
    std::thread(&HeatmapRenderer::GenerateTask, this).detach();
}

void HeatmapRenderer::GenerateTask()
{
    m_minLat = *std::min_element(m_lats.begin(), m_lats.end());
    m_maxLat = *std::max_element(m_lats.begin(), m_lats.end());
    m_minLon = *std::min_element(m_lons.begin(), m_lons.end());
    m_maxLon = *std::max_element(m_lons.begin(), m_lons.end());

    constexpr double epsilon = 1e-6;

    auto dBmToLinear = [](double dbm) {
        return std::pow(10.0, dbm / 10.0);
    };

    auto linearToDbm = [](double lin) {
        return 10.0 * std::log10(std::max(lin, 1e-12));
    };

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            double curLon = m_minLon + (double)x / w * (m_maxLon - m_minLon);
            double curLat = m_maxLat - (double)y / h * (m_maxLat - m_minLat);

            double sumWeights = 0.0;
            double sumValues = 0.0;

            for (size_t i = 0; i < m_lats.size(); ++i)
            {
                double dist = calculateDistance(
                    curLat, curLon,
                    m_lats[i], m_lons[i]
                );

                if (dist <= m_maxRadius)
                {
                    double weight =
                        1.0 / std::pow(dist + epsilon, m_idwPower);

                    double valueLinear = dBmToLinear(m_vals[i]);

                    sumWeights += weight;
                    sumValues += valueLinear * weight;
                }
            }

            int index = (y * w + x) * channels;

            if (sumWeights > 0.0)
            {
                double avgLinear = sumValues / sumWeights;
                double avgDbm = linearToDbm(avgLinear);
                double ratio = (avgDbm + 140.0) / 70.0;
                ratio = std::clamp(ratio, 0.0, 1.0);

                Color current = gradientColor(
                    {0, 0, 255},
                    {255, 0, 0},
                    ratio
                );

                image_data[index + 0] = current.r;
                image_data[index + 1] = current.g;
                image_data[index + 2] = current.b;
                image_data[index + 3] = 140;
            }
            else
            {
                image_data[index + 0] = 0;
                image_data[index + 1] = 0;
                image_data[index + 2] = 0;
                image_data[index + 3] = 0;
            }
        }
    }

    m_needsUpload = true;
    m_isGenerating = false;

    SaveToFile("build/heatmap.png");
}

void HeatmapRenderer::SaveToFile(const std::string &path)
{
    stbi_write_png(
        path.c_str(),
        w,
        h,
        channels,
        image_data.data(),
        w * channels
    );
}

void HeatmapRenderer::ProcessGPUUpload()
{
    if (!m_needsUpload)
        return;
    if (m_textureID == 0)
        glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_needsUpload = false;
}

void HeatmapRenderer::UI_DrawOverlay()
{
    if (m_textureID == 0)
        return;
    ImPlotPoint b_min{m_minLon, m_minLat};
    ImPlotPoint b_max{m_maxLon, m_maxLat};
    ImPlot::PlotImage("##HeatmapLayer", (ImTextureID)(intptr_t)m_textureID, b_min, b_max);
}