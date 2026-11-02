// gradio.hpp - Gradio-like web UI for C++ on top of cpp-httplib + nlohmann/json.
//
//   gr::Interface app(fn, {gr::Textbox("name")}, {gr::Textbox("greeting")});
//   app.launch();                                   // http://127.0.0.1:7860
//
// The HTTP surface follows Gradio's own where it exists (/config,
// /gradio_api/info, /gradio_api/call/<api_name>, /gradio_api/upload,
// /gradio_api/file=<path>), so the curl recipes from Gradio's docs work.
#pragma once

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00  // cpp-httplib refuses anything older than Windows 10
#endif

#include "httplib.h"
#include "json.hpp"
#include "gradio_frontend.hpp"

// <windows.h> (pulled in by httplib) defines Yield() as an empty macro.
#ifdef Yield
#undef Yield
#endif

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace gr {

using json = nlohmann::json;
namespace fs = std::filesystem;

inline constexpr const char* version = "0.4.0";
inline constexpr const char* gradio_api_version = "5.0.0";

// Throw from a handler to show the message to the user (like gr.Error).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

#if (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || __cplusplus >= 202002L
inline fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
#else
inline fs::path u8path(const std::string& s) { return fs::u8path(s); }
#endif

inline std::string u8str(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

inline std::string random_hex(size_t n) {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::lock_guard<std::mutex> lk(m);
    std::string s;
    for (size_t i = 0; i < n; ++i) s += hex[rng() & 15];
    return s;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string extension(const std::string& name) {
    auto dot = name.find_last_of('.');
    auto slash = name.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return lower(name.substr(dot));
}

inline std::string guess_mime(const std::string& name) {
    static const std::map<std::string, std::string> m = {
        {".png", "image/png"},        {".jpg", "image/jpeg"},       {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},        {".webp", "image/webp"},      {".bmp", "image/bmp"},
        {".svg", "image/svg+xml"},    {".txt", "text/plain"},       {".csv", "text/csv"},
        {".json", "application/json"},{".pdf", "application/pdf"},  {".zip", "application/zip"},
        {".mp3", "audio/mpeg"},       {".wav", "audio/wav"},        {".ogg", "audio/ogg"},
        {".mp4", "video/mp4"},        {".webm", "video/webm"},      {".html", "text/html"},
    };
    auto it = m.find(extension(name));
    return it == m.end() ? "application/octet-stream" : it->second;
}

inline std::string ext_for_mime(const std::string& mime) {
    static const std::map<std::string, std::string> m = {
        {"image/png", ".png"},  {"image/jpeg", ".jpg"}, {"image/gif", ".gif"},
        {"image/webp", ".webp"},{"image/bmp", ".bmp"},  {"text/plain", ".txt"},
        {"application/json", ".json"}, {"text/csv", ".csv"}, {"audio/wav", ".wav"},
        {"audio/mpeg", ".mp3"}, {"video/mp4", ".mp4"},
    };
    auto it = m.find(mime);
    return it == m.end() ? "" : it->second;
}

// Keep only a safe base name.
inline std::string sanitize_name(const std::string& name) {
    std::string base = name;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string out;
    for (char c : base) out += (std::string("\\/:*?\"<>|").find(c) != std::string::npos || (unsigned char)c < 32) ? '_' : c;
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty() || out == "." || out == "..") out = "file";
    return out;
}

inline std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error("cannot read file: " + u8str(p));
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Temporary directory holding uploads and handler outputs, plus the set of
// extra paths a handler returned. Only these are served by /gradio_api/file=.
class Cache {
public:
    static Cache& get() {
        static Cache c;
        return c;
    }
    const fs::path& dir() const { return dir_; }

    // Store bytes under a fresh sub-directory, return the UTF-8 path.
    std::string store(const std::string& bytes, const std::string& name) {
        fs::path d = dir_ / random_hex(16);
        fs::create_directories(d);
        fs::path p = d / u8path(sanitize_name(name));
        std::ofstream f(p, std::ios::binary);
        if (!f) throw Error("cannot write cache file");
        f.write(bytes.data(), (std::streamsize)bytes.size());
        return u8str(p);
    }

    // A fresh file in the cache for streaming writes; returns its UTF-8 path.
    std::string create(const std::string& name, std::ofstream& out) {
        fs::path d = dir_ / random_hex(16);
        fs::create_directories(d);
        fs::path p = d / u8path(sanitize_name(name));
        out.open(p, std::ios::binary);
        return u8str(p);
    }

    void allow(const std::string& path) {
        std::error_code ec;
        auto c = fs::weakly_canonical(u8path(path), ec);
        if (ec) return;
        std::lock_guard<std::mutex> lk(m_);
        allowed_.insert(u8str(c));
    }

    bool allowed(const std::string& path) const {
        std::error_code ec;
        auto c = fs::weakly_canonical(u8path(path), ec);
        if (ec) return false;
        auto [a, b] = std::mismatch(root_.begin(), root_.end(), c.begin(), c.end());
        if (a == root_.end() && b != c.end()) return true;
        std::lock_guard<std::mutex> lk(m_);
        return allowed_.count(u8str(c)) > 0;
    }

private:
    Cache() {
        dir_ = fs::temp_directory_path() / "gradio_cpp" / random_hex(12);
        fs::create_directories(dir_);
        root_ = fs::weakly_canonical(dir_);
    }
    ~Cache() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_, root_;
    mutable std::mutex m_;
    std::set<std::string> allowed_;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// FileData: the value of Image / File components (gradio.FileData on the wire)
// ---------------------------------------------------------------------------
struct FileData {
    std::string path;       // local path (UTF-8)
    std::string url;        // filled in when sent to the browser
    std::string orig_name;
    std::string mime_type;
    long long size = -1;
    std::string bytes;      // in-memory content for outputs; written to the cache when sent

    bool empty() const { return path.empty() && bytes.empty(); }

    std::string read() const {
        if (!bytes.empty() || path.empty()) return bytes;
        return detail::read_file(detail::u8path(path));
    }

    static FileData from_path(const std::string& p, const std::string& mime = "") {
        FileData f;
        f.path = p;
        f.orig_name = detail::u8str(detail::u8path(p).filename());
        f.mime_type = mime.empty() ? detail::guess_mime(p) : mime;
        return f;
    }

    static FileData from_bytes(std::string data, const std::string& mime = "", const std::string& name = "") {
        FileData f;
        f.bytes = std::move(data);
        f.mime_type = mime.empty() ? (name.empty() ? "application/octet-stream" : detail::guess_mime(name)) : mime;
        f.orig_name = name.empty() ? "file" + detail::ext_for_mime(f.mime_type) : name;
        f.size = (long long)f.bytes.size();
        return f;
    }
};

namespace detail {
// URL prefix when served behind a reverse proxy under a sub-path (LaunchOptions::root_path).
inline std::string& root_path() {
    static std::string r;
    return r;
}
}  // namespace detail

inline std::string file_url(const std::string& path) {
    return detail::root_path() + "/gradio_api/file=" + detail::url_encode_path(path);
}

inline void to_json(json& j, const FileData& f) {
    if (f.empty()) {
        j = nullptr;
        return;
    }
    std::string path = f.path;
    if (path.empty()) {
        path = detail::Cache::get().store(f.bytes, f.orig_name);
    } else {
        detail::Cache::get().allow(path);
    }
    long long size = f.size;
    if (size < 0) {
        std::error_code ec;
        auto s = fs::file_size(detail::u8path(path), ec);
        size = ec ? -1 : (long long)s;
    }
    j = json{{"path", path},
             {"url", f.url.empty() ? file_url(path) : f.url},
             {"orig_name", f.orig_name.empty() ? detail::u8str(detail::u8path(path).filename()) : f.orig_name},
             {"mime_type", f.mime_type.empty() ? detail::guess_mime(path) : f.mime_type},
             {"size", size < 0 ? json(nullptr) : json(size)},
             {"is_stream", false},
             {"meta", {{"_type", "gradio.FileData"}}}};
}

inline void from_json(const json& j, FileData& f) {
    f = FileData{};
    if (j.is_null()) return;
    if (j.is_string()) {
        f.path = j.get<std::string>();
    } else if (j.is_object()) {
        auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
        f.path = str("path");
        f.url = str("url");
        f.orig_name = str("orig_name");
        f.mime_type = str("mime_type");
        if (j.contains("size") && j["size"].is_number()) f.size = j["size"].get<long long>();
    } else {
        throw Error("expected a file");
    }
    if (f.orig_name.empty() && !f.path.empty()) f.orig_name = detail::u8str(detail::u8path(f.path).filename());
    if (f.mime_type.empty() && !f.path.empty()) f.mime_type = detail::guess_mime(f.path);
}

// ---------------------------------------------------------------------------
// Chat values
// ---------------------------------------------------------------------------

// One chat message, Gradio's "messages" format. A message holds text or a file.
struct ChatMessage {
    std::string role;  // "user" / "assistant" / "system"
    std::string text;
    FileData file;
    json metadata;     // e.g. {"title": "thinking"}; null when unused

    bool is_file() const { return !file.empty(); }
    static ChatMessage user(std::string t) { return {"user", std::move(t), {}, nullptr}; }
    static ChatMessage assistant(std::string t) { return {"assistant", std::move(t), {}, nullptr}; }
    static ChatMessage user_file(FileData f) { return {"user", "", std::move(f), nullptr}; }
};
using History = std::vector<ChatMessage>;

inline void to_json(json& j, const ChatMessage& m) {
    j = json{{"role", m.role}, {"metadata", m.metadata}};
    if (m.is_file())
        j["content"] = json{{"file", m.file}, {"alt_text", m.text.empty() ? json(nullptr) : json(m.text)}};
    else
        j["content"] = m.text;
}

inline void from_json(const json& j, ChatMessage& m) {
    m = ChatMessage{};
    if (!j.is_object()) throw Error("expected a chat message object");
    m.role = j.value("role", std::string("user"));
    if (j.contains("metadata")) m.metadata = j["metadata"];
    const json& c = j.contains("content") ? j["content"] : json();
    if (c.is_string()) {
        m.text = c.get<std::string>();
    } else if (c.is_object()) {
        if (c.contains("file")) {
            m.file = c["file"].get<FileData>();
            if (c.contains("alt_text") && c["alt_text"].is_string()) m.text = c["alt_text"].get<std::string>();
        } else if (c.contains("path")) {
            m.file = c.get<FileData>();
        }
    }
}

// Value of a MultimodalTextbox: text plus attached files.
struct MultimodalValue {
    std::string text;
    std::vector<FileData> files;
    bool empty() const { return text.empty() && files.empty(); }
};

inline void to_json(json& j, const MultimodalValue& v) { j = json{{"text", v.text}, {"files", v.files}}; }

inline void from_json(const json& j, MultimodalValue& v) {
    v = MultimodalValue{};
    if (j.is_null()) return;
    if (j.is_string()) {
        v.text = j.get<std::string>();
        return;
    }
    if (!j.is_object()) throw Error("expected {\"text\": ..., \"files\": [...]}");
    if (j.contains("text") && j["text"].is_string()) v.text = j["text"].get<std::string>();
    if (j.contains("files") && j["files"].is_array())
        for (auto& f : j["files"]) v.files.push_back(f.get<FileData>());
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------
struct Component {
    std::string type;
    json props = json::object();
    int id = -1;

    Component() = default;
    Component(std::string t, json p = json::object()) : type(std::move(t)), props(std::move(p)) {}

    Component& set(const std::string& k, json v) {
        props[k] = std::move(v);
        return *this;
    }
    Component& label(const std::string& s) { return set("label", s); }
    Component& value(json v) { return set("value", std::move(v)); }
    Component& interactive(bool b) { return set("interactive", b); }
    Component& visible(bool b) { return set("visible", b); }
    Component& info(const std::string& s) { return set("info", s); }
    Component& elem_id(const std::string& s) { return set("elem_id", s); }
    Component& scale(int s) { return set("scale", s); }
};

inline bool is_file_type(const std::string& t) {
    return t == "image" || t == "file" || t == "audio" || t == "video" || t == "model3d";
}

inline bool skip_api_type(const std::string& t) { return t == "state"; }

inline bool is_layout_type(const std::string& t) {
    return t == "row" || t == "column" || t == "tabs" || t == "tabitem" || t == "accordion";
}

// Components whose value nests files somewhere inside.
inline bool has_nested_files(const std::string& t) {
    return t == "chatbot" || t == "multimodaltextbox" || t == "gallery" || t == "annotatedimage" || t == "imageeditor";
}

inline Component Textbox(const std::string& label = "", int lines = 1, const std::string& placeholder = "",
                         const std::string& value = "") {
    return Component("textbox", {{"label", label}, {"lines", lines}, {"max_lines", lines > 1 ? 20 : 1},
                                 {"placeholder", placeholder}, {"value", value}});
}

inline Component Number(const std::string& label = "", double value = 0) {
    return Component("number", {{"label", label}, {"value", value}});
}

inline Component Slider(const std::string& label = "", double minimum = 0, double maximum = 100,
                        double value = NAN, double step = 1) {
    return Component("slider", {{"label", label}, {"minimum", minimum}, {"maximum", maximum},
                                {"value", std::isnan(value) ? minimum : value}, {"step", step}});
}

inline Component Checkbox(const std::string& label = "", bool value = false) {
    return Component("checkbox", {{"label", label}, {"value", value}});
}

// sources: "upload" and/or "webcam" (camera capture in the browser)
inline Component Image(const std::string& label = "", std::vector<std::string> sources = {"upload", "webcam"}) {
    return Component("image", {{"label", label}, {"value", nullptr}, {"type", "filepath"}, {"sources", sources}});
}

inline Component File(const std::string& label = "") {
    return Component("file", {{"label", label}, {"value", nullptr}});
}

inline Component Markdown(const std::string& text = "") {
    return Component("markdown", {{"value", text}, {"show_label", false}});
}

inline Component JSON(const std::string& label = "") {
    return Component("json", {{"label", label}, {"value", nullptr}});
}

inline Component Button(const std::string& text = "Run", const std::string& variant = "secondary") {
    return Component("button", {{"value", text}, {"variant", variant}});
}

inline Component Chatbot(const std::string& label = "Chatbot", int height = 480) {
    return Component("chatbot", {{"label", label}, {"value", json::array()}, {"type", "messages"}, {"height", height}});
}

inline Component MultimodalTextbox(const std::string& label = "", const std::string& placeholder = "Type a message...",
                                   const std::string& accept = "image/*") {
    return Component("multimodaltextbox", {{"label", label},
                                           {"show_label", !label.empty()},
                                           {"placeholder", placeholder},
                                           {"file_types", accept},
                                           {"value", {{"text", ""}, {"files", json::array()}}}});
}

inline json choice_pairs(const std::vector<std::string>& choices) {
    json a = json::array();
    for (auto& c : choices) a.push_back({c, c});
    return a;
}

// value: a choice (or a list of them with multiselect); null for none.
inline Component Dropdown(const std::string& label, const std::vector<std::string>& choices, json value = nullptr,
                          bool multiselect = false) {
    if (value.is_null() && multiselect) value = json::array();
    return Component("dropdown", {{"label", label}, {"choices", choice_pairs(choices)}, {"value", value},
                                  {"multiselect", multiselect}});
}

inline Component Radio(const std::string& label, const std::vector<std::string>& choices, json value = nullptr) {
    return Component("radio", {{"label", label}, {"choices", choice_pairs(choices)}, {"value", value}});
}

inline Component CheckboxGroup(const std::string& label, const std::vector<std::string>& choices,
                               std::vector<std::string> value = {}) {
    return Component("checkboxgroup", {{"label", label}, {"choices", choice_pairs(choices)}, {"value", value}});
}

// Per-session value kept between events (gr.State). Not shown, not part of the API.
inline Component State(json value = nullptr) { return Component("state", {{"value", std::move(value)}}); }

inline Component Code(const std::string& label = "", const std::string& language = "cpp", const std::string& value = "",
                      int lines = 10) {
    return Component("code", {{"label", label}, {"language", language}, {"value", value}, {"lines", lines}});
}

// sources: "upload" and/or "microphone"
inline Component Audio(const std::string& label = "", std::vector<std::string> sources = {"upload", "microphone"}) {
    return Component("audio", {{"label", label}, {"value", nullptr}, {"type", "filepath"}, {"sources", sources}});
}

// Image with detection boxes / masks drawn over it (value: gr::AnnotatedImage from gradio_image.hpp).
inline Component AnnotatedImage(const std::string& label = "") {
    return Component("annotatedimage", {{"label", label}, {"value", nullptr}});
}

// Charts (Gradio's native plots). Return a gr::DataFrame (or gr::PlotData); x / y / color
// name its columns. The value on the wire is {columns, data, datatypes, mark}.
inline Component NativePlot(const std::string& mark, const std::string& label, const std::string& x,
                            const std::string& y, const std::string& color = "") {
    return Component("nativeplot", {{"label", label}, {"x", x}, {"y", y},
                                    {"color", color.empty() ? json(nullptr) : json(color)},
                                    {"value", nullptr}, {"_mark", mark}});
}
inline Component LinePlot(const std::string& label, const std::string& x, const std::string& y,
                          const std::string& color = "") {
    return NativePlot("line", label, x, y, color);
}
inline Component BarPlot(const std::string& label, const std::string& x, const std::string& y,
                         const std::string& color = "") {
    return NativePlot("bar", label, x, y, color);
}
inline Component ScatterPlot(const std::string& label, const std::string& x, const std::string& y,
                             const std::string& color = "") {
    return NativePlot("point", label, x, y, color);
}

// Draw on an image (gr.ImageEditor). Value: gr::EditorValue.
inline Component ImageEditor(const std::string& label = "") {
    return Component("imageeditor", {{"label", label}, {"value", nullptr}, {"brush_colors", {"#000000", "#ff0000", "#00a000", "#0050ff", "#ffffff"}}});
}

// 3D model viewer (.obj / .stl shown in the browser; other formats offered for download).
inline Component Model3D(const std::string& label = "") {
    return Component("model3d", {{"label", label}, {"value", nullptr}});
}

inline Component Video(const std::string& label = "") {
    return Component("video", {{"label", label}, {"value", nullptr}});
}

// Classification result: top label plus a bar per class.
inline Component Label(const std::string& label = "", int num_top_classes = 5) {
    return Component("label", {{"label", label}, {"value", nullptr}, {"num_top_classes", num_top_classes}});
}

inline Component Dataframe(const std::string& label = "", const std::vector<std::string>& headers = {}) {
    return Component("dataframe", {{"label", label},
                                   {"headers", headers},
                                   {"value", {{"headers", headers}, {"data", json::array()}, {"metadata", nullptr}}}});
}

// Value of an ImageEditor: the uploaded background, the drawn layers (transparent PNGs)
// and the composite of both.
struct EditorValue {
    FileData background;
    std::vector<FileData> layers;
    FileData composite;
};
inline void to_json(json& j, const EditorValue& v) {
    j = {{"background", v.background}, {"layers", v.layers}, {"composite", v.composite}, {"id", nullptr}};
}
inline void from_json(const json& j, EditorValue& v) {
    v = EditorValue{};
    if (!j.is_object()) return;
    if (j.contains("background")) v.background = j["background"].get<FileData>();
    if (j.contains("composite")) v.composite = j["composite"].get<FileData>();
    if (j.contains("layers") && j["layers"].is_array())
        for (auto& l : j["layers"])
            if (!l.is_null()) v.layers.push_back(l.get<FileData>());
}

// Value of a native plot, if you want to set datatypes yourself; a gr::DataFrame also works.
struct PlotData {
    std::vector<std::string> columns;
    std::vector<std::vector<json>> data;
    std::map<std::string, std::string> datatypes;  // column -> "quantitative" / "nominal" / "temporal"
    std::string mark = "line";
};
inline void to_json(json& j, const PlotData& v) {
    j = {{"columns", v.columns}, {"data", v.data}, {"datatypes", v.datatypes}, {"mark", v.mark}};
}

// Value of a Label output: {"label": top, "confidences": [{"label", "confidence"}, ...]}.
struct LabelValue {
    std::vector<std::pair<std::string, double>> confidences;  // any order; sorted when sent
    std::string label;                                          // empty: the most confident class
};
inline void to_json(json& j, const LabelValue& v) {
    auto c = v.confidences;
    std::stable_sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.second > b.second; });
    json arr = json::array();
    for (auto& [k, x] : c) arr.push_back({{"label", k}, {"confidence", x}});
    j = {{"label", !v.label.empty() ? v.label : c.empty() ? std::string() : c[0].first}, {"confidences", arr}};
}
inline void from_json(const json& j, LabelValue& v) {
    v = LabelValue{};
    if (!j.is_object()) return;
    if (j.contains("label") && j["label"].is_string()) v.label = j["label"].get<std::string>();
    if (j.contains("confidences") && j["confidences"].is_array())
        for (auto& c : j["confidences"]) v.confidences.push_back({c.value("label", ""), c.value("confidence", 0.0)});
}

// Value of a Dataframe: column headers and rows of cells (numbers or strings).
struct DataFrame {
    std::vector<std::string> headers;
    std::vector<std::vector<json>> data;
};
inline void to_json(json& j, const DataFrame& d) {
    j = {{"headers", d.headers}, {"data", d.data}, {"metadata", nullptr}};
}
inline void from_json(const json& j, DataFrame& d) {
    d = DataFrame{};
    if (!j.is_object()) return;
    if (j.contains("headers") && j["headers"].is_array())
        for (auto& h : j["headers"]) d.headers.push_back(h.is_string() ? h.get<std::string>() : h.dump());
    if (j.contains("data") && j["data"].is_array())
        for (auto& r : j["data"]) d.data.push_back(r.is_array() ? r.get<std::vector<json>>() : std::vector<json>{});
}

inline Component HTML(const std::string& html = "") {
    return Component("html", {{"value", html}, {"show_label", false}});
}

inline Component Gallery(const std::string& label = "", int columns = 3) {
    return Component("gallery", {{"label", label}, {"value", json::array()}, {"columns", columns}});
}

// One Gallery entry (Gradio's GalleryImage).
struct GalleryItem {
    FileData image;
    std::string caption;
};
inline void to_json(json& j, const GalleryItem& g) {
    j = json{{"image", g.image}, {"caption", g.caption.empty() ? json(nullptr) : json(g.caption)}};
}
inline void from_json(const json& j, GalleryItem& g) {
    g = GalleryItem{};
    if (j.is_object() && j.contains("image")) {
        g.image = j["image"].get<FileData>();
        if (j.contains("caption") && j["caption"].is_string()) g.caption = j["caption"].get<std::string>();
    } else {
        g.image = j.get<FileData>();
    }
}

// Return in place of a value to change a component's props (Gradio's gr.update):
//   return gr::update().visible(false);   return gr::update("new text").label("x");
struct Update {
    json props = json::object();
    Update& set(const std::string& k, json v) {
        props[k] = std::move(v);
        return *this;
    }
    Update& value(json v) { return set("value", std::move(v)); }
    Update& visible(bool b) { return set("visible", b); }
    Update& interactive(bool b) { return set("interactive", b); }
    Update& label(const std::string& s) { return set("label", s); }
    Update& placeholder(const std::string& s) { return set("placeholder", s); }
    Update& choices(const std::vector<std::string>& c) { return set("choices", choice_pairs(c)); }
};
inline Update update() { return Update{}; }
inline Update update(json value) { return Update{}.value(std::move(value)); }
inline void to_json(json& j, const Update& u) {
    j = u.props;
    j["__type__"] = "update";
}

// Resets the given components to their initial values (browser only).
inline Component ClearButton(std::vector<int> components = {}, const std::string& text = "Clear") {
    return Component("clearbutton", {{"value", text}, {"variant", "secondary"}, {"components", components}});
}

// ---------------------------------------------------------------------------
// Typed handler plumbing
// ---------------------------------------------------------------------------
using Emit = std::function<void(const json& outputs)>;
using ProgressFn = std::function<void(double fraction, const std::string& desc)>;
// Who is calling (like gr.Request): take `const gr::Request&` anywhere in the parameter list.
struct Request {
    std::string username;      // logged-in user ("" without auth)
    std::string client_host;   // remote address
    std::string session_hash;  // gradio_client / queue session ("" for /call)
    std::map<std::string, std::string> headers;  // lower-case names
    // "http://host:port" as the client sees it (honours X-Forwarded-Proto / X-Forwarded-Host).
    std::string origin() const {
        std::string proto = header("x-forwarded-proto"), host = header("x-forwarded-host");
        if (host.empty()) host = header("host");
        if (host.empty()) return "";
        return (proto.empty() ? "http" : proto.substr(0, proto.find(','))) + "://" + host.substr(0, host.find(','));
    }
    std::string header(const std::string& name) const {
        std::string k = detail::lower(name);
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct CallContext {
    Emit emit;
    ProgressFn progress;
    Request request;
};
using Handler = std::function<json(const json& args, const CallContext& ctx)>;

// Take `const gr::Progress&` as a parameter to report progress (like gr.Progress):
//   [](int n, const gr::Progress& progress) { for (...) progress(i / (double)n, "working"); return r; }
class Progress {
public:
    explicit Progress(ProgressFn p = {}) : p_(std::move(p)) {}
    void operator()(double fraction, const std::string& desc = "") const {
        if (p_) p_(fraction, desc);
    }

private:
    ProgressFn p_;
};

// Take `const gr::Yield&` as a parameter to stream intermediate results (a generator):
//   [](int n, const gr::Yield& y) { for (...) y(partial); }
class Yield {
public:
    explicit Yield(Emit e, ProgressFn p = {}) : e_(std::move(e)), p_(std::move(p)) {}
    template <class... T>
    void operator()(T&&... v) const {
        json a = json::array();
        (a.push_back(json(std::forward<T>(v))), ...);
        e_(a);
    }
    void progress(double fraction, const std::string& desc = "") const {
        if (p_) p_(fraction, desc);
    }

private:
    Emit e_;
    ProgressFn p_;
};

namespace detail {

template <class T>
struct fn_traits : fn_traits<decltype(&std::decay_t<T>::operator())> {};
template <class R, class... A>
struct fn_traits<R (*)(A...)> {
    using ret = R;
    using args = std::tuple<std::decay_t<A>...>;
};
template <class R, class... A>
struct fn_traits<R(A...)> : fn_traits<R (*)(A...)> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const> : fn_traits<R (*)(A...)> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...)> : fn_traits<R (*)(A...)> {};

template <class T>
struct is_tuple : std::false_type {};
template <class... T>
struct is_tuple<std::tuple<T...>> : std::true_type {};

// Yield / Progress / Request parameters are filled by the framework, the others from the inputs.
template <class T>
inline constexpr bool is_special_v = std::is_same_v<T, Yield> || std::is_same_v<T, Progress> || std::is_same_v<T, Request>;

template <class Args, size_t... J>
constexpr size_t count_plain(std::index_sequence<J...>) {
    return (size_t(0) + ... + (is_special_v<std::tuple_element_t<J, Args>> ? 0 : 1));
}
template <class Args, size_t... J>
constexpr bool has_yield(std::index_sequence<J...>) {
    return (false || ... || std::is_same_v<std::tuple_element_t<J, Args>, Yield>);
}
template <class Args>
struct last_is_yield : std::bool_constant<has_yield<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{})> {};

template <class R>
json result_to_json(R&& r) {
    json a = json::array();
    if constexpr (is_tuple<std::decay_t<R>>::value) {
        std::apply([&](auto&&... x) { (a.push_back(json(x)), ...); }, r);
    } else {
        a.push_back(json(std::forward<R>(r)));
    }
    return a;
}

template <class T, class Args, size_t I>
decltype(auto) arg_at(const json& args, const Yield& y, const Progress& p, const Request& r) {
    if constexpr (std::is_same_v<T, Yield>) return (y);
    else if constexpr (std::is_same_v<T, Progress>) return (p);
    else if constexpr (std::is_same_v<T, Request>) return (r);
    else return args[count_plain<Args>(std::make_index_sequence<I>{})].template get<T>();
}

template <class F, class Args, size_t... I>
json invoke_typed(F& fn, const json& args, const CallContext& ctx, std::index_sequence<I...>) {
    using R = typename fn_traits<F>::ret;
    Yield y(ctx.emit, ctx.progress);
    Progress pr(ctx.progress);
    if constexpr (last_is_yield<Args>::value) {
        fn(arg_at<std::tuple_element_t<I, Args>, Args, I>(args, y, pr, ctx.request)...);
        return nullptr;
    } else if constexpr (std::is_void_v<R>) {
        fn(arg_at<std::tuple_element_t<I, Args>, Args, I>(args, y, pr, ctx.request)...);
        return json::array();
    } else {
        return result_to_json(fn(arg_at<std::tuple_element_t<I, Args>, Args, I>(args, y, pr, ctx.request)...));
    }
}

template <class F>
Handler make_handler(F fn, bool& streaming, size_t& n_args) {
    using Args = typename fn_traits<F>::args;
    static constexpr size_t N = std::tuple_size_v<Args>;
    static constexpr size_t NA = count_plain<Args>(std::make_index_sequence<N>{});
    streaming = last_is_yield<Args>::value;
    n_args = NA;
    return [fn](const json& args, const CallContext& ctx) mutable -> json {
        if (!args.is_array() || args.size() < NA)
            throw Error("expected " + std::to_string(NA) + " input value(s), got " +
                        std::to_string(args.is_array() ? args.size() : 0));
        return invoke_typed<F, Args>(fn, args, ctx, std::make_index_sequence<N>{});
    };
}

class Semaphore {
public:
    explicit Semaphore(int n) : n_(n) {}
    void acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return n_ > 0; });
        --n_;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            ++n_;
        }
        cv_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int n_;
};

struct Cancelled {};

inline std::string csv_cell(const std::string& v) {
    std::string s = v;
    if (!s.empty() && std::string("=+-@").find(s[0]) != std::string::npos) s = "'" + s;  // no formulas in Excel
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

// Append one flagged sample to <dir>/dataset1.csv; files are copied to <dir>/<label>/<id>/.
inline void flag_row(const std::string& dir, const std::vector<std::string>& labels, const json& values, std::mutex& m) {
    std::lock_guard<std::mutex> lk(m);
    fs::path root = u8path(dir);
    fs::create_directories(root);
    std::vector<std::string> row;
    for (size_t i = 0; i < labels.size(); ++i) {
        const json v = i < values.size() ? values[i] : json();
        if (v.is_object() && v.contains("path") && v["path"].is_string()) {
            std::string src = v["path"].get<std::string>();
            if (!Cache::get().allowed(src)) {
                row.push_back("");
                continue;
            }
            fs::path d = root / u8path(sanitize_name(labels[i])) / random_hex(12);
            fs::create_directories(d);
            fs::path dst = d / u8path(sanitize_name(u8str(u8path(src).filename())));
            std::error_code ec;
            fs::copy_file(u8path(src), dst, fs::copy_options::overwrite_existing, ec);
            row.push_back(ec ? "" : u8str(fs::relative(dst, root, ec)));
        } else if (v.is_string()) {
            row.push_back(v.get<std::string>());
        } else if (v.is_null()) {
            row.push_back("");
        } else {
            row.push_back(v.dump(-1, ' ', false, json::error_handler_t::replace));
        }
    }
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    row.push_back(ts);

    fs::path file = root / "dataset1.csv";
    bool fresh = !fs::exists(file);
    std::ofstream f(file, std::ios::binary | std::ios::app);
    if (!f) throw Error("cannot write " + u8str(file));
    auto line = [&](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); ++i) f << (i ? "," : "") << csv_cell(cells[i]);
        f << "\r\n";
    };
    if (fresh) {
        std::vector<std::string> h = labels;
        h.push_back("timestamp");
        line(h);
    }
    line(row);
}

}  // namespace detail

struct EventOptions {
    std::string api_name;       // empty: "predict", "predict_1", ...
    int concurrency_limit = 1;  // <= 0: unlimited (Gradio's default is 1)
    bool show_api = true;
    bool streaming = false;     // on_raw only: the handler emits intermediate results
};

// Login check: return true to let (username, password) in.
using AuthFn = std::function<bool(const std::string& username, const std::string& password)>;

// A fixed list of users, compared in constant time: auth_users({{"admin", "secret"}})
inline AuthFn auth_users(std::map<std::string, std::string> users) {
    return [users = std::move(users)](const std::string& u, const std::string& pw) {
        auto it = users.find(u);
        const std::string& want = it != users.end() ? it->second : pw;
        unsigned char diff = (unsigned char)(want.size() != pw.size());
        for (size_t i = 0; i < want.size(); ++i) diff |= (unsigned char)(want[i] ^ (i < pw.size() ? pw[i] : 0));
        return it != users.end() && diff == 0;
    };
}

struct LaunchOptions {
    std::string host = "127.0.0.1";
    int port = 7860;
    bool auto_port = true;      // try the next ports if busy, like Gradio
    AuthFn auth;                // like launch(auth=...): set to require a login
    std::string auth_message;   // shown on the login page
    std::string root_path;      // e.g. "/gradio" when a reverse proxy serves the app under that sub-path
};

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------
class Blocks {
public:
    explicit Blocks(std::string title = "Gradio") : title_(std::move(title)) {
        stack_.push_back(0);
        children_[0];
    }
    Blocks(const Blocks&) = delete;
    Blocks& operator=(const Blocks&) = delete;

    // Add a component to the current container; returns its id.
    // Add a component inside a given container (a row/column id).
    int add_to(int parent, Component c) {
        stack_.push_back(parent);
        int id = add(std::move(c));
        stack_.pop_back();
        return id;
    }

    // Clickable examples that fill `inputs` (gr.Examples). Each sample has one value per input.
    int examples(const std::vector<std::vector<json>>& samples, const std::vector<int>& inputs,
                 const std::string& label = "Examples") {
        json headers = json::array(), types = json::array();
        for (int id : inputs) {
            const Component* c = find_comp(id);
            headers.push_back(c ? c->props.value("label", std::string()) : std::string());
            types.push_back(c ? c->type : std::string());
        }
        json rows = json::array();
        for (auto& sm : samples) {
            if (sm.size() != inputs.size())
                throw std::invalid_argument("each example needs " + std::to_string(inputs.size()) + " value(s)");
            rows.push_back(sm);
        }
        return add(Component("dataset", {{"label", label}, {"components", inputs}, {"component_types", types},
                                         {"headers", headers}, {"samples", rows}}));
    }

    int add(Component c) {
        c.id = next_id_++;
        int id = c.id;
        comps_.push_back(std::move(c));
        children_[stack_.back()].push_back(id);
        return id;
    }
    Component& component(int id) {
        for (auto& c : comps_)
            if (c.id == id) return c;
        throw std::out_of_range("no component with id " + std::to_string(id));
    }

    template <class Body>
    int row(Body body) { return container("row", body); }
    template <class Body>
    int column(Body body) { return container("column", body); }
    // tabs([&]{ tab("A", [&]{...}); tab("B", [&]{...}); })
    template <class Body>
    int tabs(Body body) { return container("tabs", body); }
    template <class Body>
    int tab(const std::string& label, Body body) { return container("tabitem", body, {{"label", label}}); }
    template <class Body>
    int accordion(const std::string& label, Body body, bool open = true) {
        return container("accordion", body, {{"label", label}, {"open", open}});
    }

    // Register an event handler; returns the event id (for then() / cancel()).
    template <class F>
    int on(std::vector<std::pair<int, std::string>> targets, F fn, std::vector<int> inputs,
           std::vector<int> outputs, EventOptions opt = {}) {
        Dep d;
        d.targets = std::move(targets);
        d.inputs = std::move(inputs);
        d.outputs = std::move(outputs);
        d.fn = detail::make_handler(std::move(fn), d.streaming, d.n_args);
        if (d.n_args != d.inputs.size())
            throw std::invalid_argument("handler takes " + std::to_string(d.n_args) + " argument(s) but " +
                                        std::to_string(d.inputs.size()) + " input(s) were given");
        d.api_name = unique_api_name(opt.api_name.empty() ? "predict" : opt.api_name);
        d.show_api = opt.show_api;
        if (opt.concurrency_limit > 0) d.sem = std::make_shared<detail::Semaphore>(opt.concurrency_limit);
        deps_.push_back(std::move(d));
        return (int)deps_.size() - 1;
    }
    template <class F>
    int click(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "click"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }
    template <class F>
    int submit(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "submit"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }
    template <class F>
    int change(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "change"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }
    // Untyped handler: gets the raw input list, returns the output list.
    int on_raw(std::vector<std::pair<int, std::string>> targets, Handler fn, std::vector<int> inputs,
               std::vector<int> outputs, EventOptions opt = {}) {
        Dep d;
        d.targets = std::move(targets);
        d.inputs = std::move(inputs);
        d.outputs = std::move(outputs);
        d.n_args = d.inputs.size();
        d.fn = std::move(fn);
        d.streaming = opt.streaming;
        d.api_name = unique_api_name(opt.api_name.empty() ? "predict" : opt.api_name);
        d.show_api = opt.show_api;
        if (opt.concurrency_limit > 0) d.sem = std::make_shared<detail::Semaphore>(opt.concurrency_limit);
        deps_.push_back(std::move(d));
        return (int)deps_.size() - 1;
    }

    // Flag button: appends the values of `components` to <dir>/dataset1.csv (Gradio's CSVLogger
    // layout: one column per label plus "timestamp"; files are copied next to it).
    int flagging(int button, std::vector<int> components, const std::string& dir = ".gradio/flagged") {
        std::vector<std::string> labels;
        for (size_t i = 0; i < components.size(); ++i) {
            const Component* c = find_comp(components[i]);
            std::string l = c ? c->props.value("label", std::string()) : std::string();
            labels.push_back(l.empty() ? "component " + std::to_string(i) : l);
        }
        auto mtx = std::make_shared<std::mutex>();
        return on_raw({{button, "click"}},
                      [labels, dir, mtx](const json& args, const CallContext&) -> json {
                          detail::flag_row(dir, labels, args, *mtx);
                          return json::array();
                      },
                      components, {}, {"flag", 0, false});
    }

    // Run after event `after` finishes successfully (like Gradio's .then / .success).
    template <class F>
    int then(int after, F fn, std::vector<int> inputs, std::vector<int> outputs, EventOptions opt = {}) {
        int id = on({}, std::move(fn), std::move(inputs), std::move(outputs), std::move(opt));
        deps_[id].trigger_after = after;
        return id;
    }
    // Clicking `trigger` aborts the running events (Gradio's cancels=[...]).
    int cancel(int trigger, std::vector<int> events, const std::string& event = "click") {
        Dep d;
        d.targets = {{trigger, event}};
        d.cancels = std::move(events);
        deps_.push_back(std::move(d));
        return (int)deps_.size() - 1;
    }

    void set_mode(const std::string& m) { mode_ = m; }
    void stack_push(int container) { stack_.push_back(container); }
    void stack_pop() { stack_.pop_back(); }

    json config() const {
        std::set<int> as_input, as_output;
        for (auto& d : deps_) {
            as_input.insert(d.inputs.begin(), d.inputs.end());
            as_output.insert(d.outputs.begin(), d.outputs.end());
        }
        json comps = json::array();
        comps.push_back({{"id", 0}, {"type", "form"}, {"props", json::object()}});
        for (auto& c : comps_) {
            json props = c.props;
            if (!props.contains("interactive") && !is_layout_type(c.type))
                props["interactive"] = !(as_output.count(c.id) && !as_input.count(c.id));
            if (!props.contains("visible")) props["visible"] = true;
            props["name"] = c.type;
            if (skip_api_type(c.type)) {
                comps.push_back({{"id", c.id}, {"type", c.type}, {"props", props}, {"skip_api", true}});
                continue;
            }
            comps.push_back({{"id", c.id}, {"type", c.type}, {"props", props}});
        }
        json deps = json::array();
        for (size_t i = 0; i < deps_.size(); ++i) {
            auto& d = deps_[i];
            json targets = json::array();
            for (auto& t : d.targets) targets.push_back({t.first, t.second});
            bool backend = (bool)d.fn;
            deps.push_back({{"id", i}, {"targets", targets}, {"inputs", d.inputs}, {"outputs", d.outputs},
                            {"api_name", backend ? json(d.api_name) : json(nullptr)}, {"backend_fn", backend},
                            {"js", nullptr}, {"queue", true}, {"show_api", backend && d.show_api},
                            {"cancels", d.cancels},
                            {"trigger_after", d.trigger_after < 0 ? json(nullptr) : json(d.trigger_after)},
                            {"trigger_only_on_success", d.trigger_after >= 0},
                            {"types", {{"generator", d.streaming}, {"cancel", !backend}}}});
        }
        // "version" is the Gradio API level we speak (gradio_client parses it as a version
        // number); our own version is in "gradio_cpp_version".
        return {{"version", gradio_api_version},
                {"gradio_cpp_version", version},
                {"protocol", "sse_v3"},
                {"connect_heartbeat", false},
                {"auth_required", (bool)auth_},
                {"root", detail::root_path()},
                {"auth_message", auth_message_},
                {"mode", mode_},
                {"title", title_},
                {"api_prefix", "/gradio_api"},
                {"components", comps},
                {"dependencies", deps},
                {"layout", layout(0)}};
    }

    json api_info() const {
        json named = json::object();
        for (auto& d : deps_) {
            if (!d.show_api || !d.fn) continue;
            json params = json::array(), rets = json::array();
            std::set<std::string> used;
            for (size_t k = 0; k < d.inputs.size(); ++k) {
                if (const Component* c = find_comp(d.inputs[k]); c && skip_api_type(c->type)) continue;
                json pi = param_info(d.inputs[k], true);
                std::string name = parameter_name(pi["label"].is_string() ? pi["label"].get<std::string>() : "", k);
                while (used.count(name)) name += "_";
                used.insert(name);
                pi["parameter_name"] = name;
                params.push_back(pi);
            }
            for (int id : d.outputs)
                if (const Component* c = find_comp(id); !c || !skip_api_type(c->type)) rets.push_back(param_info(id, false));
            named["/" + d.api_name] = {{"parameters", params}, {"returns", rets}};
        }
        return {{"named_endpoints", named}, {"unnamed_endpoints", json::object()}};
    }

    // Require a login (Gradio's auth=). Call before mount(); launch() does it from LaunchOptions.
    void set_auth(AuthFn fn, std::string message = "") {
        auth_ = std::move(fn);
        auth_message_ = std::move(message);
    }

    // The user logged in with this request's cookie ("" when none / auth is off).
    std::string user_of(const httplib::Request& req) const {
        std::string cookie = req.get_header_value("Cookie"), key = cookie_name() + "=";
        for (size_t pos = 0; (pos = cookie.find(key, pos)) != std::string::npos; pos += key.size()) {
            if (pos > 0 && cookie[pos - 1] != ' ' && cookie[pos - 1] != ';') continue;
            size_t end = cookie.find(';', pos);
            std::string tok = cookie.substr(pos + key.size(), end == std::string::npos ? std::string::npos : end - pos - key.size());
            std::lock_guard<std::mutex> lk(tokens_m_);
            auto it = tokens_.find(tok);
            if (it != tokens_.end()) return it->second;
        }
        return "";
    }

    // Register all routes on an existing server.
    void mount(httplib::Server& svr) {
        mount_auth(svr);
        svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(frontend_html(), "text/html; charset=utf-8");
        });
        svr.Get("/config", [this](const httplib::Request& req, httplib::Response& res) {
            json c = config();
            c["root"] = make_request(req, "").origin() + detail::root_path();
            res.set_content(c.dump(), "application/json");
        });
        svr.Get("/gradio_api/info", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(api_info().dump(), "application/json");
        });
        svr.Post(R"(/gradio_api/call/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            const Dep* d = find_api(req.matches[1]);
            if (!d) return not_found(res);
            json body = json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("data") || !body["data"].is_array()) {
                res.status = 422;
                res.set_content(json{{"detail", "body must be {\"data\": [...]}"}}.dump(), "application/json");
                return;
            }
            std::string id = detail::random_hex(32);
            {
                std::lock_guard<std::mutex> lk(events_m_);
                auto now = std::chrono::steady_clock::now();
                for (auto it = events_.begin(); it != events_.end();)
                    it = (now - it->second.created > std::chrono::minutes(10)) ? events_.erase(it) : std::next(it);
                events_[id] = {make_request(req, ""), (size_t)(d - deps_.data()), body["data"], now};
            }
            res.set_content(json{{"event_id", id}}.dump(), "application/json");
        });
        svr.Get(R"(/gradio_api/call/([^/]+)/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            const Dep* d = find_api(req.matches[1]);
            if (!d) return not_found(res);
            Event ev;
            {
                std::lock_guard<std::mutex> lk(events_m_);
                auto it = events_.find(req.matches[2]);
                if (it == events_.end() || it->second.dep != (size_t)(d - deps_.data())) return not_found(res);
                ev = std::move(it->second);
                events_.erase(it);
            }
            res.set_header("Cache-Control", "no-cache");
            res.set_chunked_content_provider("text/event-stream", [this, ev](size_t, httplib::DataSink& sink) {
                run_event(ev, sink);
                return true;
            });
        });
        mount_queue(svr);
        // Uploads are written to disk as they arrive (no whole-file buffering).
        svr.Post("/gradio_api/upload",
                 [](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
                     if (!req.is_multipart_form_data()) {
                         res.status = 400;
                         res.set_content(json{{"detail", "expected multipart/form-data with \"files\""}}.dump(),
                                         "application/json");
                         return;
                     }
                     json paths = json::array();
                     std::ofstream out;
                     bool ok = true;
                     reader(
                         [&](const httplib::MultipartFormData& part) {
                             if (out.is_open()) out.close();
                             if (part.name != "files") return true;
                             paths.push_back(detail::Cache::get().create(part.filename, out));
                             return (bool)out;
                         },
                         [&](const char* data, size_t len) {
                             if (out.is_open()) ok = ok && (bool)out.write(data, (std::streamsize)len);
                             return ok;
                         });
                     if (out.is_open()) out.close();
                     if (!ok) {
                         res.status = 500;
                         res.set_content(json{{"detail", "cannot store upload"}}.dump(), "application/json");
                         return;
                     }
                     res.set_content(paths.dump(), "application/json");
                 });
        svr.Get(R"(/gradio_api/file=(.+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string p = req.matches[1];
            if (!detail::Cache::get().allowed(p)) {
                res.status = 403;
                res.set_content(json{{"detail", "file not allowed"}}.dump(), "application/json");
                return;
            }
            // Streamed in 64 KB pieces; httplib serves Range requests (video seeking) from it.
            fs::path path = detail::u8path(p);
            std::error_code ec;
            auto size = fs::file_size(path, ec);
            if (ec) return not_found(res);
            auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
            if (!*file) return not_found(res);
            res.set_header("Accept-Ranges", "bytes");
            res.set_content_provider((size_t)size, detail::guess_mime(p),
                                     [file](size_t offset, size_t length, httplib::DataSink& sink) {
                                         std::vector<char> buf(std::min<size_t>(length, 64 * 1024));
                                         file->clear();
                                         file->seekg((std::streamoff)offset);
                                         file->read(buf.data(), (std::streamsize)buf.size());
                                         auto n = (size_t)file->gcount();
                                         return n > 0 && sink.write(buf.data(), n);
                                     });
        });
    }

    void launch(LaunchOptions opt = {}) {
        httplib::Server svr;
        if (opt.auth) set_auth(opt.auth, opt.auth_message);
        detail::root_path() = opt.root_path;
        while (!detail::root_path().empty() && detail::root_path().back() == '/') detail::root_path().pop_back();
        mount(svr);
        int port = opt.port;
        int last = opt.auto_port ? port + 99 : port;
        while (!svr.bind_to_port(opt.host, port)) {
            if (++port > last)
                throw std::runtime_error("cannot bind " + opt.host + ":" + std::to_string(opt.port));
        }
        std::cout << "* Running on local URL:  http://" << (opt.host == "0.0.0.0" ? "127.0.0.1" : opt.host) << ":"
                  << port << std::endl;
        svr.listen_after_bind();
    }
    void launch(int port) {
        LaunchOptions o;
        o.port = port;
        launch(o);
    }

private:
    struct Dep {
        std::vector<std::pair<int, std::string>> targets;
        std::vector<int> inputs, outputs;
        std::string api_name;
        Handler fn;
        bool streaming = false;
        bool show_api = true;
        size_t n_args = 0;
        std::shared_ptr<detail::Semaphore> sem;
        std::vector<int> cancels;
        int trigger_after = -1;
    };
    struct Event {
        Request request;
        size_t dep = 0;
        json data;
        std::chrono::steady_clock::time_point created;
    };

    template <class Body>
    int container(const std::string& type, Body& body, json props = json::object()) {
        int id = add(Component(type, std::move(props)));
        children_[id];
        stack_.push_back(id);
        try {
            body();
        } catch (...) {
            stack_.pop_back();
            throw;
        }
        stack_.pop_back();
        return id;
    }

    json layout(int id) const {
        json node = {{"id", id}};
        auto it = children_.find(id);
        if (it != children_.end()) {
            json ch = json::array();
            for (int c : it->second) ch.push_back(layout(c));
            node["children"] = ch;
        }
        return node;
    }

    std::string unique_api_name(const std::string& base) const {
        auto taken = [&](const std::string& n) {
            for (auto& d : deps_)
                if (d.api_name == n) return true;
            return false;
        };
        if (!taken(base)) return base;
        for (int i = 1;; ++i)
            if (!taken(base + "_" + std::to_string(i))) return base + "_" + std::to_string(i);
    }

    const Dep* find_api(const std::string& name) const {
        for (auto& d : deps_)
            if (d.fn && d.api_name == name) return &d;
        return nullptr;
    }

    const Component* find_comp(int id) const {
        for (auto& c : comps_)
            if (c.id == id) return &c;
        return nullptr;
    }

    json param_info(int id, bool input) const {
        const Component* c = find_comp(id);
        std::string type = c ? c->type : "";
        json t, py;
        if (type == "textbox" || type == "markdown") {
            t = {{"type", "string"}};
            py = {{"type", "str"}, {"description", ""}};
        } else if (type == "number" || type == "slider") {
            t = {{"type", "number"}};
            py = {{"type", "float"}, {"description", ""}};
        } else if (type == "checkbox") {
            t = {{"type", "boolean"}};
            py = {{"type", "bool"}, {"description", ""}};
        } else if ((type == "dropdown" && !(c && c->props.value("multiselect", false))) || type == "radio") {
            json e = json::array();
            for (auto& ch : c->props.value("choices", json::array())) e.push_back(ch[1]);
            t = {{"type", "string"}, {"enum", e}};
            py = {{"type", "Literal[" + e.dump().substr(1, e.dump().size() - 2) + "]"}, {"description", ""}};
        } else if (type == "dropdown" || type == "checkboxgroup") {
            t = {{"type", "array"}, {"items", {{"type", "string"}}}};
            py = {{"type", "list[str]"}, {"description", ""}};
        } else if (type == "multimodaltextbox") {
            t = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}, {"files", {{"type", "array"}}}}}};
            py = {{"type", "dict(text: str, files: list[filepath])"}, {"description", ""}};
        } else if (type == "chatbot") {
            t = {{"type", "array"}};
            py = {{"type", "list[dict(role, content, metadata)]"}, {"description", ""}};
        } else if (type == "gallery") {
            t = {{"type", "array"}};
            py = {{"type", "list[dict(image: filepath, caption: str | None)]"}, {"description", ""}};
        } else if (is_file_type(type)) {
            t = {{"type", "object"}, {"additionalProperties", false}, {"title", "FileData"},
                 {"properties", {{"path", {{"type", "string"}}}, {"url", {{"type", "string"}}},
                                 {"orig_name", {{"type", "string"}}}, {"mime_type", {{"type", "string"}}},
                                 {"size", {{"type", "integer"}}}, {"meta", {{"type", "object"}}}}},
                 {"required", {"path"}}};
            py = {{"type", "filepath"}, {"description", ""}};
        } else {
            t = json::object();
            py = {{"type", "Any"}, {"description", ""}};
        }
        json info = {{"label", c && c->props.contains("label") ? c->props["label"] : json(nullptr)},
                     {"type", t}, {"python_type", py}, {"component", type}};
        if (input) {
            info["parameter_has_default"] = false;
            info["parameter_default"] = nullptr;
            info["example_input"] = example_input(c);
        }
        return info;
    }

    static std::string parameter_name(const std::string& label, size_t index) {
        std::string n;
        for (unsigned char ch : label) {
            if (std::isalnum(ch)) n += (char)std::tolower(ch);
            else if (ch == ' ' || ch == '_' || ch == '-') n += '_';
        }
        if (n.empty() || std::isdigit((unsigned char)n[0])) n = "param_" + std::to_string(index);
        return n;
    }

    static json example_input(const Component* c) {
        if (!c) return nullptr;
        const std::string& t = c->type;
        if (t == "textbox") return "Hello!!";
        if (t == "number") return 3;
        if (t == "slider") return c->props.value("minimum", 0.0);
        if (t == "checkbox") return true;
        if ((t == "dropdown" || t == "radio") && !c->props.value("choices", json::array()).empty()) {
            json v = c->props["choices"][0][1];
            return c->props.value("multiselect", false) ? json::array({v}) : v;
        }
        if (t == "checkboxgroup") return json::array();
        if (t == "multimodaltextbox") return {{"text", "Hello!!"}, {"files", json::array()}};
        if (is_file_type(t))
            return {{"path", "https://raw.githubusercontent.com/gradio-app/gradio/main/test/test_files/bus.png"},
                    {"meta", {{"_type", "gradio.FileData"}}}};
        return nullptr;
    }

    // Reject input files that are neither uploads nor files we handed out.
    static void check_file(const FileData& f) {
        if (f.path.empty()) throw Error("file input has no path; upload it to /gradio_api/upload first");
        if (f.path.rfind("http://", 0) == 0 || f.path.rfind("https://", 0) == 0)
            throw Error("remote URLs are not supported yet; upload the file to /gradio_api/upload first");
        if (!detail::Cache::get().allowed(f.path)) throw Error("file not allowed: " + f.path);
    }
    // Any object carrying a "path" inside chat / multimodal values is a file reference.
    static void check_nested(const json& j) {
        if (j.is_object()) {
            if (j.contains("path") && j["path"].is_string()) check_file(j.get<FileData>());
            for (auto& [k, v] : j.items()) check_nested(v);
        } else if (j.is_array()) {
            for (auto& v : j) check_nested(v);
        }
    }
    void check_input_files(const Dep& d, const json& data) const {
        for (size_t i = 0; i < d.inputs.size() && i < data.size(); ++i) {
            const Component* c = find_comp(d.inputs[i]);
            if (!c || data[i].is_null()) continue;
            if (is_file_type(c->type)) check_file(data[i].get<FileData>());
            else if (has_nested_files(c->type)) check_nested(data[i]);
        }
    }

    // Run a handler: input checks, concurrency limit, streaming. Returns the final
    // outputs. emit may throw detail::Cancelled to abort.
    json execute(const Dep& d, json data, const Emit& emit, const ProgressFn& progress = {},
                 std::map<int, json>* state = nullptr, const Request& request = {}) {
        for (size_t i = 0; i < d.inputs.size(); ++i) {
            const Component* c = find_comp(d.inputs[i]);
            if (!c || c->type != "state") continue;
            while (data.size() <= i) data.push_back(nullptr);
            if (state && state->count(c->id)) data[i] = (*state)[c->id];
            else if (data[i].is_null()) data[i] = c->props.value("value", json());
        }
        check_input_files(d, data);
        struct Guard {
            detail::Semaphore* s;
            ~Guard() {
                if (s) s->release();
            }
        } guard{nullptr};
        if (d.sem) {
            d.sem->acquire();
            guard.s = d.sem.get();
        }
        json last = json::array();
        CallContext ctx{[&](const json& o) {
                            json v = o;
                            postprocess(d, v);
                            absolutize(v, request.origin());
                            last = v;
                            emit(v);
                        },
                        progress, request};
        json out = d.fn(data, ctx);
        if (out.is_null()) out = last;
        postprocess(d, out);
        absolutize(out, request.origin());
        if (state)
            for (size_t i = 0; i < d.outputs.size() && i < out.size(); ++i)
                if (const Component* c = find_comp(d.outputs[i]); c && c->type == "state") (*state)[c->id] = out[i];
        return out;
    }

    // File URLs go out absolute ("http://host/gradio_api/file=..."), like Gradio's.
    static void absolutize(json& j, const std::string& origin) {
        if (origin.empty()) return;
        if (j.is_object()) {
            if (j.contains("path") && j.contains("url") && j["url"].is_string()) {
                const std::string& u = j["url"].get_ref<const std::string&>();
                if (!u.empty() && u[0] == '/') j["url"] = origin + u;
            }
            for (auto& [k, v] : j.items()) absolutize(v, origin);
        } else if (j.is_array()) {
            for (auto& v : j) absolutize(v, origin);
        }
    }

    // A DataFrame returned for a native plot becomes {columns, data, datatypes, mark}.
    void postprocess(const Dep& d, json& out) const {
        if (!out.is_array()) return;
        for (size_t i = 0; i < d.outputs.size() && i < out.size(); ++i) {
            const Component* c = find_comp(d.outputs[i]);
            if (!c || c->type != "nativeplot" || !out[i].is_object() || !out[i].contains("headers")) continue;
            json df = out[i];
            json types = json::object();
            const json& rows = df.value("data", json::array());
            const json& heads = df["headers"];
            for (size_t k = 0; k < heads.size(); ++k) {
                bool num = !rows.empty();
                for (auto& r : rows)
                    if (k < r.size() && !r[k].is_number()) num = false;
                types[heads[k].get<std::string>()] = num ? "quantitative" : "nominal";
            }
            out[i] = {{"columns", heads}, {"data", rows}, {"datatypes", types},
                      {"mark", c->props.value("_mark", std::string("line"))}};
        }
    }

    static std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

    // /gradio_api/call/<name>/<id>: event-named SSE (generating / complete / error).
    void run_event(const Event& ev, httplib::DataSink& sink) {
        auto send = [&](const std::string& event, const json& data) {
            std::string msg = "event: " + event + "\ndata: " + dump(data) + "\n\n";
            if (!sink.write(msg.data(), msg.size())) throw detail::Cancelled{};
        };
        try {
            json out = execute(deps_[ev.dep], ev.data, [&](const json& o) { send("generating", o); },
                               [&](double f, const std::string& desc) {
                                   send("progress", {{"progress", f}, {"desc", desc}});
                               },
                               nullptr, ev.request);
            send("complete", out);
        } catch (const detail::Cancelled&) {
        } catch (const std::exception& e) {
            try {
                send("error", json(std::string(e.what())));
            } catch (...) {
            }
        } catch (...) {
            try {
                send("error", json("unknown error"));
            } catch (...) {
            }
        }
        sink.done();
    }

    // ---- Gradio queue protocol (sse_v3), used by gradio_client ----
    struct Session {
        std::mutex m;
        std::condition_variable cv;
        std::deque<json> q;
        int pending = 0;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> running;  // event_id -> cancel flag
        std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
        std::map<int, json> state;  // gr::State values of this session (gradio_client path)
    };

    std::shared_ptr<Session> session(const std::string& hash, bool create) {
        std::lock_guard<std::mutex> lk(sessions_m_);
        auto now = std::chrono::steady_clock::now();
        auto it = sessions_.find(hash);
        if (it != sessions_.end()) {
            it->second->touched = now;
            return it->second;
        }
        if (!create) return nullptr;
        for (auto i = sessions_.begin(); i != sessions_.end();) {  // drop idle sessions
            bool idle;
            {
                std::lock_guard<std::mutex> sl(i->second->m);
                idle = i->second->pending == 0 && now - i->second->touched > std::chrono::hours(1);
            }
            i = idle ? sessions_.erase(i) : std::next(i);
        }
        auto s = std::make_shared<Session>();
        sessions_[hash] = s;
        return s;
    }

    static void push(Session& s, json msg) {
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.q.push_back(std::move(msg));
        }
        s.cv.notify_all();
    }

    // Gradio streams generator outputs as diffs after the first message.
    static json diff(const json& prev, const json& cur) {
        if (prev.is_string() && cur.is_string()) {
            const std::string &a = prev.get_ref<const std::string&>(), &b = cur.get_ref<const std::string&>();
            if (b.size() >= a.size() && b.compare(0, a.size(), a) == 0)
                return json::array({json::array({"append", json::array(), b.substr(a.size())})});
        }
        return json::array({json::array({"replace", json::array(), cur})});
    }

    void run_queued(size_t dep, json data, std::string event_id, Request request, std::shared_ptr<Session> s,
                    std::shared_ptr<std::atomic<bool>> cancelled) {
        const Dep& d = deps_[dep];
        push(*s, {{"msg", "process_starts"}, {"event_id", event_id}, {"eta", nullptr}});
        json sent;
        json done;
        std::map<int, json> st;  // a copy, so concurrent events of one session don't race on it
        {
            std::lock_guard<std::mutex> lk(s->m);
            st = s->state;
        }
        try {
            json out = execute(d, data, [&](const json& o) {
                if (*cancelled) throw detail::Cancelled{};
                json payload = o;
                if (sent.is_array() && sent.size() == o.size()) {
                    payload = json::array();
                    for (size_t i = 0; i < o.size(); ++i) payload.push_back(diff(sent[i], o[i]));
                }
                sent = o;
                push(*s, {{"msg", "process_generating"}, {"event_id", event_id}, {"success", true},
                          {"output", {{"data", payload}, {"is_generating", true}}}});
            }, [&](double f, const std::string& desc) {
                if (*cancelled) throw detail::Cancelled{};
                push(*s, {{"msg", "progress"}, {"event_id", event_id},
                          {"progress_data", json::array({{{"index", nullptr}, {"length", nullptr}, {"unit", "steps"},
                                                          {"progress", f}, {"desc", desc}}})}});
            }, &st, request);
            {
                std::lock_guard<std::mutex> lk(s->m);
                for (auto& [k, v] : st) s->state[k] = v;
            }
            done = {{"msg", "process_completed"}, {"event_id", event_id}, {"success", true},
                    {"output", {{"data", out}, {"is_generating", false}}}};
        } catch (const detail::Cancelled&) {
            done = {{"msg", "process_completed"}, {"event_id", event_id}, {"success", false},
                    {"output", {{"error", "cancelled"}}}};
        } catch (const std::exception& e) {
            done = {{"msg", "process_completed"}, {"event_id", event_id}, {"success", false},
                    {"output", {{"error", std::string(e.what())}}}};
        } catch (...) {
            done = {{"msg", "process_completed"}, {"event_id", event_id}, {"success", false},
                    {"output", {{"error", "unknown error"}}}};
        }
        {
            std::lock_guard<std::mutex> lk(s->m);
            s->q.push_back(std::move(done));
            s->running.erase(event_id);
            --s->pending;
        }
        s->cv.notify_all();
    }

    void mount_queue(httplib::Server& svr) {
        svr.Post("/gradio_api/queue/join", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body, nullptr, false);
            auto bad = [&](const std::string& why) {
                res.status = 422;
                res.set_content(json{{"detail", why}}.dump(), "application/json");
            };
            if (body.is_discarded() || !body.is_object()) return bad("invalid JSON");
            if (!body.contains("fn_index") || !body["fn_index"].is_number_integer()) return bad("fn_index required");
            if (!body.contains("session_hash") || !body["session_hash"].is_string()) return bad("session_hash required");
            long long fi = body["fn_index"].get<long long>();
            if (fi < 0 || fi >= (long long)deps_.size() || !deps_[(size_t)fi].fn) return bad("invalid fn_index");
            json data = body.value("data", json::array());
            if (!data.is_array()) return bad("data must be a list");
            std::string event_id = detail::random_hex(32);
            auto s = session(body["session_hash"].get<std::string>(), true);
            auto flag = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lk(s->m);
                ++s->pending;
                s->running[event_id] = flag;
                s->q.push_back({{"msg", "estimation"}, {"event_id", event_id}, {"rank", 0},
                                {"queue_size", s->pending}, {"rank_eta", nullptr}});
            }
            s->cv.notify_all();
            std::thread(&Blocks::run_queued, this, (size_t)fi, std::move(data), event_id,
                        make_request(req, body["session_hash"].get<std::string>()), s, flag)
                .detach();
            res.set_content(json{{"event_id", event_id}}.dump(), "application/json");
        });
        svr.Get("/gradio_api/queue/data", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = session(req.get_param_value("session_hash"), false);
            res.set_header("Cache-Control", "no-cache");
            res.set_chunked_content_provider("text/event-stream", [s](size_t, httplib::DataSink& sink) {
                auto write = [&](const json& m) {
                    std::string line = "data: " + dump(m) + "\n\n";
                    return sink.write(line.data(), line.size());
                };
                if (!s) {
                    write({{"msg", "close_stream"}});
                    sink.done();
                    return true;
                }
                for (;;) {
                    std::deque<json> batch;
                    bool finished = false;
                    {
                        std::unique_lock<std::mutex> lk(s->m);
                        s->cv.wait_for(lk, std::chrono::seconds(15), [&] { return !s->q.empty() || s->pending == 0; });
                        batch.swap(s->q);
                        finished = s->pending == 0;
                    }
                    bool ok = true;
                    for (auto& m : batch) ok = ok && write(m);
                    if (ok && batch.empty() && !finished) ok = write({{"msg", "heartbeat"}});
                    if (!ok) {  // client went away: stop its jobs
                        std::lock_guard<std::mutex> lk(s->m);
                        for (auto& [id, f] : s->running) *f = true;
                        return false;
                    }
                    if (finished) {
                        std::lock_guard<std::mutex> lk(s->m);
                        if (s->pending == 0 && s->q.empty()) break;
                    }
                }
                write({{"msg", "close_stream"}});
                sink.done();
                return true;
            });
        });
        svr.Post("/gradio_api/cancel", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body, nullptr, false);
            bool found = false;
            if (!body.is_discarded() && body.is_object() && body.contains("session_hash") && body.contains("event_id") &&
                body["session_hash"].is_string() && body["event_id"].is_string()) {
                if (auto s = session(body["session_hash"].get<std::string>(), false)) {
                    std::lock_guard<std::mutex> lk(s->m);
                    auto it = s->running.find(body["event_id"].get<std::string>());
                    if (it != s->running.end()) {
                        *it->second = true;
                        found = true;
                    }
                }
            }
            res.set_content(json{{"success", found}}.dump(), "application/json");
        });
    }

    static void not_found(httplib::Response& res) {
        res.status = 404;
        res.set_content(json{{"detail", "Not Found"}}.dump(), "application/json");
    }

    std::string title_;
    std::string mode_ = "blocks";
    int next_id_ = 1;
    std::vector<Component> comps_;
    std::map<int, std::vector<int>> children_;
    std::vector<int> stack_;
    std::vector<Dep> deps_;
    std::mutex events_m_;
    std::map<std::string, Event> events_;
    std::string cookie_name() const { return "access-token-" + cookie_id_; }

    Request make_request(const httplib::Request& req, const std::string& session) const {
        Request r;
        r.username = user_of(req);
        r.client_host = req.remote_addr;
        r.session_hash = session;
        for (auto& [k, v] : req.headers)
            if (detail::lower(k) != "cookie") r.headers[detail::lower(k)] = v;
        return r;
    }

    void mount_auth(httplib::Server& svr) {
        if (!auth_) return;
        // Everything except the page itself and the login routes needs a login.
        svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            static const std::set<std::string> open = {"/", "/login", "/login/", "/logout", "/login_check",
                                                       "/login_check/", "/favicon.ico"};
            if (open.count(req.path) || !user_of(req).empty()) return httplib::Server::HandlerResponse::Unhandled;
            res.status = 401;
            res.set_content(json{{"detail", {{"error", "Not authenticated"}, {"auth_message", auth_message_}}}}.dump(),
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });
        auto login = [this](const httplib::Request& req, httplib::Response& res) {
            auto field = [&](const char* k) {  // form-urlencoded (browsers, gradio_client) or multipart (@gradio/client)
                return req.has_param(k) ? req.get_param_value(k) : req.has_file(k) ? req.get_file_value(k).content : std::string();
            };
            std::string u = field("username"), pw = field("password");
            while (!u.empty() && std::isspace((unsigned char)u.back())) u.pop_back();
            while (!u.empty() && std::isspace((unsigned char)u.front())) u.erase(0, 1);
            if (u.empty() || !auth_(u, pw)) {
                res.status = 400;
                res.set_content(json{{"detail", "Incorrect credentials."}}.dump(), "application/json");
                return;
            }
            std::string tok = detail::random_hex(32);
            {
                std::lock_guard<std::mutex> lk(tokens_m_);
                tokens_[tok] = u;
            }
            res.set_header("Set-Cookie", cookie_name() + "=" + tok + "; Path=/; HttpOnly; SameSite=Lax");
            res.set_content(json{{"success", true}}.dump(), "application/json");
        };
        svr.Post("/login", login);
        svr.Post("/login/", login);
        auto check = [this](const httplib::Request& req, httplib::Response& res) {
            if (user_of(req).empty()) {
                res.status = 401;
                res.set_content(json{{"detail", {{"error", "Not authenticated"}, {"auth_message", auth_message_}}}}.dump(),
                                "application/json");
            } else {
                res.set_content("null", "application/json");
            }
        };
        svr.Get("/login_check", check);
        svr.Get("/login_check/", check);
        svr.Get("/logout", [this](const httplib::Request& req, httplib::Response& res) {
            std::string u = user_of(req);
            if (!u.empty()) {  // like Gradio: log this user out everywhere
                std::lock_guard<std::mutex> lk(tokens_m_);
                for (auto it = tokens_.begin(); it != tokens_.end();) it = it->second == u ? tokens_.erase(it) : std::next(it);
            }
            res.set_header("Set-Cookie", cookie_name() + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
            res.set_redirect(detail::root_path() + "/");
        });
    }

    AuthFn auth_;
    std::string auth_message_;
    std::string cookie_id_ = detail::random_hex(12);
    mutable std::mutex tokens_m_;
    std::map<std::string, std::string> tokens_;  // token -> username
    std::mutex sessions_m_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
};

// ---------------------------------------------------------------------------
// Interface: inputs on the left, outputs on the right, Clear / Submit buttons
// ---------------------------------------------------------------------------
class Interface {
public:
    template <class F>
    Interface(F fn, std::vector<Component> inputs, std::vector<Component> outputs, const std::string& title = "",
              const std::string& description = "")
        : blocks_(title.empty() ? "Gradio" : title) {
        blocks_.set_mode("interface");
        if (!title.empty()) blocks_.add(Markdown("# " + title).set("elem_classes", "title"));
        if (!description.empty()) blocks_.add(Markdown(description));
        std::vector<int> in, out;
        std::vector<std::pair<int, std::string>> targets;
        int clear = -1;
        blocks_.row([&] {
            input_col_ = blocks_.column([&] {
                for (size_t i = 0; i < inputs.size(); ++i) {
                    auto& c = inputs[i];
                    if (c.props.value("label", std::string()).empty())
                        c.label(inputs.size() == 1 ? "input" : "input " + std::to_string(i));
                    int id = blocks_.add(c);
                    in.push_back(id);
                    if (c.type == "textbox" && c.props.value("lines", 1) == 1) targets.push_back({id, "submit"});
                }
                blocks_.row([&] {
                    clear = blocks_.add(ClearButton());
                    int submit = blocks_.add(Button("Submit", "primary"));
                    targets.insert(targets.begin(), {submit, "click"});
                });
            });
            blocks_.column([&] {
                for (size_t i = 0; i < outputs.size(); ++i) {
                    auto& c = outputs[i];
                    if (c.props.value("label", std::string()).empty())
                        c.label(outputs.size() == 1 ? "output" : "output " + std::to_string(i));
                    out.push_back(blocks_.add(c));
                }
                flag_btn_ = blocks_.add(Button("Flag"));
            });
        });
        std::vector<int> all = in;
        all.insert(all.end(), out.begin(), out.end());
        blocks_.component(clear).set("components", all);
        blocks_.on(targets, std::move(fn), in, out, {"predict"});
        inputs_ = in;
        blocks_.flagging(flag_btn_, all);
    }

    // Example inputs shown under the input column; one value per input.
    Interface& examples(const std::vector<std::vector<json>>& samples) {
        blocks_.stack_push(input_col_);
        blocks_.examples(samples, inputs_);
        blocks_.stack_pop();
        return *this;
    }

    Blocks& blocks() { return blocks_; }
    void launch(LaunchOptions opt = {}) { blocks_.launch(opt); }
    void launch(int port) { blocks_.launch(port); }

private:
    Blocks blocks_;
    int input_col_ = 0;
    int flag_btn_ = -1;
    std::vector<int> inputs_;
};

// ---------------------------------------------------------------------------
// ChatInterface: Chatbot + input box around fn(message, history) -> reply.
//
//   fn's first parameter is std::string (text only) or gr::MultimodalValue
//   (text + attached files, shows the attach button). The second is
//   const gr::History& (earlier turns). Return the reply as std::string, or take
//   a last `const gr::Yield&` and yield the reply so far to stream it.
// ---------------------------------------------------------------------------
class ChatInterface {
public:
    // additional_inputs: extra components (sliders, a system prompt...) shown in an
    // "Additional Inputs" accordion; their values follow `history` in fn's parameters:
    //   fn(message, history, double temperature, const std::string& system, const gr::Yield&)
    template <class F>
    ChatInterface(F fn, const std::string& title = "", const std::string& description = "",
                  const std::string& placeholder = "Type a message...", std::vector<Component> additional_inputs = {})
        : blocks_(title.empty() ? "Chat" : title) {
        using Args = typename detail::fn_traits<F>::args;
        static_assert(std::tuple_size_v<Args> >= 2, "fn(message, history, [additional inputs...], [yield])");
        using Msg = std::tuple_element_t<0, Args>;
        static constexpr bool multimodal = std::is_same_v<Msg, MultimodalValue>;
        static_assert(multimodal || std::is_same_v<Msg, std::string>, "message must be std::string or gr::MultimodalValue");

        blocks_.set_mode("chat_interface");
        if (!title.empty()) blocks_.add(Markdown("# " + title).set("elem_classes", "title"));
        if (!description.empty()) blocks_.add(Markdown(description));
        int chatbot = blocks_.add(Chatbot("Chatbot").set("show_label", false));
        int box = blocks_.add(multimodal ? MultimodalTextbox("", placeholder)
                                         : Textbox("", 1, placeholder).set("show_label", false));
        int stop = -1;
        blocks_.row([&] {
            stop = blocks_.add(Button("Stop", "stop").set("visible", false));
            blocks_.add(ClearButton({chatbot, box}));
        });
        std::vector<int> extra;
        if (!additional_inputs.empty())
            blocks_.accordion("Additional Inputs", [&] {
                for (auto& c : additional_inputs) extra.push_back(blocks_.add(c));
            }, false);

        bool streaming = false;
        size_t n_args = 0;
        auto user = std::make_shared<Handler>(detail::make_handler(std::move(fn), streaming, n_args));
        if (n_args != 2 + extra.size())
            throw std::invalid_argument("chat fn takes " + std::to_string(n_args) + " input(s); expected message, history and " +
                                        std::to_string(extra.size()) + " additional input(s)");
        // Run the user's fn on [message, history, extras...]; on_text gets the reply so far.
        auto run = [user](const MultimodalValue& m, const json& history, const json& args, size_t first_extra,
                          const CallContext& ctx, const std::function<void(const std::string&)>& on_text) {
            json a = json::array();
            a.push_back(multimodal ? json(m) : json(m.text));
            a.push_back(history);
            for (size_t i = first_extra; i < args.size(); ++i) a.push_back(args[i]);
            std::string last;
            CallContext c2{[&](const json& out) {
                               last = out.empty() || !out[0].is_string() ? std::string() : out[0].get<std::string>();
                               on_text(last);
                           },
                           ctx.progress, ctx.request};
            json out = (*user)(a, c2);
            if (out.is_array() && !out.empty() && out[0].is_string()) last = out[0].get<std::string>();
            return last;
        };

        // UI event: shows the user's turn at once, clears the box, then streams the reply.
        std::vector<int> ui_in = {box, chatbot};
        ui_in.insert(ui_in.end(), extra.begin(), extra.end());
        int ui = blocks_.on_raw(
            {{box, "submit"}},
            [run](const json& args, const CallContext& ctx) -> json {
                MultimodalValue m = args[0].get<MultimodalValue>();
                History history = args[1].is_array() ? args[1].get<History>() : History{};
                json empty = multimodal ? json{{"text", ""}, {"files", json::array()}} : json("");
                if (m.empty()) return json::array({args[0], history});
                History shown = history;
                for (auto& f : m.files) shown.push_back(ChatMessage::user_file(f));
                if (!m.text.empty()) shown.push_back(ChatMessage::user(m.text));
                ctx.emit(json::array({empty, shown}));
                History reply = shown;
                reply.push_back(ChatMessage::assistant(""));
                reply.back().text = run(m, json(history), args, 2, ctx, [&](const std::string& partial) {
                    reply.back().text = partial;
                    ctx.emit(json::array({empty, reply}));
                });
                return json::array({empty, reply});
            },
            ui_in, {box, chatbot}, {"_chat_ui", 1, false, true});
        blocks_.component(stop).set("cancels", json::array({ui}));
        blocks_.cancel(stop, {ui});

        // API event (Gradio's /chat): message (+ additional inputs) in, reply out, no history.
        int response = blocks_.add(Textbox("response").set("visible", false));
        std::vector<int> api_in = {box};
        api_in.insert(api_in.end(), extra.begin(), extra.end());
        blocks_.on_raw(
            {},
            [run](const json& args, const CallContext& ctx) -> json {
                MultimodalValue m = args[0].get<MultimodalValue>();
                std::string text = run(m, json::array(), args, 1, ctx,
                                       [&](const std::string& partial) { ctx.emit(json::array({partial})); });
                return json::array({text});
            },
            api_in, {response}, {"chat", 1, true, true});
    }

    Blocks& blocks() { return blocks_; }
    void launch(LaunchOptions opt = {}) { blocks_.launch(opt); }
    void launch(int port) { blocks_.launch(port); }

private:
    Blocks blocks_;
};

}  // namespace gr
