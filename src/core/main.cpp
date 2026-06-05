#include "Application.hpp"

int main(int argc, char** argv) {
    Application app;
    if (app.init(argc, argv)) {
        app.run();
    }
    return 0;
}
