#pragma once

#include <CandidateDiscovery.hpp>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

struct LoaderConfig
{
    bool valid{};
    std::optional<SemanticVersion> exact_version;
    std::string error;
};

namespace loader_config_detail
{
class Parser
{
public:
    explicit Parser(std::string_view source) : source_(source) {}

    LoaderConfig parse()
    {
        skip_space();
        if (!consume('{')) return fail("configuration must be a JSON object");
        bool have_schema{};
        bool have_version{};
        int schema{};
        std::string version;
        skip_space();
        while (!consume('}'))
        {
            const auto key = string();
            if (!key) return fail("configuration key must be a string");
            skip_space();
            if (!consume(':')) return fail("configuration key must have a value");
            skip_space();
            if (*key == "schema")
            {
                if (have_schema || !integer(schema)) return fail("schema must be a unique integer");
                have_schema = true;
            }
            else if (*key == "version")
            {
                const auto parsed = string();
                if (have_version || !parsed) return fail("version must be a unique string");
                version = *parsed;
                have_version = true;
            }
            else
            {
                return fail("unknown configuration key");
            }
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) return fail("configuration members must be comma-separated");
            skip_space();
            if (position_ < source_.size() && source_[position_] == '}')
                return fail("trailing comma is not allowed");
        }
        skip_space();
        if (position_ != source_.size()) return fail("unexpected data after configuration");
        if (!have_schema || schema != 1) return fail("unsupported configuration schema");
        if (!have_version) return fail("configuration version is required");
        if (version == "auto") return {true, std::nullopt, {}};
        const auto exact = parse_semantic_version(version);
        return exact ? LoaderConfig{true, exact, {}} : fail("version must be auto or MAJOR.MINOR.PATCH");
    }

private:
    std::string_view source_;
    std::size_t position_{};

    void skip_space()
    {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_;
    }

    bool consume(char value)
    {
        skip_space();
        if (position_ >= source_.size() || source_[position_] != value) return false;
        ++position_;
        return true;
    }

    std::optional<std::string> string()
    {
        skip_space();
        if (position_ >= source_.size() || source_[position_++] != '"') return {};
        std::string result;
        while (position_ < source_.size())
        {
            const char value = source_[position_++];
            if (value == '"') return result;
            if (value == '\\' || static_cast<unsigned char>(value) < 0x20) return {};
            result.push_back(value);
        }
        return {};
    }

    bool integer(int& result)
    {
        skip_space();
        const auto start = position_;
        while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_])))
            ++position_;
        if (start == position_) return false;
        const auto parsed = std::from_chars(source_.data() + start, source_.data() + position_, result);
        return parsed.ec == std::errc{} && parsed.ptr == source_.data() + position_;
    }

    static LoaderConfig fail(std::string message) { return {false, std::nullopt, std::move(message)}; }
};
}

inline LoaderConfig parse_loader_config(const std::optional<std::string_view>& source)
{
    if (!source) return {true, std::nullopt, {}};
    if (source->size() > 4096) return {false, std::nullopt, "configuration exceeds 4096 bytes"};
    auto contents = *source;
    if (contents.starts_with("\xEF\xBB\xBF")) contents.remove_prefix(3);
    return loader_config_detail::Parser(contents).parse();
}
