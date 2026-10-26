// Beam - a Raycast-style launcher, Linux (GNOME / GTK 4) edition.
//
//   beam               show or hide the launcher (what the GNOME shortcut runs)
//   beam --windows     show or hide the window switcher
//   beam --show        show the launcher
//   beam --background  start without showing anything (autostart)
//   beam --quit        quit the running instance
//
// Only one instance runs; later invocations hand their command line to it and exit.
#include <csignal>

#include "linux/app.hpp"

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // plugins that exit early must not take Beam down
    beam::App app;
    return app.Run(argc, argv);
}
