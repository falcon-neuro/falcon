#include "ui/falcon_ui.cpp"

int main() {
    FalconUI ui;

    if (!ui.launch()) {
        return -1;
    }

    return 0;
}
