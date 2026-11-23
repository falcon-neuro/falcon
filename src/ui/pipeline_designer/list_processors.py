#!/usr/bin/env python3
"""Lists Falcon's processors for the UI and the generator.

Reads each processor's definition from clang's syntax tree of its file and writes them all as C++
data (see processor_list.cpp). It parses no C++ itself. It refuses a processor whose description,
options, connections or checks lack the text a user is shown. It also lists the kinds of event those
files define, each a struct derived from Event, and has clang say which of them each input that
takes events takes. The build runs it (CMakeLists.txt):

    list_processors.py --clang clang++ --source-root src --output all_processors.cpp FILE... -- CLANG_FLAG...
"""

import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# A member of Connections is recognised by how its type starts. What an input with slots takes
# isn't in how it is written, so its kind is left open here and clang is asked (see
# find_what_inputs_take): a kind that is guessed lets the UI make a connection that can't compile.
KIND_ASKED_OF_CLANG = None
PORT_TYPES = {
    "SampleInputBuffer<": ("Input", "Samples", False),
    "SampleOutputBuffer<": ("Output", "Samples", False),
    "EventInputBuffer<": ("Input", "Events", False),
    "EventOutputBuffer<": ("Output", "Events", False),
    "StateInput<": ("Input", "State", False),
    "StateOutput<": ("Output", "State", False),
    "InputSlots<": ("Input", KIND_ASKED_OF_CLANG, True),
    "OutputSlots<": ("Output", "Any", True),
}


# The stream that stands in wherever a question to clang needs one: no graph exists when Falcon is
# built, and whether an input takes a kind of event is the same for any stream.
MADE_UP_STREAM = "StreamInfo<16, 30000.0, float>"


def fail(message):
    sys.exit("list_processors.py: " + message)


def arguments():
    args = sys.argv[1:]
    clang_flags = []
    if "--" in args:
        clang_flags = args[args.index("--") + 1:]
        args = args[:args.index("--")]
    named, files = {}, []
    while args:
        argument = args.pop(0)
        if argument in ("--clang", "--source-root", "--output"):
            named[argument] = args.pop(0)
        else:
            files.append(Path(argument))
    for name in ("--clang", "--source-root", "--output"):
        if name not in named:
            fail("missing " + name)
    return named["--clang"], Path(named["--source-root"]), Path(named["--output"]), files, clang_flags


def declared_names(source):
    """The structs a file declares, only to ask clang about each: clang's answer is what counts."""
    text = source.decode("utf-8", errors="replace")
    return sorted(set(re.findall(r"^\s*(?:struct|class)\s+([A-Za-z_]\w*)", text, re.MULTILINE)) - {"Connections"})


def syntax_trees(clang, clang_flags, path, name):
    """What clang parsed of the declarations of `path` whose name holds `name`."""
    command = [clang, *clang_flags, "-fsyntax-only", "-Wno-pragma-once-outside-header",
               "-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=" + name, str(path)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        # A file that doesn't compile still gives a tree, with wrong types in it.
        fail("clang couldn't compile %s, so nothing in it can be trusted:\n%s" % (path, result.stderr[-2000:]))
    trees, position, decoder = [], 0, json.JSONDecoder()
    while True:
        while position < len(result.stdout) and result.stdout[position] in " \n\r\t":
            position += 1
        if position >= len(result.stdout):
            return trees
        tree, position = decoder.raw_decode(result.stdout, position)
        trees.append(tree)


def children(node):
    return [child for child in node.get("inner", []) if child]


def offset_of(location):
    if "offset" in location:
        return location["offset"]
    return location.get("expansionLoc", {}).get("offset")


def source_text(source, node):
    """A node's text as written. Clang counts in bytes, so `source` is the file's bytes."""
    begin = offset_of(node["range"]["begin"])
    end_location = node["range"]["end"]
    end = offset_of(end_location)
    if begin is None or end is None:
        return None
    length = end_location.get("tokLen", end_location.get("expansionLoc", {}).get("tokLen", 0))
    return " ".join(source[begin:end + length].decode("utf-8").split())


def parameter_text(source, parameter):
    """A template parameter as written. Clang's range starts after a concept's namespace, so that
    is taken from the source in front of it."""
    text = source_text(source, parameter) or parameter.get("name", "")
    begin = offset_of(parameter["range"]["begin"])
    qualifier = re.search(rb"((?:[A-Za-z_]\w*::)+)$", source[:begin] if begin is not None else b"")
    return (qualifier.group(1).decode("utf-8") if qualifier else "") + text


def template_arguments(written_type):
    """What stands between a type's outermost angle brackets, argument by argument, as written."""
    arguments, depth, current = [], 0, ""
    for character in written_type[written_type.index("<") + 1:written_type.rindex(">")]:
        if character == "," and depth == 0:
            arguments.append(current.strip())
            current = ""
            continue
        depth += (character in "<({[") - (character in ">)}]")
        current += character
    return arguments + [current.strip()]


def constraint_of(parameter_as_written):
    """A template type parameter's constraint as written, e.g. `StreamEvent` of `StreamEvent
    EventType`; None for a plain `typename`, which takes any type."""
    match = re.fullmatch(r"(.+?)\s*(?:\.\.\.)?\s*[A-Za-z_]\w*", parameter_as_written.split("=")[0].strip())
    constraint = match.group(1).strip() if match else "typename"
    return None if constraint in ("typename", "class") else constraint


def with_first_argument(constraint, argument):
    """The question a constraint asks of a type: `C<A>` written before a parameter means `C<T, A>`."""
    if "<" not in constraint:
        return "%s<%s>" % (constraint, argument)
    name, rest = constraint.split("<", 1)
    return "%s<%s, %s" % (name, argument, rest)


def line_of(source, node):
    begin = offset_of(node["range"]["begin"])
    return source.count(b"\n", 0, begin) + 1 if begin is not None else 0


def doc_comment(node):
    """The node's doc comment (`///`), as one line of text."""
    words = []

    def gather(comment):
        if comment.get("kind") == "TextComment":
            words.append(comment.get("text", "").strip())
        for child in children(comment):
            gather(child)

    for child in children(node):
        if child.get("kind") == "FullComment":
            gather(child)
    return " ".join(word for word in words if word)


def uses_options(node):
    if node.get("kind") == "DeclRefExpr" and node.get("referencedDecl", {}).get("name") == "options":
        return True
    return any(uses_options(child) for child in children(node))


def fields_of(record):
    return [child for child in children(record) if child.get("kind") == "FieldDecl"]


def initializer_text(source, field):
    initializers = [child for child in children(field) if child.get("kind") != "FullComment"]
    return source_text(source, initializers[0]) if initializers else ""


# A type written as one of these words is no fixed list of values, so clang isn't asked about it.
PLAIN_TYPES = {"bool", "char", "double", "float", "int", "long", "short", "unsigned"}


def choices_of(clang, clang_flags, path, written_type, cache):
    """The values an option can have, each as written in pipeline.cpp, when its type is a fixed
    list of values (an enum); none for any other type. Clang is asked about the type by its name,
    wherever the processor's file takes it from."""
    if not re.fullmatch(r"[A-Za-z_]\w*", written_type) or written_type in PLAIN_TYPES:
        return []
    if written_type not in cache:
        choices = []
        for tree in syntax_trees(clang, clang_flags, path, written_type):
            if tree.get("kind") == "EnumDecl" and tree.get("name") == written_type and not choices:
                # A value of an enum that isn't scoped is written without the enum's name.
                prefix = written_type + "::" if tree.get("scopedEnumTag") else ""
                choices = [prefix + child["name"] for child in children(tree)
                           if child.get("kind") == "EnumConstantDecl"]
        cache[written_type] = choices
    return cache[written_type]


# The names by which a samples connection says which stream it carries (see stream_of_port).
INPUT_STREAM = "InputStreamInfo"
OUTPUT_STREAM = "OutputStreamInfo"


def stream_of_port(name, direction, port_kind, inner_type, where, problems):
    """Which stream a samples connection carries: "Input" for the stream its processor takes at its
    input, "Output" for one its processor starts, "None" for a connection without samples. The UI
    and the generator follow a stream through the graph by these two names alone, so a samples
    connection written with any other is refused."""
    if port_kind != "Samples":
        return "None"
    written_stream = inner_type[inner_type.index("<") + 1:inner_type.rindex(">")].strip()
    if written_stream == INPUT_STREAM:
        return "Input"
    if written_stream == OUTPUT_STREAM and direction == "Output":
        return "Output"
    allowed = "%s or %s" % (INPUT_STREAM, OUTPUT_STREAM) if direction == "Output" else INPUT_STREAM
    problems.append("%s: the samples connection `%s` is written with the stream `%s`: it must be %s, "
                    "so that its stream can be followed through a graph" % (where, name, written_stream, allowed))
    return "None"


def read_port(source, field, where, problems):
    written_type = field["type"]["qualType"]
    initializer = initializer_text(source, field) or ""
    is_optional = written_type.startswith("std::optional<") or initializer == "UNCONNECTED"
    inner_type = written_type[len("std::optional<"):] if written_type.startswith("std::optional<") else written_type
    kind = next((value for start, value in PORT_TYPES.items() if inner_type.startswith(start)), None)
    if kind is None:
        problems.append("%s: `%s` is of a type this script doesn't know as a connection: %s" % (where, field["name"], written_type))
        return None
    description = doc_comment(field)
    if not description:
        problems.append("%s: the connection `%s` has no doc comment (///) saying what it expects or gives" % (where, field["name"]))
    direction, port_kind, has_slots = kind
    # What the connection carries, as written: an event type, a template parameter that stands for
    # one, or the parameter that stands for the list of slots of a port with slots.
    carried = template_arguments(inner_type)[0].strip() if port_kind == "Events" or has_slots else ""
    return {"name": field["name"], "direction": direction, "kind": port_kind, "is_optional": is_optional,
            "has_slots": has_slots, "type": written_type, "description": description,
            "stream": stream_of_port(field["name"], direction, port_kind, inner_type, where, problems),
            "carried": carried, "where": where, "event_kind": "", "takes_event_kinds": [],
            "takes_samples": port_kind == "Samples"}


def read_file(clang, clang_flags, source_root, path):
    """The processors and the kinds of event one file defines, and what is wrong with them."""
    source = path.read_bytes()
    shown_path = path.resolve().relative_to(source_root.resolve()).as_posix()
    records, templates = {}, {}

    def gather(node, template):
        kind = node.get("kind")
        if kind == "ClassTemplateDecl":
            template = node
        if kind == "CXXRecordDecl" and node.get("completeDefinition") and node.get("name"):
            records.setdefault(node["name"], node)
            if template is not None:
                templates.setdefault(node["name"], template)
            template = None
        for child in children(node):
            gather(child, template)

    for name in declared_names(source):
        for tree in syntax_trees(clang, clang_flags, path, name):
            gather(tree, None)

    processors, events, problems, choices_cache = [], [], [], {}
    for name, record in sorted(records.items()):
        if not any(base.get("type", {}).get("qualType") == "Event" for base in record.get("bases", [])):
            continue
        where = "%s:%d" % (path, line_of(source, record))
        template = templates.get(name)
        template_parameters = [child for child in children(template or {}) if child.get("kind", "").endswith("ParmDecl")]
        if template is not None and [parameter.get("kind") for parameter in template_parameters] != ["TemplateTypeParmDecl"]:
            problems.append("%s: the event type `%s` may have one template parameter, its stream, and no other" % (where, name))
        description = (doc_comment(template) if template is not None else "") or doc_comment(record)
        if not description:
            problems.append("%s: the event type `%s` has no doc comment (///) saying what such an event means" % (where, name))
        events.append({"name": name, "path": path, "description": description, "has_stream": template is not None, "where": where})

    for name, record in sorted(records.items()):
        connections = [child for child in children(record)
                       if child.get("kind") == "CXXRecordDecl" and child.get("name") == "Connections" and child.get("completeDefinition")]
        if not connections:
            continue
        where = "%s:%d" % (path, line_of(source, record))
        template = templates.get(name)
        parameters = [child for child in children(template or {})
                      if child.get("kind") in ("TemplateTypeParmDecl", "NonTypeTemplateParmDecl", "TemplateTemplateParmDecl")]

        description = doc_comment(template) if template is not None and doc_comment(template) else doc_comment(record)
        if not description:
            problems.append("%s: the processor `%s` has no doc comment (///) saying what it does" % (where, name))

        options_name = next((re.sub(r"<.*", "", parameter["type"]["qualType"]) for parameter in parameters
                             if parameter.get("kind") == "NonTypeTemplateParmDecl" and parameter.get("name") == "options"), None)
        if options_name not in records:
            options_name = name + "Options"
        options = []
        for field in fields_of(records.get(options_name, {})):
            option_description = doc_comment(field)
            if not option_description:
                problems.append("%s:%d: the option `%s` has no doc comment (///) written for the user" % (path, line_of(source, field), field["name"]))
            options.append({"name": field["name"], "type": field["type"]["qualType"],
                            "default": initializer_text(source, field) or "", "description": option_description,
                            "choices": choices_of(clang, clang_flags, path, field["type"]["qualType"], choices_cache)})

        ports = [read_port(source, field, "%s:%d" % (path, line_of(source, field)), problems) for field in fields_of(connections[0])]

        for check in children(record):
            if check.get("kind") != "StaticAssertDecl":
                continue
            parts = children(check)
            has_message = any(part.get("kind") == "StringLiteral" for part in parts[1:])
            if parts and uses_options(parts[0]) and not has_message:
                problems.append("%s:%d: a static_assert about an option has no message for the user" % (path, line_of(source, check)))

        parameter_names = [parameter.get("name") for parameter in parameters]
        for port in ports:
            if port is not None and port["stream"] == "Output" and OUTPUT_STREAM not in parameter_names:
                problems.append("%s: the connection `%s` carries %s, which isn't a template parameter of `%s`" % (
                    where, port["name"], OUTPUT_STREAM, name))
        processors.append({"name": name, "file": shown_path, "path": path, "description": description,
                           "parameters": [parameter_text(source, parameter) for parameter in parameters],
                           "constraints": {parameter.get("name"): constraint_of(parameter_text(source, parameter))
                                           for parameter in parameters if parameter.get("kind") == "TemplateTypeParmDecl"},
                           "options": options, "ports": [port for port in ports if port is not None],
                           "starts_stream": OUTPUT_STREAM in parameter_names})
    return path, processors, events, problems


# Stands for samples among the kinds of event that an input is asked about.
SAMPLES = object()


def name_kinds_of_inputs_with_slots(processors, problems):
    """Names the kind of every input with slots from what clang said its slots take."""
    for processor in processors:
        for port in processor["ports"]:
            if port["kind"] is not KIND_ASKED_OF_CLANG:
                continue
            takes_events = bool(port["takes_event_kinds"])
            if port["takes_samples"] and takes_events:
                port["kind"] = "Any"
            elif takes_events:
                port["kind"] = "Events"
            elif port["takes_samples"]:
                port["kind"] = "Samples"
            else:
                problems.append("%s: the connection `%s` takes neither samples nor any kind of event" % (
                    port["where"], port["name"]))


def find_what_inputs_take(clang, clang_flags, output, processors, events, problems):
    """Gives every event output its kind, every input that takes events the kinds it takes, and
    every input with slots its kind: samples, events or both. What an input takes is the
    constraint on the template parameter its type names, so clang is asked that constraint about
    samples and about each kind of event, all in one file: a static_assert per input and question,
    of which those that fail are what the input doesn't take."""
    event_names = [event["name"] for event in events]
    for name in sorted(set(event_names)):
        if event_names.count(name) > 1:
            problems.append("the event type `%s` is defined more than once: %s" % (
                name, ", ".join(event["where"] for event in events if event["name"] == name)))
    type_of_kind = {event["name"]: event["name"] + ("<MadeUpStream>" if event["has_stream"] else "") for event in events}

    questions, files = [], {event["path"] for event in events}
    for processor in processors:
        for port in processor["ports"]:
            if port["kind"] != "Events" and not port["has_slots"]:
                continue
            carried = port["carried"]
            if port["direction"] == "Output":
                written_kind = re.sub(r"<.*", "", carried)
                if written_kind in type_of_kind:
                    port["event_kind"] = written_kind
                elif port["kind"] == "Events" and carried not in processor["constraints"]:
                    problems.append("%s: the connection `%s` gives `%s`, which is no event type of a processor's file" % (
                        port["where"], port["name"], carried))
                continue
            # An input with slots is asked about a list of one slot that takes such a buffer.
            asked_type = 'Slots<Slot<"asked", %s>>' if port["has_slots"] else "%s"
            asked_events = "EventBuffer<%s, 1>" if port["has_slots"] else "%s"
            if carried in processor["constraints"]:
                constraint = processor["constraints"][carried]
                if constraint is None:
                    port["takes_event_kinds"] = sorted(type_of_kind)
                    port["takes_samples"] = port["takes_samples"] or port["has_slots"]
                    continue
                question = with_first_argument(constraint, asked_type)
            else:
                question = "std::same_as<%s, %s>" % (asked_type, carried)
            files.add(processor["path"])
            for kind in sorted(type_of_kind):
                questions.append((port, kind, question % (asked_events % type_of_kind[kind])))
            if port["has_slots"]:
                questions.append((port, SAMPLES, question % "SampleBuffer<MadeUpStream>"))
    if problems:
        return
    if not questions:
        name_kinds_of_inputs_with_slots(processors, problems)
        return

    lines = ["// Written by src/ui/pipeline_designer/list_processors.py when Falcon is built: each static_assert",
             "// asks whether one input takes samples or one kind of event, and clang's answers go into all_processors.cpp.",
             "#include <concepts>"]
    lines += ['#include "%s"' % path.resolve().as_posix() for path in sorted(files)]
    lines += ["", "namespace event_kind_questions {", "",
              "using MadeUpStream = %s;" % MADE_UP_STREAM,
              "// A constraint may name its processor's stream, which it does by these two names.",
              "using %s = MadeUpStream;" % INPUT_STREAM, "using %s = MadeUpStream;" % OUTPUT_STREAM, ""]
    lines += ['static_assert(%s, "QUESTION %d");' % (question, index) for index, (_, _, question) in enumerate(questions)]
    lines += ["", "}  // namespace event_kind_questions", ""]
    questions_file = output.parent / "event_kind_questions.cpp"
    questions_file.parent.mkdir(parents=True, exist_ok=True)
    questions_file.write_text("\n".join(lines), encoding="utf-8")

    command = [clang, *clang_flags, "-fsyntax-only", "-ferror-limit=0", "-Wno-pragma-once-outside-header", str(questions_file)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    refused, other_errors = set(), []
    for line in result.stderr.splitlines():
        if " error: " not in line:
            continue
        answer = re.search(r"static assertion failed.*QUESTION (\d+)", line)
        if answer:
            refused.add(int(answer.group(1)))
        else:
            other_errors.append(line)
    if other_errors:
        # An input whose constraint this script can't ask, e.g. one that names another template
        # parameter than the stream's, shows here.
        fail("clang couldn't say what each input takes (%s):\n  %s" % (questions_file, "\n  ".join(other_errors[:20])))
    for index, (port, asked, _) in enumerate(questions):
        if index in refused:
            continue
        if asked is SAMPLES:
            port["takes_samples"] = True
        else:
            port["takes_event_kinds"].append(asked)
    name_kinds_of_inputs_with_slots(processors, problems)


def literal(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ") + '"'


def write_output(output, processors, events):
    lines = ["// Written by src/ui/pipeline_designer/list_processors.py from the processors' source when Falcon is",
             "// built.",
             "// Not to be edited: change the processor, and build again.",
             "#pragma once",
             "#include <array>",
             '#include "ui/pipeline_designer/processor_list.cpp"',
             "",
             "namespace all_processors_detail {",
             ""]
    for processor in processors:
        name = processor["name"]
        lines.append("inline constexpr std::array<const char*, %d> %s_template_parameters{{" % (len(processor["parameters"]), name))
        lines += ["    %s," % literal(parameter) for parameter in processor["parameters"]]
        lines.append("}};")
        for option in processor["options"]:
            if option["choices"]:
                lines.append("inline constexpr std::array<const char*, %d> %s_%s_choices{{" % (
                    len(option["choices"]), name, option["name"]))
                lines += ["    %s," % literal(choice) for choice in option["choices"]]
                lines.append("}};")
        lines.append("inline constexpr std::array<OptionDescription, %d> %s_options{{" % (len(processor["options"]), name))
        for option in processor["options"]:
            lines.append("    OptionDescription{.name = %s, .type = %s, .default_value = %s," % (
                literal(option["name"]), literal(option["type"]), literal(option["default"])))
            lines.append("                      .description = %s," % literal(option["description"]))
            lines.append("                      .choices = %s}," % (
                "%s_%s_choices" % (name, option["name"]) if option["choices"] else "{}"))
        lines.append("}};")
        for port in processor["ports"]:
            if port["takes_event_kinds"]:
                lines.append("inline constexpr std::array<const char*, %d> %s_%s_takes_event_kinds{{" % (
                    len(port["takes_event_kinds"]), name, port["name"]))
                lines += ["    %s," % literal(kind) for kind in port["takes_event_kinds"]]
                lines.append("}};")
        lines.append("inline constexpr std::array<PortDescription, %d> %s_ports{{" % (len(processor["ports"]), name))
        for port in processor["ports"]:
            lines.append("    PortDescription{.name = %s, .direction = PortDirection::%s, .kind = PortKind::%s," % (
                literal(port["name"]), port["direction"], port["kind"]))
            lines.append("                    .is_optional = %s, .has_slots = %s, .type = %s," % (
                "true" if port["is_optional"] else "false", "true" if port["has_slots"] else "false", literal(port["type"])))
            lines.append("                    .description = %s," % literal(port["description"]))
            lines.append("                    .stream = PortStream::%s, .event_kind = %s," % (port["stream"], literal(port["event_kind"])))
            lines.append("                    .takes_event_kinds = %s}," % (
                "%s_%s_takes_event_kinds" % (name, port["name"]) if port["takes_event_kinds"] else "{}"))
        lines.append("}};")
        lines.append("")
    lines += ["}  // namespace all_processors_detail", "",
              "inline constexpr std::array<EventKindDescription, %d> ALL_EVENT_KINDS{{" % len(events)]
    lines += ["    EventKindDescription{.name = %s, .description = %s}," % (literal(event["name"]), literal(event["description"]))
              for event in events]
    lines += ["}};", "",
              "inline constexpr std::array<ProcessorDescription, %d> ALL_PROCESSORS{{" % len(processors)]
    for processor in processors:
        name = processor["name"]
        lines.append("    ProcessorDescription{.name = %s, .file = %s," % (literal(name), literal(processor["file"])))
        lines.append("                         .description = %s," % literal(processor["description"]))
        lines.append("                         .template_parameters = all_processors_detail::%s_template_parameters," % name)
        lines.append("                         .options = all_processors_detail::%s_options," % name)
        lines.append("                         .ports = all_processors_detail::%s_ports," % name)
        lines.append("                         .starts_stream = %s}," % ("true" if processor["starts_stream"] else "false"))
    lines += ["}};", ""]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")


def main():
    clang, source_root, output, files, clang_flags = arguments()
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda path: read_file(clang, clang_flags, source_root, path), sorted(files)))
    processors, events, problems = [], [], []
    for path, found, found_events, file_problems in results:
        if not found:
            print("list_processors.py: %s defines no processor (nothing with a Connections struct), so it is left out" % path)
        processors += found
        events += found_events
        problems += file_problems
    processors.sort(key=lambda processor: processor["name"].lower())
    events.sort(key=lambda event: event["name"].lower())
    find_what_inputs_take(clang, clang_flags, output, processors, events, problems)
    if problems:
        fail("these must be put right before Falcon builds:\n  " + "\n  ".join(problems))
    write_output(output, processors, events)
    print("list_processors.py: %d processors and %d kinds of event listed in %s" % (len(processors), len(events), output))


if __name__ == "__main__":
    main()
