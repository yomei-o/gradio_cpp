// The Gradio quickstart, in C++.
#include "gradio.hpp"

int main() {
    gr::Interface demo(
        [](const std::string& name, double intensity) {
            return "Hello, " + name + std::string((size_t)intensity, '!');
        },
        {gr::Textbox("name"), gr::Slider("intensity", 1, 10, 3)},
        {gr::Textbox("greeting")},
        "Hello");
    demo.examples({{"World", 3}, {"C++", 7}});
    demo.launch();
}
