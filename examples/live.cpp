// Live Interface (no Submit button: every input change reruns), like gr.Interface(live=True).
#include "gradio.hpp"

#include <cstdio>

int main() {
    gr::Interface demo(
        [](double celsius, const std::string& unit) {
            double v = unit == "Fahrenheit" ? celsius * 9 / 5 + 32 : celsius + 273.15;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.2f %s", v, unit == "Fahrenheit" ? "F" : "K");
            return std::string(buf);
        },
        {gr::Slider("Celsius", -50, 150, 20, 0.5), gr::Radio("to", {"Fahrenheit", "Kelvin"}, "Fahrenheit")},
        {gr::Textbox("converted")}, "Temperature (live)");
    demo.live();
    demo.launch();
}
