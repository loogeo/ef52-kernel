/* Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "subsys-restart: %s(): " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/wakelock.h>
#include <linux/suspend.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/debugfs.h>
#include <asm/current.h>
#include <mach/peripheral-loader.h>
#include <mach/socinfo.h>
#include <mach/subsystem_notif.h>
#include <mach/subsystem_restart.h>

#include "smd_private.h"

#ifdef CONFIG_PANTECH_ERR_CRASH_LOGGING
#include <linux/kobject.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>

int ssrEvent = 16;  // SSR_UNKNOWN_KERNEL_LOG_FILE
void ssr_uevent_set( void );
#endif

#ifdef CONFIG_PANTECH_RESET_REASON
#include "sky_sys_reset.h"
#endif

struct subsys_soc_restart_order {
	const char * const *subsystem_list;
	int count;

	struct mutex shutdown_lock;
	struct mutex powerup_lock;
	struct subsys_device *subsys_ptrs[];
};

struct restart_log {
	struct timeval time;
	struct subsys_device *dev;
	struct list_head list;
};

enum subsys_state {
	SUBSYS_OFFLINE,
	SUBSYS_ONLINE,
	SUBSYS_CRASHED,
};

static const char * const subsys_states[] = {
	[SUBSYS_OFFLINE] = "OFFLINE",
	[SUBSYS_ONLINE] = "ONLINE",
	[SUBSYS_CRASHED] = "CRASHED",
};

struct subsys_device {
	struct subsys_desc *desc;
	struct wake_lock wake_lock;
	char wlname[64];
	struct work_struct work;
	spinlock_t restart_lock;
	bool restarting;

	void *notify;
	struct device dev;
	struct module *owner;
	int count;
	enum subsys_state state;
	int id;

	struct mutex shutdown_lock;
	struct mutex powerup_lock;

	void *restart_order;
#ifdef CONFIG_DEBUG_FS
	struct dentry *dentry;
#endif
};

static struct subsys_device *to_subsys(struct device *d)
{
	return container_of(d, struct subsys_device, dev);
}

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", to_subsys(dev)->desc->name);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	enum subsys_state state = to_subsys(dev)->state;
	return snprintf(buf, PAGE_SIZE, "%s\n", subsys_states[state]);
}

static void subsys_set_state(struct subsys_device *subsys,
			     enum subsys_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&subsys->restart_lock, flags);
	if (subsys->state != state) {
		subsys->state = state;
		spin_unlock_irqrestore(&subsys->restart_lock, flags);
		sysfs_notify(&subsys->dev.kobj, NULL, "state");
		return;
	}
	spin_unlock_irqrestore(&subsys->restart_lock, flags);
}

static struct device_attribute subsys_attrs[] = {
	__ATTR_RO(name),
	__ATTR_RO(state),
	__ATTR_NULL,
};

static struct bus_type subsys_bus_type = {
	.name		= "msm_subsys",
	.dev_attrs	= subsys_attrs,
};

static DEFINE_IDA(subsys_ida);

static int enable_ramdumps;
module_param(enable_ramdumps, int, S_IRUGO | S_IWUSR);

struct workqueue_struct *ssr_wq;

static LIST_HEAD(restart_log_list);
static DEFINE_MUTEX(soc_order_reg_lock);
static DEFINE_MUTEX(restart_log_mutex);

/* SOC specific restart orders go here */

#define DEFINE_SINGLE_RESTART_ORDER(name, order)		\
	static struct subsys_soc_restart_order __##name = {	\
		.subsystem_list = order,			\
		.count = ARRAY_SIZE(order),			\
		.subsys_ptrs = {[ARRAY_SIZE(order)] = NULL}	\
	};							\
	static struct subsys_soc_restart_order *name[] = {      \
		&__##name,					\
	}

/* MSM 8x60 restart ordering info */
static const char * const _order_8x60_all[] = {
	"external_modem",  "modem", "lpass"
};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_all, _order_8x60_all);

static const char * const _order_8x60_modems[] = {"external_modem", "modem"};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_modems, _order_8x60_modems);

/*SGLTE restart ordering info*/
static const char * const order_8960_sglte[] = {"external_modem",
						"modem"};

static struct subsys_soc_restart_order restart_orders_8960_fusion_sglte = {
	.subsystem_list = order_8960_sglte,
	.count = ARRAY_SIZE(order_8960_sglte),
	.subsys_ptrs = {[ARRAY_SIZE(order_8960_sglte)] = NULL}
	};

static struct subsys_soc_restart_order *restart_orders_8960_sglte[] = {
	&restart_orders_8960_fusion_sglte,
	};

/* These will be assigned to one of the sets above after
 * runtime SoC identification.
 */
static struct subsys_soc_restart_order **restart_orders;
static int n_restart_orders;

static int restart_level = RESET_SOC;

int get_restart_level()
{
	return restart_level;
}
EXPORT_SYMBOL(get_restart_level);
#if defined(CONFIG_FEATURE_PANTECH_RESET_DSPS_CORE) //p12911
int restore_restart_level(int restart)
{
	int ret=0;
	restart_level=restart;
	ret=restart_level;
	return restart_level;
}
EXPORT_SYMBOL(restore_restart_level);
#endif



static int restart_level_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = restart_level;

	if (cpu_is_msm9615()) {
		pr_err("Only Phase 1 subsystem restart is supported\n");
		return -EINVAL;
	}

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	switch (restart_level) {
	case RESET_SUBSYS_INDEPENDENT:
		if (socinfo_get_platform_subtype() == PLATFORM_SUBTYPE_SGLTE) {
			pr_info("Phase 3 is currently unsupported. Using phase 2 instead.\n");
			restart_level = RESET_SUBSYS_COUPLED;
		}
	case RESET_SUBSYS_COUPLED:
	case RESET_SOC:
		pr_info("Phase %d behavior activated.\n", restart_level);
		break;
	default:
		restart_level = old_val;
		return -EINVAL;
	}
	return 0;
}

module_param_call(restart_level, restart_level_set, param_get_int,
			&restart_level, 0644);

static struct subsys_soc_restart_order *
update_restart_order(struct subsys_device *dev)
{
	int i, j;
	struct subsys_soc_restart_order *order;
	const char *name = dev->desc->name;
	int len = SUBSYS_NAME_MAX_LENGTH;

	mutex_lock(&soc_order_reg_lock);
	for (j = 0; j < n_restart_orders; j++) {
		order = restart_orders[j];
		for (i = 0; i < order->count; i++) {
			if (!strncmp(order->subsystem_list[i], name, len)) {
				order->subsys_ptrs[i] = dev;
				goto found;
			}
		}
	}
	order = NULL;
found:
	mutex_unlock(&soc_order_reg_lock);

	return order;
}

static int max_restarts;
module_param(max_restarts, int, 0644);

static long max_history_time = 3600;
module_param(max_history_time, long, 0644);

static void do_epoch_check(struct subsys_device *dev)
{
	int n = 0;
	struct timeval *time_first = NULL, *curr_time;
	struct restart_log *r_log, *temp;
	static int max_restarts_check;
	static long max_history_time_check;

	mutex_lock(&restart_log_mutex);

	max_restarts_check = max_restarts;
	max_history_time_check = max_history_time;

	/* Check if epoch checking is enabled */
	if (!max_restarts_check)
		goto out;

	r_log = kmalloc(sizeof(struct restart_log), GFP_KERNEL);
	if (!r_log)
		goto out;
	r_log->dev = dev;
	do_gettimeofday(&r_log->time);
	curr_time = &r_log->time;
	INIT_LIST_HEAD(&r_log->list);

	list_add_tail(&r_log->list, &restart_log_list);

	list_for_each_entry_safe(r_log, temp, &restart_log_list, list) {

		if ((curr_time->tv_sec - r_log->time.tv_sec) >
				max_history_time_check) {

			pr_debug("Deleted node with restart_time = %ld\n",
					r_log->time.tv_sec);
			list_del(&r_log->list);
			kfree(r_log);
			continue;
		}
		if (!n) {
			time_first = &r_log->time;
			pr_debug("Time_first: %ld\n", time_first->tv_sec);
		}
		n++;
		pr_debug("Restart_time: %ld\n", r_log->time.tv_sec);
	}

	if (time_first && n >= max_restarts_check) {
		if ((curr_time->tv_sec - time_first->tv_sec) <
				max_history_time_check)
			panic("Subsystems have crashed %d times in less than "
				"%ld seconds!", max_restarts_check,
				max_history_time_check);
	}

out:
	mutex_unlock(&restart_log_mutex);
}

static void for_each_subsys_device(struct subsys_device **list, unsigned count,
		void *data, void (*fn)(struct subsys_device *, void *))
{
	while (count--) {
		struct subsys_device *dev = *list++;
		if (!dev)
			continue;
		fn(dev, data);
	}
}

static void __send_notification_to_order(struct subsys_device *dev, void *data)
{
	enum subsys_notif_type type = (enum subsys_notif_type)data;

	subsys_notif_queue_notification(dev->notify, type);
}

static void send_notification_to_order(struct subsys_device **l, unsigned n,
		enum subsys_notif_type t)
{
	for_each_subsys_device(l, n, (void *)t, __send_notification_to_order);
}

static void subsystem_shutdown(struct subsys_device *dev, void *data)
{
	const char *name = dev->desc->name;

	pr_info("[%p]: Shutting down %s\n", current, name);
	if (dev->desc->shutdown(dev->desc) < 0)
		panic("subsys-restart: [%p]: Failed to shutdown %s!",
			current, name);
	subsys_set_state(dev, SUBSYS_OFFLINE);
}

static void subsystem_ramdump(struct subsys_device *dev, void *data)
{
	const char *name = dev->desc->name;

	if (dev->desc->ramdump)
    {
		if (dev->desc->ramdump(enable_ramdumps, dev->desc) < 0)
			pr_warn("%s[%p]: Ramdump failed.\n", name, current);

#ifdef CONFIG_PANTECH_ERR_CRASH_LOGGING
        // 11: SSR_LPASS_KERNEL_LOG_FILE
        // 12: SSR_MODEM_KERNEL_LOG_FILE
        // 13: SSR_DSPS_KERNEL_LOG_FILE
        // 14: SSR_RIVA_KERNEL_LOG_FILE
        // 15: SSR_MDM_KERNEL_LOG_FILE
        // 16: SSR_UNKNOWN_KERNEL_LOG_FILE
        if(!strncmp(name, "lpass", 5))
        {
            ssrEvent = 11;
}
        else if(!strncmp(name, "modem", 5))
        {
            ssrEvent = 12;
        }
        else if(!strncmp(name, "dsps", 4))
        {
            ssrEvent = 13;
        }
        else if(!strncmp(name, "wcnss", 5))
        {
            ssrEvent = 14;
        }
        else if(!strncmp(name, "external_modem", 14))
        {
            ssrEvent = 15;
        }
        else
        {
            ssrEvent = 16;
        }
#if defined(CONFIG_FEATURE_PANTECH_RESET_DSPS_CORE)				
		if(!dsps_reset_state()){
        ssr_uevent_set();
#else
        ssr_uevent_set();
#endif
    }
#endif
}
}

static void subsystem_powerup(struct subsys_device *dev, void *data)
{
	const char *name = dev->desc->name;

	pr_info("[%p]: Powering up %s\n", current, name);
	if (dev->desc->powerup(dev->desc) < 0)
		panic("[%p]: Failed to powerup %s!", current, name);
	subsys_set_state(dev, SUBSYS_ONLINE);
}

static int __find_subsys(struct device *dev, void *data)
{
	struct subsys_device *subsys = to_subsys(dev);
	return !strcmp(subsys->desc->name, data);
}

static struct subsys_device *find_subsys(const char *str)
{
	struct device *dev;

	if (!str)
		return NULL;

	dev = bus_find_device(&subsys_bus_type, NULL, (void *)str,
			__find_subsys);
	return dev ? to_subsys(dev) : NULL;
}

static void subsystem_restart_wq_func(struct work_struct *work)
{
	struct subsys_device *dev = container_of(work,
						struct subsys_device, work);
	struct subsys_device **list;
	struct subsys_desc *desc = dev->desc;
	struct subsys_soc_restart_order *soc_restart_order = NULL;
	struct mutex *powerup_lock;
	struct mutex *shutdown_lock;
	unsigned count;
	unsigned long flags;

	if (restart_level != RESET_SUBSYS_INDEPENDENT)
		soc_restart_order = dev->restart_order;

	/*
	 * It's OK to not take the registration lock at this point.
	 * This is because the subsystem list inside the relevant
	 * restart order is not being traversed.
	 */
	if (!soc_restart_order) {
		list = &dev;
		count = 1;
		powerup_lock = &dev->powerup_lock;
		shutdown_lock = &dev->shutdown_lock;
	} else {
		list = soc_restart_order->subsys_ptrs;
		count = soc_restart_order->count;
		powerup_lock = &soc_restart_order->powerup_lock;
		shutdown_lock = &soc_restart_order->shutdown_lock;
	}

	pr_debug("[%p]: Attempting to get shutdown lock!\n", current);

	/*
	 * Try to acquire shutdown_lock. If this fails, these subsystems are
	 * already being restarted - return.
	 */
	if (!mutex_trylock(shutdown_lock))
		goto out;

	pr_debug("[%p]: Attempting to get powerup lock!\n", current);

	/*
	 * Now that we've acquired the shutdown lock, either we're the first to
	 * restart these subsystems or some other thread is doing the powerup
	 * sequence for these subsystems. In the latter case, panic and bail
	 * out, since a subsystem died in its powerup sequence. This catches
	 * the case where a subsystem in a restart order isn't the one
	 * who initiated the original restart but has crashed while the restart
	 * order is being rebooted.
	 */
	if (!mutex_trylock(powerup_lock))
		panic("%s[%p]: Subsystem died during powerup!",
						__func__, current);

	do_epoch_check(dev);

	/*
	 * It's necessary to take the registration lock because the subsystem
	 * list in the SoC restart order will be traversed and it shouldn't be
	 * changed until _this_ restart sequence completes.
	 */
	mutex_lock(&soc_order_reg_lock);

	pr_debug("[%p]: Starting restart sequence for %s\n", current,
			desc->name);
	send_notification_to_order(list, count, SUBSYS_BEFORE_SHUTDOWN);
	for_each_subsys_device(list, count, NULL, subsystem_shutdown);
	send_notification_to_order(list, count, SUBSYS_AFTER_SHUTDOWN);

	/*
	 * Now that we've finished shutting down these subsystems, release the
	 * shutdown lock. If a subsystem restart request comes in for a
	 * subsystem in _this_ restart order after the unlock below, and
	 * before the powerup lock is released, panic and bail out.
	 */
	mutex_unlock(shutdown_lock);

	/* Collect ram dumps for all subsystems in order here */
	for_each_subsys_device(list, count, NULL, subsystem_ramdump);

	send_notification_to_order(list, count, SUBSYS_BEFORE_POWERUP);
	for_each_subsys_device(list, count, NULL, subsystem_powerup);
	send_notification_to_order(list, count, SUBSYS_AFTER_POWERUP);

	pr_info("[%p]: Restart sequence for %s completed.\n",
			current, desc->name);

	mutex_unlock(powerup_lock);

	mutex_unlock(&soc_order_reg_lock);

	pr_debug("[%p]: Released powerup lock!\n", current);

out:
	spin_lock_irqsave(&dev->restart_lock, flags);
	dev->restarting = false;
	wake_unlock(&dev->wake_lock);
	spin_unlock_irqrestore(&dev->restart_lock, flags);
}

static void __subsystem_restart_dev(struct subsys_device *dev)
{
	struct subsys_desc *desc = dev->desc;
	const char *name = dev->desc->name;
	unsigned long flags;

	pr_debug("Restarting %s [level=%d]!\n", desc->name, restart_level);

	/*
	 * We want to allow drivers to call subsystem_restart{_dev}() as many
	 * times as they want up until the point where the subsystem is
	 * shutdown.
	 */
	spin_lock_irqsave(&dev->restart_lock, flags);
	if (dev->state != SUBSYS_CRASHED) {
		if (dev->state == SUBSYS_ONLINE && !dev->restarting) {
			dev->restarting = true;
			dev->state = SUBSYS_CRASHED;
			wake_lock(&dev->wake_lock);
			queue_work(ssr_wq, &dev->work);
		} else {
			panic("Subsystem %s crashed during SSR!", name);
		}
	}
	spin_unlock_irqrestore(&dev->restart_lock, flags);
}

// (+) p15060
#ifdef CONFIG_PANTECH_ERR_CRASH_LOGGING

struct ssr_obj {
	struct kobject kobj;
    int ssr;
	int cmd;
};

struct ssr_attribute {
	struct attribute attr;
	ssize_t (*show)(struct ssr_obj *ssr, struct ssr_attribute *attr, char *buf);
	ssize_t (*store)(struct ssr_obj *ssr, struct ssr_attribute *attr, const char *buf, size_t count);
};

static struct kset *ssr_kset;
static struct ssr_obj *ssr_obj;
static int kset_ssr_uevent(struct kset *kset, struct kobject *kobj, struct kobj_uevent_env *env);
static ssize_t ssr_show(struct ssr_obj *ssr_obj, struct ssr_attribute *attr, char *buf);
static ssize_t ssr_store(struct ssr_obj *ssr_obj, struct ssr_attribute *attr,
                         const char *buf, size_t count);
static ssize_t ssr_attr_store(struct kobject *kobj,
                              struct attribute *attr,
                              const char *buf, size_t len);
static ssize_t ssr_attr_show(struct kobject *kobj,
                             struct attribute *attr,
                             char *buf);

static struct ssr_attribute ssr_attribute = __ATTR(ssr, 0664, ssr_show, ssr_store);

struct attribute ssr_attr;
static struct attribute *ssr_default_attrs[] = {
	&ssr_attribute.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static const struct sysfs_ops ssr_sysfs_ops = {
	.show = ssr_attr_show,
	.store = ssr_attr_store,
};

static struct kset_uevent_ops kset_ssr_uevent_ops = {
	.uevent = kset_ssr_uevent,
};

static struct kobj_type ssr_ktype = {
	.sysfs_ops = &ssr_sysfs_ops,
      //.release = ssr_release,
	.default_attrs = ssr_default_attrs,
};

#define to_ssr_obj(x) container_of(x, struct ssr_obj, kobj)
#define to_ssr_attr(x) container_of(x, struct ssr_attribute, attr)

static ssize_t ssr_attr_show(struct kobject *kobj,
                             struct attribute *attr,
                             char *buf)
{
	struct ssr_attribute *attribute;
	struct ssr_obj *ssr;

	attribute = to_ssr_attr(attr);
	ssr = to_ssr_obj(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(ssr, attribute, buf);
}

static ssize_t ssr_attr_store(struct kobject *kobj,
                              struct attribute *attr,
                              const char *buf, size_t len)
{
	struct ssr_attribute *attribute;
	struct ssr_obj *ssr;

    char cmd_buf[12];

    memset( cmd_buf, 0, sizeof(cmd_buf) );
    snprintf( cmd_buf, sizeof(cmd_buf)-1, "%s", buf );

	attribute = to_ssr_attr(attr);
	ssr = to_ssr_obj(kobj);

	if (!attribute->store)
		return -EIO;
    
	return attribute->store(ssr, attribute, cmd_buf, len);
}

static int kset_ssr_uevent(struct kset *kset, struct kobject *kobj, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "SSR_EVENT=%d", ssrEvent);

	return 0;
}

void ssr_uevent_set( void )
{
    char event[32];

    memset( event, 0, sizeof(event) );
	sprintf(event, "SSR_EVENT=%d\n", ssrEvent);
    if( NULL != ssr_obj )
    {
//        kobject_uevent(&ssr_obj->kobj, KOBJ_CHANGE);
        char *envp[] = { event, NULL };
        kobject_uevent_env(&ssr_obj->kobj, KOBJ_CHANGE, envp); 
    }
}

static struct ssr_obj *create_ssr_obj(const char *name)
{
	struct ssr_obj *ssr;
	int retval;

	/* allocate the memory for the whole object */
	ssr = kzalloc(sizeof(*ssr), GFP_KERNEL);
	if (!ssr)
		return NULL;

	/*
	 * As we have a kset for this kobject, we need to set it before calling
	 * the kobject core.
	 */
	ssr->kobj.kset = ssr_kset;

	/*
	 * Initialize and add the kobject to the kernel.  All the default files
	 * will be created here.  As we have already specified a kset for this
	 * kobject, we don't have to set a parent for the kobject, the kobject
	 * will be placed beneath that kset automatically.
	 */
	retval = kobject_init_and_add(&ssr->kobj, &ssr_ktype, NULL, "%s", name);
	if (retval) {
		kobject_put(&ssr->kobj);
		return NULL;
	}

	/*
	 * We are always responsible for sending the uevent that the kobject
	 * was added to the system.
	 */
	kobject_uevent(&ssr->kobj, KOBJ_ADD);

	return ssr;
}

void ssr_dump_work( void )
{
    ssr_uevent_set( );
}
EXPORT_SYMBOL( ssr_dump_work );

void ssr_dump_work_init( void )
{
    ssr_kset = kset_create_and_add("ssr_dump_work", &kset_ssr_uevent_ops, kernel_kobj);
    ssr_obj = create_ssr_obj( "ssr" );
}

static ssize_t ssr_show(struct ssr_obj *ssr_obj, struct ssr_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", ssr_obj->ssr);
}

static ssize_t ssr_store(struct ssr_obj *ssr_obj, struct ssr_attribute *attr,
                         const char *buf, size_t count)
{
    int cmd = 0;
    int i;
    char cmd_buf[12];

    memset( cmd_buf, 0, sizeof(cmd_buf) );
    snprintf( cmd_buf, sizeof(cmd_buf)-1, "%s", buf );

    //printk(KERN_INFO "%s : buf: %s\n", __func__, buf);

    for( i = 0; i < count-1; i++ )
    {

        cmd = cmd*10 + (cmd_buf[i]-'0');
    }

        ssrEvent = cmd;
        ssr_uevent_set();

	return count;
}
#endif
// (-) p15060


int subsystem_restart_dev(struct subsys_device *dev)
{
	const char *name;

#ifdef CONFIG_PANTECH_RESET_REASON
	if(sky_reset_reason != SYS_RESET_REASON_ABNORMAL){
		pr_err("Other failure detected first, skip subsystem_restart_dev, reset_reason : %d\n", sky_reset_reason);
		return -EINVAL;
	}
#endif

	if (!get_device(&dev->dev))
		return -ENODEV;

	if (!try_module_get(dev->owner)) {
		put_device(&dev->dev);
		return -ENODEV;
	}

	name = dev->desc->name;

	if (!name) {
		pr_err("Invalid subsystem name.\n");
		return -EINVAL;
	}


	/*
	 * If a system reboot/shutdown is underway, ignore subsystem errors.
	 * However, print a message so that we know that a subsystem behaved
	 * unexpectedly here.
	 */
	if (system_state == SYSTEM_RESTART
		|| system_state == SYSTEM_POWER_OFF) {
		pr_err("%s crashed during a system poweroff/shutdown.\n", name);
		return -EBUSY;
	}

	pr_info("Restart sequence requested for %s, restart_level = %d.\n", name, restart_level);

    switch (restart_level) {

	case RESET_SUBSYS_COUPLED:
	case RESET_SUBSYS_INDEPENDENT:

#ifndef CONFIG_PANTECH_ERR_CRASH_LOGGING
		__subsystem_restart_dev(dev);
#else
		if(!strncmp(name, "external_modem", 14)){
#ifdef CONFIG_PANTECH_RESET_REASON
			sky_sys_rst_set_reboot_info(SYS_RESET_REASON_MDM);
#endif
			panic("subsys-restart: Resetting the SoC - %s crashed.", name);
		}else{
#if defined(CONFIG_FEATURE_PANTECH_RESET_DSPS_CORE)		
			if(!dsps_reset_state()){		
			ssrEvent = 4;     // SSR reset
			ssr_uevent_set();
			}
#else
				ssrEvent = 4;     // SSR reset
				ssr_uevent_set();
#endif
            
            // (+) p16652 for ssr reset count, 20130124
            if(!strncmp(name, "dsps", 4))
            {
#if defined(CONFIG_FEATURE_PANTECH_RESET_DSPS_CORE)		
			if(!dsps_reset_state()){						
                ssrEvent = 64;
                ssr_uevent_set();
            }
#else				
                ssrEvent = 64;
                ssr_uevent_set();

#endif
            }
            else if(!strncmp(name, "wcnss", 5))
            {
                ssrEvent = 32;
                ssr_uevent_set();
            }

			__subsystem_restart_dev(dev);
		}
#endif
		break;

	case RESET_SOC:
#ifdef CONFIG_PANTECH_RESET_REASON
		if(!strncmp(name, "lpass", 5)) 
		{
			sky_sys_rst_set_reboot_info(SYS_RESET_REASON_LPASS);
		}
		else if(!strncmp(name, "dsps", 4)) 
		{
			sky_sys_rst_set_reboot_info(SYS_RESET_REASON_DSPS);
		}
		else if(!strncmp(name, "riva", 4)) 
		{
			sky_sys_rst_set_reboot_info(SYS_RESET_REASON_RIVA);
		}
		else if(!strncmp(name, "external_modem", 14)) 
		{
			sky_sys_rst_set_reboot_info(SYS_RESET_REASON_MDM);
		}
#endif
		panic("subsys-restart: Resetting the SoC - %s crashed.", name);
		break;
	default:
		panic("subsys-restart: Unknown restart level!\n");
		break;
	}
	module_put(dev->owner);
	put_device(&dev->dev);

	return 0;
}
EXPORT_SYMBOL(subsystem_restart_dev);

int subsystem_restart(const char *name)
{
	int ret;
	struct subsys_device *dev = find_subsys(name);

	if (!dev)
		return -ENODEV;

	ret = subsystem_restart_dev(dev);
	put_device(&dev->dev);
	return ret;
}
EXPORT_SYMBOL(subsystem_restart);

#ifdef CONFIG_DEBUG_FS
static ssize_t subsys_debugfs_read(struct file *filp, char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	int r;
	char buf[40];
	struct subsys_device *subsys = filp->private_data;

	r = snprintf(buf, sizeof(buf), "%d\n", subsys->count);
	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t subsys_debugfs_write(struct file *filp,
		const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct subsys_device *subsys = filp->private_data;
	char buf[10];
	char *cmp;

	cnt = min(cnt, sizeof(buf) - 1);
	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	cmp = strstrip(buf);

	if (!strcmp(cmp, "restart")) {
		if (subsystem_restart_dev(subsys))
			return -EIO;
	} else {
		return -EINVAL;
	}

	return cnt;
}

static const struct file_operations subsys_debugfs_fops = {
	.open	= simple_open,
	.read	= subsys_debugfs_read,
	.write	= subsys_debugfs_write,
};

static struct dentry *subsys_base_dir;

static int __init subsys_debugfs_init(void)
{
	subsys_base_dir = debugfs_create_dir("msm_subsys", NULL);
	return !subsys_base_dir ? -ENOMEM : 0;
}

static void subsys_debugfs_exit(void)
{
	debugfs_remove_recursive(subsys_base_dir);
}

static int subsys_debugfs_add(struct subsys_device *subsys)
{
	if (!subsys_base_dir)
		return -ENOMEM;

	subsys->dentry = debugfs_create_file(subsys->desc->name,
				S_IRUGO | S_IWUSR, subsys_base_dir,
				subsys, &subsys_debugfs_fops);
	return !subsys->dentry ? -ENOMEM : 0;
}

static void subsys_debugfs_remove(struct subsys_device *subsys)
{
	debugfs_remove(subsys->dentry);
}
#else
static int __init subsys_debugfs_init(void) { return 0; };
static void subsys_debugfs_exit(void) { }
static int subsys_debugfs_add(struct subsys_device *subsys) { return 0; }
static void subsys_debugfs_remove(struct subsys_device *subsys) { }
#endif

static void subsys_device_release(struct device *dev)
{
	struct subsys_device *subsys = to_subsys(dev);

	wake_lock_destroy(&subsys->wake_lock);
	mutex_destroy(&subsys->shutdown_lock);
	mutex_destroy(&subsys->powerup_lock);
	ida_simple_remove(&subsys_ida, subsys->id);
	kfree(subsys);
}

struct subsys_device *subsys_register(struct subsys_desc *desc)
{
	struct subsys_device *subsys;
	int ret;

	subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	if (!subsys)
		return ERR_PTR(-ENOMEM);

	subsys->desc = desc;
	subsys->owner = desc->owner;
	subsys->dev.parent = desc->dev;
	subsys->dev.bus = &subsys_bus_type;
	subsys->dev.release = subsys_device_release;
	subsys->state = SUBSYS_ONLINE; /* Until proper refcounting appears */

	subsys->notify = subsys_notif_add_subsys(desc->name);
	subsys->restart_order = update_restart_order(subsys);

	snprintf(subsys->wlname, sizeof(subsys->wlname), "ssr(%s)", desc->name);
	wake_lock_init(&subsys->wake_lock, WAKE_LOCK_SUSPEND, subsys->wlname);
	INIT_WORK(&subsys->work, subsystem_restart_wq_func);
	spin_lock_init(&subsys->restart_lock);

	subsys->id = ida_simple_get(&subsys_ida, 0, 0, GFP_KERNEL);
	if (subsys->id < 0) {
		ret = subsys->id;
		goto err_ida;
	}
	dev_set_name(&subsys->dev, "subsys%d", subsys->id);

	mutex_init(&subsys->shutdown_lock);
	mutex_init(&subsys->powerup_lock);

	ret = subsys_debugfs_add(subsys);
	if (ret)
		goto err_debugfs;

	ret = device_register(&subsys->dev);
	if (ret) {
		put_device(&subsys->dev);
		goto err_register;
	}

	return subsys;

err_register:
	subsys_debugfs_remove(subsys);
err_debugfs:
	mutex_destroy(&subsys->shutdown_lock);
	mutex_destroy(&subsys->powerup_lock);
	ida_simple_remove(&subsys_ida, subsys->id);
err_ida:
	wake_lock_destroy(&subsys->wake_lock);
	kfree(subsys);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(subsys_register);

void subsys_unregister(struct subsys_device *subsys)
{
	if (IS_ERR_OR_NULL(subsys))
		return;

	if (get_device(&subsys->dev)) {
		mutex_lock(&subsys->powerup_lock);
		WARN_ON(subsys->count);
		device_unregister(&subsys->dev);
		mutex_unlock(&subsys->powerup_lock);
		subsys_debugfs_remove(subsys);
		put_device(&subsys->dev);
	}
}
EXPORT_SYMBOL(subsys_unregister);

static int subsys_panic(struct device *dev, void *data)
{
	struct subsys_device *subsys = to_subsys(dev);

	if (subsys->desc->crash_shutdown)
		subsys->desc->crash_shutdown(subsys->desc);
	return 0;
}

static int ssr_panic_handler(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	bus_for_each_dev(&subsys_bus_type, NULL, NULL, subsys_panic);
	return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
	.notifier_call  = ssr_panic_handler,
};

static int __init ssr_init_soc_restart_orders(void)
{
	int i;

	atomic_notifier_chain_register(&panic_notifier_list,
			&panic_nb);

	if (cpu_is_msm8x60()) {
		for (i = 0; i < ARRAY_SIZE(orders_8x60_all); i++) {
			mutex_init(&orders_8x60_all[i]->powerup_lock);
			mutex_init(&orders_8x60_all[i]->shutdown_lock);
		}

		for (i = 0; i < ARRAY_SIZE(orders_8x60_modems); i++) {
			mutex_init(&orders_8x60_modems[i]->powerup_lock);
			mutex_init(&orders_8x60_modems[i]->shutdown_lock);
		}

		restart_orders = orders_8x60_all;
		n_restart_orders = ARRAY_SIZE(orders_8x60_all);
	}

	if (socinfo_get_platform_subtype() == PLATFORM_SUBTYPE_SGLTE) {
		restart_orders = restart_orders_8960_sglte;
		n_restart_orders = ARRAY_SIZE(restart_orders_8960_sglte);
	}

	for (i = 0; i < n_restart_orders; i++) {
		mutex_init(&restart_orders[i]->powerup_lock);
		mutex_init(&restart_orders[i]->shutdown_lock);
	}

	if (restart_orders == NULL || n_restart_orders < 1) {
		WARN_ON(1);
	}

	return 0;
}

static int __init subsys_restart_init(void)
{
	int ret;

	ssr_wq = alloc_workqueue("ssr_wq", WQ_CPU_INTENSIVE, 0);
	BUG_ON(!ssr_wq);

	ret = bus_register(&subsys_bus_type);
	if (ret)
		goto err_bus;
	ret = subsys_debugfs_init();
	if (ret)
		goto err_debugfs;
	ret = ssr_init_soc_restart_orders();
	if (ret)
		goto err_soc;

// (+) p15060
#ifdef CONFIG_PANTECH_ERR_CRASH_LOGGING
    ssr_dump_work_init();
#endif
// (-) p15060

    return ret;

err_soc:
	subsys_debugfs_exit();
err_debugfs:
	bus_unregister(&subsys_bus_type);
err_bus:
	destroy_workqueue(ssr_wq);
	return ret;
}
arch_initcall(subsys_restart_init);

MODULE_DESCRIPTION("Subsystem Restart Driver");
MODULE_LICENSE("GPL v2");
