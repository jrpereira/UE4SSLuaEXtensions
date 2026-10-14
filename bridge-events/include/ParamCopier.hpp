#pragma once

// Payload writer and flattener (LEX_hooks.md §5.1, §7, §9.2). Portable.
//
// Record (little endian): tag(1) | field index(2) | length word(2) | value.
//   length word = (element index << 13) | value length in bytes.
//   Value lengths are below 8192 (strings are bounded at max_string_bytes), so
//   the low 13 bits hold the length; the high 3 bits hold the element index of
//   a fixed array (0..7; dim 1 fields always use 0). Elements 8 and above are
//   not copied and the copy reports `truncated`.
// Values by kind:
//   boolean      b  1 byte, 0/1 (BoolProperty: byte at byte_offset, & bit_mask; mask 0/0xFF = whole byte)
//   int8..int64  i  8 bytes int64, sign-extended
//   uint8..32    i  8 bytes int64, zero-extended
//   uint64       i  8 bytes, raw (Lua sees values above 2^63 as negative)
//   float32/64   n  8 bytes double
//   enumeration  e  int64 value (1/2 byte zero-extended, 4/8 byte sign-extended) + int32 name
//                   index into Field::enum_names (-1 when unknown); 12 bytes
//   name         s  the raw FName, comparison index u32 + number u32; the Flattener
//                   turns it into a string through a NameSource
//   string       s  UTF-16LE code units without the terminator, at most max_string_bytes
//   object       o  address u64 | index i32 | serial i32 (16 bytes), then the object
//                   path in UTF-8 (at most max_string_bytes) when object_paths is on;
//                   a null pointer is address 0, index -1, serial 0
//   unsupported  x  length 0 (delivered as nil)
// Structs have no record of their own: their supported leaves are copied with
// their ancestors' names. A payload that would pass max_payload_bytes stops
// before the record that does not fit and reports `truncated`.

#include <HookTypes.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace UE4SSLuaEventBridge::Hooks
{
inline constexpr unsigned record_length_bits = 13;
inline constexpr unsigned max_array_elements = 8;
inline constexpr std::size_t record_header_bytes = 5;

// Injected by the Windows side (weak identity through FWeakObjectPtr::assign,
// path through outer names) and by the tests.
class ObjectReader
{
public:
    virtual ~ObjectReader() = default;
    virtual ObjectIdentity identity(std::uintptr_t address) = 0;
    virtual std::string path(std::uintptr_t address) { (void)address; return {}; }
};

// FName::ToString (thread-safe) on Windows, a fake in tests; returns UTF-8.
class NameSource
{
public:
    virtual ~NameSource() = default;
    virtual std::string name(uint32_t comparison_index, uint32_t number) = 0;
};

// Receives one dispatcher call's triplets. begin() comes first with the count
// that becomes `n`; every call then maps to one (name, tag, value) triplet.
class TripletSink
{
public:
    virtual ~TripletSink() = default;
    virtual void begin(std::size_t count) = 0;
    virtual void boolean(const std::string& name, bool value) = 0;          // 'b'
    virtual void integer(const std::string& name, int64_t value) = 0;       // 'i'
    virtual void number(const std::string& name, double value) = 0;         // 'n'
    virtual void string(const std::string& name, const std::string& utf8) = 0; // 's'
    virtual void enumeration(const std::string& name, const std::string& text) = 0; // 'e', "n|Name"
    virtual void object(const std::string& name, const std::string& text) = 0;      // 'o', "addr:index:serial[:path]"
    virtual void unsupported(const std::string& name) = 0;                  // 'x', nil
};

struct CopyLimits
{
    std::size_t max_payload{max_payload_bytes};
    std::size_t max_string{max_string_bytes};
};

struct CopyResult
{
    bool truncated{};
};

namespace detail
{
template <typename T>
inline T load(const std::byte* at)
{
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <typename T>
inline void put(std::vector<std::byte>& out, T value)
{
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

inline int64_t read_integer(FieldKind kind, const std::byte* at)
{
    switch (kind)
    {
    case FieldKind::int8: return load<int8_t>(at);
    case FieldKind::int16: return load<int16_t>(at);
    case FieldKind::int32: return load<int32_t>(at);
    case FieldKind::int64: return load<int64_t>(at);
    case FieldKind::uint8: return load<uint8_t>(at);
    case FieldKind::uint16: return load<uint16_t>(at);
    case FieldKind::uint32: return load<uint32_t>(at);
    default: return static_cast<int64_t>(load<uint64_t>(at));
    }
}
}

class ParamCopier
{
public:
    // selection: schema indices (empty = every supported parameter). Selecting a
    // struct copies its leaves; selecting a nested field copies only that field.
    // include_return: with an empty selection, also copy the return value (post phase).
    ParamCopier(const Schema& schema, const std::vector<uint16_t>& selection, bool include_return,
                bool object_paths, CopyLimits limits = {})
        : schema_(schema), object_paths_(object_paths), limits_(limits)
    {
        std::vector<bool> taken(schema.fields.size(), false);
        if (selection.empty())
        {
            for (std::size_t i = 0; i < schema.fields.size(); ++i)
            {
                const Field& field = schema.fields[i];
                if (field.depth != 0 || (field.flags & cpf_parm) == 0) continue;
                if ((field.flags & cpf_return_parm) != 0 && !include_return) continue;
                add(static_cast<uint16_t>(i), taken);
            }
        }
        else
        {
            for (const uint16_t index : selection)
                if (index < schema.fields.size()) add(index, taken);
        }
    }

    std::size_t leaf_count() const { return leaves_.size(); }

    // Reads `block` (the parameter block, at least parms_size bytes) and
    // replaces `out` with the payload.
    CopyResult copy(const std::byte* block, ObjectReader* objects, std::vector<std::byte>& out) const
    {
        out.clear();
        CopyResult result;
        for (const Leaf& leaf : leaves_)
        {
            const Field& field = schema_.fields[leaf.field];
            if (field.kind == FieldKind::unsupported)
            {
                if (!begin(out, Tag::unsupported, leaf.field, 0, 0, result)) return result;
                continue;
            }
            const unsigned elements = field.dim < max_array_elements ? field.dim : max_array_elements;
            if (field.dim > max_array_elements) result.truncated = true;
            for (unsigned e = 0; e < elements; ++e)
            {
                const std::byte* at = block + leaf.offset + static_cast<std::size_t>(e) * field.size;
                if (!write(out, field, leaf.field, e, at, objects, result)) return result;
            }
        }
        return result;
    }

private:
    struct Leaf
    {
        uint16_t field;
        std::size_t offset; // of element 0 from the start of the parameter block
    };

    void add(uint16_t index, std::vector<bool>& taken)
    {
        if (taken[index]) return;
        taken[index] = true;
        const Field& field = schema_.fields[index];
        if (field.kind == FieldKind::structure)
        {
            for (std::size_t i = 0; i < schema_.fields.size(); ++i)
                if (schema_.fields[i].parent == index) add(static_cast<uint16_t>(i), taken);
            return;
        }
        std::size_t offset = field.offset;
        for (uint16_t p = field.parent; p != no_parent; p = schema_.fields[p].parent) offset += schema_.fields[p].offset;
        leaves_.push_back({index, offset});
    }

    bool begin(std::vector<std::byte>& out, Tag tag, uint16_t field, unsigned element, std::size_t length,
               CopyResult& result) const
    {
        if (out.size() + record_header_bytes + length > limits_.max_payload)
        {
            result.truncated = true;
            return false;
        }
        detail::put<uint8_t>(out, static_cast<uint8_t>(tag));
        detail::put<uint16_t>(out, field);
        detail::put<uint16_t>(out, static_cast<uint16_t>((element << record_length_bits) | length));
        return true;
    }

    bool write(std::vector<std::byte>& out, const Field& field, uint16_t index, unsigned element,
               const std::byte* at, ObjectReader* objects, CopyResult& result) const
    {
        switch (field.kind)
        {
        case FieldKind::boolean:
        {
            const uint8_t byte = static_cast<uint8_t>(at[field.byte_offset]);
            const uint8_t mask = (field.bit_mask == 0) ? 0xFF : field.bit_mask;
            if (!begin(out, Tag::boolean, index, element, 1, result)) return false;
            detail::put<uint8_t>(out, (byte & mask) != 0 ? 1 : 0);
            return true;
        }
        case FieldKind::float32:
            if (!begin(out, Tag::number, index, element, 8, result)) return false;
            detail::put<double>(out, static_cast<double>(detail::load<float>(at)));
            return true;
        case FieldKind::float64:
            if (!begin(out, Tag::number, index, element, 8, result)) return false;
            detail::put<double>(out, detail::load<double>(at));
            return true;
        case FieldKind::enumeration:
        {
            int64_t value = 0;
            switch (field.size)
            {
            case 1: value = detail::load<uint8_t>(at); break;
            case 2: value = detail::load<uint16_t>(at); break;
            case 4: value = detail::load<int32_t>(at); break;
            default: value = detail::load<int64_t>(at); break;
            }
            int32_t name_index = -1;
            if (value >= 0 && static_cast<uint64_t>(value) < field.enum_names.size() &&
                !field.enum_names[static_cast<std::size_t>(value)].empty())
                name_index = static_cast<int32_t>(value);
            if (!begin(out, Tag::enumeration, index, element, 12, result)) return false;
            detail::put<int64_t>(out, value);
            detail::put<int32_t>(out, name_index);
            return true;
        }
        case FieldKind::name:
            if (!begin(out, Tag::string, index, element, 8, result)) return false;
            detail::put<uint32_t>(out, detail::load<uint32_t>(at));
            detail::put<uint32_t>(out, detail::load<uint32_t>(at + 4));
            return true;
        case FieldKind::string:
        {
            const auto* data = reinterpret_cast<const std::byte*>(detail::load<std::uintptr_t>(at));
            const int32_t num = detail::load<int32_t>(at + 8);
            std::size_t chars = (data != nullptr && num > 1) ? static_cast<std::size_t>(num - 1) : 0;
            if (chars * 2 > limits_.max_string) chars = limits_.max_string / 2;
            if (!begin(out, Tag::string, index, element, chars * 2, result)) return false;
            out.insert(out.end(), data, data + chars * 2);
            return true;
        }
        case FieldKind::object:
        {
            const auto address = detail::load<std::uintptr_t>(at);
            ObjectIdentity identity{0, -1, 0};
            std::string path;
            if (address != 0)
            {
                identity = objects != nullptr ? objects->identity(address) : ObjectIdentity{address, -1, 0};
                identity.address = address;
                if (object_paths_ && objects != nullptr)
                {
                    path = objects->path(address);
                    if (path.size() > limits_.max_string) path.resize(limits_.max_string);
                }
            }
            if (!begin(out, Tag::object, index, element, 16 + path.size(), result)) return false;
            detail::put<uint64_t>(out, identity.address);
            detail::put<int32_t>(out, identity.index);
            detail::put<int32_t>(out, identity.serial);
            const auto* raw = reinterpret_cast<const std::byte*>(path.data());
            out.insert(out.end(), raw, raw + path.size());
            return true;
        }
        case FieldKind::structure:
        case FieldKind::unsupported:
            return begin(out, Tag::unsupported, index, element, 0, result);
        default: // integers
            if (!begin(out, Tag::integer, index, element, 8, result)) return false;
            detail::put<int64_t>(out, detail::read_integer(field.kind, at));
            return true;
        }
    }

    const Schema& schema_;
    bool object_paths_;
    CopyLimits limits_;
    std::vector<Leaf> leaves_;
};

inline std::string utf16le_to_utf8(const std::byte* bytes, std::size_t length)
{
    std::string out;
    const std::size_t units = length / 2;
    for (std::size_t i = 0; i < units; ++i)
    {
        uint32_t cp = detail::load<uint16_t>(bytes + i * 2);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units)
        {
            const uint32_t low = detail::load<uint16_t>(bytes + (i + 1) * 2);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
            else cp = 0xFFFD;
        }
        else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Turns a payload into dispatcher triplets. Names: "Param", "Struct.Field",
// "out:Param" (out parameters, not const), "ret" / "ret.Field" for the return
// value; fixed array elements append "[i]" to the field's own name
// ("Arr[2]", "Struct.Arr[2]").
class Flattener
{
public:
    explicit Flattener(const Schema& schema) : schema_(schema)
    {
        names_.reserve(schema.fields.size());
        for (std::size_t i = 0; i < schema.fields.size(); ++i)
        {
            const Field& field = schema.fields[i];
            std::string name = field.name;
            if (field.parent != no_parent) name = names_[field.parent] + "." + field.name;
            else if ((field.flags & cpf_return_parm) != 0) name = "ret";
            else if ((field.flags & cpf_out_parm) != 0 && (field.flags & cpf_const_parm) == 0) name = "out:" + field.name;
            names_.push_back(std::move(name));
        }
    }

    const std::string& path(uint16_t field) const { return names_[field]; }

    // False (and nothing emitted) when the payload is malformed.
    bool flatten(const std::byte* payload, std::size_t size, NameSource* names, TripletSink& sink) const
    {
        std::size_t count = 0;
        if (!walk(payload, size, nullptr, nullptr, count)) return false;
        sink.begin(count);
        std::size_t unused = 0;
        return walk(payload, size, names, &sink, unused);
    }

private:
    bool walk(const std::byte* payload, std::size_t size, NameSource* names, TripletSink* sink, std::size_t& count) const
    {
        std::size_t at = 0;
        while (at < size)
        {
            if (size - at < record_header_bytes) return false;
            const auto tag = static_cast<Tag>(static_cast<uint8_t>(payload[at]));
            const uint16_t index = detail::load<uint16_t>(payload + at + 1);
            const uint16_t word = detail::load<uint16_t>(payload + at + 3);
            const std::size_t length = word & ((1u << record_length_bits) - 1);
            const unsigned element = word >> record_length_bits;
            at += record_header_bytes;
            if (index >= schema_.fields.size() || size - at < length) return false;
            const Field& field = schema_.fields[index];
            const std::byte* value = payload + at;
            at += length;
            if (!record(field, index, tag, element, value, length, names, sink)) return false;
            ++count;
        }
        return true;
    }

    bool record(const Field& field, uint16_t index, Tag tag, unsigned element, const std::byte* value,
                std::size_t length, NameSource* names, TripletSink* sink) const
    {
        std::string name = names_[index];
        if (field.dim > 1) name += "[" + std::to_string(element) + "]";
        if (tag == Tag::unsupported)
        {
            if (length != 0) return false;
            if (sink) sink->unsupported(name);
            return true;
        }
        switch (field.kind)
        {
        case FieldKind::boolean:
            if (tag != Tag::boolean || length != 1) return false;
            if (sink) sink->boolean(name, static_cast<uint8_t>(value[0]) != 0);
            return true;
        case FieldKind::float32:
        case FieldKind::float64:
            if (tag != Tag::number || length != 8) return false;
            if (sink) sink->number(name, detail::load<double>(value));
            return true;
        case FieldKind::enumeration:
        {
            if (tag != Tag::enumeration || length != 12) return false;
            if (sink)
            {
                const int64_t number = detail::load<int64_t>(value);
                const int32_t named = detail::load<int32_t>(value + 8);
                std::string text = std::to_string(number) + "|";
                if (named >= 0 && static_cast<std::size_t>(named) < field.enum_names.size())
                    text += field.enum_names[static_cast<std::size_t>(named)];
                sink->enumeration(name, text);
            }
            return true;
        }
        case FieldKind::name:
            if (tag != Tag::string || length != 8) return false;
            if (sink)
                sink->string(name, names ? names->name(detail::load<uint32_t>(value), detail::load<uint32_t>(value + 4)) : std::string());
            return true;
        case FieldKind::string:
            if (tag != Tag::string || length % 2 != 0) return false;
            if (sink) sink->string(name, utf16le_to_utf8(value, length));
            return true;
        case FieldKind::object:
        {
            if (tag != Tag::object || length < 16) return false;
            if (sink)
            {
                std::string text = std::to_string(detail::load<uint64_t>(value)) + ":" +
                                   std::to_string(detail::load<int32_t>(value + 8)) + ":" +
                                   std::to_string(detail::load<int32_t>(value + 12));
                if (length > 16) text += ":" + std::string(reinterpret_cast<const char*>(value + 16), length - 16);
                sink->object(name, text);
            }
            return true;
        }
        case FieldKind::structure:
        case FieldKind::unsupported:
            return false;
        default:
            if (tag != Tag::integer || length != 8) return false;
            if (sink) sink->integer(name, detail::load<int64_t>(value));
            return true;
        }
    }

    const Schema& schema_;
    std::vector<std::string> names_;
};
}
