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

#define DRV_NAME "nas_frontpanel_drv"
#define BTN_COUNT 5
#define NAS_DISK_MAX 6

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
static unsigned int poll_interval_ms = 250;
static bool invoke_sw_handler = true;
static char *sw_handler_path = "/mnt/data/libexec/button/sw_handler";

module_param_array(button_gpios, int, NULL, 0644);
MODULE_PARM_DESC(button_gpios, "GPIO numbers for func,power,reset,select,enter");
module_param_array(active_low, int, NULL, 0644);
MODULE_PARM_DESC(active_low, "Active-low flags (0/1) for func,power,reset,select,enter");
module_param(poll_interval_ms, uint, 0644);
MODULE_PARM_DESC(poll_interval_ms, "Polling interval in milliseconds");
module_param(invoke_sw_handler, bool, 0644);
MODULE_PARM_DESC(invoke_sw_handler, "Call usermode helper on button action");
module_param(sw_handler_path, charp, 0644);
MODULE_PARM_DESC(sw_handler_path, "Path to sw_handler executable");

struct btn_state {
    bool valid;
    bool pressed;
    bool fired;
    u32 ticks;
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
    int v = gpio_get_value_cansleep(button_gpios[idx]);

    if (v < 0)
        return false;
    return active_low[idx] ? !v : !!v;
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

    now_pressed = gpio_pressed(idx);

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
    seq_puts(m, "name gpio active_low pressed ticks fired\n");

    for (i = 0; i < BTN_COUNT; i++) {
        seq_printf(m, "%s %d %d %d %u %d\n",
                   btn_names[i],
                   button_gpios[i],
                   active_low[i],
                   states[i].pressed,
                   states[i].ticks,
                   states[i].fired);
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
            gpio_free(button_gpios[i]);
        states[i].valid = false;
    }
}

static int request_gpios(void)
{
    int i;
    int ret;

    for (i = 0; i < BTN_COUNT; i++) {
        if (!gpio_is_valid(button_gpios[i]))
            continue;

        ret = gpio_request_one(button_gpios[i], GPIOF_IN, btn_names[i]);
        if (ret)
            return ret;

        states[i].valid = true;
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
