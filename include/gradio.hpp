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
#include <cmath>
#include <condition_variable>
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

inline constexpr const char* version = "0.1.0";

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

inline std::string file_url(const std::string& path) {
    return "/gradio_api/file=" + detail::url_encode_path(path);
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
    return t == "image" || t == "file" || t == "audio" || t == "video";
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

inline Component Image(const std::string& label = "") {
    return Component("image", {{"label", label}, {"value", nullptr}, {"type", "filepath"}});
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

// Resets the given components to their initial values (browser only).
inline Component ClearButton(std::vector<int> components = {}, const std::string& text = "Clear") {
    return Component("clearbutton", {{"value", text}, {"variant", "secondary"}, {"components", components}});
}

// ---------------------------------------------------------------------------
// Typed handler plumbing
// ---------------------------------------------------------------------------
using Emit = std::function<void(const json& outputs)>;
using Handler = std::function<json(const json& args, const Emit& emit)>;

// Take `const gr::Yield&` as the last parameter to stream intermediate results:
//   [](int n, const gr::Yield& y) { for (...) y(partial); }
class Yield {
public:
    explicit Yield(Emit e) : e_(std::move(e)) {}
    template <class... T>
    void operator()(T&&... v) const {
        json a = json::array();
        (a.push_back(json(std::forward<T>(v))), ...);
        e_(a);
    }

private:
    Emit e_;
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

template <class Args, size_t N = std::tuple_size_v<Args>>
struct last_is_yield : std::bool_constant<std::is_same_v<std::tuple_element_t<N - 1, Args>, Yield>> {};
template <class Args>
struct last_is_yield<Args, 0> : std::false_type {};

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

template <class F, class Args, bool Stream, size_t... I>
json invoke_typed(F& fn, const json& args, const Emit& emit, std::index_sequence<I...>) {
    using R = typename fn_traits<F>::ret;
    if constexpr (Stream) {
        Yield y(emit);
        fn(args[I].template get<std::tuple_element_t<I, Args>>()..., y);
        return nullptr;
    } else if constexpr (std::is_void_v<R>) {
        fn(args[I].template get<std::tuple_element_t<I, Args>>()...);
        return json::array();
    } else {
        return result_to_json(fn(args[I].template get<std::tuple_element_t<I, Args>>()...));
    }
}

template <class F>
Handler make_handler(F fn, bool& streaming, size_t& n_args) {
    using Args = typename fn_traits<F>::args;
    static constexpr size_t N = std::tuple_size_v<Args>;
    static constexpr bool stream = last_is_yield<Args>::value;
    static constexpr size_t NA = stream ? N - 1 : N;
    streaming = stream;
    n_args = NA;
    return [fn](const json& args, const Emit& emit) mutable -> json {
        if (!args.is_array() || args.size() < NA)
            throw Error("expected " + std::to_string(NA) + " input value(s), got " +
                        std::to_string(args.is_array() ? args.size() : 0));
        return invoke_typed<F, Args, stream>(fn, args, emit, std::make_index_sequence<NA>{});
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

}  // namespace detail

struct EventOptions {
    std::string api_name;       // empty: "predict", "predict_1", ...
    int concurrency_limit = 1;  // <= 0: unlimited (Gradio's default is 1)
    bool show_api = true;
};

struct LaunchOptions {
    std::string host = "127.0.0.1";
    int port = 7860;
    bool auto_port = true;      // try the next ports if busy, like Gradio
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

    template <class F>
    Blocks& on(std::vector<std::pair<int, std::string>> targets, F fn, std::vector<int> inputs,
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
        return *this;
    }
    template <class F>
    Blocks& click(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "click"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }
    template <class F>
    Blocks& submit(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "submit"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }
    template <class F>
    Blocks& change(int trigger, F fn, std::vector<int> inputs, std::vector<int> outputs, const std::string& api_name = "") {
        return on({{trigger, "change"}}, std::move(fn), std::move(inputs), std::move(outputs), {api_name});
    }

    void set_mode(const std::string& m) { mode_ = m; }

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
            if (!props.contains("interactive") && c.type != "row" && c.type != "column")
                props["interactive"] = !(as_output.count(c.id) && !as_input.count(c.id));
            if (!props.contains("visible")) props["visible"] = true;
            props["name"] = c.type;
            comps.push_back({{"id", c.id}, {"type", c.type}, {"props", props}});
        }
        json deps = json::array();
        for (size_t i = 0; i < deps_.size(); ++i) {
            auto& d = deps_[i];
            json targets = json::array();
            for (auto& t : d.targets) targets.push_back({t.first, t.second});
            deps.push_back({{"id", i}, {"targets", targets}, {"inputs", d.inputs}, {"outputs", d.outputs},
                            {"api_name", d.api_name}, {"backend_fn", true}, {"js", nullptr},
                            {"queue", true}, {"show_api", d.show_api}, {"types", {{"generator", d.streaming}}}});
        }
        return {{"version", std::string("gradio_cpp-") + version},
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
            if (!d.show_api) continue;
            json params = json::array(), rets = json::array();
            for (int id : d.inputs) params.push_back(param_info(id, true));
            for (int id : d.outputs) rets.push_back(param_info(id, false));
            named["/" + d.api_name] = {{"parameters", params}, {"returns", rets}};
        }
        return {{"named_endpoints", named}, {"unnamed_endpoints", json::object()}};
    }

    // Register all routes on an existing server.
    void mount(httplib::Server& svr) {
        svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(frontend_html(), "text/html; charset=utf-8");
        });
        svr.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(config().dump(), "application/json");
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
                events_[id] = {(size_t)(d - deps_.data()), body["data"], now};
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
        svr.Post("/gradio_api/upload", [](const httplib::Request& req, httplib::Response& res) {
            json paths = json::array();
            auto range = req.files.equal_range("files");
            for (auto it = range.first; it != range.second; ++it)
                paths.push_back(detail::Cache::get().store(it->second.content, it->second.filename));
            res.set_content(paths.dump(), "application/json");
        });
        svr.Get(R"(/gradio_api/file=(.+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string p = req.matches[1];
            if (!detail::Cache::get().allowed(p)) {
                res.status = 403;
                res.set_content(json{{"detail", "file not allowed"}}.dump(), "application/json");
                return;
            }
            try {
                res.set_content(detail::read_file(detail::u8path(p)), detail::guess_mime(p));
            } catch (const std::exception&) {
                not_found(res);
            }
        });
    }

    void launch(LaunchOptions opt = {}) {
        httplib::Server svr;
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
    };
    struct Event {
        size_t dep = 0;
        json data;
        std::chrono::steady_clock::time_point created;
    };

    template <class Body>
    int container(const std::string& type, Body& body) {
        int id = add(Component(type));
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
            if (d.api_name == name) return &d;
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

    static json example_input(const Component* c) {
        if (!c) return nullptr;
        const std::string& t = c->type;
        if (t == "textbox") return "Hello!!";
        if (t == "number") return 3;
        if (t == "slider") return c->props.value("minimum", 0.0);
        if (t == "checkbox") return true;
        if (is_file_type(t))
            return {{"path", "https://raw.githubusercontent.com/gradio-app/gradio/main/test/test_files/bus.png"},
                    {"meta", {{"_type", "gradio.FileData"}}}};
        return nullptr;
    }

    // Reject input files that are neither uploads nor files we handed out.
    void check_input_files(const Dep& d, const json& data) const {
        for (size_t i = 0; i < d.inputs.size() && i < data.size(); ++i) {
            const Component* c = find_comp(d.inputs[i]);
            if (!c || !is_file_type(c->type) || data[i].is_null()) continue;
            FileData f = data[i].get<FileData>();
            if (f.path.empty()) throw Error("file input has no path; upload it to /gradio_api/upload first");
            if (f.path.rfind("http://", 0) == 0 || f.path.rfind("https://", 0) == 0)
                throw Error("remote URLs are not supported yet; upload the file to /gradio_api/upload first");
            if (!detail::Cache::get().allowed(f.path)) throw Error("file not allowed: " + f.path);
        }
    }

    void run_event(const Event& ev, httplib::DataSink& sink) {
        const Dep& d = deps_[ev.dep];
        auto send = [&](const std::string& event, const json& data) {
            std::string msg = "event: " + event + "\ndata: " + data.dump() + "\n\n";
            if (!sink.write(msg.data(), msg.size())) throw detail::Cancelled{};
        };
        json last = json::array();
        bool locked = false;
        try {
            check_input_files(d, ev.data);
            if (d.sem) {
                d.sem->acquire();
                locked = true;
            }
            json out = d.fn(ev.data, [&](const json& o) {
                last = o;
                send("generating", o);
            });
            if (locked) d.sem->release();
            locked = false;
            send("complete", out.is_null() ? last : out);
        } catch (const detail::Cancelled&) {
            if (locked) d.sem->release();
            sink.done();
            return;
        } catch (const std::exception& e) {
            if (locked) d.sem->release();
            try {
                send("error", json(std::string(e.what())));
            } catch (...) {
            }
        } catch (...) {
            if (locked) d.sem->release();
            try {
                send("error", json("unknown error"));
            } catch (...) {
            }
        }
        sink.done();
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
            blocks_.column([&] {
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
            });
        });
        std::vector<int> all = in;
        all.insert(all.end(), out.begin(), out.end());
        blocks_.component(clear).set("components", all);
        blocks_.on(targets, std::move(fn), in, out, {"predict"});
    }

    Blocks& blocks() { return blocks_; }
    void launch(LaunchOptions opt = {}) { blocks_.launch(opt); }
    void launch(int port) { blocks_.launch(port); }

private:
    Blocks blocks_;
};

}  // namespace gr
