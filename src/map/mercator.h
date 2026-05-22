#pragma once
#include <cmath>
#include <algorithm>

double MercatorXToTileX(double mercatorX, int zoom);
double MercatorYToTileY(double mercatorY, int zoom);
double TileXToMercatorX(int tileX, int zoom);
double TileYToMercatorY(int tileY, int zoom);
double LatToMercatorY(double lat);
double MercatorYToLat(double mercator_y);
int CalculateZoom(double lonMin, double lonMax);