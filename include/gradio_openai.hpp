// gradio_openai.hpp - streaming client for OpenAI-compatible chat servers
// (llama.cpp's llama-server, Ollama, vLLM, ...), with images sent as data URIs.
//
//   gr::openai::Client llm("http://127.0.0.1:8080");
//   std::string reply = llm.chat(gr::openai::to_messages(history, message),
//                                [&](const std::string& so_far) { yield(so_far); });
#pragma once

#include "gradio.hpp"

namespace gr {
namespace openai {

inline std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

inline std::string data_uri(const FileData& f) {
    std::string mime = f.mime_type.empty() ? detail::guess_mime(f.path) : f.mime_type;
    return "data:" + mime + ";base64," + base64(f.read());
}

inline bool is_image(const FileData& f) {
    std::string mime = f.mime_type.empty() ? detail::guess_mime(f.path) : f.mime_type;
    return mime.rfind("image/", 0) == 0;
}

// Chat history (+ the new message) -> OpenAI "messages". Consecutive turns of the
// same role are merged, so an image and its question become one user message.
inline json to_messages(const History& history, const MultimodalValue& message = {},
                        const std::string& system_prompt = "") {
    json msgs = json::array();
    if (!system_prompt.empty()) msgs.push_back({{"role", "system"}, {"content", system_prompt}});
    auto part_of = [](const ChatMessage& m) -> json {
        if (m.is_file()) {
            if (is_image(m.file)) return {{"type", "image_url"}, {"image_url", {{"url", data_uri(m.file)}}}};
            return {{"type", "text"}, {"text", "[file: " + m.file.orig_name + "]"}};
        }
        return {{"type", "text"}, {"text", m.text}};
    };
    auto push = [&](const ChatMessage& m) {
        if (m.metadata.is_object() && m.metadata.contains("title")) return;  // thoughts / tool logs stay local
        if (m.role == "assistant" || m.role == "system") {
            if (m.is_file()) return;
            if (!msgs.empty() && msgs.back()["role"] == m.role && msgs.back()["content"].is_string())
                msgs.back()["content"] = msgs.back()["content"].get<std::string>() + "\n" + m.text;
            else
                msgs.push_back({{"role", m.role}, {"content", m.text}});
            return;
        }
        if (msgs.empty() || msgs.back()["role"] != "user") msgs.push_back({{"role", "user"}, {"content", json::array()}});
        msgs.back()["content"].push_back(part_of(m));
    };
    for (auto& m : history) push(m);
    for (auto& f : message.files) push(ChatMessage::user_file(f));
    if (!message.text.empty()) push(ChatMessage::user(message.text));
    return msgs;
}

struct Options {
    std::string model = "default";
    double temperature = 0.7;
    int max_tokens = -1;
    json extra = json::object();  // merged into the request body (top_p, seed, ...)
};

class Client {
public:
    // base_url like "http://127.0.0.1:8080" (llama-server) or "http://127.0.0.1:11434" (Ollama)
    explicit Client(std::string base_url, std::string api_key = "") : base_(std::move(base_url)), key_(std::move(api_key)) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
    }

    // Streams the reply. on_text gets the whole reply so far after each chunk.
    // Exceptions thrown by on_text (e.g. the browser went away) stop the request
    // and are rethrown.
    std::string chat(const json& messages, const std::function<void(const std::string&)>& on_text = {},
                     const Options& opt = {}) {
        json body = {{"model", opt.model}, {"messages", messages}, {"stream", true}, {"temperature", opt.temperature}};
        if (opt.max_tokens > 0) body["max_tokens"] = opt.max_tokens;
        for (auto& [k, v] : opt.extra.items()) body[k] = v;

        httplib::Client cli(base_);
        cli.set_read_timeout(600, 0);
        cli.set_connection_timeout(10, 0);
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/chat/completions";
        req.headers = {{"Accept", "text/event-stream"}};
        if (!key_.empty()) req.headers.emplace("Authorization", "Bearer " + key_);
        req.set_header("Content-Type", "application/json");
        req.body = body.dump();

        std::string buf, text, raw;
        std::exception_ptr err;
        req.content_receiver = [&](const char* data, size_t n, uint64_t, uint64_t) {
            raw.append(data, n);
            buf.append(data, n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                std::string payload = line.substr(line[5] == ' ' ? 6 : 5);
                if (payload == "[DONE]") continue;
                json j = json::parse(payload, nullptr, false);
                if (j.is_discarded() || !j.contains("choices") || j["choices"].empty()) continue;
                const json& delta = j["choices"][0].value("delta", json::object());
                if (delta.contains("content") && delta["content"].is_string()) {
                    text += delta["content"].get<std::string>();
                    if (on_text) {
                        try {
                            on_text(text);
                        } catch (...) {
                            err = std::current_exception();
                            return false;
                        }
                    }
                }
            }
            return true;
        };
        httplib::Response res;
        httplib::Error e = httplib::Error::Success;
        bool ok = cli.send(req, res, e);
        if (err) std::rethrow_exception(err);
        if (!ok) throw Error("cannot reach " + base_ + ": " + httplib::to_string(e));
        if (res.status != 200) {
            json j = json::parse(raw, nullptr, false);
            std::string msg = raw;
            if (!j.is_discarded() && j.contains("error"))
                msg = j["error"].is_object() ? j["error"].value("message", raw) : j["error"].dump();
            throw Error("LLM server returned " + std::to_string(res.status) + ": " + msg);
        }
        return text;
    }

private:
    std::string base_, key_;
};

}  // namespace openai
}  // namespace gr
