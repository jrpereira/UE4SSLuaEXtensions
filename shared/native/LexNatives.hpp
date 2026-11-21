#pragma once

// The native-call scaffolding of the File, Log and Sound Bridges: results,
// argument reading, the shape check, the native table's entry points and their
// registration. Each bridge keeps its own natives, contract and bridge lookup.
//
// A bridge's ErrorCode lives in its NativeContract namespace next to
// to_string(ErrorCode), which `failure` finds by argument-dependent lookup.

#include <UE4SSLuaABI.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Lex::Natives
{
using RC::LuaMadeSimple::Lua;

// --- Results ---------------------------------------------------------------

using Value = std::variant<std::monostate, bool, int64_t, double, std::string>;

struct Reply
{
    std::vector<Value> values;
};

inline Reply failure_named(std::string_view code, std::string message)
{
    return Reply{{std::monostate{}, std::string(code), std::move(message)}};
}

// nil, code, message (contract: failure is always these three values).
template<class Code>
Reply failure(Code code, std::string message)
{
    return failure_named(to_string(code), std::move(message));
}

template<class... Values>
Reply success(Values&&... values)
{
    Reply reply;
    (reply.values.emplace_back(Value(std::forward<Values>(values))), ...);
    return reply;
}

// Runs after the native's try block: a Lua memory error while pushing is
// raised by Lua itself and can't become `nil, code, message`.
inline int push(const Lua& lua, const Reply& reply)
{
    for (const auto& value : reply.values)
    {
        if (std::holds_alternative<std::monostate>(value)) lua.set_nil();
        else if (const auto* flag = std::get_if<bool>(&value)) lua.set_bool(*flag);
        else if (const auto* integer = std::get_if<int64_t>(&value)) lua.set_integer(*integer);
        else if (const auto* number = std::get_if<double>(&value)) lua.set_number(*number);
        else lua.set_string(std::get<std::string>(value));
    }
    return static_cast<int>(reply.values.size());
}

// --- Arguments -------------------------------------------------------------

// Arguments are consumed left to right (get_* remove argument 1).
class Call
{
public:
    explicit Call(const Lua& lua) : lua_(lua) {}
    int64_t integer() { return lua_.get_integer(1); }
    std::string string()
    {
        const auto view = lua_.get_string(1);
        return std::string(view.data() ? view : std::string_view{}); // copy before any other Lua call (R3)
    }

private:
    const Lua& lua_;
};

// `shape`: one letter per argument, i = integer, s = string.
inline bool shape_matches(const Lua& lua, std::string_view shape)
{
    if (lua.get_stack_size() != static_cast<int32_t>(shape.size())) return false;
    for (std::size_t i = 0; i < shape.size(); ++i)
    {
        const auto index = static_cast<int32_t>(i + 1);
        if (shape[i] == 'i' ? !lua.is_integer(index) : !lua.is_string(index)) return false;
    }
    return true;
}

inline std::string session_gone()
{
    return "Lua session is not registered or is stopping";
}

// UTF-16 mod name to UTF-8; unpaired surrogates fail.
inline bool utf16_to_utf8(std::wstring_view text, std::string& out)
{
    out.clear();
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        uint32_t code = static_cast<uint16_t>(text[i]);
        if (code >= 0xD800 && code <= 0xDBFF)
        {
            if (i + 1 >= text.size()) return false;
            const uint32_t low = static_cast<uint16_t>(text[i + 1]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            ++i;
        }
        else if (code >= 0xDC00 && code <= 0xDFFF)
        {
            return false;
        }
        if (code < 0x80) out.push_back(static_cast<char>(code));
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | code >> 6));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | code >> 12));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | code >> 18));
            out.push_back(static_cast<char>(0x80 | (code >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return true;
}

// --- The native table ------------------------------------------------------

template<class Bridge>
using Body = Reply (*)(Bridge&, Call&);

// One native: its name, argument shape and body. A bridge may use its own
// entry type with more fields; the functions below read only these three.
template<class Bridge>
struct NativeEntry
{
    std::string_view name;
    std::string_view shape;
    Body<Bridge> body;
};

// Whether a native table has the contract's names in its order, and its
// shapes (or, for a contract that lists only arities, its arities).
template<const auto& Table, const auto& Contract>
constexpr bool natives_match_contract()
{
    if (Table.size() != Contract.size()) return false;
    for (std::size_t i = 0; i < Table.size(); ++i)
    {
        if (Table[i].name != Contract[i].name) return false;
        if constexpr (requires { Contract[0].shape; })
        {
            if (Table[i].shape != Contract[i].shape) return false;
        }
        else
        {
            if (static_cast<int32_t>(Table[i].shape.size()) != Contract[i].arity) return false;
        }
    }
    return true;
}

// The bridge a native may run against, or nullptr with `refusal` set.
// `Ready` has the shape: Bridge* ready(const Entry&, Reply& refusal).
//
// Checks the shape, runs the body and turns any exception into
// nil, "io", message.
template<const auto& Table, std::size_t Index, auto Ready>
int native_entry(const Lua& lua)
{
    const auto& entry = Table[Index];
    Reply reply;
    try
    {
        if (!shape_matches(lua, entry.shape))
        {
            reply = failure_named("invalid", std::string(entry.name) + ": bad arguments");
        }
        else if (auto* bridge = Ready(entry, reply))
        {
            Call call(lua);
            reply = entry.body(*bridge, call);
        }
    }
    catch (const std::exception& error)
    {
        reply = failure_named("io", std::string(entry.name) + ": internal error: " + error.what());
    }
    catch (...)
    {
        reply = failure_named("io", std::string(entry.name) + ": internal error");
    }
    return push(lua, reply);
}

// The refusal when no bridge is active.
template<class Entry>
Reply inactive(const Entry& entry)
{
    return failure_named("io", std::string(entry.name) + ": the bridge is not active");
}

namespace Detail
{
template<const auto& Table, auto Ready, std::size_t... Index>
void register_all(Lua& lua, std::string_view prefix, std::index_sequence<Index...>)
{
    (lua.register_function(std::string(prefix) + std::string(Table[Index].name),
         Lua::LuaFunction{&native_entry<Table, Index, Ready>}),
        ...);
}
} // namespace Detail

// Registers every native of `Table` on `lua` as prefix + name.
template<const auto& Table, auto Ready>
void register_natives(Lua& lua, std::string_view prefix)
{
    Detail::register_all<Table, Ready>(lua, prefix, std::make_index_sequence<Table.size()>{});
}
} // namespace Lex::Natives
