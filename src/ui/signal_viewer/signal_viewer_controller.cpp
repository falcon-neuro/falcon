#pragma once
#include <chrono>
#include <cstring>
#include <vector>
#include "signal_parser.cpp"

class SignalViewerController {
   private:
    SignalParser parser_;
    double current_ingestion_rate_ = 0.0;

   public:
    SignalViewerController() = default;
    ~SignalViewerController() { stop(); }

    void start() { parser_.start(); }

    void stop() { parser_.stop(); }

    void sync_presentation_buffer(std::vector<float>& local_ui_buf, size_t pixel_width) {
        parser_.get_latest_render_data(local_ui_buf, pixel_width);
    }

    void update_benchmarks() {
        static auto last_time = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = now - last_time;

        if (elapsed.count() >= 1.0) {
            uint64_t samples = parser_.flush_samples_count();
            current_ingestion_rate_ = static_cast<double>(samples) / elapsed.count();
            last_time = now;
        }
    }

    double get_ingestion_rate() const { return current_ingestion_rate_; }
};