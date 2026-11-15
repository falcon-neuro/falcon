#pragma once
#include <cerrno>
#include <concepts>
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include "../core/engine.cpp"

// EventRecorder: writes the header of every event it receives to a file in the run's directory, as
// raw little-endian records with no file header: the event's sample number, device timestamp,
// arrival TSC and lost-sample count, each a u64. The sample number matches an event with the
// samples a SampleRecorder wrote. Writing to a file can block, so it runs on a core that isn't
// realtime, behind an event ring.

struct EventRecorderOptions {
    Str file_name = "events.bin";  // in the run's directory
};

template <std::derived_from<StreamEvent> EventType, int Capacity,
          EventRecorderOptions options = EventRecorderOptions{}>
struct EventRecorder {
    static_assert(is_file_name(options.file_name),
                  "file_name is a name inside the run's directory, without a /");
    static_assert(sizeof(EventType) == sizeof(StreamEvent),
                  "only an event's header is written, so an event with fields of its own would "
                  "lose them");
    static_assert(sizeof(SampleHeader) == 4 * sizeof(u64),
                  "a record is the header's four u64 fields, with no padding");

    struct Connections {
        EventInputBuffer<EventType, Capacity> input;
    };

    explicit EventRecorder(ProcessorName processor_name) : name(processor_name) {
        path_ = (run_directory() / options.file_name.c_str()).string();
        file_ = create_file(name, path_);
        log(name, "recording events to {}", path_.c_str());
    }

    // A destructor must not throw, so a failure to write the last events is warned.
    ~EventRecorder() {
        if (std::fclose(file_.release()) != 0) {
            warn(name, "can't finish writing {}, which may lack its last events", path_.c_str());
            return;
        }
        log(name, "recorded {} events to {}", recorded_event_count_, path_.c_str());
    }

    EventRecorder(const EventRecorder&) = delete;
    EventRecorder& operator=(const EventRecorder&) = delete;
    EventRecorder(EventRecorder&&) = delete;
    EventRecorder& operator=(EventRecorder&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        for (const EventType& event : connections.input) {
            if (std::fwrite(&event.header, sizeof(SampleHeader), 1, file_.get()) != 1) {
                fail(name, "can't write to {}: {}", path_.c_str(),
                     std::system_category().message(errno).c_str());
            }
            ++recorded_event_count_;
        }
    }

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };

    std::string path_;
    std::unique_ptr<std::FILE, FileCloser> file_;
    u64 recorded_event_count_ = 0;

    // "x" makes fopen fail if the file exists, so a second recorder with the same file name can't
    // overwrite the first's events.
    [[nodiscard]] static std::unique_ptr<std::FILE, FileCloser> create_file(
        ProcessorName processor_name, const std::string& path) {
        std::FILE* const file = std::fopen(path.c_str(), "wbx");
        if (file == nullptr) {
            fail(processor_name, "can't create {}: {}", path.c_str(),
                 std::system_category().message(errno).c_str());
        }
        return std::unique_ptr<std::FILE, FileCloser>(file);
    }
};
