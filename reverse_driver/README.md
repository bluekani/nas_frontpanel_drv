# nas frontpanel replacement driver (for Linux 5.10+)

This is a clean-room replacement driver based on reverse engineering findings from `nas_ctrl.ko`.

Implemented scope:
- Poll GPIO buttons: `func`, `power`, `reset`, `select`, `enter`
- Poll low-level Super I/O GPIO lines (`msGpioGet` compatible path)
- Emit Linux input key events
- Optionally invoke usermode helper:
  - `/mnt/data/libexec/button/sw_handler --type <name> --second <sec>`
- Export runtime state under `/proc/nas_ctrl/buttons`
- Provide `/dev/nas_ctrl` (misc device) with partial ioctl compatibility

Not implemented (yet):
- Melody/buzzer/MCU programming path
- HDD LED and power control ioctls

Partially implemented ioctl commands:
- `0x400c6702` set LED
- `0x800c6703` get LED
- `0x40086715` set HDD power
- `0x80086716` get HDD power
- `0x80086718` get HDD detect
- `0x40046720` set power-resume
- `0x8004671e` get power-resume
- `0x4010670f` set MCU program (accepted, no-op)
- `0x44046710` set MCU burning (accepted, no-op)
- `0x6711`, `0x671a`, `0x6709`, `0x670a` (accepted, no-op)

## Build

```sh
make
```

If build fails with missing kernel headers/toolchain, install prerequisites first:

```sh
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r)
```

## Load example

```sh
sudo insmod nas_frontpanel_drv.ko \
  button_gpios=17,18,19,20,21 \
  active_low=1,1,1,1,1 \
  poll_interval_ms=250 \
  invoke_sw_handler=1
```

Low-level Super I/O backend example (no Linux gpiochip required):

```sh
sudo insmod nas_frontpanel_drv.ko \
  button_gpios=-1,-1,-1,-1,-1 \
  msio_button_lines=0,-1,-1,6,7 \
  use_msio_backend=1 \
  msio_autodiscover=0 \
  active_low=1,1,1,1,1 \
  poll_interval_ms=250 \
  invoke_sw_handler=1
```

Autodiscovery mode (to find unknown line numbers):

```sh
sudo insmod nas_frontpanel_drv.ko \
  button_gpios=-1,-1,-1,-1,-1 \
  msio_button_lines=-1,-1,-1,-1,-1 \
  use_msio_backend=1 \
  msio_autodiscover=1
dmesg -w
```

Press front-panel buttons and map lines from logs like:

```text
nas_frontpanel_drv: msio line 23 changed -> 0
nas_frontpanel_drv: msio line 23 changed -> 1
```

Then reload with `msio_button_lines=` set to discovered line numbers.

Verified mapping on tested target:

- `func=0`
- `select=6`
- `enter=7`
- `power/reset` not confirmed yet (`-1`)
- observed frequent toggles on lines `22/23` during testing are likely unrelated activity (for example LED/HDD access paths)

## Validate

- Input events:
```sh
sudo evtest
```
- Proc status:
```sh
cat /proc/nas_ctrl/buttons
```

- Legacy helper compatibility (after install to `/lib/modules/...` and load):
```sh
test -e /dev/nas_ctrl && echo ok
setPowerResume on
get_power_resume_status
setHDDPower 1 on
detectHDD 1
setLED POWER GREEN ON
```

## Notes

- GPIO numbers are board-specific; set `button_gpios=` accordingly.
- If Linux GPIO is unavailable on your board, use `msio_button_lines=` and `use_msio_backend=1`.
- `msio_index_port=-1` auto-detects `0x2e/0x4e`; `msio_gpio_base=-1` auto-detects from SIO regs `0x62/0x63`.
- Driver defaults currently include the verified mapping: `msio_button_lines=0,-1,-1,6,7`.
- Threshold ticks are reverse-engineered defaults: `8,4,32,4,4`.
- Current remote host check (2026-05-28): no `/lib/modules/$(uname -r)/build` and no `gcc/make`,
  so module build is blocked until prerequisites are installed.
