# nas_ctrl ioctl mapping (recovered)

Source binary:
- deb/lib/modules/3.6.6/kernel/landisk/nas_ctrl.ko

Method:
- ELF symbol parsing (pyelftools)
- relocation overlay on disassembly (capstone)
- cross-check against userland helper binaries in deb/usr/local/bin

## Command map

| ioctl hex | _IOC(type,nr,size,dir) | recovered handler path | userland helper evidence |
|---|---|---|---|
| 0x400c6702 | ('g',0x02,12,WRITE) | led_ioctl_set | setLED |
| 0x800c6703 | ('g',0x03,12,READ) | led_ioctl_get | none found |
| 0x6709 | ('g',0x09,0,NONE) | inline: updates hddpower_unplug + score_data_lock-derived field | none found |
| 0x670a | ('g',0x0a,0,NONE) | inline no-op success | none found |
| 0x4010670f | ('g',0x0f,16,WRITE) | mcu_program | none found |
| 0x44046710 | ('g',0x10,1028,WRITE) | mcu_burning | setMCUBurning |
| 0x6711 | ('g',0x11,0,NONE) | mcu_erase_all | none found |
| 0x40086715 | ('g',0x15,8,WRITE) | hdd_power_set | setHDDPower |
| 0x80086716 | ('g',0x16,8,READ) | hdd_power_get (+copy_to_user) | none found |
| 0x80086718 | ('g',0x18,8,READ) | hdd_detect_get (+copy_to_user) | detectHDD |
| 0x671a | ('g',0x1a,0,NONE) | disable_rtc | none found |
| 0x8004671e | ('g',0x1e,4,READ) | get_ac_failure_state (+copy_to_user) | get_power_resume_status |
| 0x40046720 | ('g',0x20,4,WRITE) | power_resume_set (uses pwr_state) | setPowerResume |

## Notes

- nas_ctrl.ko depends on nas_gpio.ko (vermagic 3.6.6).
- These modules are not ABI-compatible with Linux 5.10 kernels.
- MinGW objdump on this host cannot disassemble the module, so Python-based analysis is the reliable path in this workspace.
