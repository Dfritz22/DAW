#include "ui/dock_persist.h"
#include "ui/panel.h"

#include <shlobj.h>
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace daw::ui {
namespace {

// ── Utilities ───────────────────────────────────────────────────────────────

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// Map a persistKey ("transport", "tracks", ...) back to its PanelKind.
// Returns false if the key isn't registered.
bool PanelKindFromKey(const std::wstring& key, PanelKind& out) {
    const int n = PanelCount();
    for (int i = 0; i < n; ++i) {
        const PanelKind k = PanelKindAt(i);
        if (key == PanelGet(k).persistKey) { out = k; return true; }
    }
    return false;
}

// ── Serialization ───────────────────────────────────────────────────────────
// We hand-roll a tiny JSON writer to avoid pulling in a 3rd-party library
// for a single ~30-line tree. The format is whitespace-tolerant on read.

void WriteEscaped(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

void WriteNode(std::ostringstream& os, const DockNode* n, int indent) {
    const std::string pad(static_cast<size_t>(indent * 2), ' ');
    const std::string pad1(static_cast<size_t>((indent + 1) * 2), ' ');
    os << pad << "{\n";
    if (n->kind == DockKind::Leaf) {
        os << pad1 << "\"kind\": \"leaf\",\n";
        os << pad1 << "\"activeTab\": " << n->activeTab << ",\n";
        os << pad1 << "\"panels\": [";
        for (size_t i = 0; i < n->panels.size(); ++i) {
            if (i) os << ", ";
            const std::string key = WideToUtf8(PanelGet(n->panels[i]).persistKey);
            WriteEscaped(os, key);
        }
        os << "]\n";
    } else {
        os << pad1 << "\"kind\": \""
           << (n->kind == DockKind::SplitH ? "splitH" : "splitV") << "\",\n";
        os << pad1 << "\"ratio\": " << n->ratio << ",\n";
        os << pad1 << "\"children\": [\n";
        WriteNode(os, n->children[0].get(), indent + 2);
        os << ",\n";
        WriteNode(os, n->children[1].get(), indent + 2);
        os << "\n" << pad1 << "]\n";
    }
    os << pad << "}";
}

void WriteFloating(std::ostringstream& os,
                   const std::vector<DockFloatingPanel>& floats,
                   int indent) {
    if (floats.empty()) { os << "[]"; return; }
    const std::string pad(static_cast<size_t>(indent * 2), ' ');
    const std::string pad1(static_cast<size_t>((indent + 1) * 2), ' ');
    os << "[\n";
    for (size_t i = 0; i < floats.size(); ++i) {
        const auto& f = floats[i];
        const std::string key = WideToUtf8(PanelGet(f.panel).persistKey);
        os << pad1 << "{ \"panel\": ";
        WriteEscaped(os, key);
        os << ", \"x\": " << f.x
           << ", \"y\": " << f.y
           << ", \"w\": " << f.w
           << ", \"h\": " << f.h
           << " }";
        if (i + 1 < floats.size()) os << ",";
        os << "\n";
    }
    os << pad << "]";
}

// ── Tiny JSON parser ────────────────────────────────────────────────────────
// Just enough to read what WriteNode emits (objects, arrays, strings,
// numbers, booleans). Throws on parse error so the public entry point can
// catch and return nullptr.

struct ParseError {};

struct Parser {
    const std::string& src;
    size_t pos = 0;

    void skipWs() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
    }
    bool eof() const { return pos >= src.size(); }
    char peek() {
        skipWs();
        if (eof()) throw ParseError{};
        return src[pos];
    }
    char get() {
        skipWs();
        if (eof()) throw ParseError{};
        return src[pos++];
    }
    void expect(char c) {
        if (get() != c) throw ParseError{};
    }
    std::string parseString() {
        if (get() != '"') throw ParseError{};
        std::string out;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return out;
            if (c == '\\' && pos < src.size()) {
                char e = src[pos++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'u': {
                        // Skip 4 hex digits; we don't need full unicode escape
                        // support since persistKeys are plain ASCII.
                        if (pos + 4 > src.size()) throw ParseError{};
                        pos += 4;
                        out.push_back('?');
                        break;
                    }
                    default: throw ParseError{};
                }
            } else {
                out.push_back(c);
            }
        }
        throw ParseError{};
    }
    double parseNumber() {
        skipWs();
        size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) ++pos;
        while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                                    src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' ||
                                    src[pos] == '+' || src[pos] == '-')) ++pos;
        if (pos == start) throw ParseError{};
        try { return std::stod(src.substr(start, pos - start)); }
        catch (...) { throw ParseError{}; }
    }

    // Skip an arbitrary JSON value (object/array/string/number/literal).
    // Used for unknown-field forward compatibility.
    void skipValue() {
        char c = peek();
        if (c == '"')      { (void)parseString(); return; }
        if (c == '{')      { skipObject();        return; }
        if (c == '[')      { skipArray();         return; }
        // number or literal — read until terminator
        while (pos < src.size() && src[pos] != ',' && src[pos] != '}' && src[pos] != ']') ++pos;
    }
    void skipObject() {
        expect('{');
        skipWs();
        if (peek() == '}') { ++pos; return; }
        while (true) {
            skipWs();
            (void)parseString();
            skipWs();
            expect(':');
            skipValue();
            skipWs();
            char c = get();
            if (c == '}') return;
            if (c != ',') throw ParseError{};
        }
    }
    void skipArray() {
        expect('[');
        skipWs();
        if (peek() == ']') { ++pos; return; }
        while (true) {
            skipValue();
            skipWs();
            char c = get();
            if (c == ']') return;
            if (c != ',') throw ParseError{};
        }
    }

    std::unique_ptr<DockNode> parseNode() {
        expect('{');
        std::string kind;
        int activeTab = 0;
        float ratio = 0.5f;
        std::vector<std::string> panelKeys;
        std::unique_ptr<DockNode> childA, childB;

        while (true) {
            skipWs();
            if (eof()) throw ParseError{};
            if (src[pos] == '}') { ++pos; break; }
            std::string key = parseString();
            skipWs();
            expect(':');
            skipWs();

            if (key == "kind") {
                kind = parseString();
            } else if (key == "activeTab") {
                activeTab = static_cast<int>(parseNumber());
            } else if (key == "ratio") {
                ratio = static_cast<float>(parseNumber());
            } else if (key == "panels") {
                expect('[');
                skipWs();
                if (peek() != ']') {
                    while (true) {
                        skipWs();
                        panelKeys.push_back(parseString());
                        skipWs();
                        char c = get();
                        if (c == ']') break;
                        if (c != ',') throw ParseError{};
                    }
                } else {
                    ++pos; // consume ']'
                }
            } else if (key == "children") {
                expect('[');
                childA = parseNode();
                skipWs();
                expect(',');
                childB = parseNode();
                skipWs();
                expect(']');
            } else {
                // Unknown field — skip a value of any shape. Keeps
                // forward-compat with future fields.
                skipValue();
            }
            skipWs();
            if (peek() == ',') { ++pos; continue; }
        }

        auto node = std::make_unique<DockNode>();
        if (kind == "leaf") {
            node->kind = DockKind::Leaf;
            node->activeTab = activeTab;
            for (const auto& k : panelKeys) {
                PanelKind pk;
                if (!PanelKindFromKey(Utf8ToWide(k), pk)) throw ParseError{};
                node->panels.push_back(pk);
            }
            if (node->panels.empty()) throw ParseError{};
            if (node->activeTab < 0 ||
                node->activeTab >= static_cast<int>(node->panels.size())) {
                node->activeTab = 0;
            }
        } else if (kind == "splitH" || kind == "splitV") {
            node->kind = (kind == "splitH") ? DockKind::SplitH : DockKind::SplitV;
            if (!childA || !childB) throw ParseError{};
            // Clamp ratio to a sane visible range so a corrupted file can't
            // hide a panel by collapsing it to zero width/height.
            if (ratio < 0.05f) ratio = 0.05f;
            if (ratio > 0.95f) ratio = 0.95f;
            node->ratio = ratio;
            node->children[0] = std::move(childA);
            node->children[1] = std::move(childB);
        } else {
            throw ParseError{};
        }
        return node;
    }

    // Parse the schema-v2 floating list. Caller has already consumed the
    // "floating": prefix; we expect `[ {...}, {...} ]` next.
    std::vector<DockFloatingPanel> parseFloatingArray() {
        std::vector<DockFloatingPanel> out;
        expect('[');
        skipWs();
        if (peek() == ']') { ++pos; return out; }
        while (true) {
            skipWs();
            expect('{');
            DockFloatingPanel f{};
            std::string panelKey;
            bool hasPanel = false, hasX = false, hasY = false, hasW = false, hasH = false;
            while (true) {
                skipWs();
                if (eof()) throw ParseError{};
                if (src[pos] == '}') { ++pos; break; }
                std::string key = parseString();
                skipWs();
                expect(':');
                skipWs();
                if (key == "panel") {
                    panelKey = parseString();
                    hasPanel = true;
                } else if (key == "x") { f.x = static_cast<int>(parseNumber()); hasX = true; }
                else if (key == "y")   { f.y = static_cast<int>(parseNumber()); hasY = true; }
                else if (key == "w")   { f.w = static_cast<int>(parseNumber()); hasW = true; }
                else if (key == "h")   { f.h = static_cast<int>(parseNumber()); hasH = true; }
                else { skipValue(); }
                skipWs();
                if (peek() == ',') { ++pos; continue; }
            }
            if (!(hasPanel && hasX && hasY && hasW && hasH)) throw ParseError{};
            PanelKind pk;
            if (!PanelKindFromKey(Utf8ToWide(panelKey), pk)) throw ParseError{};
            // Primary panels can never float — they're always docked. A
            // file claiming otherwise is corrupt; reject loudly so the
            // user gets the default layout instead of a broken one.
            if (PanelGet(pk).primary) throw ParseError{};
            // Clamp degenerate sizes; everything else is just trusted as
            // screen pixels (caller may reposition onto a visible monitor).
            if (f.w < 100) f.w = 100;
            if (f.h < 60)  f.h = 60;
            f.panel = pk;
            out.push_back(f);
            skipWs();
            char c = get();
            if (c == ']') break;
            if (c != ',') throw ParseError{};
        }
        return out;
    }
};

// ── Validation ──────────────────────────────────────────────────────────────

void CollectPanels(const DockNode* n, std::set<PanelKind>& seen, bool& dup) {
    if (!n) { dup = true; return; }
    if (n->kind == DockKind::Leaf) {
        for (PanelKind p : n->panels) {
            if (!seen.insert(p).second) { dup = true; return; }
        }
    } else {
        CollectPanels(n->children[0].get(), seen, dup);
        if (dup) return;
        CollectPanels(n->children[1].get(), seen, dup);
    }
}

// Validate the full document. Each PanelKind must appear exactly once,
// either in the docked tree OR in the floating list — never both, never
// twice. Every primary panel must be present in the docked tree (primaries
// can never float).
bool ValidateDocument(const DockNode* root,
                      const std::vector<DockFloatingPanel>& floats) {
    if (!root) return false;
    std::set<PanelKind> seen;
    bool dup = false;
    CollectPanels(root, seen, dup);
    if (dup) return false;
    for (const auto& f : floats) {
        if (PanelGet(f.panel).primary) return false;
        if (!seen.insert(f.panel).second) return false;
    }
    const int n = PanelCount();
    for (int i = 0; i < n; ++i) {
        const PanelKind k = PanelKindAt(i);
        if (PanelGet(k).primary && !seen.count(k)) return false;
    }
    return true;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────

std::wstring DockGetLayoutFilePath() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, appdata))) {
        return {};
    }
    std::wstring dir = std::wstring(appdata) + L"\\DAW";
    CreateDirectoryW(dir.c_str(), nullptr); // ignore "already exists"
    return dir + L"\\layout.json";
}

std::string DockSerializeToJson(const DockNode* root,
                                const std::vector<DockFloatingPanel>& floating) {
    if (!root) return "{}";
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": 2,\n";
    os << "  \"root\":\n";
    WriteNode(os, root, 1);
    os << ",\n";
    os << "  \"floating\": ";
    WriteFloating(os, floating, 1);
    os << "\n}\n";
    return os.str();
}

DockLayoutDocument DockDeserializeFromJson(const std::string& json) {
    DockLayoutDocument doc;
    try {
        Parser p{json};
        p.expect('{');
        std::unique_ptr<DockNode> root;
        std::vector<DockFloatingPanel> floats;
        int version = 0;
        bool sawVersion = false;
        while (true) {
            p.skipWs();
            if (p.eof()) throw ParseError{};
            if (p.src[p.pos] == '}') { ++p.pos; break; }
            std::string key = p.parseString();
            p.skipWs();
            p.expect(':');
            p.skipWs();
            if (key == "version") {
                version = static_cast<int>(p.parseNumber());
                sawVersion = true;
            } else if (key == "root") {
                root = p.parseNode();
            } else if (key == "floating") {
                floats = p.parseFloatingArray();
            } else {
                p.skipValue();
            }
            p.skipWs();
            if (p.peek() == ',') { ++p.pos; continue; }
        }
        // Strict version gate: layout.json is owned by us, no shipped
        // legacy versions exist. Anything missing or off-version is
        // discarded so the caller falls back to DockBuildDefault().
        if (!sawVersion || version != 2) return {};
        if (!root) return {};
        doc.root = std::move(root);
        doc.floating = std::move(floats);
        if (!ValidateDocument(doc.root.get(), doc.floating)) return {};
        return doc;
    } catch (...) {
        return {};
    }
}

bool DockSaveLayout(const DockNode* root,
                    const std::vector<DockFloatingPanel>& floating) {
    if (!root) return false;
    const std::wstring path = DockGetLayoutFilePath();
    if (path.empty()) return false;
    const std::string json = DockSerializeToJson(root, floating);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) return false;
    }
    // MoveFileEx with REPLACE_EXISTING is the cheapest "atomic" rename on
    // NTFS. Worst case we lose the previous file but the new one is intact.
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

DockLayoutDocument DockLoadLayout() {
    const std::wstring path = DockGetLayoutFilePath();
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return DockDeserializeFromJson(buf.str());
}

void DockDeleteLayoutFile() {
    const std::wstring path = DockGetLayoutFilePath();
    if (!path.empty()) DeleteFileW(path.c_str());
}

} // namespace daw::ui
