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

    // System prompt / temperature / max tokens can be changed in "Additional Inputs".
    gr::ChatInterface chat(
        [&](const gr::MultimodalValue& message, const gr::History& history, const std::string& system,
            double temperature, double max_tokens, const gr::Yield& yield) {
            gr::openai::Options o = opt;
            o.temperature = temperature;
            o.max_tokens = (int)max_tokens;
            gr::json messages = gr::openai::to_messages(history, message, system);
            llm.chat(messages, [&](const std::string& so_far) { yield(so_far); }, o);
        },
        "llama.cpp chat", "Attach an image with **+**, drag & drop, or paste, then ask about it. LLM: `" + server + "`",
        "Type a message...",
        {gr::Textbox("System prompt", 2, "", system_prompt), gr::Slider("Temperature", 0, 2, 0.7, 0.05),
         gr::Slider("Max new tokens", 16, 4096, 1024, 16)});
    chat.launch(launch);
}
