#pragma once
#include <chrono>
#include <cstring>
#include <vector>
#include "signal_parser.cpp"

class SignalViewerController {
   private:
    SignalParser parser_;

   public:
    SignalViewerController() = default;
    ~SignalViewerController() { stop(); }

    void start() { parser_.start(); }

    void stop() { parser_.stop(); }

    void sync_presentation_buffer(std::vector<float>& local_ui_buf, size_t pixel_width,
                                  size_t& ingestion_per_s) {
        parser_.get_latest_render_data(local_ui_buf, pixel_width, ingestion_per_s);
    }
};