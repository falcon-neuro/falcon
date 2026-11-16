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
};

// One member of a processor's Connections.
struct PortDescription {
    const char* name = "";
    PortDirection direction = PortDirection::Input;
    PortKind kind = PortKind::Samples;
    bool is_optional = false;  // a graph may leave it unconnected
    bool has_slots = false;    // it takes several connections
    const char* type = "";     // as written in Connections
    const char* description = "";
};

struct ProcessorDescription {
    const char* name = "";
    const char* file = "";  // from src/
    const char* description = "";
    std::span<const char* const> template_parameters;  // each as written
    std::span<const OptionDescription> options;
    std::span<const PortDescription> ports;
};
