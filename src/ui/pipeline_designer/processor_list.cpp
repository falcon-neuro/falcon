#pragma once
#include <cstdint>
#include <span>

// What the UI and the generator know of each kind of processor. list_processors.py reads it from
// the processors' source when Falcon is built and writes it into all_processors.cpp.

enum class PortDirection : std::uint8_t { Input, Output };

// Any: a port with slots, whose connections each have a kind of their own.
enum class PortKind : std::uint8_t { Samples, Events, State, Any };

struct OptionDescription {
    const char* name = "";
    const char* type = "";           // as written in the options struct
    const char* default_value = "";  // its initializer, as written
    const char* description = "";
    // Every value the option can have, each as written in pipeline.cpp, when its type is a fixed
    // list of values (an enum); empty for any other type.
    std::span<const char* const> choices;
};

// A kind of event: a struct derived from Event in a processor's source.
struct EventKindDescription {
    const char* name = "";  // its type's name, without the stream of a kind that belongs to one
    const char* description = "";
};

// Which stream a samples port carries: the one its processor takes at its input, or the one its
// processor starts. None for a port that carries no samples.
enum class PortStream : std::uint8_t { None, Input, Output };

// One member of a processor's Connections.
struct PortDescription {
    const char* name = "";
    PortDirection direction = PortDirection::Input;
    PortKind kind = PortKind::Samples;
    bool is_optional = false;  // a graph may leave it unconnected
    bool has_slots = false;    // it takes several connections
    const char* type = "";     // as written in Connections
    const char* description = "";
    PortStream stream = PortStream::None;
    // The kind of event an event output gives, by its EventKindDescription's name; empty for any
    // other port.
    const char* event_kind = "";
    // The kinds of event an input takes, each by that name; empty for a port that takes no events.
    // Asked with one made-up stream on both sides, so it doesn't say whether an event of another
    // stream than the input's is taken.
    std::span<const char* const> takes_event_kinds;
};

struct ProcessorDescription {
    const char* name = "";
    const char* file = "";  // from src/
    const char* description = "";
    std::span<const char* const> template_parameters;  // each as written
    std::span<const OptionDescription> options;
    std::span<const PortDescription> ports;
    // It starts a stream of its own, whose channel count and rate the user sets: it has a
    // template parameter OutputStreamInfo.
    bool starts_stream = false;
};
