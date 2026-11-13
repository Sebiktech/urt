#include "value_codec.hpp"

#include "urt/net/bytes.hpp"
#include "urt/net/protocol.hpp"

#include <cmath>

#include <lua.h>
#include <lualib.h>

namespace urt::script {
namespace {

constexpr int max_arguments = 16;
constexpr int max_depth = 8;
constexpr int max_values = 2048; // in one event, counting table keys and values
constexpr std::size_t max_string_bytes = 8 * 1024;

enum Tag : std::uint8_t { Nil = 0, False = 1, True = 2, Number = 3, String = 4, Table = 5 };

struct Encoder {
    lua_State* L;
    net::ByteWriter writer;
    int values = 0;

    void count() {
        if (++values > max_values) {
            luaL_error(L, "too many values in one event (the limit is %d)", max_values);
        }
    }

    void encode(int index, int depth) {
        count();
        switch (lua_type(L, index)) {
        case LUA_TNIL:
            writer.u8(Nil);
            break;
        case LUA_TBOOLEAN:
            writer.u8(lua_toboolean(L, index) != 0 ? True : False);
            break;
        case LUA_TNUMBER: {
            const double value = lua_tonumber(L, index);
            if (!std::isfinite(value)) {
                luaL_error(L, "NaN and infinity cannot be sent in an event");
            }
            writer.u8(Number);
            writer.f64(value);
            break;
        }
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            if (length > max_string_bytes) {
                luaL_error(L, "a string in an event may be at most %d bytes", static_cast<int>(max_string_bytes));
            }
            writer.u8(String);
            writer.u16(static_cast<std::uint16_t>(length));
            for (std::size_t i = 0; i < length; ++i) {
                writer.u8(static_cast<std::uint8_t>(text[i]));
            }
            break;
        }
        case LUA_TTABLE:
            encode_table(index, depth);
            break;
        default:
            luaL_error(L, "a %s cannot be sent in an event", lua_typename(L, lua_type(L, index)));
        }
        if (writer.size() > net::max_event_bytes) {
            luaL_error(L, "the event is too large (the limit is %d bytes)", static_cast<int>(net::max_event_bytes));
        }
    }

    void encode_table(int index, int depth) {
        if (depth >= max_depth) {
            // Also what stops a table that contains itself.
            luaL_error(L, "tables in an event may be nested at most %d deep", max_depth);
        }
        const int table = lua_absindex(L, index);
        luaL_checkstack(L, 4, "event too complex");

        // Two passes: the count has to be written before the entries.
        std::uint32_t entries = 0;
        lua_pushnil(L);
        while (lua_next(L, table) != 0) {
            ++entries;
            lua_pop(L, 1);
        }
        if (entries > 0xFFFF) {
            luaL_error(L, "a table in an event has too many entries");
        }
        writer.u8(Table);
        writer.u16(static_cast<std::uint16_t>(entries));

        lua_pushnil(L);
        while (lua_next(L, table) != 0) {
            const int key_type = lua_type(L, -2);
            if (key_type != LUA_TNUMBER && key_type != LUA_TSTRING) {
                luaL_error(L, "table keys in an event must be numbers or strings");
            }
            encode(lua_gettop(L) - 1, depth + 1); // key
            encode(lua_gettop(L), depth + 1);     // value
            lua_pop(L, 1);
        }
    }
};

// Checks the whole encoding before anything is pushed, so that malformed
// data never leaves half-built values behind.
bool validate(net::ByteReader& reader, int depth, int& values, bool as_key) {
    if (++values > max_values) {
        return false;
    }
    const std::uint8_t tag = reader.u8();
    if (!reader.ok()) {
        return false;
    }
    switch (tag) {
    case Nil:
    case False:
    case True:
        return !as_key;
    case Number:
        reader.f64(); // fails on NaN and infinity
        return reader.ok();
    case String: {
        const std::size_t length = reader.u16();
        if (length > max_string_bytes || length > reader.remaining()) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            reader.u8();
        }
        return reader.ok();
    }
    case Table: {
        if (as_key || depth >= max_depth) {
            return false;
        }
        const std::size_t entries = reader.u16();
        for (std::size_t i = 0; i < entries; ++i) {
            if (!validate(reader, depth + 1, values, true) || !validate(reader, depth + 1, values, false)) {
                return false;
            }
        }
        return reader.ok();
    }
    default:
        return false;
    }
}

// Pushes one value. Only called on data validate() has accepted.
void push(lua_State* L, net::ByteReader& reader) {
    switch (reader.u8()) {
    case Nil:
        lua_pushnil(L);
        break;
    case False:
        lua_pushboolean(L, 0);
        break;
    case True:
        lua_pushboolean(L, 1);
        break;
    case Number:
        lua_pushnumber(L, reader.f64());
        break;
    case String: {
        const std::size_t length = reader.u16();
        std::string text;
        text.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            text.push_back(static_cast<char>(reader.u8()));
        }
        lua_pushlstring(L, text.data(), text.size());
        break;
    }
    case Table: {
        const std::size_t entries = reader.u16();
        lua_createtable(L, 0, static_cast<int>(entries));
        for (std::size_t i = 0; i < entries; ++i) {
            push(L, reader); // key
            push(L, reader); // value
            lua_rawset(L, -3);
        }
        break;
    }
    default:
        lua_pushnil(L);
        break;
    }
}

} // namespace

void encode_values(lua_State* L, int first, int last, std::vector<std::uint8_t>& out) {
    if (last - first + 1 > max_arguments) {
        luaL_error(L, "an event may carry at most %d arguments", max_arguments);
    }
    Encoder encoder{L, {}, 0};
    encoder.writer.u8(static_cast<std::uint8_t>(last >= first ? last - first + 1 : 0));
    for (int index = first; index <= last; ++index) {
        encoder.encode(index, 0);
    }
    out.assign(encoder.writer.bytes().begin(), encoder.writer.bytes().end());
}

int decode_values(lua_State* L, std::span<const std::uint8_t> data) {
    if (data.size() > net::max_event_bytes) {
        return -1;
    }
    net::ByteReader check(data);
    const int count = check.u8();
    int values = 0;
    if (!check.ok() || count > max_arguments) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        if (!validate(check, 0, values, false)) {
            return -1;
        }
    }
    if (!check.finished()) {
        return -1;
    }

    net::ByteReader reader(data);
    reader.u8();
    lua_checkstack(L, count + 2 * max_depth + 4);
    for (int i = 0; i < count; ++i) {
        push(L, reader);
    }
    return count;
}

} // namespace urt::script
