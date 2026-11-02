// Chat with a local LLM (text + images) through llama.cpp's llama-server.
//
//   llama-server -m SmolVLM-500M-Instruct-Q8_0.gguf --mmproj mmproj-SmolVLM-500M-Instruct-Q8_0.gguf --port 8080
//   llama_chat --server http://127.0.0.1:8080 [--system "..."] [--port 7860]
//
// Any OpenAI-compatible server works (Ollama: --server http://127.0.0.1:11434 --model llava).
#include "gradio_openai.hpp"

int main(int argc, char** argv) {
    std::string server = "http://127.0.0.1:8080", system_prompt, model = "default";
    gr::LaunchOptions launch;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--server") server = v;
        else if (k == "--system") system_prompt = v;
        else if (k == "--model") model = v;
        else if (k == "--port") launch.port = std::stoi(v);
        else if (k == "--host") launch.host = v;
        else {
            std::cerr << "unknown option " << k << "\n";
            return 1;
        }
    }

    gr::openai::Client llm(server);
    gr::openai::Options opt;
    opt.model = model;

    gr::ChatInterface chat(
        [&](const gr::MultimodalValue& message, const gr::History& history, const gr::Yield& yield) {
            gr::json messages = gr::openai::to_messages(history, message, system_prompt);
            llm.chat(messages, [&](const std::string& so_far) { yield(so_far); }, opt);
        },
        "llama.cpp chat", "Attach an image with **+**, drag & drop, or paste, then ask about it. LLM: `" + server + "`");
    chat.launch(launch);
}
