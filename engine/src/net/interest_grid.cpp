#include "urt/net/interest_grid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace urt::net {
namespace {

// Positions this far out (in cells) are clamped. It keeps the float-to-int
// conversion defined for absurd coordinates.
constexpr float max_cell = 1.0e9f;

} // namespace

InterestGrid::InterestGrid(float cell_size) : cell_size_(cell_size) {
    if (!(cell_size > 0.0f)) {
        throw std::invalid_argument("InterestGrid: cell size must be positive");
    }
}

void InterestGrid::clear() {
    for (auto& [key, items] : cells_) {
        items.clear();
    }
    size_ = 0;
}

std::uint64_t InterestGrid::cell_key(std::int32_t cell_x, std::int32_t cell_z) {
    // Pack the two cell coordinates into one 64-bit map key.
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_x)) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_z));
}

std::int32_t InterestGrid::cell_of(float coordinate) const {
    if (!std::isfinite(coordinate)) {
        return 0; // a broken position must not break the grid
    }
    // floor, not a plain cast: -0.5 must land in cell -1, not in cell 0.
    const float cell = std::floor(coordinate / cell_size_);
    return static_cast<std::int32_t>(std::clamp(cell, -max_cell, max_cell));
}

void InterestGrid::insert(std::uint32_t index, const glm::vec3& position) {
    cells_[cell_key(cell_of(position.x), cell_of(position.z))].push_back(
        Item{index, position.x, position.z});
    ++size_;
}

void InterestGrid::query(const glm::vec3& center, float radius,
                         std::vector<std::uint32_t>& out) const {
    if (!(radius >= 0.0f)) {
        return;
    }
    const std::int32_t min_x = cell_of(center.x - radius);
    const std::int32_t max_x = cell_of(center.x + radius);
    const std::int32_t min_z = cell_of(center.z - radius);
    const std::int32_t max_z = cell_of(center.z + radius);
    const float radius_squared = radius * radius;

    const auto check_cell = [&](const std::vector<Item>& items) {
        for (const Item& item : items) {
            const float dx = item.x - center.x;
            const float dz = item.z - center.z;
            // Compare squared distances to avoid a square root per item.
            if (dx * dx + dz * dz <= radius_squared) {
                out.push_back(item.index);
            }
        }
    };

    // A radius covering more cells than exist: walking the existing cells is
    // cheaper than walking the whole square.
    const std::uint64_t cells_x = static_cast<std::uint64_t>(std::int64_t{max_x} - min_x) + 1;
    const std::uint64_t cells_z = static_cast<std::uint64_t>(std::int64_t{max_z} - min_z) + 1;
    if (cells_x * cells_z > cells_.size()) {
        for (const auto& [key, items] : cells_) {
            check_cell(items);
        }
        return;
    }

    // 64-bit loop counters so max_x + 1 cannot overflow.
    for (std::int64_t cell_x = min_x; cell_x <= max_x; ++cell_x) {
        for (std::int64_t cell_z = min_z; cell_z <= max_z; ++cell_z) {
            const auto found = cells_.find(
                cell_key(static_cast<std::int32_t>(cell_x), static_cast<std::int32_t>(cell_z)));
            if (found == cells_.end()) {
                continue;
            }
            check_cell(found->second);
        }
    }
}

} // namespace urt::net
