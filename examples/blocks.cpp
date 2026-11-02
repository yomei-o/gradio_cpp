// Blocks: free layout, several events, a streaming (generator) handler.
#include "gradio.hpp"

#include <thread>

int main() {
    gr::Blocks demo("Blocks demo");
    demo.add(gr::Markdown("# Blocks demo\nType text, then press **Count** or **Stream**."));

    int text = -1, n = -1, count_btn = -1, stream_btn = -1, flip = -1, out = -1, stats = -1;
    demo.row([&] {
        demo.column([&] {
            text = demo.add(gr::Textbox("text", 3, "write something..."));
            n = demo.add(gr::Slider("repeat (stream)", 1, 20, 5));
            flip = demo.add(gr::Checkbox("uppercase"));
            demo.row([&] {
                count_btn = demo.add(gr::Button("Count", "primary"));
                stream_btn = demo.add(gr::Button("Stream"));
            });
        });
        demo.column([&] {
            out = demo.add(gr::Textbox("output", 6));
            stats = demo.add(gr::JSON("stats"));
        });
    });

    demo.click(count_btn,
               [](const std::string& s, bool upper) {
                   std::string t = s;
                   if (upper)
                       for (auto& c : t) c = (char)std::toupper((unsigned char)c);
                   size_t words = 0;
                   bool in = false;
                   for (unsigned char c : s) {
                       bool sp = std::isspace(c);
                       if (!sp && !in) ++words;
                       in = !sp;
                   }
                   gr::json st = {{"bytes", s.size()}, {"words", words}};
                   return std::make_tuple(t, st);
               },
               {text, flip}, {out, stats}, "count");

    // Last parameter `const gr::Yield&` makes it a generator: each call streams to the browser.
    demo.click(stream_btn,
               [](const std::string& s, double times, const gr::Yield& yield) {
                   std::string acc;
                   for (int i = 0; i < (int)times; ++i) {
                       acc += std::to_string(i + 1) + ": " + s + "\n";
                       yield(acc);
                       std::this_thread::sleep_for(std::chrono::milliseconds(300));
                   }
               },
               {text, n}, {out}, "stream");

    demo.launch();
}
