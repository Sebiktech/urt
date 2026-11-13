// Fuzzes the decoder for event arguments: whatever bytes arrive, it must
// either refuse them or build ordinary Lua values, and never crash, read out
// of bounds or leave the Lua stack in a mess. The sanitizer builds are what
// make this meaningful.

#include <doctest/doctest.h>

#include "script/value_codec.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <lua.h>
#include <lualib.h>

using urt::script::decode_values;
using urt::script::encode_values;

TEST_CASE("event arguments round-trip through the codec") {
    lua_State* L = luaL_newstate();

    lua_pushnumber(L, 42.5);
    lua_pushstring(L, "text");
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    lua_newtable(L);
    lua_pushnumber(L, 7);
    lua_setfield(L, -2, "seven");
    lua_newtable(L);
    lua_pushstring(L, "inner");
    lua_rawseti(L, -2, 1);
    lua_setfield(L, -2, "list");

    std::vector<std::uint8_t> bytes;
    encode_values(L, 1, 5, bytes);
    lua_settop(L, 0);

    REQUIRE(decode_values(L, bytes) == 5);
    CHECK(lua_gettop(L) == 5);
    CHECK(lua_tonumber(L, 1) == 42.5);
    CHECK(std::string(lua_tostring(L, 2)) == "text");
    CHECK(lua_toboolean(L, 3) == 1);
    CHECK(lua_isnil(L, 4));
    REQUIRE(lua_istable(L, 5));
    lua_getfield(L, 5, "seven");
    CHECK(lua_tonumber(L, -1) == 7);
    lua_getfield(L, 5, "list");
    lua_rawgeti(L, -1, 1);
    CHECK(std::string(lua_tostring(L, -1)) == "inner");

    lua_close(L);
}

TEST_CASE("the event argument decoder survives random and mutated data") {
    lua_State* L = luaL_newstate();

    // A valid encoding to mutate: nested tables, strings, numbers.
    lua_newtable(L);
    for (int i = 1; i <= 6; ++i) {
        lua_newtable(L);
        lua_pushstring(L, "some text that is a little longer");
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, i * 1.25);
        lua_setfield(L, -2, "value");
        lua_pushboolean(L, i % 2);
        lua_rawseti(L, -2, 1);
        lua_rawseti(L, -2, i);
    }
    lua_pushstring(L, "tail");
    std::vector<std::uint8_t> valid;
    encode_values(L, 1, 2, valid);
    lua_settop(L, 0);
    REQUIRE(decode_values(L, valid) == 2);
    lua_settop(L, 0);

    std::mt19937 random(424242);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> small(0, 5);
    int accepted = 0;
    int refused = 0;

    for (int round = 0; round < 30000; ++round) {
        std::vector<std::uint8_t> data;
        if (round % 3 == 0) {
            // Pure noise, biased towards valid tags so it gets past the first byte.
            data.resize(static_cast<std::size_t>(byte(random) % 64));
            for (std::uint8_t& value : data) {
                value = static_cast<std::uint8_t>(round % 2 == 0 ? byte(random) : small(random));
            }
        } else {
            // The valid encoding with a few bytes changed, or cut short.
            data = valid;
            const int changes = 1 + byte(random) % 4;
            for (int i = 0; i < changes; ++i) {
                data[static_cast<std::size_t>(random()) % data.size()] = static_cast<std::uint8_t>(byte(random));
            }
            if (round % 7 == 0) {
                data.resize(static_cast<std::size_t>(random()) % data.size());
            }
        }

        const int count = decode_values(L, data);
        if (count < 0) {
            ++refused;
            REQUIRE(lua_gettop(L) == 0); // nothing half-built left behind
        } else {
            ++accepted;
            REQUIRE(lua_gettop(L) == count);
            // What was accepted can be encoded again without trouble.
            std::vector<std::uint8_t> again;
            encode_values(L, 1, count, again);
        }
        lua_settop(L, 0);
    }
    // Both paths were exercised.
    CHECK(accepted > 100);
    CHECK(refused > 1000);

    lua_close(L);
}
