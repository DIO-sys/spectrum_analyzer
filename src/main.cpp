#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "capture.hpp"
#include "processing.hpp"
#include "display.hpp"

#include <thread>
#include <iostream>

using namespace std;

int main() {
    cout << "[main] starting\n";

    AppState state;

    // 2^18 samples ~ 6.5ms of headroom at 40 MSPS
    CircularBuffer<complex<float>> buffer(1 << 18);

    CaptureThread    capture   (buffer, state);
    ProcessingThread processing(buffer, state);
    DisplayThread    display   (state);

    thread capture_thread   ([&capture]()    { capture.run();    });
    thread processing_thread([&processing]() { processing.run(); });

    display.run(); // blocks until window closed

    state.running.store(false);
    capture_thread.join();
    processing_thread.join();

    cout << "[main] shutdown complete\n";
    return 0;
}

//i think i've made my initial commit???