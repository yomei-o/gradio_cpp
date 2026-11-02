// Login required (like Gradio's launch(auth=...)).
//   login --user admin --password secret
#include "gradio.hpp"

int main(int argc, char** argv) {
    std::string user = "admin", password = "secret";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--user") user = argv[i + 1];
        else if (k == "--password") password = argv[i + 1];
    }
    gr::Interface demo([](const std::string& s) { return "Hello, " + s + "!"; }, {gr::Textbox("name")},
                       {gr::Textbox("greeting")}, "Members only");
    gr::LaunchOptions opt;
    opt.auth = gr::auth_users({{user, password}});  // or any (user, password) -> bool
    opt.auth_message = "Log in to use this demo.";
    demo.launch(opt);
}
