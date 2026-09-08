#pragma once
// Params packs are opaque to the core (ADR 0008 §2): a parsed TOML table + provenance. Each
// model reads the keys it documents. Dotted overrides come from the scenario's [model.overrides].
#include <filesystem>
#include <map>
#include <string>

#include "../third_party/toml.hpp"

namespace embersim {

struct ParamsPack {
    std::filesystem::path path;  // may be empty (built-in defaults)
    std::string sha256;          // of the file as loaded (before overrides)
    toml::table table;
    std::map<std::string, std::string> overrides;  // dotted key -> value text, as applied

    // Typed getters with dotted paths ("spotting.enabled", "class.GR.base_rate_mms").
    // Throw std::runtime_error naming the key when missing/mistyped unless a default is given.
    int64_t get_int(const std::string& dotted) const;
    int64_t get_int(const std::string& dotted, int64_t dflt) const;
    bool get_bool(const std::string& dotted) const;
    bool get_bool(const std::string& dotted, bool dflt) const;
    std::string get_str(const std::string& dotted, const std::string& dflt = "") const;
    bool has(const std::string& dotted) const;
    const toml::node* find(const std::string& dotted) const;

    // Apply "a.b.c" = "value" (value parsed as TOML: 12, true, "x"); records in `overrides`.
    void apply_override(const std::string& dotted, const std::string& value_text);
};

ParamsPack load_params(const std::filesystem::path& path);
ParamsPack params_from_text(const std::string& toml_text, const std::string& label = "<inline>");

}  // namespace embersim
