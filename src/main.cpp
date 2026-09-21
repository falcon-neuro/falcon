#include "ui/falcon_ui.cpp"

int main(int argc, char* argv[]) {
    FalconUI ui;

    if (!ui.launch()) {
        return -1;
    }

    return 0;
}
