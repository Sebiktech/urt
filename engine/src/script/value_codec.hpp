#pragma once

// Turning Lua values into bytes for network events, and back.
//
// What can be sent: nil, booleans, finite numbers, strings, and tables of
// those with number or string keys. Functions, threads and userdata cannot
// cross the network. Everything is bounded: nesting depth, the number of
// values and the total size, in both directions, because the other side is
// not trusted.

#include <cstdint>
#include <span>
#include <vector>

struct lua_State;

namespace urt::script {

// Encodes the stack values first..last of L. Raises a script error (so call
// it only from inside a function scripts call) if a value cannot be sent or
// the result would exceed the limits.
void encode_values(lua_State* L, int first, int last, std::vector<std::uint8_t>& out);

// Decodes all values and pushes them onto L's stack. Returns how many were
// pushed, or -1 for malformed data, in which case the stack is unchanged.
int decode_values(lua_State* L, std::span<const std::uint8_t> data);

} // namespace urt::script
