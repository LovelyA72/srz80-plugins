# Z80 CTC card

An emulated Zilog Z80 CTC with four independently programmable counter/timer
channels. The card maps four consecutive I/O ports and uses the project's
external clock index, so it runs from the same clock source as the Z80 CPU.

## Project configuration

```json
{
  "plugin": "ctc",
  "space": "cpu0.io",
  "base": "0x80",
  "size": 4,
  "clock": 0,
  "config": {
    "irq": "IRQ",
    "intack_enable": true,
    "intack_port": 0,
    "clk_trg_signals": ["", "", "", ""]
  }
}
```

Config keys:

- `irq`: host signal name asserted when a channel has a pending interrupt.
  Set to `""` to use the CTC as a counter/timer without CPU interrupts.
- `intack_enable`: exposed for compatibility. When true, the card also maps a
  one-byte read-only INTACK alias that returns the highest-priority pending
  channel vector and clears that channel's request.
- `intack_port`: I/O address of that INTACK alias. It defaults to `0`, which is
  the address the stock Z80 CPU card reads during interrupt acknowledge.
  Disable `intack_enable` or change `intack_port` only if the CPU card's
  interrupt-acknowledge behaviour is changed.
- `clk_trg_signals`: optional array of four host signal names for the
  `CLK/TRG0..3` inputs. An empty string leaves that input unconnected.

## CTC programming

The cards follow the standard Z80 CTC register layout:

- D7: interrupt enable
- D6: 0 = timer, 1 = counter
- D5: 0 = prescaler /16, 1 = prescaler /256
- D4: 0 = falling CLK/TRG edge, 1 = rising edge
- D3: 0 = auto-start timer, 1 = wait for a CLK/TRG edge (timer mode)
- D2: next word is a time constant
- D1: software reset
- D0: 1 = control word, 0 = interrupt vector word

A vector word must be written to channel 0. Channel `n` supplies
`(vector & 0xF8) | (n << 1)` during interrupt acknowledge, matching the Z80
Interrupt Mode 2 vector table.

Timer period in external clock ticks is `prescaler * time_constant`, where the
time constant is `1..256`; `0x00` means 256. Counter mode ignores the prescaler
and counts the selected edges on `CLK/TRG`.
