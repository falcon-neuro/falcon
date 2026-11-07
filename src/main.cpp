#include <cstdio>
#include <exception>
#include "ui/falcon_ui.cpp"

int main() {
    try {
        FalconUI falcon_ui;
        if (!falcon_ui.setup()) {
            return -1;
        }
        falcon_ui.run();
    } catch (const std::exception& error) {
        // fputs, unlike println, can't throw here
        std::fputs("Falcon UI: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
    return 0;
}
