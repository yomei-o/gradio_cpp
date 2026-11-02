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
    // gr::Request (anywhere in the parameter list) tells who is calling.
    gr::Interface demo(
        [](const gr::Request& req, const std::string& s) {
            return "Hello, " + s + "! (logged in as " + req.username + " from " + req.client_host + ")";
        },
        {gr::Textbox("name")}, {gr::Textbox("greeting")}, "Members only");
    gr::LaunchOptions opt;
    opt.auth = gr::auth_users({{user, password}});  // or any (user, password) -> bool
    opt.auth_message = "Log in to use this demo.";
    demo.launch(opt);
}
