#pragma once

// Interest management: finding what is near a player.
//
// A server with thousands of entities cannot send all of them to everyone.
// Each client only gets what is within some radius of its player. Checking
// every entity against every player would cost players x entities; the grid
// sorts entities into square cells first, so a query only looks at the few
// cells the radius touches.
//
// The grid is flat: it uses x and z and ignores height.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

namespace urt::net {

class InterestGrid {
public:
    // cell_size in metres. Around a third of the usual query radius works well.
    explicit InterestGrid(float cell_size);

    // Removes all items but keeps the memory for the next tick.
    void clear();

    // Adds an item. `index` is whatever the caller wants back from query(),
    // typically a position in the caller's own array.
    void insert(std::uint32_t index, const glm::vec3& position);

    // Appends the index of every item within `radius` of `center` to `out`.
    void query(const glm::vec3& center, float radius, std::vector<std::uint32_t>& out) const;

    std::size_t size() const { return size_; }

private:
    struct Item {
        std::uint32_t index;
        float x;
        float z;
    };

    static std::uint64_t cell_key(std::int32_t cell_x, std::int32_t cell_z);
    std::int32_t cell_of(float coordinate) const;

    float cell_size_;
    std::size_t size_ = 0;
    std::unordered_map<std::uint64_t, std::vector<Item>> cells_;
};

} // namespace urt::net
