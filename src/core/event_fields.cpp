#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#ifdef __cpp_impl_reflection
#include <meta>
#include <string>
#include <vector>
#endif
#include "events.cpp"

// Engine: an event's fields as a list, for whatever writes or shows events. It is the one place
// that holds reflection, so that no other file needs any.

enum class FieldType : std::uint8_t {
    Bool,
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    ChannelMask,
};

// One field of an event. A field of a struct inside the event is listed by itself, under a dotted
// name such as header.sample_number.
struct EventField {
    const char* name;
    FieldType type;
    std::size_t offset;  // in bytes, from the event's start
    std::size_t bytes;   // the whole field's size
    std::size_t count;   // how many values of that type; of a channel mask, its stream's channels
};

// The name a recording gives a field's type.
[[nodiscard]] constexpr const char* field_type_name(FieldType type) {
    switch (type) {
        case FieldType::Bool:
            return "bool";
        case FieldType::U8:
            return "u8";
        case FieldType::U16:
            return "u16";
        case FieldType::U32:
            return "u32";
        case FieldType::U64:
            return "u64";
        case FieldType::I8:
            return "i8";
        case FieldType::I16:
            return "i16";
        case FieldType::I32:
            return "i32";
        case FieldType::I64:
            return "i64";
        case FieldType::F32:
            return "f32";
        case FieldType::F64:
            return "f64";
        case FieldType::ChannelMask:
            return "channel_mask";
    }
    std::unreachable();
}

namespace event_fields_detail {

template <typename Number>
concept ListableNumber =
    std::is_arithmetic_v<Number> &&
    (std::same_as<Number, bool> ||
     (std::integral<Number> &&
      (sizeof(Number) == 1 || sizeof(Number) == 2 || sizeof(Number) == 4 || sizeof(Number) == 8)) ||
     (std::floating_point<Number> && (sizeof(Number) == 4 || sizeof(Number) == 8)));

template <ListableNumber Number>
[[nodiscard]] consteval FieldType field_type_of_number() {
    if (std::same_as<Number, bool>) {
        return FieldType::Bool;
    }
    if (std::floating_point<Number>) {
        return sizeof(Number) == 4 ? FieldType::F32 : FieldType::F64;
    }
    const bool is_signed = std::is_signed_v<Number>;
    switch (sizeof(Number)) {
        case 1:
            return is_signed ? FieldType::I8 : FieldType::U8;
        case 2:
            return is_signed ? FieldType::I16 : FieldType::U16;
        case 4:
            return is_signed ? FieldType::I32 : FieldType::U32;
        default:
            return is_signed ? FieldType::I64 : FieldType::U64;
    }
}

template <typename Type>
inline constexpr bool IS_NUMBER_ARRAY = false;
template <ListableNumber Number, std::size_t Count>
inline constexpr bool IS_NUMBER_ARRAY<std::array<Number, Count>> = true;

template <typename Type>
inline constexpr int CHANNELS_OF_MASK = 0;
template <typename StreamInfoType>
inline constexpr int CHANNELS_OF_MASK<ChannelMask<StreamInfoType>> = StreamInfoType::channels;

// What a field of an event may be. A struct counts here, and its own fields are judged in turn.
template <typename Type>
concept ListableField = ListableNumber<Type> || std::is_enum_v<Type> || IS_NUMBER_ARRAY<Type> ||
                        IS_CHANNEL_MASK<Type> || std::is_class_v<Type>;

}  // namespace event_fields_detail

#ifdef __cpp_impl_reflection

// Not formatted: clang-format doesn't know the reflection operators and would pull them apart.
// clang-format off
namespace event_fields_detail {

template <typename Type>
consteval void append_fields_of(std::vector<EventField>& fields, const std::string& prefix,
                                std::size_t base_offset);

template <ListableField Type>
consteval void append_field(std::vector<EventField>& fields, const std::string& name,
                            std::size_t offset) {
    if constexpr (ListableNumber<Type>) {
        fields.push_back({.name = std::define_static_string(name),
                          .type = field_type_of_number<Type>(),
                          .offset = offset,
                          .bytes = sizeof(Type),
                          .count = 1});
    } else if constexpr (std::is_enum_v<Type>) {
        append_field<std::underlying_type_t<Type>>(fields, name, offset);
    } else if constexpr (IS_NUMBER_ARRAY<Type>) {
        fields.push_back({.name = std::define_static_string(name),
                          .type = field_type_of_number<typename Type::value_type>(),
                          .offset = offset,
                          .bytes = sizeof(Type),
                          .count = std::tuple_size_v<Type>});
    } else if constexpr (IS_CHANNEL_MASK<Type>) {
        fields.push_back({.name = std::define_static_string(name),
                          .type = FieldType::ChannelMask,
                          .offset = offset,
                          .bytes = sizeof(Type),
                          .count = static_cast<std::size_t>(CHANNELS_OF_MASK<Type>)});
    } else {
        append_fields_of<Type>(fields, name + ".", offset);
    }
}

template <typename Type>
consteval void append_fields_of(std::vector<EventField>& fields, const std::string& prefix,
                                std::size_t base_offset) {
    template for (constexpr std::meta::info base : std::define_static_array(
                      std::meta::bases_of(^^Type, std::meta::access_context::unchecked()))) {
        append_fields_of<typename [:std::meta::type_of(base):]>(
            fields, prefix, base_offset + static_cast<std::size_t>(std::meta::offset_of(base).bytes));
    }
    template for (constexpr std::meta::info member : std::define_static_array(
                      std::meta::nonstatic_data_members_of(
                          ^^Type, std::meta::access_context::unchecked()))) {
        using MemberType = std::remove_cv_t<typename [:std::meta::type_of(member):]>;
        static_assert(!std::meta::is_bit_field(member) && ListableField<MemberType>,
                      std::string("an event's field must be a number, an enum, a std::array of "
                                  "numbers, a channel mask or a struct of these, which this one "
                                  "is not: ") +
                          std::string(std::meta::identifier_of(member)));
        append_field<MemberType>(
            fields, prefix + std::string(std::meta::identifier_of(member)),
            base_offset + static_cast<std::size_t>(std::meta::offset_of(member).bytes));
    }
}

template <typename EventType>
[[nodiscard]] consteval std::span<const EventField> list_fields() {
    std::vector<EventField> fields;
    append_fields_of<EventType>(fields, "", 0);
    return std::define_static_array(fields);
}

}  // namespace event_fields_detail
// clang-format on

template <std::derived_from<Event> EventType>
inline constexpr std::span<const EventField> EVENT_FIELDS =
    event_fields_detail::list_fields<EventType>();

#else

// Clang, with which the lint and the editor read the code, has no reflection: to them every event
// has no fields.
template <std::derived_from<Event> EventType>
inline constexpr std::span<const EventField> EVENT_FIELDS{};

#endif
