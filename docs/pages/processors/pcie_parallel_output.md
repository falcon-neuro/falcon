---
description: "Falcon's PCIeParallelOutput: a pulse on one pin of a PCIe parallel port card for each event, for example to trigger a stimulator. Its input and options, the card it needs and how the pulse is timed."
---

# PCIeParallelOutput

{{ api("PCIeParallelOutput") }} turns each event into a pulse on one data pin of a PCIe parallel port card, for example to trigger a stimulator.

It times the pulse itself: it sets the pin high and sets it low again when it next runs after the pulse width has passed. It therefore needs a core that never sleeps.

## Input

| Name | Carries |
|------|---------|
| `input` | Events of any kind. Each one starts a pulse, unless a pulse is on. |

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `pin` | The pin of the parallel port that pulses: 2 to 9. These are the port's data lines D0 to D7, so pin 2 is D0 and pin 9 is D7. | 5 |
| `pulse_width_microseconds` | The time the pin stays high for each pulse, in microseconds. It must be above 0. | 1000 |

## The card

The output drives one card model, the Delock 89445 (PCI Express card with one parallel port). The card is detected automatically at the start of a run.

Requirements:

- Exactly one such card is installed. With two, Falcon can't tell which one to drive.
- Falcon has permission to write to the card's I/O ports: it runs as root or has the capability `CAP_SYS_RAWIO`.

If a requirement isn't met, the run fails at its start with an error message.

The output writes all eight data pins at once. The chosen pin carries the pulses and the other seven stay low. A card can therefore be driven by one `PCIeParallelOutput` only, and by nothing else at the same time.

## The pulse

When an event arrives while the pin is low, the output sets the pin high. The pin goes low again the first time the output runs after the pulse width has passed. All pins are low before the first pulse, and the output sets the pin low when the run ends.

With a pulse width of 1000 µs:

| Event arrives at (µs) | Pin | What happens |
|-----------------------|-----|--------------|
| 0 | Low | A pulse starts. It lasts until 1000 µs. |
| 400 | High | The event is ignored. |
| 1200 | Low | A pulse starts. It lasts until 2200 µs. |

!!! note "Edge cases"
    - An event that arrives during a pulse is ignored. It neither lengthens the pulse nor starts one afterwards.
    - Events that arrive together start one pulse.
    - A pulse lasts at least the pulse width. It ends when the output next runs after that, which on a core that never sleeps is the next pass.
    - An event that arrives just as a pulse ends starts the next pulse at once. Between the two pulses the pin is low only for the time of one write to the card, which a connected device may not see.

## Delay

The output sets the pin high in the same pass in which the event arrives. The time from there to the edge on the pin is not measured.

The pulse width is timed with the computer's clock, not the clock of an acquisition device.

## Limitations

- One output drives one pin and one card takes one output, so two pins of a card can't be pulsed by two outputs.
- The output reports nothing back to the graph: no processor can read whether a pulse is on.
