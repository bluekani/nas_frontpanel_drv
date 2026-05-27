/* Recovered from deb/lib/modules/3.6.6/kernel/landisk/nas_ctrl.ko
 * via symbol + relocation + instruction analysis.
 * NOTE: Some names are inferred from call targets and data references.
 */
#ifndef NAS_CTRL_IOCTL_RECOVERED_H
#define NAS_CTRL_IOCTL_RECOVERED_H

#include <linux/ioctl.h>

#define NAS_CTRL_IOC_MAGIC 'g'

/* Confirmed by userland tools in deb/usr/local/bin */
#define NAS_CTRL_IOW_SET_LED            _IOW(NAS_CTRL_IOC_MAGIC, 0x02, unsigned char[12])   /* setLED -> 0x400c6702 */
#define NAS_CTRL_IOR_GET_POWER_RESUME   _IOR(NAS_CTRL_IOC_MAGIC, 0x1e, unsigned int)        /* get_power_resume_status -> 0x8004671e */
#define NAS_CTRL_IOW_SET_MCU_BURNING    _IOW(NAS_CTRL_IOC_MAGIC, 0x10, unsigned char[1028]) /* setMCUBurning -> 0x44046710 */
#define NAS_CTRL_IO_SET_MCU_ERASE_ALL   _IO(NAS_CTRL_IOC_MAGIC, 0x11)                        /* nas_ctrl_ioctl -> mcu_erase_all */
#define NAS_CTRL_IOW_SET_POWER_RESUME   _IOW(NAS_CTRL_IOC_MAGIC, 0x20, unsigned int)        /* setPowerResume -> 0x40046720 */
#define NAS_CTRL_IOW_SET_HDD_POWER      _IOW(NAS_CTRL_IOC_MAGIC, 0x15, unsigned char[8])    /* setHDDPower -> 0x40086715 */
#define NAS_CTRL_IOR_GET_HDD_DETECT     _IOR(NAS_CTRL_IOC_MAGIC, 0x18, unsigned char[8])    /* detectHDD -> 0x80086718 */

/* High-confidence inferred from nas_ctrl_ioctl branch targets */
#define NAS_CTRL_IOR_GET_LED            _IOR(NAS_CTRL_IOC_MAGIC, 0x03, unsigned char[12])   /* -> led_ioctl_get, 0x800c6703 */
#define NAS_CTRL_IOW_SET_MCU_PROGRAM    _IOW(NAS_CTRL_IOC_MAGIC, 0x0f, unsigned char[16])   /* -> mcu_program, 0x4010670f */
#define NAS_CTRL_IOR_GET_HDD_POWER      _IOR(NAS_CTRL_IOC_MAGIC, 0x16, unsigned char[8])    /* copy_from_user(8) -> hdd_power_get -> copy_to_user, 0x80086716 */
#define NAS_CTRL_IO_DISABLE_RTC         _IO(NAS_CTRL_IOC_MAGIC, 0x1a)                        /* -> disable_rtc, 0x671a */

/* Internal state controls from inline blocks in nas_ctrl_ioctl */
#define NAS_CTRL_IO_SET_HDDPOWER_STATE  _IO(NAS_CTRL_IOC_MAGIC, 0x09)                        /* 0x6709: updates hddpower_unplug and score_data_lock-derived field */
#define NAS_CTRL_IO_NOP_0A              _IO(NAS_CTRL_IOC_MAGIC, 0x0a)                        /* 0x670a: immediate success path */

#endif /* NAS_CTRL_IOCTL_RECOVERED_H */
