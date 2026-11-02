// State, Progress, Examples, Code, Dataframe, Label, Audio.
#include "gradio.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

int main() {
    gr::Blocks demo("Extras");
    demo.add(gr::Markdown("# Extras"));
    demo.tabs([&] {
        demo.tab("State", [&] {
            // A per-session counter: the State goes in and comes back out.
            int count = demo.add(gr::State(0));
            int btn = demo.add(gr::Button("Add one", "primary"));
            int shown = demo.add(gr::Textbox("clicks in this session"));
            demo.click(btn,
                       [](int n) {
                           ++n;
                           return std::make_tuple(n, std::to_string(n));
                       },
                       {count}, {count, shown}, "add_one");
        });
        demo.tab("Progress", [&] {
            int steps = demo.add(gr::Slider("steps", 1, 20, 8));
            int go = demo.add(gr::Button("Run slow job", "primary"));
            int out = demo.add(gr::Textbox("result"));
            demo.click(go,
                       [](double n, const gr::Progress& progress) {
                           for (int i = 0; i < (int)n; ++i) {
                               progress(i / n, "step " + std::to_string(i + 1));
                               std::this_thread::sleep_for(std::chrono::milliseconds(150));
                           }
                           return "done " + std::to_string((int)n) + " steps";
                       },
                       {steps}, {out}, "slow");
        });
        demo.tab("Text tools", [&] {
            int code = demo.add(gr::Code("source", "cpp", "int main() {\n    return 0;\n}\n", 8));
            int stats = demo.add(gr::Dataframe("character counts", {"char", "count"}));
            int lang = demo.add(gr::Label("looks like"));
            int analyze = demo.add(gr::Button("Analyze", "primary"));
            demo.click(analyze,
                       [](const std::string& src) {
                           std::map<char, int> m;
                           for (unsigned char ch : src)
                               if (std::isgraph(ch)) ++m[(char)ch];
                           std::vector<std::pair<char, int>> v(m.begin(), m.end());
                           std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                           gr::DataFrame df{{"char", "count"}, {}};
                           for (size_t i = 0; i < v.size() && i < 8; ++i)
                               df.data.push_back({std::string(1, v[i].first), v[i].second});
                           auto has = [&](const char* w) { return src.find(w) != std::string::npos ? 1.0 : 0.0; };
                           double c = 1 + 3 * has("int ") + 3 * has("#include") + has(";");
                           double py = 1 + 3 * has("def ") + 3 * has("import ") + has(":\n");
                           double js = 1 + 3 * has("function") + 3 * has("const ") + has("=>");
                           double sum = c + py + js;
                           gr::LabelValue lv{{{"C/C++", c / sum}, {"Python", py / sum}, {"JavaScript", js / sum}}};
                           return std::make_tuple(df, lv);
                       },
                       {code}, {stats, lang}, "analyze");
            int sum_btn = demo.add(gr::Button("Sum the counts"));
            int total = demo.add(gr::Number("total"));
            demo.click(sum_btn,
                       [](const gr::DataFrame& df) {
                           double t = 0;
                           for (auto& r : df.data)
                               if (r.size() > 1 && r[1].is_number()) t += r[1].get<double>();
                           return t;
                       },
                       {stats}, {total}, "sum_counts");
            demo.examples({{"def f(x):\n    return x * 2\n"}, {"const f = (x) => x * 2;\n"}}, {code});
        });
        demo.tab("Audio", [&] {
            int in = demo.add(gr::Audio("input"));
            int out = demo.add(gr::Audio("same audio back"));
            int info = demo.add(gr::Textbox("info"));
            demo.change(in,
                        [](const gr::FileData& f) {
                            if (f.empty()) return std::make_tuple(gr::FileData{}, std::string());
                            return std::make_tuple(f, f.orig_name + " (" + f.mime_type + ", " +
                                                          std::to_string(f.read().size()) + " bytes)");
                        },
                        {in}, {out, info}, "echo_audio");
        });
    });
    demo.launch();
}
