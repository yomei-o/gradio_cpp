// ChatInterface without an LLM: echoes the message back word by word.
#include "gradio_image.hpp"

#include <thread>

int main() {
    gr::ChatInterface chat(
        [](const gr::MultimodalValue& message, const gr::History& history, const gr::Yield& yield) {
            std::string reply;
            for (auto& f : message.files) {
                if (f.mime_type.rfind("image/", 0) == 0) {
                    gr::Pixels px = gr::load_pixels(f, 3);
                    reply += "Got image **" + f.orig_name + "** (" + std::to_string(px.width) + " x " +
                             std::to_string(px.height) + ").\n\n";
                } else {
                    reply += "Got file **" + f.orig_name + "**.\n\n";
                }
            }
            reply += "You said: " + message.text + "\n\n(" + std::to_string(history.size()) + " earlier messages)";
            std::string shown;
            for (char c : reply) {
                shown += c;
                if (c == ' ' || c == '\n') {
                    yield(shown);
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            }
            yield(shown);
        },
        "Echo chat", "No LLM needed: replies with what you sent.");
    chat.launch();
}
