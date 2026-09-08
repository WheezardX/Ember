#include "params.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "sha256.h"

namespace embersim {

namespace {
std::vector<std::string> split_dotted(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '.') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    return parts;
}
}  // namespace

const toml::node* ParamsPack::find(const std::string& dotted) const {
    const toml::node* n = &table;
    for (const auto& part : split_dotted(dotted)) {
        const toml::table* t = n->as_table();
        if (!t) return nullptr;
        n = t->get(part);
        if (!n) return nullptr;
    }
    return n;
}

bool ParamsPack::has(const std::string& dotted) const { return find(dotted) != nullptr; }

int64_t ParamsPack::get_int(const std::string& dotted) const {
    const toml::node* n = find(dotted);
    if (!n) throw std::runtime_error("params: missing key '" + dotted + "' in " + path.string());
    if (auto v = n->value<int64_t>()) return *v;
    if (auto b = n->value<bool>()) return *b ? 1 : 0;
    throw std::runtime_error("params: key '" + dotted + "' is not an integer");
}
int64_t ParamsPack::get_int(const std::string& dotted, int64_t dflt) const { return has(dotted) ? get_int(dotted) : dflt; }

bool ParamsPack::get_bool(const std::string& dotted) const {
    const toml::node* n = find(dotted);
    if (!n) throw std::runtime_error("params: missing key '" + dotted + "'");
    if (auto v = n->value<bool>()) return *v;
    if (auto i = n->value<int64_t>()) return *i != 0;
    throw std::runtime_error("params: key '" + dotted + "' is not a boolean");
}
bool ParamsPack::get_bool(const std::string& dotted, bool dflt) const { return has(dotted) ? get_bool(dotted) : dflt; }

std::string ParamsPack::get_str(const std::string& dotted, const std::string& dflt) const {
    const toml::node* n = find(dotted);
    if (!n) return dflt;
    if (auto v = n->value<std::string>()) return *v;
    return dflt;
}

void ParamsPack::apply_override(const std::string& dotted, const std::string& value_text) {
    auto parts = split_dotted(dotted);
    if (parts.empty() || parts.back().empty()) throw std::runtime_error("params: bad override key '" + dotted + "'");
    // Parse the value as a TOML fragment.
    toml::table frag;
    try {
        frag = toml::parse("v = " + value_text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error("params: override '" + dotted + "' value '" + value_text + "' is not valid TOML: " + std::string(e.description()));
    }
    toml::table* t = &table;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        toml::node* n = t->get(parts[i]);
        if (!n) {
            t->insert(parts[i], toml::table{});
            n = t->get(parts[i]);
        }
        t = n->as_table();
        if (!t) throw std::runtime_error("params: override '" + dotted + "' crosses a non-table key");
    }
    t->insert_or_assign(parts.back(), *frag.get("v"));
    overrides[dotted] = value_text;
}

ParamsPack load_params(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("params: cannot open " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    ParamsPack p = params_from_text(ss.str(), path.string());
    p.path = path;
    return p;
}

ParamsPack params_from_text(const std::string& text, const std::string& label) {
    ParamsPack p;
    try {
        p.table = toml::parse(text, label);
    } catch (const toml::parse_error& e) {
        std::ostringstream m;
        m << "params: " << label << ": " << e.description() << " (line " << e.source().begin.line << ")";
        throw std::runtime_error(m.str());
    }
    Sha256 h;
    h.update(text.data(), text.size());
    p.sha256 = Sha256::hex(h.digest());
    return p;
}

}  // namespace embersim
