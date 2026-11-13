#pragma once

// Helpers for tests that check a rendered picture by its colours.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

inline bool read_ppm(const std::string& path, Image& image) {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    int max_value = 0;
    file >> magic >> image.width >> image.height >> max_value;
    file.get(); // the single whitespace byte after the header
    if (!file || magic != "P6" || max_value != 255 || image.width <= 0 || image.height <= 0) {
        return false;
    }
    image.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    file.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    return static_cast<bool>(file);
}

struct Counts {
    int orange = 0;
    int blue = 0;
    int ground = 0;
    int grid = 0;
    double orange_x = 0.0; // centre of the orange pixels, in pixels
    double orange_y = 0.0;
};

// Sorts every pixel into "which thing was drawn here" by its colour.
inline Counts classify(const Image& image) {
    Counts counts;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                   static_cast<std::size_t>(x)) * 3;
            const int r = image.rgb[i];
            const int g = image.rgb[i + 1];
            const int b = image.rgb[i + 2];
            if (r > 90 && r > g * 14 / 10 && g > b * 15 / 10) {
                ++counts.orange; // the local player: (1.0, 0.6, 0.2) at any brightness
                sum_x += x;
                sum_y += y;
            } else if (b > 90 && b > g * 14 / 10 && g > r * 15 / 10) {
                ++counts.blue; // other entities: (0.3, 0.6, 1.0)
            } else if (r >= 38 && r <= 44 && g >= 41 && g <= 46 && b >= 48 && b <= 54) {
                ++counts.ground; // (0.16, 0.17, 0.20)
            } else if (r >= 60 && r <= 78 && g >= 62 && g <= 80 && b >= 70 && b <= 88) {
                ++counts.grid; // ground plus the line brightness
            }
        }
    }
    if (counts.orange > 0) {
        counts.orange_x = sum_x / counts.orange;
        counts.orange_y = sum_y / counts.orange;
    }
    return counts;
}
