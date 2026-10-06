#include "ui/falcon_ui.cpp"

int main() {
    FalconUI ui;
    if (!ui.setup()) {
        return -1;
    }

    ui.run();

    return 0;
}
