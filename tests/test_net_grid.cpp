#include <doctest/doctest.h>

#include "urt/net/interest_grid.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using urt::net::InterestGrid;

TEST_CASE("InterestGrid finds exactly the items inside the radius") {
    InterestGrid grid(10.0f);
    grid.insert(0, {0.0f, 0.0f, 0.0f});
    grid.insert(1, {9.0f, 0.0f, 0.0f});
    grid.insert(2, {0.0f, 0.0f, -10.0f}); // exactly on the edge: included
    grid.insert(3, {10.1f, 0.0f, 0.0f});
    grid.insert(4, {0.0f, 500.0f, 3.0f}); // height is ignored
    grid.insert(5, {-200.0f, 0.0f, 200.0f});
    CHECK(grid.size() == 6);

    std::vector<std::uint32_t> found;
    grid.query({0.0f, 0.0f, 0.0f}, 10.0f, found);
    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<std::uint32_t>{0, 1, 2, 4});

    grid.clear();
    found.clear();
    grid.query({0.0f, 0.0f, 0.0f}, 1000.0f, found);
    CHECK(found.empty());
    CHECK(grid.size() == 0);
}

TEST_CASE("InterestGrid agrees with a brute-force search") {
    std::mt19937 random(2026);
    std::uniform_real_distribution<float> coordinate(-400.0f, 400.0f);

    std::vector<glm::vec3> positions;
    InterestGrid grid(25.0f);
    for (std::uint32_t i = 0; i < 3000; ++i) {
        positions.push_back({coordinate(random), 0.0f, coordinate(random)});
        grid.insert(i, positions.back());
    }

    // Small radii use the cell walk, the huge one the "all cells" path.
    for (const float radius : {0.0f, 5.0f, 40.0f, 130.0f, 1.0e7f}) {
        for (int query = 0; query < 20; ++query) {
            const glm::vec3 center{coordinate(random), 0.0f, coordinate(random)};

            std::vector<std::uint32_t> expected;
            for (std::uint32_t i = 0; i < positions.size(); ++i) {
                const float dx = positions[i].x - center.x;
                const float dz = positions[i].z - center.z;
                if (dx * dx + dz * dz <= radius * radius) {
                    expected.push_back(i);
                }
            }

            std::vector<std::uint32_t> found;
            grid.query(center, radius, found);
            std::sort(found.begin(), found.end());
            CHECK(found == expected);
        }
    }
}

TEST_CASE("InterestGrid tolerates extreme and invalid input") {
    CHECK_THROWS_AS(InterestGrid(0.0f), std::invalid_argument);

    InterestGrid grid(1.0f);
    grid.insert(0, {1.0e30f, 0.0f, -1.0e30f});
    grid.insert(1, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f});
    grid.insert(2, {3.0f, 0.0f, 4.0f});

    std::vector<std::uint32_t> found;
    grid.query({0.0f, 0.0f, 0.0f}, 5.0f, found);
    CHECK(found == std::vector<std::uint32_t>{2});

    found.clear();
    grid.query({0.0f, 0.0f, 0.0f}, -1.0f, found);
    grid.query({0.0f, 0.0f, 0.0f}, std::numeric_limits<float>::quiet_NaN(), found);
    CHECK(found.empty());
}
