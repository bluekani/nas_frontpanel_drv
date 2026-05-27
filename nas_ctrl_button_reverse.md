# nas_ctrl button path reverse engineering

Target:
- deb/lib/modules/3.6.6/kernel/landisk/nas_ctrl.ko

Method:
- pyelftools for symbols/relocations
- capstone for instruction-level disassembly
- relocation overlay to resolve strings and data pointers

## 1) Button polling core

Main function:
- btn_poll_timer_handler

Key behavior reconstructed:
- Acquires spinlock (`btn_lock`), iterates `num_btns` entries in `Btn[]`.
- Calls `isButtonPressed(Btn[i])` each cycle.
- Uses `Btn[i].pressed_time`-like counter (`Btn + 56` slot in disassembly) and state flags (`Btn + 52`).
- Re-arms timer with `mod_timer(btn_poll_timer, jiffies + 0x1f)`.

GPIO read path:
- isButtonPressed -> `msGpioGet`
- isButtonReleased -> logical NOT of `isButtonPressed`

## 2) Usermode callback execution (critical finding)

Inside `btn_poll_timer_handler`, module invokes:
- `call_usermodehelper_fns`

Resolved argument vector storage:
- `btn_handler_argv` at `.data+0x1860`

Recovered static argv elements:
- argv[0] = `/mnt/data/libexec/button/sw_handler`
- argv[1] = `--type`
- argv[3] = `--second`
- argv[4] = `second_str` (3-byte buffer in `.bss` at `second_str`)

Dynamic argv element:
- argv[2] updated per button from `btn_name_list`.

Recovered button names (`btn_name_list`):
- `func`
- `power`
- `reset`
- `select`
- `enter`

So effective call shape is:
- `/mnt/data/libexec/button/sw_handler --type <func|power|reset|select|enter> --second <N>`

## 3) Press-time thresholds

Two read-only tables are referenced from `.rodata` offsets 456 and 468:
- `(8, 4, 32)` and `(8, 4, 32)` as 32-bit ints

In handler logic these table values are selected for button types `<= 2` and compared against press counter; otherwise fallback constant path appears (`eax=4`).

## 4) Practical implication for current 5.10 host

This button pipeline exists only in `nas_ctrl.ko` (kernel 3.6.6 module).
Without loaded `nas_ctrl` + `nas_gpio`:
- No `/dev/nas_ctrl`
- No `/proc/nas_ctrl`
- No front-panel enter/select events observed on `/dev/input/event*`

Therefore front-panel ENTER/SELECT on the current 5.10 system is not reachable until equivalent driver path is ported/loaded.
