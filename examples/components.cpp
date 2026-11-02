// Tabs, Accordion, Dropdown / Radio / CheckboxGroup, Gallery, HTML and gr::update.
#include "gradio_image.hpp"

#include <cmath>

static gr::FileData swatch(int r, int g, int b, int size = 96) {
    gr::Pixels px{size, size, 3, std::vector<uint8_t>((size_t)size * size * 3)};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint8_t* p = px.at(x, y);
            double shade = 0.6 + 0.4 * std::sin((x + y) * 0.08);
            p[0] = (uint8_t)(r * shade);
            p[1] = (uint8_t)(g * shade);
            p[2] = (uint8_t)(b * shade);
        }
    return gr::to_png(px);
}

int main() {
    gr::Blocks demo("Components");
    demo.add(gr::Markdown("# Components"));

    const std::vector<std::string> fruits = {"apple", "banana", "cherry"};
    const std::vector<std::string> veggies = {"carrot", "leek", "onion"};

    demo.tabs([&] {
        demo.tab("Choices", [&] {
            int kind = demo.add(gr::Radio("kind", {"fruit", "vegetable"}, "fruit"));
            int item = demo.add(gr::Dropdown("item", fruits, "apple"));
            int extras = demo.add(gr::CheckboxGroup("extras", {"salt", "sugar", "pepper"}, {"salt"}));
            int order = demo.add(gr::Button("Order", "primary"));
            int summary = demo.add(gr::Textbox("summary"));
            // Changing the radio swaps the dropdown's choices: gr::update with new props.
            demo.change(kind,
                        [&](const std::string& k) {
                            auto& list = k == "fruit" ? fruits : veggies;
                            return gr::update(list[0]).choices(list).label(k == "fruit" ? "fruit" : "vegetable");
                        },
                        {kind}, {item}, "set_kind");
            demo.click(order,
                       [](const std::string& k, const std::string& it, const std::vector<std::string>& ex) {
                           std::string s = k + ": " + it;
                           for (auto& e : ex) s += " +" + e;
                           return s;
                       },
                       {kind, item, extras}, {summary}, "order");
        });
        demo.tab("Gallery", [&] {
            int n = demo.add(gr::Slider("how many", 1, 8, 4));
            int make = demo.add(gr::Button("Make swatches", "primary"));
            int gallery = demo.add(gr::Gallery("swatches", 4));
            demo.click(make,
                       [](double count) {
                           std::vector<gr::GalleryItem> items;
                           for (int i = 0; i < (int)count; ++i) {
                               int r = (i * 97) % 256, g = (i * 53 + 80) % 256, b = (i * 151 + 160) % 256;
                               items.push_back({swatch(r, g, b), "#" + std::to_string(i + 1)});
                           }
                           return items;
                       },
                       {n}, {gallery}, "swatches");
        });
        demo.tab("Visibility", [&] {
            int show = demo.add(gr::Checkbox("show the secret box", false));
            int secret = demo.add(gr::Textbox("secret", 1, "", "peekaboo").visible(false));
            demo.change(show, [](bool v) { return gr::update().visible(v); }, {show}, {secret}, "toggle");
            demo.accordion("More", [&] {
                demo.add(gr::HTML("<p>This is <b>raw HTML</b> inside an accordion.</p>"));
            }, false);
        });
    });
    demo.launch();
}
