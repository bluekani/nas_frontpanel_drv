// SPDX-License-Identifier: GPL-2.0
/*
 * nas_frontpanel_drv.c
 *
 * Clean-room replacement for button handling path observed in legacy nas_ctrl.ko.
 * Targets Linux 5.10+.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/kmod.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <asm/io.h>

#define DRV_NAME "nas_frontpanel_drv"
#define BTN_COUNT 5
#define NAS_DISK_MAX 6

#define BACKEND_NONE 0
#define BACKEND_GPIO 1
#define BACKEND_MSIO 2

#define IT87_CHIP_ID 0x8728
#define IT87_LDN_GPIO 0x07

/* Recovered nas_ctrl ioctl values */
#define NAS_CTRL_IOW_SET_LED          0x400c6702u
#define NAS_CTRL_IOR_GET_LED          0x800c6703u
#define NAS_CTRL_IO_SET_INTERNAL_09   0x00006709u
#define NAS_CTRL_IO_NOP_0A            0x0000670au
#define NAS_CTRL_IOW_SET_MCU_PROGRAM  0x4010670fu
#define NAS_CTRL_IOW_SET_MCU_BURNING  0x44046710u
#define NAS_CTRL_IO_SET_MCU_ERASE_ALL 0x00006711u
#define NAS_CTRL_IOW_SET_HDD_POWER    0x40086715u
#define NAS_CTRL_IOR_GET_HDD_POWER    0x80086716u
#define NAS_CTRL_IOR_GET_HDD_DETECT   0x80086718u
#define NAS_CTRL_IO_DISABLE_RTC       0x0000671au
#define NAS_CTRL_IOR_GET_POWER_RESUME 0x8004671eu
#define NAS_CTRL_IOW_SET_POWER_RESUME 0x40046720u

enum btn_idx {
    FP_BTN_FUNC = 0,
    FP_BTN_POWER,
    FP_BTN_RESET,
    FP_BTN_SELECT,
    FP_BTN_ENTER,
};

static const char *const btn_names[BTN_COUNT] = {
    "func", "power", "reset", "select", "enter"
};

static const unsigned short btn_keycodes[BTN_COUNT] = {
    KEY_PROG1, KEY_POWER, KEY_RESTART, KEY_SELECT, KEY_ENTER
};

/* Reverse-engineered default tick thresholds (poll based) */
static unsigned int threshold_ticks[BTN_COUNT] = {8, 4, 32, 4, 4};

static int button_gpios[BTN_COUNT] = {-1, -1, -1, -1, -1};
static int active_low[BTN_COUNT] = {1, 1, 1, 1, 1};
static int msio_button_lines[BTN_COUNT] = {-1, -1, -1, -1, -1};
static unsigned int poll_interval_ms = 250;
static bool invoke_sw_handler = true;
static char *sw_handler_path = "/mnt/data/libexec/button/sw_handler";
static bool use_msio_backend = true;
static int msio_index_port = -1;
static int msio_gpio_base = -1;
static bool msio_autodiscover = true;

module_param_array(button_gpios, int, NULL, 0644);
MODULE_PARM_DESC(button_gpios, "GPIO numbers for func,power,reset,select,enter");
module_param_array(active_low, int, NULL, 0644);
MODULE_PARM_DESC(active_low, "Active-low flags (0/1) for func,power,reset,select,enter");
module_param_array(msio_button_lines, int, NULL, 0644);
MODULE_PARM_DESC(msio_button_lines, "MSIO line numbers for func,power,reset,select,enter");
module_param(poll_interval_ms, uint, 0644);
MODULE_PARM_DESC(poll_interval_ms, "Polling interval in milliseconds");
module_param(invoke_sw_handler, bool, 0644);
MODULE_PARM_DESC(invoke_sw_handler, "Call usermode helper on button action");
module_param(sw_handler_path, charp, 0644);
MODULE_PARM_DESC(sw_handler_path, "Path to sw_handler executable");
module_param(use_msio_backend, bool, 0644);
MODULE_PARM_DESC(use_msio_backend, "Enable low-level Super I/O msGpioGet-compatible backend");
module_param(msio_index_port, int, 0644);
MODULE_PARM_DESC(msio_index_port, "Super I/O index port (0x2e or 0x4e). -1 means auto detect");
module_param(msio_gpio_base, int, 0644);
MODULE_PARM_DESC(msio_gpio_base, "GPIO IO base override. -1 means auto detect from Super I/O regs 0x62/0x63");
module_param(msio_autodiscover, bool, 0644);
MODULE_PARM_DESC(msio_autodiscover, "Log low-level line changes for mapping discovery when button lines are unknown");

struct btn_state {
    bool valid;
    bool pressed;
    bool fired;
    u32 ticks;
    u8 backend;
    int line;
};

struct nas_led_ioctl {
    u32 led;
    u32 color;
    u32 state;
};

struct nas_disk_ioctl {
    u32 disk;
    u32 value;
};

struct nas_mcu_small {
    u8 data[16];
};

struct nas_mcu_big {
    u8 data[1028];
};

static struct btn_state states[BTN_COUNT];
static struct input_dev *fp_input;
static struct delayed_work poll_work;
static DEFINE_MUTEX(state_lock);

static struct proc_dir_entry *proc_nas_ctrl;
static struct proc_dir_entry *proc_buttons;
static struct miscdevice nas_ctrl_miscdev;

static bool msio_ready;
static u8 msio_idx;
static u16 msio_base;
static DEFINE_SPINLOCK(msio_lock);
static bool msio_prev_valid;
static u8 msio_prev[9];

static char last_type[16] = "none";
static u32 last_second;

static struct nas_led_ioctl led_state;
static u32 pwr_resume_state;
static u32 hdd_power_state[NAS_DISK_MAX] = {1, 1, 1, 1, 1, 1};
static u32 hdd_detect_state[NAS_DISK_MAX];

static unsigned int ticks_to_seconds(u32 ticks)
{
    unsigned int sec = ticks / 4;

    if (sec > 99)
        sec = 99;
    return sec;
}

static void record_last_event(int idx, u32 ticks)
{
    strscpy(last_type, btn_names[idx], sizeof(last_type));
    last_second = ticks_to_seconds(ticks);
}

static void call_sw_handler(int idx, u32 ticks)
{
    char second_buf[4];
    char *argv[] = {
        sw_handler_path,
        "--type",
        (char *)btn_names[idx],
        "--second",
        second_buf,
        NULL,
    };
    char *envp[] = {
        "HOME=/",
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin",
        NULL,
    };

    snprintf(second_buf, sizeof(second_buf), "%u", ticks_to_seconds(ticks));
    record_last_event(idx, ticks);

    if (!invoke_sw_handler)
        return;

    call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
}

static bool gpio_pressed(int idx)
{
    int v;

    if (states[idx].backend != BACKEND_GPIO)
        return false;

    v = gpio_get_value_cansleep(states[idx].line);

    if (v < 0)
        return false;
    return active_low[idx] ? !v : !!v;
}

static bool msio_pressed(int idx)
{
    unsigned long flags;
    u8 val;
    u16 port;
    int bit;

    if (states[idx].backend != BACKEND_MSIO || !msio_ready)
        return false;

    if (states[idx].line < 0 || states[idx].line > 0x40)
        return false;

    port = msio_base + (states[idx].line >> 3);
    bit = states[idx].line & 7;

    spin_lock_irqsave(&msio_lock, flags);
    val = inb(port);
    spin_unlock_irqrestore(&msio_lock, flags);

    return active_low[idx] ? !((val >> bit) & 1) : !!((val >> bit) & 1);
}

static void msio_discover_changes(void)
{
    unsigned long flags;
    u8 cur[9];
    int i;

    if (!msio_ready || !msio_autodiscover)
        return;

    spin_lock_irqsave(&msio_lock, flags);
    for (i = 0; i < 9; i++)
        cur[i] = inb(msio_base + i);
    spin_unlock_irqrestore(&msio_lock, flags);

    if (!msio_prev_valid) {
        memcpy(msio_prev, cur, sizeof(cur));
        msio_prev_valid = true;
        return;
    }

    for (i = 0; i < 9; i++) {
        u8 diff = msio_prev[i] ^ cur[i];
        int b;

        if (!diff)
            continue;

        for (b = 0; b < 8; b++) {
            int line;

            if (!(diff & BIT(b)))
                continue;
            line = i * 8 + b;
            if (line > 0x40)
                continue;
            pr_info(DRV_NAME ": msio line %d changed -> %d\n",
                    line, !!(cur[i] & BIT(b)));
        }
    }

    memcpy(msio_prev, cur, sizeof(cur));
}

static void sio_enter_cfg(u8 idx_port)
{
    outb(0x87, idx_port);
    outb(0x01, idx_port);
    outb(0x55, idx_port);
    outb(idx_port == 0x2e ? 0x55 : 0xaa, idx_port);
}

static void sio_exit_cfg(u8 idx_port)
{
    outb(0x02, idx_port);
    outb(0x02, idx_port + 1);
}

static u8 sio_read(u8 idx_port, u8 reg)
{
    outb(reg, idx_port);
    return inb(idx_port + 1);
}

static void sio_write(u8 idx_port, u8 reg, u8 value)
{
    outb(reg, idx_port);
    outb(value, idx_port + 1);
}

static int msio_init(void)
{
    u8 idx_ports[2] = {0x2e, 0x4e};
    int i;

    if (msio_gpio_base >= 0) {
        msio_base = (u16)msio_gpio_base;
        if (!request_region(msio_base, 8, DRV_NAME ":msio"))
            return -EBUSY;
        msio_ready = true;
        pr_info(DRV_NAME ": msio base forced at 0x%x\n", msio_base);
        return 0;
    }

    if (msio_index_port >= 0) {
        idx_ports[0] = (u8)msio_index_port;
        idx_ports[1] = 0;
    }

    for (i = 0; i < ARRAY_SIZE(idx_ports); i++) {
        u8 p = idx_ports[i];
        u16 id;
        u8 c1;
        u16 base;

        if (!p)
            continue;

        sio_enter_cfg(p);
        id = ((u16)sio_read(p, 0x20) << 8) | sio_read(p, 0x21);
        if (id != IT87_CHIP_ID) {
            sio_exit_cfg(p);
            continue;
        }

        sio_write(p, 0x07, IT87_LDN_GPIO);
        c1 = sio_read(p, 0xc1);
        sio_write(p, 0xc1, c1 | 0x02);
        base = ((u16)sio_read(p, 0x62) << 8) | sio_read(p, 0x63);
        sio_exit_cfg(p);

        if (!base)
            continue;
        if (!request_region(base, 8, DRV_NAME ":msio"))
            return -EBUSY;

        msio_idx = p;
        msio_base = base;
        msio_ready = true;
        pr_info(DRV_NAME ": Found IT%04x at index 0x%x, gpio base 0x%x\n",
                IT87_CHIP_ID, msio_idx, msio_base);
        return 0;
    }

    return -ENODEV;
}

static void msio_exit(void)
{
    if (msio_ready)
        release_region(msio_base, 8);
    msio_ready = false;
    msio_base = 0;
    msio_prev_valid = false;
}

static int disk_to_index(u32 disk)
{
    if (disk < 1 || disk > NAS_DISK_MAX)
        return -EINVAL;
    return (int)disk - 1;
}

static long nas_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    void __user *uarg = (void __user *)arg;
    struct nas_led_ioctl led;
    struct nas_disk_ioctl disk;
    struct nas_mcu_small mcu_s;
    struct nas_mcu_big mcu_b;
    u32 value;
    int idx;

    mutex_lock(&state_lock);

    switch (cmd) {
    case NAS_CTRL_IOW_SET_LED:
        if (copy_from_user(&led, uarg, sizeof(led)))
            goto efault;
        led_state = led;
        break;

    case NAS_CTRL_IOR_GET_LED:
        if (copy_to_user(uarg, &led_state, sizeof(led_state)))
            goto efault;
        break;

    case NAS_CTRL_IOW_SET_HDD_POWER:
        if (copy_from_user(&disk, uarg, sizeof(disk)))
            goto efault;
        idx = disk_to_index(disk.disk);
        if (idx < 0)
            goto einval;
        hdd_power_state[idx] = disk.value ? 1 : 0;
        break;

    case NAS_CTRL_IOR_GET_HDD_POWER:
        if (copy_from_user(&disk, uarg, sizeof(disk)))
            goto efault;
        idx = disk_to_index(disk.disk);
        if (idx < 0)
            goto einval;
        disk.value = hdd_power_state[idx];
        if (copy_to_user(uarg, &disk, sizeof(disk)))
            goto efault;
        break;

    case NAS_CTRL_IOR_GET_HDD_DETECT:
        if (copy_from_user(&disk, uarg, sizeof(disk)))
            goto efault;
        idx = disk_to_index(disk.disk);
        if (idx < 0)
            goto einval;
        disk.value = hdd_detect_state[idx];
        if (copy_to_user(uarg, &disk, sizeof(disk)))
            goto efault;
        break;

    case NAS_CTRL_IOW_SET_POWER_RESUME:
        if (copy_from_user(&value, uarg, sizeof(value)))
            goto efault;
        pwr_resume_state = value;
        break;

    case NAS_CTRL_IOR_GET_POWER_RESUME:
        if (copy_to_user(uarg, &pwr_resume_state, sizeof(pwr_resume_state)))
            goto efault;
        break;

    case NAS_CTRL_IOW_SET_MCU_PROGRAM:
        if (copy_from_user(&mcu_s, uarg, sizeof(mcu_s)))
            goto efault;
        break;

    case NAS_CTRL_IOW_SET_MCU_BURNING:
        if (copy_from_user(&mcu_b, uarg, sizeof(mcu_b)))
            goto efault;
        break;

    case NAS_CTRL_IO_SET_MCU_ERASE_ALL:
    case NAS_CTRL_IO_DISABLE_RTC:
    case NAS_CTRL_IO_SET_INTERNAL_09:
    case NAS_CTRL_IO_NOP_0A:
        break;

    default:
        mutex_unlock(&state_lock);
        return -ENOTTY;
    }

    mutex_unlock(&state_lock);
    return 0;

efault:
    mutex_unlock(&state_lock);
    return -EFAULT;
einval:
    mutex_unlock(&state_lock);
    return -EINVAL;
}

static ssize_t nas_ctrl_read(struct file *file, char __user *buf,
                             size_t count, loff_t *ppos)
{
    return 0;
}

static ssize_t nas_ctrl_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    return count;
}

static const struct file_operations nas_ctrl_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = nas_ctrl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = nas_ctrl_ioctl,
#endif
    .read = nas_ctrl_read,
    .write = nas_ctrl_write,
};

static void process_button(int idx)
{
    struct btn_state *st = &states[idx];
    bool now_pressed;

    if (!st->valid)
        return;

    if (st->backend == BACKEND_GPIO)
        now_pressed = gpio_pressed(idx);
    else
        now_pressed = msio_pressed(idx);

    if (now_pressed) {
        if (!st->pressed) {
            st->ticks = 0;
            st->fired = false;
            input_report_key(fp_input, btn_keycodes[idx], 1);
            input_sync(fp_input);
        }

        if (st->ticks < UINT_MAX)
            st->ticks++;

        if (!st->fired && st->ticks >= threshold_ticks[idx]) {
            st->fired = true;
            call_sw_handler(idx, st->ticks);
        }
    } else {
        if (st->pressed) {
            input_report_key(fp_input, btn_keycodes[idx], 0);
            input_sync(fp_input);

            if (!st->fired && st->ticks > 0)
                call_sw_handler(idx, st->ticks);
        }

        st->ticks = 0;
        st->fired = false;
    }

    st->pressed = now_pressed;
}

static void poll_work_fn(struct work_struct *work)
{
    int i;

    mutex_lock(&state_lock);
    for (i = 0; i < BTN_COUNT; i++)
        process_button(i);
    msio_discover_changes();
    mutex_unlock(&state_lock);

    schedule_delayed_work(&poll_work, msecs_to_jiffies(poll_interval_ms));
}

static int buttons_show(struct seq_file *m, void *v)
{
    int i;

    mutex_lock(&state_lock);
    seq_printf(m, "last_type=%s\n", last_type);
    seq_printf(m, "last_second=%u\n", last_second);
    seq_printf(m, "pwr_resume_state=%u\n", pwr_resume_state);
    seq_printf(m, "msio_ready=%d\n", msio_ready ? 1 : 0);
    seq_printf(m, "msio_index=0x%x\n", msio_idx);
    seq_printf(m, "msio_base=0x%x\n", msio_base);
    seq_puts(m, "name cfg_gpio cfg_msio active_low pressed ticks fired backend line\n");

    for (i = 0; i < BTN_COUNT; i++) {
        seq_printf(m, "%s %d %d %d %d %u %d %u %d\n",
                   btn_names[i],
                   button_gpios[i],
                   msio_button_lines[i],
                   active_low[i],
                   states[i].pressed,
                   states[i].ticks,
                   states[i].fired,
                   states[i].backend,
                   states[i].line);
    }
    mutex_unlock(&state_lock);
    return 0;
}

static int buttons_open(struct inode *inode, struct file *file)
{
    return single_open(file, buttons_show, NULL);
}

static const struct proc_ops buttons_proc_ops = {
    .proc_open = buttons_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int setup_input(void)
{
    int i;
    int ret;

    fp_input = input_allocate_device();
    if (!fp_input)
        return -ENOMEM;

    fp_input->name = "NAS Frontpanel Buttons";
    fp_input->phys = "nas/frontpanel/input0";
    fp_input->id.bustype = BUS_HOST;

    for (i = 0; i < BTN_COUNT; i++)
        input_set_capability(fp_input, EV_KEY, btn_keycodes[i]);

    ret = input_register_device(fp_input);
    if (ret) {
        input_free_device(fp_input);
        fp_input = NULL;
        return ret;
    }

    return 0;
}

static void release_gpios(void)
{
    int i;

    for (i = 0; i < BTN_COUNT; i++) {
        if (states[i].valid)
            if (states[i].backend == BACKEND_GPIO)
                gpio_free(states[i].line);
        states[i].valid = false;
        states[i].backend = BACKEND_NONE;
        states[i].line = -1;
    }

    msio_exit();
}

static int request_gpios(void)
{
    int i;
    int ret;
    int configured = 0;

    for (i = 0; i < BTN_COUNT; i++) {
        if (!gpio_is_valid(button_gpios[i]))
            continue;

        ret = gpio_request_one(button_gpios[i], GPIOF_IN, btn_names[i]);
        if (ret)
            return ret;

        states[i].valid = true;
        states[i].backend = BACKEND_GPIO;
        states[i].line = button_gpios[i];
        configured++;
    }

    if (!configured && use_msio_backend) {
        ret = msio_init();
        if (ret)
            return ret;

        for (i = 0; i < BTN_COUNT; i++) {
            if (msio_button_lines[i] < 0)
                continue;
            if (msio_button_lines[i] > 0x40)
                continue;
            states[i].valid = true;
            states[i].backend = BACKEND_MSIO;
            states[i].line = msio_button_lines[i];
            configured++;
        }
    }

    if (!configured) {
        if (msio_ready && msio_autodiscover)
            pr_warn(DRV_NAME ": no button lines configured; running in msio autodiscover mode\n");
        else {
            pr_err(DRV_NAME ": no valid inputs configured; set button_gpios=... or msio_button_lines=...\n");
            return -EINVAL;
        }
    }

    return 0;
}

static void cleanup_proc(void)
{
    if (proc_buttons)
        proc_remove(proc_buttons);
    proc_buttons = NULL;

    if (proc_nas_ctrl)
        proc_remove(proc_nas_ctrl);
    proc_nas_ctrl = NULL;
}

static int setup_proc(void)
{
    proc_nas_ctrl = proc_mkdir("nas_ctrl", NULL);
    if (!proc_nas_ctrl)
        return -ENOMEM;

    proc_buttons = proc_create("buttons", 0444, proc_nas_ctrl, &buttons_proc_ops);
    if (!proc_buttons) {
        cleanup_proc();
        return -ENOMEM;
    }

    return 0;
}

static int __init nas_frontpanel_init(void)
{
    int ret;

    ret = request_gpios();
    if (ret)
        goto err;

    ret = setup_input();
    if (ret)
        goto err_gpio;

    ret = setup_proc();
    if (ret)
        goto err_input;

    nas_ctrl_miscdev.minor = MISC_DYNAMIC_MINOR;
    nas_ctrl_miscdev.name = "nas_ctrl";
    nas_ctrl_miscdev.fops = &nas_ctrl_fops;

    ret = misc_register(&nas_ctrl_miscdev);
    if (ret)
        goto err_proc;

    INIT_DELAYED_WORK(&poll_work, poll_work_fn);
    schedule_delayed_work(&poll_work, msecs_to_jiffies(poll_interval_ms));

    pr_info(DRV_NAME ": loaded\n");
    return 0;

err_proc:
    cleanup_proc();

err_input:
    if (fp_input)
        input_unregister_device(fp_input);
    fp_input = NULL;
err_gpio:
    release_gpios();
err:
    return ret;
}

static void __exit nas_frontpanel_exit(void)
{
    cancel_delayed_work_sync(&poll_work);
    misc_deregister(&nas_ctrl_miscdev);
    cleanup_proc();

    if (fp_input)
        input_unregister_device(fp_input);
    fp_input = NULL;

    release_gpios();

    pr_info(DRV_NAME ": unloaded\n");
}

module_init(nas_frontpanel_init);
module_exit(nas_frontpanel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Reverse-engineering based implementation");
MODULE_DESCRIPTION("NAS frontpanel replacement driver with sw_handler callback");
