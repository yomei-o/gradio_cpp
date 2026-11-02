// Image + text chat with llama.cpp linked in directly (libllama + libmtmd): one exe,
// no separate llama-server.
//
//   cmake -S . -B build -DGRADIO_LLAMA_CPP=path/to/llama.cpp
//   llama_local -m SmolVLM2-500M-Video-Instruct-Q8_0.gguf --mmproj mmproj-SmolVLM2-500M-Video-Instruct-Q8_0.gguf
//
// Each turn re-evaluates the whole conversation (images included), which keeps the
// code short and makes edits to the history (Clear, Stop) trivially correct.
#include "gradio.hpp"

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include <memory>
#include <thread>

struct Options {
    std::string model, mmproj, system_prompt;
    int n_ctx = 8192, n_threads = 0, max_tokens = 1024;
    float temperature = 0.7f;
    gr::LaunchOptions launch;
};

// Only the complete UTF-8 prefix: a token can end in the middle of a character.
static size_t utf8_complete(const std::string& s) {
    size_t n = s.size(), i = n;
    while (i > 0 && i + 4 > n && ((unsigned char)s[i - 1] & 0xC0) == 0x80) --i;
    if (i == 0) return n;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return (n - (i - 1) >= need) ? n : i - 1;
}

class LocalLLM {
public:
    explicit LocalLLM(const Options& o) : opt_(o) {
        ggml_backend_load_all();
        llama_backend_init();
        model_ = llama_model_load_from_file(o.model.c_str(), llama_model_default_params());
        if (!model_) throw std::runtime_error("cannot load model " + o.model);
        vocab_ = llama_model_get_vocab(model_);
        int threads = o.n_threads > 0 ? o.n_threads : (int)std::max(1u, std::thread::hardware_concurrency() / 2);
        auto cp = llama_context_default_params();
        cp.n_ctx = (uint32_t)o.n_ctx;
        cp.n_batch = 2048;
        cp.n_threads = cp.n_threads_batch = threads;
        ctx_ = llama_init_from_model(model_, cp);
        if (!ctx_) throw std::runtime_error("cannot create llama context");
        auto mp = mtmd_context_params_default();
        mp.n_threads = threads;
        mp.print_timings = false;
        mtmd_ = mtmd_init_from_file(o.mmproj.c_str(), model_, mp);
        if (!mtmd_) throw std::runtime_error("cannot load mmproj " + o.mmproj);
        const char* t = llama_model_chat_template(model_, nullptr);
        if (t) tmpl_ = t;
    }
    ~LocalLLM() {
        if (mtmd_) mtmd_free(mtmd_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
    }

    // One context, so one generation at a time (the UI and the /chat API share it).
    std::string chat(const gr::History& history, const gr::MultimodalValue& message,
                     const std::function<void(const std::string&)>& on_text) {
        std::lock_guard<std::mutex> lk(m_);

        // 1. history -> chat messages, images become media markers + bitmaps
        struct Msg { std::string role, content; };
        std::vector<Msg> msgs;
        std::vector<std::unique_ptr<mtmd_bitmap, decltype(&mtmd_bitmap_free)>> bitmaps;
        const std::string marker = mtmd_default_marker();
        auto add = [&](const std::string& role, const std::string& text) {
            if (!msgs.empty() && msgs.back().role == role) msgs.back().content += (role == "user" ? "" : "\n") + text;
            else msgs.push_back({role, text});
        };
        auto add_image = [&](const gr::FileData& f) {
            std::string bytes = f.read();
            auto w = mtmd_helper_bitmap_init_from_buf(mtmd_, (const unsigned char*)bytes.data(), bytes.size(), false,
                                                      mtmd_helper_init_opt_default());
            if (!w.bitmap) throw gr::Error("cannot decode image " + f.orig_name);
            bitmaps.emplace_back(w.bitmap, &mtmd_bitmap_free);
            add("user", marker + "\n");
        };
        if (!opt_.system_prompt.empty()) add("system", opt_.system_prompt);
        for (auto& m : history) {
            if (m.metadata.is_object() && m.metadata.contains("title")) continue;
            if (m.is_file()) {
                if (m.role == "user" && m.file.mime_type.rfind("image/", 0) == 0) add_image(m.file);
            } else {
                add(m.role, m.text);
            }
        }
        for (auto& f : message.files)
            if (f.mime_type.rfind("image/", 0) == 0) add_image(f);
        if (!message.text.empty()) add("user", message.text);
        if (msgs.empty() || msgs.back().role != "user") throw gr::Error("nothing to answer");

        // 2. chat template
        std::vector<llama_chat_message> cm;
        for (auto& m : msgs) cm.push_back({m.role.c_str(), m.content.c_str()});
        std::string prompt(8192, '\0');
        int n = llama_chat_apply_template(tmpl_.empty() ? nullptr : tmpl_.c_str(), cm.data(), cm.size(), true,
                                          prompt.data(), (int32_t)prompt.size());
        if (n > (int)prompt.size()) {
            prompt.resize(n);
            n = llama_chat_apply_template(tmpl_.empty() ? nullptr : tmpl_.c_str(), cm.data(), cm.size(), true,
                                          prompt.data(), (int32_t)prompt.size());
        }
        if (n < 0) {  // template not known to llama_chat_apply_template: plain fallback
            prompt.clear();
            for (auto& m : msgs) prompt += m.role + ": " + m.content + "\n";
            prompt += "assistant:";
        } else {
            prompt.resize(n);
        }

        // 3. tokenize text + images, evaluate from an empty KV cache
        std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)> chunks(mtmd_input_chunks_init(),
                                                                                     &mtmd_input_chunks_free);
        mtmd_input_text txt{prompt.c_str(), prompt.size(), true, true};
        std::vector<const mtmd_bitmap*> bmps;
        for (auto& b : bitmaps) bmps.push_back(b.get());
        if (int r = mtmd_tokenize(mtmd_, chunks.get(), &txt, bmps.data(), bmps.size()))
            throw gr::Error("mtmd_tokenize failed (" + std::to_string(r) + ")");
        if (mtmd_helper_get_n_tokens(chunks.get()) + 16 > llama_n_ctx(ctx_))
            throw gr::Error("conversation too long for the context; press Clear");
        llama_pos n_past = eval_prompt(chunks.get());

        // 4. sample
        auto sp = llama_sampler_chain_default_params();
        std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> smpl(llama_sampler_chain_init(sp),
                                                                           &llama_sampler_free);
        llama_sampler_chain_add(smpl.get(), llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl.get(), llama_sampler_init_temp(opt_.temperature));
        llama_sampler_chain_add(smpl.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        llama_batch batch = llama_batch_init(1, 0, 1);
        struct BatchFree {
            llama_batch& b;
            ~BatchFree() { llama_batch_free(b); }
        } bf{batch};

        std::string text;
        size_t shown = 0;
        for (int i = 0; i < opt_.max_tokens && n_past < (llama_pos)llama_n_ctx(ctx_); ++i) {
            llama_token tok = llama_sampler_sample(smpl.get(), ctx_, -1);
            if (llama_vocab_is_eog(vocab_, tok)) break;
            char buf[256];
            int len = llama_token_to_piece(vocab_, tok, buf, sizeof buf, 0, false);
            if (len > 0) text.append(buf, (size_t)len);
            size_t ok = utf8_complete(text);
            if (ok > shown && on_text) {
                shown = ok;
                on_text(text.substr(0, ok));  // may throw when the browser presses Stop
            }
            batch.n_tokens = 1;
            batch.token[0] = tok;
            batch.pos[0] = n_past++;
            batch.n_seq_id[0] = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0] = 1;
            if (llama_decode(ctx_, batch)) {
                reset_cache();
                throw gr::Error("llama_decode failed");
            }
            cache_.push_back({tok, {}, 1});  // the reply stays in the KV cache for the next turn
        }
        return text;
    }

private:
    // What KV-cache sequence 0 holds, in order: one unit per text token or per image.
    struct Unit {
        llama_token tok;
        std::string image_id;  // non-empty for an image
        llama_pos n_pos;
        bool operator==(const Unit& o) const { return tok == o.tok && image_id == o.image_id && n_pos == o.n_pos; }
    };
    std::vector<Unit> cache_;

    void reset_cache() {
        cache_.clear();
        llama_memory_clear(llama_get_memory(ctx_), true);
    }

    static std::vector<Unit> units_of(const mtmd_input_chunk* c) {
        std::vector<Unit> u;
        if (mtmd_input_chunk_get_type(c) == MTMD_INPUT_CHUNK_TYPE_TEXT) {
            size_t n = 0;
            const llama_token* t = mtmd_input_chunk_get_tokens_text(c, &n);
            for (size_t i = 0; i < n; ++i) u.push_back({t[i], {}, 1});
        } else {
            const char* id = mtmd_input_chunk_get_id(c);
            u.push_back({-1, std::string("img:") + (id ? id : "?"), mtmd_input_chunk_get_n_pos(c)});
        }
        return u;
    }

    // Evaluate the prompt, keeping whatever prefix the KV cache already holds (the
    // earlier turns, their images and the replies generated for them).
    llama_pos eval_prompt(const mtmd_input_chunks* chunks) {
        auto t0 = std::chrono::steady_clock::now();
        size_t n_chunks = mtmd_input_chunks_size(chunks);
        std::vector<std::vector<Unit>> units(n_chunks);
        size_t total = 0;
        for (size_t i = 0; i < n_chunks; ++i) {
            units[i] = units_of(mtmd_input_chunks_get(chunks, i));
            total += units[i].size();
        }
        // longest common prefix, in units; always leave something to evaluate for logits
        size_t keep = 0;
        bool diverged = false;
        for (size_t i = 0; i < n_chunks && !diverged; ++i)
            for (auto& u : units[i]) {
                if (keep < cache_.size() && cache_[keep] == u) {
                    ++keep;
                } else {
                    diverged = true;
                    break;
                }
            }
        if (keep >= total) keep = total - 1;
        // an image is all or nothing: do not stop inside one
        llama_pos keep_pos = 0;
        for (size_t k = 0; k < keep; ++k) keep_pos += cache_[k].n_pos;
        cache_.resize(keep);
        if (!llama_memory_seq_rm(llama_get_memory(ctx_), 0, keep_pos, -1)) {
            reset_cache();
            keep = 0;
            keep_pos = 0;
        }

        llama_pos n_past = keep_pos;
        size_t seen = 0, evaluated = 0;
        try {
            for (size_t i = 0; i < n_chunks; ++i) {
                const mtmd_input_chunk* c = mtmd_input_chunks_get(chunks, i);
                bool last = i + 1 == n_chunks;
                size_t n = units[i].size();
                if (seen + n <= keep) {  // entirely cached
                    seen += n;
                    continue;
                }
                size_t skip = keep > seen ? keep - seen : 0;
                if (mtmd_input_chunk_get_type(c) != MTMD_INPUT_CHUNK_TYPE_TEXT || skip == 0) {
                    if (mtmd_helper_eval_chunk_single(mtmd_, ctx_, c, n_past, 0, 2048, last, &n_past))
                        throw gr::Error("failed to evaluate the prompt");
                } else {  // text chunk with a cached head: decode only its tail
                    size_t nt = 0;
                    const llama_token* toks = mtmd_input_chunk_get_tokens_text(c, &nt);
                    for (size_t s = skip; s < nt; s += 2048) {
                        int m = (int)std::min<size_t>(2048, nt - s);
                        llama_batch b = llama_batch_init(m, 0, 1);
                        for (int j = 0; j < m; ++j) {
                            b.token[j] = toks[s + j];
                            b.pos[j] = n_past + j;
                            b.n_seq_id[j] = 1;
                            b.seq_id[j][0] = 0;
                            b.logits[j] = last && s + j + 1 == nt;
                        }
                        b.n_tokens = m;
                        int r = llama_decode(ctx_, b);
                        llama_batch_free(b);
                        if (r) throw gr::Error("failed to evaluate the prompt");
                        n_past += m;
                    }
                }
                for (size_t j = skip; j < n; ++j) cache_.push_back(units[i][j]);
                evaluated += n - skip;
                seen += n;
            }
        } catch (...) {
            reset_cache();
            throw;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cerr << "prompt: reused " << keep << " / " << total << " units, evaluated " << evaluated << " in " << ms
                  << " ms" << std::endl;
        return n_past;
    }

    Options opt_;
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    mtmd_context* mtmd_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    std::string tmpl_;
    std::mutex m_;
};

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "-m" || k == "--model") o.model = v;
        else if (k == "--mmproj") o.mmproj = v;
        else if (k == "--system") o.system_prompt = v;
        else if (k == "--ctx") o.n_ctx = std::stoi(v);
        else if (k == "--threads") o.n_threads = std::stoi(v);
        else if (k == "--max-tokens") o.max_tokens = std::stoi(v);
        else if (k == "--temp") o.temperature = std::stof(v);
        else if (k == "--port") o.launch.port = std::stoi(v);
        else if (k == "--host") o.launch.host = v;
        else {
            std::cerr << "unknown option " << k << "\n";
            return 1;
        }
    }
    if (o.model.empty() || o.mmproj.empty()) {
        std::cerr << "usage: llama_local -m model.gguf --mmproj mmproj.gguf [--system ...] [--ctx 8192] [--port 7860]\n";
        return 1;
    }
    try {
        LocalLLM llm(o);
        gr::ChatInterface chat(
            [&](const gr::MultimodalValue& message, const gr::History& history, const gr::Yield& yield) {
                llm.chat(history, message, [&](const std::string& so_far) { yield(so_far); });
            },
            "llama.cpp (linked in)", "Model: `" + gr::detail::u8str(gr::detail::u8path(o.model).filename()) + "`");
        chat.launch(o.launch);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
