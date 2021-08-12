#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/rtc.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/of_gpio.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/input/mt.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/version.h>

#include "touchpanel_common.h"
#include "touchpanel_proc.h"
#include "touch_comon_api/touch_comon_api.h"
#include "touchpanel_prevention/touchpanel_prevention.h"
#include "touchpanel_healthinfo/touchpanel_healthinfo.h"
#ifdef CONFIG_TOUCHPANEL_ALGORITHM
#include "touchpanel_algorithm/touchpanel_algorithm.h"
#endif
#ifndef REMOVE_OPLUS_FUNCTION
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
#include<mt-plat/mtk_boot_common.h>
#else
#include <soc/oplus/boot_mode.h>
#endif
#endif

#ifdef CONFIG_FB
#include <linux/fb.h>
#include <linux/notifier.h>
#endif

#ifdef CONFIG_DRM_MSM
#include <linux/msm_drm_notify.h>
#endif
#include <drm/drm_panel.h>


/*******Part0:LOG TAG Declear************************/
#if defined(CONFIG_TOUCHPANEL_MTK_PLATFORM) && defined(CONFIG_TOUCHIRQ_UPDATE_QOS)
#error CONFIG_TOUCHPANEL_MTK_PLATFORM and CONFIG_TOUCHIRQ_UPDATE_QOS
#error can not defined same time
#endif

#define TP_ALL_GESTURE_SUPPORT \
    (ts->black_gesture_support || ts->fingerprint_underscreen_support)
#define TP_ALL_GESTURE_ENABLE  \
    ((ts->gesture_enable & 0x01) == 1 || ts->fp_enable == 1)

/*******Part1:Global variables Area********************/
struct touchpanel_data *g_tp[TP_SUPPORT_MAX] = {NULL};
static DEFINE_MUTEX(tp_core_lock);
int cur_tp_index = 0 ;
struct drm_panel *lcd_active_panel;

/*******Part2:declear Area********************************/
static void speedup_resume(struct work_struct *work);
static void lcd_trigger_load_tp_fw(struct work_struct *work);
void esd_handle_switch(struct esd_information *esd_info, bool flag);

#if defined(CONFIG_FB) || defined(CONFIG_DRM_MSM)
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#endif
static int fb_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
static int tp_control_cs_gpio(bool enable, unsigned int tp_index);
void lcd_tp_load_fw(unsigned int tp_index);
int tp_control_reset_gpio(bool enable, unsigned int tp_index);
void tp_ftm_extra(unsigned int tp_index);
void sec_ts_pinctrl_configure(struct hw_resource *hw_res, bool enable);

static void tp_touch_release(struct touchpanel_data *ts);
static void tp_btnkey_release(struct touchpanel_data *ts);
static inline void tp_work_func(struct touchpanel_data *ts);

#if IS_BUILTIN(CONFIG_TOUCHPANEL_OPLUS)
extern char *saved_command_line;
#endif
__attribute__((weak)) int preconfig_power_control(struct touchpanel_data *ts)
{
    return 0;
}
__attribute__((weak)) int reconfig_power_control(struct touchpanel_data *ts)
{
    return 0;
}

/* n200 black gestrue */
static int tp_gesture_enable_flag(unsigned int tp_index);
extern int (*tp_gesture_enable_notifier)(unsigned int tp_index);
extern uint8_t DouTap_enable; // double tap
extern uint8_t UpVee_enable; // V
extern uint8_t LeftVee_enable; // >
extern uint8_t RightVee_enable; // <
extern uint8_t Circle_enable; // O
extern uint8_t DouSwip_enable;  // ||
extern uint8_t Mgestrue_enable; // M
extern uint8_t Wgestrue_enable; // W
extern uint8_t Sgestrue_enable; // S
extern uint8_t SingleTap_enable; // single tap
extern uint8_t Enable_gesture;

#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
__attribute__((weak)) enum boot_mode_t get_boot_mode(void)
{
    return 0;
}
#endif

__attribute__((weak)) int notify_prevention_handle(struct kernel_grip_info *grip_info, int obj_attention, struct point_info *points)
{
    return obj_attention;
}

#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
extern void primary_display_esd_check_enable(int enable);
#endif
void __attribute__((weak)) display_esd_check_enable_bytouchpanel(bool enable)
{
    return;
}

#if IS_BUILTIN(CONFIG_TOUCHPANEL_OPLUS)
#ifdef CONFIG_TOUCHPANEL_ALGORITHM
__attribute__((weak)) void switch_kalman_fun(struct touchpanel_data *ts, bool game_switch)
{
    return;
}

__attribute__((weak)) int touch_algorithm_handle(struct touchpanel_data *ts, int obj_attention, struct point_info *points)
{
    return obj_attention;
}
__attribute__((weak)) void oplus_touch_algorithm_init(struct touchpanel_data *ts)
{
    return;
}

__attribute__((weak)) void set_algorithm_direction(struct touchpanel_data *ts, int dir)
{
    return;
}
#endif
#endif



/*******Part3:Function  Area********************************/
bool inline is_ftm_boot_mode(struct touchpanel_data *ts)
{
#ifndef REMOVE_OPLUS_FUNCTION
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
    if((ts->boot_mode == META_BOOT || ts->boot_mode == FACTORY_BOOT))
#else
    if((ts->boot_mode == MSM_BOOT_MODE__FACTORY || ts->boot_mode == MSM_BOOT_MODE__RF || ts->boot_mode == MSM_BOOT_MODE__WLAN))
#endif
    {
        return true;
    }
#endif
    return false;
}

/**
 * operate_mode_switch - switch work mode based on current params
 * @ts: touchpanel_data struct using for common driver
 *
 * switch work mode based on current params(gesture_enable, limit_enable, glove_enable)
 * Do not care the result: Return void type
 */
void operate_mode_switch(struct touchpanel_data *ts)
{
	if (!ts->ts_ops->mode_switch) {
		TP_INFO(ts->tp_index, "not support ts_ops->mode_switch callback\n");
		return;
	}

	if (ts->is_suspended) {
		if (TP_ALL_GESTURE_SUPPORT) {
			if (TP_ALL_GESTURE_ENABLE) {
				ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, ts->gesture_enable
							|| ts->fp_enable);
				ts->ts_ops->mode_switch(ts->chip_data, MODE_NORMAL, true);

				if (ts->fingerprint_underscreen_support) {
					ts->ts_ops->enable_fingerprint(ts->chip_data, !!ts->fp_enable);
				}

				if (((ts->gesture_enable & 0x01) != 1) && ts->ts_ops->enable_gesture_mask) {
					ts->ts_ops->enable_gesture_mask(ts->chip_data, 0);
				}

			} else {
				ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, false);
				ts->ts_ops->mode_switch(ts->chip_data, MODE_SLEEP, true);
			}

		} else {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_SLEEP, true);
		}

	} else {
		if (ts->face_detect_support) {
			if (ts->fd_enable) {
				input_event(ts->ps_input_dev, EV_MSC, MSC_RAW, 0);
				input_sync(ts->ps_input_dev);
			}

			ts->ts_ops->mode_switch(ts->chip_data, MODE_FACE_DETECT, ts->fd_enable == 1);
		}

		if (ts->fingerprint_underscreen_support) {
			ts->ts_ops->enable_fingerprint(ts->chip_data, !!ts->fp_enable);
		}

		if (ts->black_gesture_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, false);
		}

		if (ts->fw_edge_limit_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_EDGE, ts->limit_enable);
		}

		if (ts->charger_pump_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_CHARGE, ts->is_usb_checked);
		}

		if (ts->wireless_charger_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_WIRELESS_CHARGE,
						ts->is_wireless_checked);
		}

		if (ts->headset_pump_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_HEADSET, ts->is_headset_checked);
		}

		if (ts->kernel_grip_support && ts->ts_ops->enable_kernel_grip) {
			ts->ts_ops->enable_kernel_grip(ts->chip_data, ts->grip_info);
		}

        if (ts->smooth_level_support && ts->ts_ops->smooth_lv_set) {
            ts->ts_ops->smooth_lv_set(ts->chip_data, ts->smooth_level);
        }

		ts->ts_ops->mode_switch(ts->chip_data, MODE_NORMAL, true);
	}
}
EXPORT_SYMBOL(operate_mode_switch);

/*
 * check_usb_state----expose to be called by charger int to get usb state
 * @usb_state : 1 if usb checked, otherwise is 0
*/
void switch_usb_state_work(struct work_struct *work)
{
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     charger_pump_work);

	mutex_lock(&ts->mutex);

	if (ts->charger_pump_support && (ts->is_usb_checked != ts->cur_usb_state)) {
		ts->is_usb_checked = !!ts->cur_usb_state;
		TP_INFO(ts->tp_index, "%s: check usb state : %d, is_suspended: %d\n", __func__,
			ts->cur_usb_state, ts->is_suspended);

		if (!ts->is_suspended && (ts->suspend_state == TP_SPEEDUP_RESUME_COMPLETE)
		    && !ts->loading_fw) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_CHARGE, ts->is_usb_checked);
		}
	}

	mutex_unlock(&ts->mutex);
}

/*
 * check_headset_state----expose to be called by audio int to get headset state
 * @headset_state : 1 if headset checked, otherwise is 0
 */
void switch_headset_work(struct work_struct *work)
{
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     headset_pump_work);

	mutex_lock(&ts->mutex);

	if (ts->headset_pump_support
	    && (ts->is_headset_checked != ts->cur_headset_state)) {
		ts->is_headset_checked = !!ts->cur_headset_state;
		TP_INFO(ts->tp_index, "%s: check headset state : %d, is_suspended: %d\n",
			__func__, ts->cur_headset_state, ts->is_suspended);

		if (!ts->is_suspended && (ts->suspend_state == TP_SPEEDUP_RESUME_COMPLETE)
		    && !ts->loading_fw) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_HEADSET, ts->is_headset_checked);
		}
	}

	mutex_unlock(&ts->mutex);
}

static inline void tp_touch_down(struct touchpanel_data *ts, struct point_info points, int touch_report_num, int id)
{
	if (ts->input_dev == NULL) {
		return;
	}

	input_report_key(ts->input_dev, BTN_TOUCH, 1);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, 1);

	if (touch_report_num == 1) {
		input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, points.width_major);
		ts->last_width_major = points.width_major;

	} else if (!(touch_report_num & 0x7f) || touch_report_num == 30) {
		/*if touch_report_num == 127, every 127 points, change width_major*/
		/*down and keep long time, auto repeat per 5 seconds, for weixing*/
		/*report move event after down event, for weixing voice delay problem, 30 -> 300ms in order to avoid the intercept by shortcut*/
		if (ts->last_width_major == points.width_major) {
			ts->last_width_major = points.width_major + 1;

		} else {
			ts->last_width_major = points.width_major;
		}

		input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, ts->last_width_major);
	}

	if ((points.x > ts->touch_major_limit.width_range)
	    && (points.x < ts->resolution_info.max_x - ts->touch_major_limit.width_range)
	    && \
	    (points.y > ts->touch_major_limit.height_range)
	    && (points.y < ts->resolution_info.max_y -
		ts->touch_major_limit.height_range)) {
		/*smart_gesture_support*/
		if (points.touch_major > SMART_GESTURE_THRESHOLD) {
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, points.touch_major);

		} else {
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, SMART_GESTURE_LOW_VALUE);
		}

		/*pressure_report_support*/
		input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
				 points.touch_major);   /*add for fixing gripview tap no function issue*/
	}

	if (!CHK_BIT(ts->irq_slot, (1 << id))) {
		TP_DETAIL(ts->tp_index, "first touch point id %d [%4d %4d %4d]\n", id, points.x,
			  points.y, points.z);
	}

	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, points.x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, points.y);

	TP_SPECIFIC_PRINT(ts->tp_index, ts->point_num,
			  "Touchpanel id %d :Down[%4d %4d %4d]\n", id, points.x, points.y, points.z);

#ifndef TYPE_B_PROTOCOL
	input_mt_sync(ts->input_dev);
#endif
}

static inline void tp_touch_up(struct touchpanel_data *ts)
{
	if (ts->input_dev == NULL) {
		return;
	}

	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
	input_mt_sync(ts->input_dev);
#endif
}

static void tp_exception_handle(struct touchpanel_data *ts)
{
	if (!ts->ts_ops->reset) {
		TP_INFO(ts->tp_index, "not support ts->ts_ops->reset callback\n");
		return;
	}

	ts->ts_ops->reset(
		ts->chip_data);    /* after reset, all registers set to default*/
	operate_mode_switch(ts);

	tp_btnkey_release(ts);
	tp_touch_release(ts);

	if (ts->fingerprint_underscreen_support) {
		ts->fp_info.touch_state = 0;
        opticalfp_irq_handler(&ts->fp_info);
    }
}

static void tp_fw_auto_reset_handle(struct touchpanel_data *ts)
{
	TP_INFO(ts->tp_index, "%s\n", __func__);

	operate_mode_switch(ts);

	tp_btnkey_release(ts);
	tp_touch_release(ts);
}

static void tp_geture_info_transform(struct gesture_info *gesture,
				     struct resolution_info *resolution_info)
{
	gesture->Point_start.x = gesture->Point_start.x *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_start.y = gesture->Point_start.y *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
	gesture->Point_end.x   = gesture->Point_end.x   *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_end.y   = gesture->Point_end.y   *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
	gesture->Point_1st.x   = gesture->Point_1st.x   *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_1st.y   = gesture->Point_1st.y   *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
	gesture->Point_2nd.x   = gesture->Point_2nd.x   *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_2nd.y   = gesture->Point_2nd.y   *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
	gesture->Point_3rd.x   = gesture->Point_3rd.x   *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_3rd.y   = gesture->Point_3rd.y   *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
	gesture->Point_4th.x   = gesture->Point_4th.x   *
				 resolution_info->LCD_WIDTH  / (resolution_info->max_x);
	gesture->Point_4th.y   = gesture->Point_4th.y   *
				 resolution_info->LCD_HEIGHT / (resolution_info->max_y);
}

static void tp_gesture_handle(struct touchpanel_data *ts)
{
	struct gesture_info gesture_info_temp;

	if (!ts->ts_ops->get_gesture_info) {
		TP_INFO(ts->tp_index, "not support ts->ts_ops->get_gesture_info callback\n");
		return;
	}
	memset(&gesture_info_temp, 0, sizeof(struct gesture_info));
	ts->ts_ops->get_gesture_info(ts->chip_data, &gesture_info_temp);
	tp_geture_info_transform(&gesture_info_temp, &ts->resolution_info);

	TP_INFO(ts->tp_index, "detect %s gesture\n",
		gesture_info_temp.gesture_type == DOU_TAP ? "double tap" :
		gesture_info_temp.gesture_type == UP_VEE ? "up vee" :
		gesture_info_temp.gesture_type == DOWN_VEE ? "down vee" :
		gesture_info_temp.gesture_type == LEFT_VEE ? "(>)" :
		gesture_info_temp.gesture_type == RIGHT_VEE ? "(<)" :
		gesture_info_temp.gesture_type == CIRCLE_GESTURE ? "circle" :
		gesture_info_temp.gesture_type == DOU_SWIP ? "(||)" :
		gesture_info_temp.gesture_type == LEFT2RIGHT_SWIP ? "(-->)" :
		gesture_info_temp.gesture_type == RIGHT2LEFT_SWIP ? "(<--)" :
		gesture_info_temp.gesture_type == UP2DOWN_SWIP ? "up to down |" :
		gesture_info_temp.gesture_type == DOWN2UP_SWIP ? "down to up |" :
		gesture_info_temp.gesture_type == M_GESTRUE ? "(M)" :
		gesture_info_temp.gesture_type == W_GESTURE ? "(W)" :
		gesture_info_temp.gesture_type == FINGER_PRINTDOWN ? "(fingerprintdown)" :
		gesture_info_temp.gesture_type == FRINGER_PRINTUP ? "(fingerprintup)" :
		gesture_info_temp.gesture_type == SINGLE_TAP ? "single tap" :
		gesture_info_temp.gesture_type == S_GESTURE ? "(S)" :"unknown");
	pr_err("[TP]: %s enable flag DouTap=%d UpVee=%d LeftVee=%d RightVee=%d  Circle=%d DouSwip=%d Mgestrue=%d Sgestrue=%d SingleTap=%d  Wgestrue=%d\n",
		__func__, DouTap_enable, UpVee_enable, LeftVee_enable, RightVee_enable,
		Circle_enable, DouSwip_enable, Mgestrue_enable, Sgestrue_enable,
		SingleTap_enable, Wgestrue_enable);

	if (ts->health_monitor_support) {
		tp_healthinfo_report(&ts->monitor_data, HEALTH_GESTURE,
				     &gesture_info_temp.gesture_type);
	}

#ifdef CONFIG_OPLUS_TP_APK

	if (ts->gesture_debug_sta) {
		input_report_key(ts->input_dev, KEY_POWER, 1);
		input_sync(ts->input_dev);
		input_report_key(ts->input_dev, KEY_POWER, 0);
		input_sync(ts->input_dev);
		return;
	}

#endif // end of CONFIG_OPLUS_TP_APK

	if ((gesture_info_temp.gesture_type == DOU_TAP && DouTap_enable) ||
		(gesture_info_temp.gesture_type == UP_VEE && UpVee_enable) ||
		(gesture_info_temp.gesture_type == LEFT_VEE&& LeftVee_enable) ||
		(gesture_info_temp.gesture_type == RIGHT_VEE && RightVee_enable) ||
		(gesture_info_temp.gesture_type == CIRCLE_GESTURE && Circle_enable) ||
		(gesture_info_temp.gesture_type == DOU_SWIP && DouSwip_enable) ||
		(gesture_info_temp.gesture_type == M_GESTRUE && Mgestrue_enable) ||
		(gesture_info_temp.gesture_type == S_GESTURE && Sgestrue_enable) ||
		(gesture_info_temp.gesture_type == SINGLE_TAP && SingleTap_enable) ||
		(gesture_info_temp.gesture_type == W_GESTURE && Wgestrue_enable)) {
		if (ts->gesture_switch == 1 && ts->is_suspended == 1) {
			pr_err("[TP]:%s suspending and ps is near so return\n", __func__);
		} else {
			pr_err("[TP]: %s gesture_type = %d report to input system\n",
				__func__, gesture_info_temp.gesture_type);
			tp_memcpy(&ts->gesture, sizeof(ts->gesture), \
				&gesture_info_temp, sizeof(struct gesture_info), \
				sizeof(struct gesture_info));
			input_report_key(ts->input_dev, KEY_F4, 1);
			input_sync(ts->input_dev);
			input_report_key(ts->input_dev, KEY_F4, 0);
			input_sync(ts->input_dev);
		}

	} else if (gesture_info_temp.gesture_type == FINGER_PRINTDOWN) {
		ts->fp_info.touch_state = 1;
		ts->fp_info.x = gesture_info_temp.Point_start.x;
		ts->fp_info.y = gesture_info_temp.Point_start.y;
		TP_INFO(ts->tp_index, "screen off down : (%d, %d)\n", ts->fp_info.x, ts->fp_info.y);
		opticalfp_irq_handler(&ts->fp_info);
		notify_display_fpd(true);
	} else if (gesture_info_temp.gesture_type == FRINGER_PRINTUP) {
		ts->fp_info.touch_state = 0;
		ts->fp_info.x = gesture_info_temp.Point_start.x;
		ts->fp_info.y = gesture_info_temp.Point_start.y;
		TP_INFO(ts->tp_index, "screen off up : (%d, %d)\n", ts->fp_info.x, ts->fp_info.y);
		opticalfp_irq_handler(&ts->fp_info);
		notify_display_fpd(false);
	} else {
		pr_err("[TP]: %s nothing to report input system\n", __func__);
	}
}

void tp_touch_btnkey_release(unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;

	if (tp_index >= TP_SUPPORT_MAX) {
		return;
	}

	ts = g_tp[tp_index];

	if (!ts) {
		TP_INFO(ts->tp_index, "ts is NULL\n");
		return;
	}

	tp_touch_release(ts);
	tp_btnkey_release(ts);
}
EXPORT_SYMBOL(tp_touch_btnkey_release);

static void tp_touch_release(struct touchpanel_data *ts)
{
#ifdef TYPE_B_PROTOCOL

	int i = 0;

	mutex_lock(&ts->report_mutex);

	for (i = 0; i < ts->max_num; i++) {
		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
	}

	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
	input_sync(ts->input_dev);
	mutex_unlock(&ts->report_mutex);
#else
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
	input_mt_sync(ts->input_dev);
	input_sync(ts->input_dev);
#endif
	TP_DEBUG(ts->tp_index,
		"release all touch point and key, clear tp touch down flag\n");
	ts->view_area_touched = 0; /*realse all touch point,must clear this flag*/
	ts->touch_count = 0;
	ts->irq_slot = 0;
}

#ifdef CONFIG_TOUCHPANEL_ALGORITHM
static void special_touch_handle(struct touchpanel_data *ts)
{
	int obj_attention = 0;
	int i = 0;
	static int touch_report_num = 0;
	struct point_info points[10];

	if (!ts->ts_ops->special_points_report) {
		return;
	}

	obj_attention = ts->ts_ops->special_points_report(ts->chip_data, points,
			ts->max_num);

	if (obj_attention <= 0) {
		return;
	}

	for (i = 0; i < ts->max_num; i++) {
		if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01
		    && (points[i].status == 0)) { /* buf[0] == 0 is wrong point, no process*/
			continue;
		}

		if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01
		    && (points[i].status != 0)) {
			TPD_DEBUG("special point report\n");
#ifdef TYPE_B_PROTOCOL
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);
#endif
			touch_report_num++;
			tp_touch_down(ts, points[i], touch_report_num, i);
			SET_BIT(ts->irq_slot, (1 << i));
		}
	}

	input_sync(ts->input_dev);
}
#endif

static inline void tp_touch_handle(struct touchpanel_data *ts)
{
	int i = 0;
	uint8_t finger_num = 0, touch_near_edge = 0, finger_num_center = 0;
	int obj_attention = 0;
	struct point_info points[MAX_FINGER_NUM];

    if (!ts->ts_ops->get_touch_points) {
        TP_INFO(ts->tp_index, "not support ts->ts_ops->get_touch_points callback\n");
        return;
    }

    memset(points, 0, sizeof(points));

    obj_attention = ts->ts_ops->get_touch_points(ts->chip_data, points, ts->max_num);
    if (obj_attention == -EINVAL) {
        TP_INFO(ts->tp_index, "Invalid points, ignore..\n");
        return;
    }
	mutex_lock(&ts->report_mutex);
	if (ts->kernel_grip_support) {
		obj_attention = notify_prevention_handle(ts->grip_info, obj_attention, points);
	}

#ifdef CONFIG_TOUCHPANEL_ALGORITHM
	obj_attention = touch_algorithm_handle(ts, obj_attention, points);
	special_touch_handle(ts);

#endif

	if ((obj_attention & TOUCH_BIT_CHECK) != 0) {
		ts->up_status = false;

		for (i = 0; i < ts->max_num; i++) {
			if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01
			    && (points[i].status == 0)) { /* buf[0] == 0 is wrong point, no process*/
				continue;
			}

			if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01
			    && (points[i].status != 0)) {
#ifdef TYPE_B_PROTOCOL
				input_mt_slot(ts->input_dev, i);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);
#endif
				ts->touch_report_num++;
				tp_touch_down(ts, points[i], ts->touch_report_num, i);
				SET_BIT(ts->irq_slot, (1 << i));
				finger_num++;

				if (ts->face_detect_support && ts->fd_enable && \
				    (points[i].y < ts->resolution_info.max_y / 2) && (points[i].x > 30)
				    && (points[i].x < ts->resolution_info.max_x - 30)) {
					finger_num_center++;
				}

				if (points[i].x > ts->resolution_info.max_x / 100
				    && points[i].x < ts->resolution_info.max_x * 99 / 100) {
					ts->view_area_touched = finger_num;

				} else {
					touch_near_edge++;
				}

				/*strore  the last point data*/
				tp_memcpy(&ts->last_point, sizeof(ts->last_point), \
					  &points[i], sizeof(struct point_info), \
					  sizeof(struct point_info));
			}

#ifdef TYPE_B_PROTOCOL

			else {
				input_mt_slot(ts->input_dev, i);

				if (ts->kernel_grip_support && ts->grip_info
				    && ts->grip_info->eli_reject_status[i]
				    && !(ts->grip_info->grip_disable_level & (1 << GRIP_DISABLE_UP2CANCEL))) {
					input_report_abs(ts->input_dev, ABS_MT_PRESSURE, UP2CANCEL_PRESSURE_VALUE);
				}

				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
			}

#endif
		}

		if (touch_near_edge ==
		    finger_num) {        /*means all the touchpoint is near the edge*/
			ts->view_area_touched = 0;
		}

	} else {
		if (ts->up_status) {
			tp_touch_up(ts);
			mutex_unlock(&ts->report_mutex);
			return;
		}

		finger_num = 0;
		finger_num_center = 0;
		ts->touch_report_num = 0;
#ifdef TYPE_B_PROTOCOL

		for (i = 0; i < ts->max_num; i++) {
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
		}

#endif
		tp_touch_up(ts);
		ts->view_area_touched = 0;
		ts->irq_slot = 0;
		ts->up_status = true;
		TP_DETAIL(ts->tp_index, "all touch up,view_area_touched=%d finger_num=%d\n",
			  ts->view_area_touched, finger_num);
		TP_DETAIL(ts->tp_index, "last point x:%d y:%d\n", ts->last_point.x,
			  ts->last_point.y);
	}

	input_sync(ts->input_dev);
	ts->touch_count = (finger_num << 4) | (finger_num_center & 0x0F);
	mutex_unlock(&ts->report_mutex);

	if (ts->health_monitor_support) {
		ts->monitor_data.touch_points = points;
		ts->monitor_data.touch_num = finger_num;
		ts->monitor_data.direction = ts->grip_info ? ts->grip_info->touch_dir :
					     ts->limit_enable;
		tp_healthinfo_report(&ts->monitor_data, HEALTH_TOUCH, &obj_attention);
	}
}

/**
 * input_report_key_oplus - Using for report virtual key
 * @work: work struct using for this thread
 *
 * before report virtual key, detect whether touch_area has been touched
 * Do not care the result: Return void type
 */
void input_report_key_oplus(struct touchpanel_data *ts, unsigned int code,
			    int value)
{
	if (!ts) {
		return;
	}

	if (value) { /*report Key[down]*/
		if (ts->view_area_touched == 0) {
			input_report_key(ts->kpd_input_dev, code, value);

		} else {
			TP_INFO(ts->tp_index, "Sorry,tp is touch down,can not report touch key\n");
		}

	} else {
		input_report_key(ts->kpd_input_dev, code, value);
	}
}

static void tp_btnkey_release(struct touchpanel_data *ts)
{
	if (CHK_BIT(ts->vk_bitmap, BIT_MENU)) {
		input_report_key_oplus(ts, KEY_MENU, 0);
	}

	if (CHK_BIT(ts->vk_bitmap, BIT_HOME)) {
		input_report_key_oplus(ts, KEY_HOMEPAGE, 0);
	}

	if (CHK_BIT(ts->vk_bitmap, BIT_BACK)) {
		input_report_key_oplus(ts, KEY_BACK, 0);
	}

	input_sync(ts->kpd_input_dev);
}

static void tp_btnkey_handle(struct touchpanel_data *ts)
{
	u8 touch_state = 0;

	if (ts->vk_type != TYPE_AREA_SEPRATE) {
		TPD_DEBUG("TP vk_type not proper, checktouchpanel, button-type\n");

		return;
	}

	if (!ts->ts_ops->get_keycode) {
		TP_INFO(ts->tp_index, "not support ts->ts_ops->get_keycode callback\n");

		return;
	}

	touch_state = ts->ts_ops->get_keycode(ts->chip_data);

	if (CHK_BIT(ts->vk_bitmap, BIT_MENU)) {
		input_report_key_oplus(ts, KEY_MENU, CHK_BIT(touch_state, BIT_MENU));
	}

	if (CHK_BIT(ts->vk_bitmap, BIT_HOME)) {
		input_report_key_oplus(ts, KEY_HOMEPAGE, CHK_BIT(touch_state, BIT_HOME));
	}

	if (CHK_BIT(ts->vk_bitmap, BIT_BACK)) {
		input_report_key_oplus(ts, KEY_BACK, CHK_BIT(touch_state, BIT_BACK));
	}

	input_sync(ts->kpd_input_dev);
}

static void tp_config_handle(struct touchpanel_data *ts)
{
	int ret = 0;

	if (!ts->ts_ops->fw_handle) {
		TP_INFO(ts->tp_index, "not support ts->ts_ops->fw_handle callback\n");
		return;
	}

	ret = ts->ts_ops->fw_handle(ts->chip_data);
}

static void health_monitor_handle(struct touchpanel_data *ts)
{
	if (!ts->ts_ops->health_report) {
		TP_INFO(ts->tp_index,
			"not support ts->debug_info_ops->health_report callback\n");
		return;
	}

	if (tp_debug || ts->health_monitor_support) {
		ts->ts_ops->health_report(ts->chip_data, &ts->monitor_data);
	}
}

static void tp_face_detect_handle(struct touchpanel_data *ts)
{
	int ps_state = 0;

	if (!ts->ts_ops->get_face_state) {
		TP_INFO(ts->tp_index, "not support ts->ts_ops->get_face_state callback\n");
		return;
	}

	ps_state = ts->ts_ops->get_face_state(ts->chip_data);

	if (ps_state < 0) {
		return;
	}

	input_event(ts->ps_input_dev, EV_MSC, MSC_RAW, ps_state);
	input_sync(ts->ps_input_dev);

	if (ts->health_monitor_support) {
		tp_healthinfo_report(&ts->monitor_data, HEALTH_FACE_DETECT, &ps_state);
	}
}

static void tp_fingerprint_handle(struct touchpanel_data *ts)
{
	struct fp_underscreen_info fp_tpinfo;

    if (!ts->ts_ops->screenon_fingerprint_info) {
        TP_INFO(ts->tp_index, "not support screenon_fingerprint_info callback.\n");
        return;
    }

    ts->ts_ops->screenon_fingerprint_info(ts->chip_data, &fp_tpinfo);
    ts->fp_info.area_rate = fp_tpinfo.area_rate;
    ts->fp_info.x = fp_tpinfo.x;
    ts->fp_info.y = fp_tpinfo.y;
    if(fp_tpinfo.touch_state == FINGERPRINT_DOWN_DETECT) {
        TP_INFO(ts->tp_index, "screen on down : (%d, %d)\n", ts->fp_info.x, ts->fp_info.y);
        ts->fp_info.touch_state = 1;
        opticalfp_irq_handler(&ts->fp_info);
        if (ts->health_monitor_support) {
            tp_healthinfo_report(&ts->monitor_data, HEALTH_FINGERPRINT, &fp_tpinfo.area_rate);
        }
    } else if(fp_tpinfo.touch_state == FINGERPRINT_UP_DETECT) {
        TP_INFO(ts->tp_index, "screen on up : (%d, %d)\n", ts->fp_info.x, ts->fp_info.y);
        ts->fp_info.touch_state = 0;
        opticalfp_irq_handler(&ts->fp_info);
    } else if (ts->fp_info.touch_state) {
        opticalfp_irq_handler(&ts->fp_info);
    }
}

static inline void tp_work_func(struct touchpanel_data *ts)
{
	u32 cur_event = 0;

	if (!ts->ts_ops->trigger_reason) {
		TP_INFO(ts->tp_index, "not support ts_ops->trigger_reason callback\n");
		return;
	}

	/*
	 *  trigger_reason:this callback determine which trigger reason should be
	 *  The value returned has some policy!
	 *  1.IRQ_EXCEPTION /IRQ_GESTURE /IRQ_IGNORE /IRQ_FW_CONFIG --->should be only reported  individually
	 *  2.IRQ_TOUCH && IRQ_BTN_KEY --->should depends on real situation && set correspond bit on trigger_reason
	 */
	if (ts->ts_ops->trigger_reason) {
		cur_event = ts->ts_ops->trigger_reason(ts->chip_data, (ts->gesture_enable
						       || ts->fp_enable), ts->is_suspended);
	}

	if (CHK_BIT(cur_event, IRQ_TOUCH) || CHK_BIT(cur_event, IRQ_BTN_KEY)
	    || CHK_BIT(cur_event, IRQ_FW_HEALTH) || \
	    CHK_BIT(cur_event, IRQ_FACE_STATE) || CHK_BIT(cur_event, IRQ_FINGERPRINT)) {
		if (CHK_BIT(cur_event, IRQ_BTN_KEY)) {
			tp_btnkey_handle(ts);
		}

		if (CHK_BIT(cur_event, IRQ_TOUCH)) {
			tp_touch_handle(ts);
		}

		if (CHK_BIT(cur_event, IRQ_FW_HEALTH) && (!ts->is_suspended)) {
			health_monitor_handle(ts);
		}

		if (CHK_BIT(cur_event, IRQ_FACE_STATE) && ts->fd_enable) {
			tp_face_detect_handle(ts);
		}

		if (CHK_BIT(cur_event, IRQ_FINGERPRINT) && ts->fp_enable) {
			tp_fingerprint_handle(ts);
		}

	} else if (CHK_BIT(cur_event, IRQ_GESTURE)) {
		tp_gesture_handle(ts);

	} else if (CHK_BIT(cur_event, IRQ_EXCEPTION)) {
		tp_exception_handle(ts);

	} else if (CHK_BIT(cur_event, IRQ_FW_CONFIG)) {
		tp_config_handle(ts);

	} else if (CHK_BIT(cur_event, IRQ_FW_AUTO_RESET)) {
		tp_fw_auto_reset_handle(ts);

	} else {
		TPD_DEBUG("unknown irq trigger reason\n");
	}
}

static void tp_fw_update_work(struct work_struct *work)
{
	const struct firmware *fw = NULL;
	int ret, fw_update_result = 0;
	int count_tmp = 0, retry = 5;
	char *p_node = NULL;
	char *fw_name_fae = NULL;
	char *postfix = "_FAE";
	uint8_t copy_len = 0;
	u64 start_time = 0;

	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     fw_update_work);

	if (!ts->ts_ops->fw_check || !ts->ts_ops->reset) {
		TP_INFO(ts->tp_index, "not support ts_ops->fw_check callback\n");
		complete(&ts->fw_complete);
		return;
	}

	TP_INFO(ts->tp_index, "%s: fw_name = %s\n", __func__, ts->panel_data.fw_name);

	if (ts->health_monitor_support) {
		reset_healthinfo_time_counter(&start_time);
	}

	mutex_lock(&ts->mutex);

	if (!ts->irq_trigger_hdl_support && ts->int_mode == BANNABLE) {
		disable_irq_nosync(ts->irq);
	}

	ts->loading_fw = true;

	if (ts->esd_handle_support) {
		esd_handle_switch(&ts->esd_info, false);
	}

	display_esd_check_enable_bytouchpanel(0);
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
	primary_display_esd_check_enable(0); /*avoid rst pulled to low while updating*/
#endif

	if (ts->ts_ops->fw_update) {
		do {
			if (ts->firmware_update_type == 0 || ts->firmware_update_type == 1) {
				if (ts->fw_update_app_support) {
					fw_name_fae = tp_devm_kzalloc(ts->dev, MAX_FW_NAME_LENGTH, GFP_KERNEL);

					if (fw_name_fae == NULL) {
						TP_INFO(ts->tp_index, "fw_name_fae kzalloc error!\n");
						goto EXIT;
					}

					p_node  = strstr(ts->panel_data.fw_name, ".");

					if (p_node == NULL) {
						TP_INFO(ts->tp_index, "p_node strstr error!\n");
						goto EXIT;
					}

					copy_len = p_node - ts->panel_data.fw_name;
					memcpy(fw_name_fae, ts->panel_data.fw_name, copy_len);
					strlcat(fw_name_fae, postfix, MAX_FW_NAME_LENGTH);
					strlcat(fw_name_fae, p_node, MAX_FW_NAME_LENGTH);
					TP_INFO(ts->tp_index, "fw_name_fae is %s\n", fw_name_fae);
					ret = request_firmware(&fw, fw_name_fae, ts->dev);

					if (!ret) {
						break;
					}

				} else {
					ret = request_firmware(&fw, ts->panel_data.fw_name, ts->dev);

					if (!ret) {
						break;
					}
				}

			} else {
				ret = request_firmware_select(&fw, ts->panel_data.fw_name, ts->dev);

				if (!ret) {
					break;
				}
			}
		} while ((ret < 0) && (--retry > 0));

		TP_INFO(ts->tp_index, "retry times %d\n", 5 - retry);

		if (!ret || ts->is_noflash_ic) {
			do {
				count_tmp++;
				ret = ts->ts_ops->fw_update(ts->chip_data, fw, ts->force_update);
				fw_update_result = ret;

				if (ret == FW_NO_NEED_UPDATE) {
					break;
				}

				if (!ts->is_noflash_ic) {       /*noflash update fw in reset and do bootloader reset in get_chip_info*/
					ret |= ts->ts_ops->reset(ts->chip_data);
					ret |= ts->ts_ops->get_chip_info(ts->chip_data);
				}

				ret |= ts->ts_ops->fw_check(ts->chip_data, &ts->resolution_info,
							    &ts->panel_data);
			} while ((count_tmp < 2) && (ret != 0));

			if (fw != NULL) {
				release_firmware(fw);
			}

		} else {
			TP_INFO(ts->tp_index, "%s: fw_name request failed %s %d\n", __func__,
				ts->panel_data.fw_name, ret);

			if (ts->health_monitor_support) {
				tp_healthinfo_report(&ts->monitor_data, HEALTH_FW_UPDATE, "FW Request Failed");
			}

			goto EXIT;
		}
	}

	tp_touch_release(ts);
	tp_btnkey_release(ts);
	operate_mode_switch(ts);

EXIT:
	ts->loading_fw = false;

	display_esd_check_enable_bytouchpanel(1);
#ifdef CONFIG_TOUCHPANEL_MTK_PLATFORM
	primary_display_esd_check_enable(1); /*avoid rst pulled to low while updating*/
#endif

	if (ts->esd_handle_support) {
		esd_handle_switch(&ts->esd_info, true);
	}

	tp_devm_kfree(ts->dev, (void **)&fw_name_fae, MAX_FW_NAME_LENGTH);

	if (ts->int_mode == BANNABLE) {
		enable_irq(ts->irq);
	}

	mutex_unlock(&ts->mutex);

	if (ts->health_monitor_support) {
		tp_healthinfo_report(&ts->monitor_data, HEALTH_FW_UPDATE_COST, &start_time);
	}


	ts->force_update = 0;

	complete(&ts->fw_complete); /*notify to init.rc that fw update finished*/
	return;
}

static irqreturn_t tp_irq_thread_fn(int irq, void *dev_id)
{
	struct touchpanel_data *ts = (struct touchpanel_data *)dev_id;

	/*for qaulcomn to stop cpu go to C4 idle state*/
#ifdef CONFIG_TOUCHIRQ_UPDATE_QOS

	if (ts->pm_qos_state && !ts->is_suspended && !ts->touch_count) {
		ts->pm_qos_value = PM_QOS_TOUCH_WAKEUP_VALUE;
		pm_qos_update_request(&ts->pm_qos_req, ts->pm_qos_value);
	}

#endif

	if (ts->ts_ops->tp_irq_throw_away) {
		if (ts->ts_ops->tp_irq_throw_away(ts->chip_data)) {
			goto exit;
		}
	}

	/*for stop system go to sleep*/
	/*wake_lock_timeout(&ts->wakelock, 1*HZ);*/

	/*for check bus i2c/spi is ready or not*/
	if (ts->bus_ready == false) {
		/*TP_INFO(ts->tp_index, "Wait device resume!");*/
		wait_event_interruptible_timeout(ts->wait, ts->bus_ready, msecs_to_jiffies(50));
		/*TP_INFO(ts->tp_index, "Device maybe resume!");*/
	}

	if (ts->bus_ready == false) {
		TP_INFO(ts->tp_index, "The device not resume 50ms!");
		goto exit;
	}

	/*for some ic such as samsung ic*/
	if (ts->sec_long_low_trigger) {
		disable_irq_nosync(ts->irq);
	}

	/*for normal ic*/
	if (ts->int_mode == BANNABLE) {
		mutex_lock(&ts->mutex);
		tp_work_func(ts);
		mutex_unlock(&ts->mutex);

	} else { /*for some ic such as synaptic tcm need get data by interrupt*/
		tp_work_func(ts);
	}

	if (ts->sec_long_low_trigger) {
		enable_irq(ts->irq);
	}

exit:

#ifdef CONFIG_TOUCHIRQ_UPDATE_QOS

	if (PM_QOS_TOUCH_WAKEUP_VALUE == ts->pm_qos_value) {
		ts->pm_qos_value = PM_QOS_DEFAULT_VALUE;
		pm_qos_update_request(&ts->pm_qos_req, ts->pm_qos_value);
	}

#endif

	return IRQ_HANDLED;
}

static ssize_t cap_vk_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct button_map *button_map = NULL;
	struct touchpanel_data *ts = NULL;
	int i = 0;
	int retval = 0;
	int count = 0;

	for (i = 0; i < TP_SUPPORT_MAX;  i++) {
		ts = g_tp[i];

		if (!ts) {
			continue;
		}

		if (ts->tp_index == i) {
			button_map = &ts->button_map;
			retval =  snprintf(buf, PAGESIZE - count,
					   __stringify(EV_KEY) ":" __stringify(KEY_MENU)   ":%d:%d:%d:%d"
					   ":" __stringify(EV_KEY) ":" __stringify(KEY_HOMEPAGE)   ":%d:%d:%d:%d"
					   ":" __stringify(EV_KEY) ":" __stringify(KEY_BACK)   ":%d:%d:%d:%d"
					   "\n", button_map->coord_menu.x, button_map->coord_menu.y, button_map->width_x,
					   button_map->height_y, \
					   button_map->coord_home.x, button_map->coord_home.y, button_map->width_x,
					   button_map->height_y, \
					   button_map->coord_back.x, button_map->coord_back.y, button_map->width_x,
					   button_map->height_y);

			if (retval < 0) {
				return retval;
			}

			buf += retval;
			count += retval;
		}
	}

	return count;
}

static struct kobj_attribute virtual_keys_attr = {
	.attr = {
		.name = "virtualkeys."TPD_DEVICE,
		.mode = S_IRUGO,
	},
	.show = &cap_vk_show,
};

static struct attribute *properties_attrs[] = {
	&virtual_keys_attr.attr,
	NULL
};

static struct attribute_group properties_attr_group = {
	.attrs = properties_attrs,
};

/**
 * init_input_device - Using for register input device
 * @ts: touchpanel_data struct using for common driver
 *
 * we should using this function setting input report capbility && register input device
 * Returning zero(success) or negative errno(failed)
 */
static int init_input_device(struct touchpanel_data *ts)
{
	int ret = 0;
	struct kobject *vk_properties_kobj;
	static  bool board_properties = false;

	TP_INFO(ts->tp_index, "%s is called\n", __func__);
	ts->input_dev = devm_input_allocate_device(ts->dev);

	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		TP_INFO(ts->tp_index, "Failed to allocate input device\n");
		return ret;
	}

	ts->kpd_input_dev  = devm_input_allocate_device(ts->dev);

	if (ts->kpd_input_dev == NULL) {
		ret = -ENOMEM;
		TP_INFO(ts->tp_index, "Failed to allocate key input device\n");
		return ret;
	}

	if (ts->face_detect_support) {
		ts->ps_input_dev  = devm_input_allocate_device(ts->dev);

		if (ts->ps_input_dev == NULL) {
			ret = -ENOMEM;
			TP_INFO(ts->tp_index, "Failed to allocate ps input device\n");
			return ret;
		}
        snprintf(ts->input_ps_name, sizeof(ts->input_ps_name),
                 "%s_ps%x", TPD_DEVICE, ts->tp_index);
        ts->ps_input_dev->name = ts->input_ps_name;
        set_bit(EV_MSC, ts->ps_input_dev->evbit);
        set_bit(MSC_RAW, ts->ps_input_dev->mscbit);
    }
    if (!ts->tp_index) {
        snprintf(ts->input_name, sizeof(ts->input_name),
                 "%s", TPD_DEVICE);
    } else {
        snprintf(ts->input_name, sizeof(ts->input_name),
                 "%s%x", TPD_DEVICE, ts->tp_index);
    }
    ts->input_dev->name = ts->input_name;
	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(ABS_MT_TOUCH_MAJOR, ts->input_dev->absbit);
	set_bit(ABS_MT_WIDTH_MAJOR, ts->input_dev->absbit);
	set_bit(ABS_MT_POSITION_X, ts->input_dev->absbit);
	set_bit(ABS_MT_POSITION_Y, ts->input_dev->absbit);
	set_bit(ABS_MT_PRESSURE, ts->input_dev->absbit);
	set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
	set_bit(BTN_TOUCH, ts->input_dev->keybit);

	if (ts->black_gesture_support) {
		set_bit(KEY_F4, ts->input_dev->keybit);
#ifdef CONFIG_OPLUS_TP_APK
		set_bit(KEY_POWER, ts->input_dev->keybit);
#endif /*end of CONFIG_OPLUS_TP_APK*/
	}

    snprintf(ts->input_kpd_name, sizeof(ts->input_kpd_name),
             "%s_kpd%x", TPD_DEVICE, ts->tp_index);
    ts->kpd_input_dev->name = ts->input_kpd_name;
	set_bit(EV_KEY, ts->kpd_input_dev->evbit);
	set_bit(EV_SYN, ts->kpd_input_dev->evbit);

	switch (ts->vk_type) {
	case TYPE_PROPERTIES : {
		if (!board_properties) {
			/*If more ic support more key, but have only one path*/
			TPD_INFO("Type 1: using board_properties\n");
			vk_properties_kobj = kobject_create_and_add("board_properties", NULL);

			if (vk_properties_kobj) {
				ret = sysfs_create_group(vk_properties_kobj, &properties_attr_group);
			}

			if (!vk_properties_kobj || ret) {
				TPD_INFO("failed to create board_properties\n");
			}

			board_properties = true;
		}

		break;
	}

	case TYPE_AREA_SEPRATE: {
		TPD_DEBUG("Type 2:using same IC (button zone &&  touch zone are seprate)\n");

		if (CHK_BIT(ts->vk_bitmap, BIT_MENU)) {
			set_bit(KEY_MENU, ts->kpd_input_dev->keybit);
		}

		if (CHK_BIT(ts->vk_bitmap, BIT_HOME)) {
			set_bit(KEY_HOMEPAGE, ts->kpd_input_dev->keybit);
		}

		if (CHK_BIT(ts->vk_bitmap, BIT_BACK)) {
			set_bit(KEY_BACK, ts->kpd_input_dev->keybit);
		}

		break;
	}

	default :
		break;
	}

#ifdef TYPE_B_PROTOCOL
	input_mt_init_slots(ts->input_dev, ts->max_num, 0);
#endif
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0,
			     ts->resolution_info.max_x - 1, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0,
			     ts->resolution_info.max_y - 1, 0, 0);
	input_set_drvdata(ts->input_dev, ts);
	input_set_drvdata(ts->kpd_input_dev, ts);

	if (input_register_device(ts->input_dev)) {
		TP_INFO(ts->tp_index, "%s: Failed to register input device\n", __func__);
		input_free_device(ts->input_dev);
		return -1;
	}

	if (input_register_device(ts->kpd_input_dev)) {
		TP_INFO(ts->tp_index, "%s: Failed to register key input device\n", __func__);
		input_free_device(ts->kpd_input_dev);
		return -1;
	}

	if (ts->face_detect_support) {
		if (input_register_device(ts->ps_input_dev)) {
			TP_INFO(ts->tp_index, "%s: Failed to register ps input device\n", __func__);
			input_free_device(ts->ps_input_dev);
			return -1;
		}
	}

	return 0;
}

/**
 * init_parse_dts - parse dts, get resource defined in Dts
 * @dev: i2c_client->dev using to get device tree
 * @ts: touchpanel_data, using for common driver
 *
 * If there is any Resource needed by chip_data, we can add a call-back func in this function
 * Do not care the result : Returning void type
 */
static int init_parse_dts(struct device *dev, struct touchpanel_data *ts)
{
	int rc, ret = 0;
	struct device_node *np;
	int temp_array[8];
	int tx_rx_num[2];
	int val = 0;
	int i = 0;
	int size = 0;
	char data_buf[32];
	np = dev->of_node;

	ts->register_is_16bit       = of_property_read_bool(np, "register-is-16bit");
	ts->esd_handle_support      = of_property_read_bool(np, "esd_handle_support");
	ts->fw_edge_limit_support   = of_property_read_bool(np,
				      "fw_edge_limit_support");
	ts->charger_pump_support    = of_property_read_bool(np, "charger_pump_support");
	ts->wireless_charger_support = of_property_read_bool(np,
				       "wireless_charger_support");
	ts->headset_pump_support    = of_property_read_bool(np, "headset_pump_support");
	ts->black_gesture_support   = of_property_read_bool(np,
				      "black_gesture_support");
	ts->gesture_test_support    = of_property_read_bool(np,
				      "black_gesture_test_support");
	ts->fw_update_app_support   = of_property_read_bool(np,
				      "fw_update_app_support");
	ts->game_switch_support     = of_property_read_bool(np, "game_switch_support");
	ts->is_noflash_ic           = of_property_read_bool(np, "noflash_support");
	ts->face_detect_support     = of_property_read_bool(np, "face_detect_support");
	ts->sec_long_low_trigger     = of_property_read_bool(np,
				       "sec_long_low_trigger");
	ts->health_monitor_support = of_property_read_bool(np,
				     "health_monitor_support");
	ts->lcd_trigger_load_tp_fw_support = of_property_read_bool(np,
					     "lcd_trigger_load_tp_fw_support");
	ts->fingerprint_underscreen_support = of_property_read_bool(np,
					      "fingerprint_underscreen_support");
	ts->suspend_gesture_cfg   = of_property_read_bool(np, "suspend_gesture_cfg");
	ts->auto_test_force_pass_support = of_property_read_bool(np,
					   "auto_test_force_pass_support");
	ts->freq_hop_simulate_support = of_property_read_bool(np,
					"freq_hop_simulate_support");
	ts->irq_trigger_hdl_support = of_property_read_bool(np,
				      "irq_trigger_hdl_support");
	ts->noise_modetest_support = of_property_read_bool(np,
				     "noise_modetest_support");
	ts->fw_update_in_probe_with_headfile = of_property_read_bool(np,
					       "fw_update_in_probe_with_headfile");
	ts->kernel_grip_support     = of_property_read_bool(np, "kernel_grip_support");
	ts->report_rate_white_list_support = of_property_read_bool(np,
					     "report_rate_white_list_support");
    ts->smooth_level_support = of_property_read_bool(np, "smooth_level_support");
    rc = of_property_read_u32(np, "smooth_level", &val);
    if (rc) {
        TPD_INFO("smooth_level not specified\n");
    } else {
        ts->smooth_level = val;
    }

	rc = of_property_read_u32(np, "vdd_2v8_volt", &ts->hw_res.vdd_volt);

	if (rc < 0) {
		ts->hw_res.vdd_volt = 0;
		TPD_INFO("vdd_2v8_volt not defined\n");
	}

	/* irq gpio*/
	ts->hw_res.irq_gpio = of_get_named_gpio_flags(np, "irq-gpio", 0,
			      &(ts->irq_flags));

	if (gpio_is_valid(ts->hw_res.irq_gpio)) {
		rc = devm_gpio_request(dev, ts->hw_res.irq_gpio, "tp_irq_gpio");

		if (rc) {
			TPD_INFO("unable to request gpio [%d]\n", ts->hw_res.irq_gpio);
		}

	} else {
		TPD_INFO("irq-gpio not specified in dts\n");
	}

	/* reset gpio*/
	ts->hw_res.reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (gpio_is_valid(ts->hw_res.reset_gpio)) {
		rc = devm_gpio_request(dev, ts->hw_res.reset_gpio, "reset-gpio");

		if (rc) {
			TPD_INFO("unable to request gpio [%d]\n", ts->hw_res.reset_gpio);
		}

	} else {
		TPD_INFO("ts->reset-gpio not specified\n");
	}

	TPD_INFO("%s : irq_gpio = %d, irq_flags = 0x%x, reset_gpio = %d\n",
		 __func__, ts->hw_res.irq_gpio, ts->irq_flags, ts->hw_res.reset_gpio);

	/* spi cs gpio */
	ts->hw_res.cs_gpio = of_get_named_gpio(np, "cs-gpio", 0);
	if (gpio_is_valid(ts->hw_res.cs_gpio)) {
		rc = gpio_request(ts->hw_res.cs_gpio, "cs-gpio");
		if (rc)
			TPD_INFO("unable to request gpio [%d]\n", ts->hw_res.cs_gpio);
		else
			TPD_INFO("%s : irq_gpio = %d\n", __func__, ts->hw_res.cs_gpio);
	} else {
		TPD_INFO("ts->cs-gpio not specified\n");
	}
	ts->hw_res.pinctrl = devm_pinctrl_get(dev);

	if (IS_ERR_OR_NULL(ts->hw_res.pinctrl)) {
		TPD_INFO("Getting pinctrl handle failed");
	} else {
		ts->hw_res.pin_set_high = pinctrl_lookup_state(ts->hw_res.pinctrl,
					  "pin_set_high");

		if (IS_ERR_OR_NULL(ts->hw_res.pin_set_high)) {
			TPD_INFO("Failed to get the high state pinctrl handle\n");
		}


		ts->hw_res.pin_set_low = pinctrl_lookup_state(ts->hw_res.pinctrl,
					 "pin_set_low");

		if (IS_ERR_OR_NULL(ts->hw_res.pin_set_low)) {
			TPD_INFO(" Failed to get the low state pinctrl handle\n");
		}

		ts->hw_res.pin_set_nopull = pinctrl_lookup_state(ts->hw_res.pinctrl,
					    "pin_set_nopull");

		if (IS_ERR_OR_NULL(ts->hw_res.pin_set_nopull)) {
			TPD_INFO("Failed to get the input state pinctrl handle\n");
		}


		ts->hw_res.active = pinctrl_lookup_state(ts->hw_res.pinctrl,
					"default");
		if (IS_ERR_OR_NULL(ts->hw_res.active)) {
			TPD_INFO("Failed to get the active pinctrl handle\n");
		}


		ts->hw_res.suspend = pinctrl_lookup_state(ts->hw_res.pinctrl,
					"suspend");
		if (IS_ERR_OR_NULL(ts->hw_res.suspend)) {
			TPD_INFO("Failed to get the suspend pinctrl handle\n");
		}
	}

	ts->hw_res.enable_avdd_gpio = of_get_named_gpio(np, "enable2v8_gpio", 0);

	if (ts->hw_res.enable_avdd_gpio < 0) {
		TPD_INFO("ts->hw_res.enable2v8_gpio not specified\n");

	} else {
		if (gpio_is_valid(ts->hw_res.enable_avdd_gpio)) {
			rc = devm_gpio_request(dev, ts->hw_res.enable_avdd_gpio, "vdd2v8-gpio");

			if (rc) {
				TPD_INFO("unable to request gpio [%d] %d\n", ts->hw_res.enable_avdd_gpio, rc);
			}
		}
	}

	ts->hw_res.enable_vddi_gpio = of_get_named_gpio(np, "enable1v8_gpio", 0);

	if (ts->hw_res.enable_vddi_gpio < 0) {
		TPD_INFO("ts->hw_res.enable1v8_gpio not specified\n");

	} else {
		if (gpio_is_valid(ts->hw_res.enable_vddi_gpio)) {
			rc = devm_gpio_request(dev, ts->hw_res.enable_vddi_gpio, "vcc1v8-gpio");

			if (rc) {
				TPD_INFO("unable to request gpio [%d], %d\n", ts->hw_res.enable_vddi_gpio, rc);
			}
		}
	}

	/* interrupt mode*/
	ts->int_mode = BANNABLE;
	rc = of_property_read_u32(np, "touchpanel,int-mode", &val);

	if (rc) {
		TPD_INFO("int-mode not specified\n");

	} else {
		if (val < INTERRUPT_MODE_MAX) {
			ts->int_mode = val;
		}
	}

	rc = of_property_read_u32(np, "project_id", &ts->panel_data.project_id);

	if (rc) {
		TPD_INFO("project_id not specified\n");
	}

	rc = of_property_count_u32_elems(np, "platform_support_project");
	ts->panel_data.project_num = rc;

	if (!rc) {
		TPD_INFO("project not specified\n");
	}

	if (ts->panel_data.project_num > 0) {
		rc = of_property_read_u32_array(np, "platform_support_project",
						ts->panel_data.platform_support_project, ts->panel_data.project_num);

		if (rc) {
			TPD_INFO("platform_support_project not specified");
			return -1;
		}

		rc = of_property_read_u32_array(np, "platform_support_project_dir",
						ts->panel_data.platform_support_project_dir, ts->panel_data.project_num);

		if (rc) {
			TPD_INFO("platform_support_project_dir not specified");
			return -1;
		}

	} else {
		TPD_INFO("project and project not specified in dts, please update dts");
	}

	/*parse chip name*/
	rc = of_property_read_u32(np, "chip-num", &ts->panel_data.chip_num);

	if (rc)  {
		TPD_INFO("panel_type not specified, need to default 1");
		ts->panel_data.chip_num = 1;
	}

	TPD_INFO("find %d num chip in dts", ts->panel_data.chip_num);

	for (i = 0; i < ts->panel_data.chip_num; i++) {
        ts->panel_data.chip_name[i] = devm_kzalloc(dev, 100, GFP_KERNEL);

        if (ts->panel_data.chip_name[i] == NULL) {
            TPD_INFO("panel_data.chip_name kzalloc error\n");
            devm_kfree(dev, ts->panel_data.chip_name[i]);
            goto dts_match_error;
        }
		rc = of_property_read_string_index(np, "chip-name", i,
						   (const char **)&ts->panel_data.chip_name[i]);
		TPD_INFO("panel_data.chip_name = %s\n", ts->panel_data.chip_name[i]);

		if (rc) {
			TPD_INFO("chip-name not specified");
		}
	}

	/*parse panel type and commandline*/
	rc = of_property_count_u32_elems(np, "panel_type");

	if (!rc) {
		TPD_INFO("panel_type not specified\n");

	} else if (rc) {
		TPD_INFO("now has %d num panel in dts\n", rc);
		ts->panel_data.panel_num = rc;
	}

	if (ts->panel_data.panel_num > 0) {
		rc = of_property_read_u32_array(np, "panel_type", ts->panel_data.panel_type,
						ts->panel_data.panel_num);

		if (rc) {
			TPD_INFO("panel_type not specified");
			goto dts_match_error;
		}
	}

	for (i = 0; i < ts->panel_data.panel_num; i++) {
        ts->panel_data.platform_support_commandline[i] = devm_kzalloc(dev, 100, GFP_KERNEL);
        if (ts->panel_data.platform_support_commandline[i] == NULL) {
            TPD_INFO("panel_data.platform_support_commandline kzalloc error\n");
            devm_kfree(dev, ts->panel_data.platform_support_commandline[i]);
            goto dts_match_error;
        }
		rc = of_property_read_string_index(np, "platform_support_project_commandline",
						   i,
						   (const char **)&ts->panel_data.platform_support_commandline[i]);

		if (rc) {
			TPD_INFO("platform_support_project_commandline not specified");
			goto dts_match_error;
		}

        ts->panel_data.firmware_name[i] = devm_kzalloc(dev, 25, GFP_KERNEL);
		rc = of_property_read_string_index(np, "firmware_name", i,
						   (const char **)&ts->panel_data.firmware_name[i]);

		if (rc) {
			TPD_INFO("firmware_name not specified");
            devm_kfree(dev, ts->panel_data.firmware_name[i]);
		}
	}

	rc = tp_judge_ic_match_commandline(&ts->panel_data);
	snprintf(data_buf, 32, "firmware-data-%d", rc);

	if (rc < 0) {
		TPD_INFO("commandline not match, please update dts");
		goto dts_match_error;
	}

	rc = of_property_read_u32(np, "vid_len", &ts->panel_data.vid_len);
		if (rc) {
		TPD_INFO("panel_data.vid_len not specified\n");
	}
	rc = of_property_read_u32(np, "tp_type", &ts->panel_data.tp_type);

	if (rc) {
		TPD_INFO("tp_type not specified\n");
	}

	TPD_INFO("read %s firmware in dts", data_buf);
	size = of_property_count_u8_elems(np, data_buf);

	if (size <= 0) {
		TPD_INFO("No firmware in dts !\n");

	} else {
		TPD_INFO("%s The firmware len id %d!\n", __func__, size);
		ts->firmware_in_dts = kzalloc(sizeof(struct firmware), GFP_KERNEL);

		if (ts->firmware_in_dts != NULL) {
			ts->firmware_in_dts->size = size;
			ts->firmware_in_dts->data = kzalloc(size, GFP_KERNEL | GFP_DMA);

			if (ts->firmware_in_dts->data == NULL) {
				kfree(ts->firmware_in_dts);
				ts->firmware_in_dts = NULL;

			} else {
				rc = of_property_read_u8_array(np, data_buf, (u8 *)ts->firmware_in_dts->data,
								size);
				if (rc) {
					TPD_INFO("Can not get the firmware in dts!\n");
					kfree(ts->firmware_in_dts->data);
					ts->firmware_in_dts->data = NULL;
					kfree(ts->firmware_in_dts);
					ts->firmware_in_dts = NULL;
					ts->firmware_in_dts->size = 0;
				} else {
					TPD_INFO("%s in %d get firmware in dts success\n", __func__, __LINE__);
				}
			}
		}
	}

	/* resolution info*/
	rc = of_property_read_u32(np, "touchpanel,max-num-support", &ts->max_num);

	if (rc) {
		TPD_INFO("ts->max_num not specified\n");
		ts->max_num = 10;
	}

	rc = of_property_read_u32_array(np, "touchpanel,tx-rx-num", tx_rx_num, 2);

	if (rc) {
		TPD_INFO("tx-rx-num not set\n");
		ts->hw_res.tx_num = 0;
		ts->hw_res.rx_num = 0;

	} else {
		ts->hw_res.tx_num = tx_rx_num[0];
		ts->hw_res.rx_num = tx_rx_num[1];
	}

	TPD_INFO("tx_num = %d, rx_num = %d \n", ts->hw_res.tx_num, ts->hw_res.rx_num);

	rc = of_property_read_u32_array(np, "touchpanel,display-coords", temp_array, 2);

	if (rc) {
		TPD_INFO("Lcd size not set\n");
		ts->resolution_info.LCD_WIDTH = 0;
		ts->resolution_info.LCD_HEIGHT = 0;

	} else {
		ts->resolution_info.LCD_WIDTH = temp_array[0];
		ts->resolution_info.LCD_HEIGHT = temp_array[1];
	}

	rc = of_property_read_u32_array(np, "touchpanel,panel-coords", temp_array, 2);

	if (rc) {
		ts->resolution_info.max_x = 0;
		ts->resolution_info.max_y = 0;

	} else {
		ts->resolution_info.max_x = temp_array[0];
		ts->resolution_info.max_y = temp_array[1];
	}

	rc = of_property_read_u32_array(np, "touchpanel,touchmajor-limit", temp_array,
					2);

	if (rc) {
		ts->touch_major_limit.width_range = 0;
		ts->touch_major_limit.height_range = 54;    /*set default value*/

	} else {
		ts->touch_major_limit.width_range = temp_array[0];
		ts->touch_major_limit.height_range = temp_array[1];
	}

	TPD_INFO("LCD_WIDTH = %d, LCD_HEIGHT = %d, max_x = %d, max_y = %d, limit_witdh = %d, limit_height = %d\n",
		 ts->resolution_info.LCD_WIDTH, ts->resolution_info.LCD_HEIGHT,
		 ts->resolution_info.max_x, ts->resolution_info.max_y, \
		 ts->touch_major_limit.width_range, ts->touch_major_limit.height_range);

	/* virturl key Related*/
	rc = of_property_read_u32_array(np, "touchpanel,button-type", temp_array, 2);

	if (rc < 0) {
		TPD_INFO("error:button-type should be setting in dts!");

	} else {
		ts->vk_type = temp_array[0];
		ts->vk_bitmap = temp_array[1] & 0xFF;

		if (ts->vk_type == TYPE_PROPERTIES) {
			rc = of_property_read_u32_array(np, "touchpanel,button-map", temp_array, 8);

			if (rc) {
				TPD_INFO("button-map not set\n");

			} else {
				ts->button_map.coord_menu.x = temp_array[0];
				ts->button_map.coord_menu.y = temp_array[1];
				ts->button_map.coord_home.x = temp_array[2];
				ts->button_map.coord_home.y = temp_array[3];
				ts->button_map.coord_back.x = temp_array[4];
				ts->button_map.coord_back.y = temp_array[5];
				ts->button_map.width_x = temp_array[6];
				ts->button_map.height_y = temp_array[7];
			}
		}
	}

	/*touchkey take tx num and rx num*/
	rc = of_property_read_u32_array(np, "touchpanel.button-TRx", temp_array, 2);

	if (rc < 0) {
		TPD_INFO("error:button-TRx should be setting in dts!\n");
		ts->hw_res.key_tx = 0;
		ts->hw_res.key_rx = 0;

	} else {
		ts->hw_res.key_tx = temp_array[0];
		ts->hw_res.key_rx = temp_array[1];
		TPD_INFO("key_tx is %d, key_rx is %d\n", ts->hw_res.key_tx, ts->hw_res.key_rx);
	}

	/*set incell panel parameter, for of_property_read_bool return 1 when success and return 0 when item is not exist*/
	rc = ts->is_incell_panel = of_property_read_bool(np, "incell_screen");

	if (rc > 0) {
		TPD_INFO("panel is incell!\n");
		ts->is_incell_panel = 1;

	} else {
		TPD_INFO("panel is oncell!\n");
		ts->is_incell_panel = 0;
	}

	ts->tp_ic_type = TYPE_ONCELL;
	rc = of_property_read_u32(np, "touchpanel,tp_ic_type", &val);

	if (rc) {
		TPD_INFO("tp_ic_type not specified\n");

	} else {
		if (val < TYPE_IC_MAX) {
			ts->tp_ic_type = val;
		}
	}

	/* tp_index*/
	rc = of_property_read_u32(np, "touchpanel,tp-index", &ts->tp_index);

	if (rc) {
		TPD_INFO("ts->tp_index not specified\n");
		ts->tp_index = 0;

	} else {
		if (ts->tp_index >= TP_SUPPORT_MAX) {
			TPD_INFO("ts->tp_index is big than %d\n", TP_SUPPORT_MAX);
			ts->tp_index = 0;
			ret = -1;
		}
	}

	cur_tp_index = ts->tp_index;
	TPD_INFO("ts->tp_index is %d\n",  cur_tp_index);

	ts->resume_no_freeirq = 0;
	rc = of_property_read_u32(np, "resume_no_freeirq", &ts->resume_no_freeirq);
	if (rc) {
		TPD_INFO("ts->resume_no_freeirq not specified\n");
	}

	rc = of_property_read_u32(np, "support_check_bus_suspend", &ts->support_check_bus_suspend);
	if (rc) {
		TPD_INFO("ts->support_check_bus_suspend not specified\n");
	}
	return ret;

dts_match_error:
	return -1;
}

static int init_power_control(struct touchpanel_data *ts)
{
	int ret = 0;

	/* 1.8v*/
	ts->hw_res.vddi = regulator_get(ts->dev, "vcc_1v8");

	if (IS_ERR_OR_NULL(ts->hw_res.vddi)) {
		TP_INFO(ts->tp_index, "Regulator get failed vcc_1v8, ret = %d\n", ret);

	} else {
		if (regulator_count_voltages(ts->hw_res.vddi) > 0) {
			ret = regulator_set_voltage(ts->hw_res.vddi, 1800000, 1800000);

			if (ret) {
				dev_err(ts->dev, "Regulator set_vtg failed vcc_i2c rc = %d\n", ret);
				goto err;
			}

			ret = regulator_set_load(ts->hw_res.vddi, 200000);

			if (ret < 0) {
				dev_err(ts->dev, "Failed to set vcc_1v8 mode(rc:%d)\n", ret);
				goto err;
			}
		}
	}

	/* vdd 2.8v*/
	ts->hw_res.avdd = regulator_get(ts->dev, "vdd_2v8");

	if (IS_ERR_OR_NULL(ts->hw_res.avdd)) {
		TP_INFO(ts->tp_index, "Regulator vdd2v8 get failed, ret = %d\n", ret);

	} else {
		if (regulator_count_voltages(ts->hw_res.avdd) > 0) {
			TP_INFO(ts->tp_index, "set avdd voltage to %d uV\n", ts->hw_res.vdd_volt);

			if (ts->hw_res.vdd_volt) {
				ret = regulator_set_voltage(ts->hw_res.avdd, ts->hw_res.vdd_volt,
							    ts->hw_res.vdd_volt);

			} else {
				ret = regulator_set_voltage(ts->hw_res.avdd, 3100000, 3100000);
			}

			if (ret) {
				dev_err(ts->dev, "Regulator set_vtg failed vdd rc = %d\n", ret);
				goto err;
			}

			ret = regulator_set_load(ts->hw_res.avdd, 200000);

			if (ret < 0) {
				dev_err(ts->dev, "Failed to set vdd_2v8 mode(rc:%d)\n", ret);
				goto err;
			}
		}
	}

	return 0;

err:
	return ret;
}

int tp_powercontrol_vddi(struct hw_resource *hw_res, bool on)
{
	int ret = 0;

	if (on) { /* 1v8 power on*/
		if (!IS_ERR_OR_NULL(hw_res->vddi)) {
			TPD_INFO("Enable the Regulator vddi.\n");
			ret = regulator_enable(hw_res->vddi);

			if (ret) {
				TPD_INFO("Regulator vcc_i2c enable failed ret = %d\n", ret);
				return ret;
			}
		}

		if (hw_res->enable_vddi_gpio > 0) {
			TPD_INFO("Enable the vddi_gpio\n");
			ret = gpio_direction_output(hw_res->enable_vddi_gpio, 1);

			if (ret) {
				TPD_INFO("enable the enable_vddi_gpio failed.\n");
				return ret;
			}
		}

	} else { /* 1v8 power off*/
		if (!IS_ERR_OR_NULL(hw_res->vddi)) {
			ret = regulator_disable(hw_res->vddi);

			if (ret) {
				TPD_INFO("Regulator vcc_i2c enable failed rc = %d\n", ret);
				return ret;
			}
		}

		if (hw_res->enable_vddi_gpio > 0) {
			TPD_INFO("disable the enable_vddi_gpio\n");
			ret = gpio_direction_output(hw_res->enable_vddi_gpio, 0);

			if (ret) {
				TPD_INFO("disable the enable_vddi_gpio failed.\n");
				return ret;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(tp_powercontrol_vddi);


int tp_powercontrol_avdd(struct hw_resource *hw_res, bool on)
{
	int ret = 0;

	if (on) { /* 2v8 power on*/
		if (!IS_ERR_OR_NULL(hw_res->avdd)) {
			TPD_INFO("Enable the Regulator2v8.\n");
			ret = regulator_enable(hw_res->avdd);

			if (ret) {
				TPD_INFO("Regulator vdd enable failed ret = %d\n", ret);
				return ret;
			}
		}

		if (hw_res->enable_avdd_gpio > 0) {
			TPD_INFO("Enable the enable_avdd_gpio, hw_res->enable2v8_gpio is %d\n",
				 hw_res->enable_avdd_gpio);
			ret = gpio_direction_output(hw_res->enable_avdd_gpio, 1);

			if (ret) {
				TPD_INFO("enable the enable_avdd_gpio failed.\n");
				return ret;
			}
		}

	} else { /* 2v8 power off*/
		if (!IS_ERR_OR_NULL(hw_res->avdd)) {
			ret = regulator_disable(hw_res->avdd);

			if (ret) {
				TPD_INFO("Regulator vdd disable failed rc = %d\n", ret);
				return ret;
			}
		}

		if (hw_res->enable_avdd_gpio > 0) {
			TPD_INFO("disable the enable_avdd_gpio\n");
			ret = gpio_direction_output(hw_res->enable_avdd_gpio, 0);

			if (ret) {
				TPD_INFO("disable the enable_avdd_gpio failed.\n");
				return ret;
			}
		}
	}

	return ret;
}
EXPORT_SYMBOL(tp_powercontrol_avdd);


static void esd_handle_func(struct work_struct *work)
{
	int ret = 0;
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     esd_info.esd_check_work.work);

	if (ts->loading_fw) {
		TP_INFO(ts->tp_index, "FW is updating, stop esd handle!\n");
		return;
	}

	mutex_lock(&ts->esd_info.esd_lock);

	if (!ts->esd_info.esd_running_flag) {
		TP_INFO(ts->tp_index, "Esd protector has stopped!\n");
		goto ESD_END;
	}

	if (ts->is_suspended == 1) {
		TP_INFO(ts->tp_index, "Touch panel has suspended!\n");
		goto ESD_END;
	}

	if (!ts->ts_ops->esd_handle) {
		TP_INFO(ts->tp_index, "not support ts_ops->esd_handle callback\n");
		goto ESD_END;
	}

	ret = ts->ts_ops->esd_handle(ts->chip_data);

	if (ret ==
	    -1) {    /*-1 means esd hanppened: handled in IC part, recovery the state here*/
		operate_mode_switch(ts);
	}

	if (ts->esd_info.esd_running_flag) {
		queue_delayed_work(ts->esd_info.esd_workqueue, &ts->esd_info.esd_check_work,
				   ts->esd_info.esd_work_time);

	} else {
		TP_INFO(ts->tp_index, "Esd protector suspended!");
	}

ESD_END:
	mutex_unlock(&ts->esd_info.esd_lock);
	return;
}

/**
 * esd_handle_switch - open or close esd thread
 * @esd_info: touchpanel_data, using for common driver resource
 * @on: bool variable using for  indicating open or close esd check function.
 *     true:open;
 *     false:close;
 */
void esd_handle_switch(struct esd_information *esd_info, bool on)
{
	mutex_lock(&esd_info->esd_lock);

	if (on) {
		if (!esd_info->esd_running_flag) {
			esd_info->esd_running_flag = 1;

			TPD_INFO("Esd protector started, cycle: %d s\n", esd_info->esd_work_time / HZ);
			queue_delayed_work(esd_info->esd_workqueue, &esd_info->esd_check_work,
					   esd_info->esd_work_time);
		}

	} else {
		if (esd_info->esd_running_flag) {
			esd_info->esd_running_flag = 0;

			TPD_INFO("Esd protector stoped!\n");
			cancel_delayed_work(&esd_info->esd_check_work);
		}
	}

	mutex_unlock(&esd_info->esd_lock);
}
EXPORT_SYMBOL(esd_handle_switch);

static int tp_register_irq_func(struct touchpanel_data *ts)
{
	int ret = 0;

	if (gpio_is_valid(ts->hw_res.irq_gpio)) {
		TP_DEBUG(ts->tp_index, "%s, irq_gpio is %d, ts->irq is %d\n", __func__,
			 ts->hw_res.irq_gpio, ts->irq);

		if (ts->irq_flags_cover) {
			ts->irq_flags = ts->irq_flags_cover;
			TP_INFO(ts->tp_index, "%s irq_flags is covered by 0x%x\n", __func__,
				ts->irq_flags_cover);
		}

		if (ts->irq <= 0) {
			ts->irq = gpio_to_irq(ts->hw_res.irq_gpio);
			TP_INFO(ts->tp_index, "%s ts->irq is %d\n", __func__, ts->irq);
		}

		snprintf(ts->irq_name, sizeof(ts->irq_name), "touch-%02d", ts->tp_index);
		ret = devm_request_threaded_irq(ts->dev, ts->irq, NULL,
						tp_irq_thread_fn,
						ts->irq_flags | IRQF_ONESHOT,
						ts->irq_name, ts);

		if (ret < 0) {
			TP_INFO(ts->tp_index, "%s request_threaded_irq ret is %d\n", __func__, ret);
		}

	} else {
		TP_INFO(ts->tp_index, "%s:no valid irq\n", __func__);
		ret = -1;
	}

	return ret;
}

static int tp_paneldata_init(struct touchpanel_data *pdata)
{
	struct touchpanel_data *ts = pdata;
	int ret = -1;

	if (!ts) {
		return ret;
	}

	/*step7 : Alloc fw_name/devinfo memory space*/
	ts->panel_data.fw_name = tp_devm_kzalloc(ts->dev, MAX_FW_NAME_LENGTH,
				 GFP_KERNEL);

	if (ts->panel_data.fw_name == NULL) {
		ret = -ENOMEM;
		TP_INFO(ts->tp_index, "panel_data.fw_name kzalloc error\n");
		return ret;
	}

#ifndef REMOVE_OPLUS_FUNCTION
	ts->panel_data.manufacture_info.version = tp_devm_kzalloc(ts->dev,
			MAX_DEVICE_VERSION_LENGTH, GFP_KERNEL);

	if (ts->panel_data.manufacture_info.version == NULL) {
		ret = -ENOMEM;
		TP_INFO(ts->tp_index, "manufacture_info.version kzalloc error\n");
		return ret;
	}

	ts->panel_data.manufacture_info.manufacture = tp_devm_kzalloc(ts->dev,
			MAX_DEVICE_MANU_LENGTH, GFP_KERNEL);

	if (ts->panel_data.manufacture_info.manufacture == NULL) {
		ret = -ENOMEM;
		TP_INFO(ts->tp_index, "panel_data.fw_name kzalloc error\n");
		return ret;
	}

#endif
	/*step8 : touchpanel vendor*/
	tp_util_get_vendor(&ts->hw_res, &ts->panel_data);

	if (ts->ts_ops->get_vendor) {
		ts->ts_ops->get_vendor(ts->chip_data, &ts->panel_data);
	}

	return 0;
}

static int tp_power_init(struct touchpanel_data *pdata)
{
	struct touchpanel_data *ts = pdata;
	int ret = -1;

	if (!ts) {
		return ret;
	}

	preconfig_power_control(ts);
	ret = init_power_control(ts);

	if (ret) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "%s: tp power init failed.\n", __func__);
		return ret;
	}

	ret = reconfig_power_control(ts);

	if (ret) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "%s: reconfig power failed.\n", __func__);
		return ret;
	}

	if (!ts->ts_ops->power_control) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "tp power_control NULL!\n");
		return ret;
	}

	ret = ts->ts_ops->power_control(ts->chip_data, true);

	if (ret) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "%s: tp power init failed.\n", __func__);
		return ret;
	}

	return 0;
}
/**
 * register_common_touch_device - parse dts, get resource defined in Dts
 * @pdata: touchpanel_data, using for common driver
 *
 * entrance of common touch Driver
 * Returning zero(sucess) or negative errno(failed)
 */

static int check_dt(struct device_node *np)
{
	int i;
	int count;
	int retry_count = 0;
	bool panel_status = true;
	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "panel", NULL);
	TPD_INFO("count is %d\n", count);
	if (count <= 0)
		return -ENODEV;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		if (node == NULL) {
			pr_err("[TP]: %s in %d NULL point\n", __func__, __LINE__);
			return -ENODEV;
		}
		TPD_INFO("node name is %s\n", node->name);
		retry_count = 0;
		while(panel_status && retry_count < 3) {
			panel = of_drm_find_panel(node);
			if (!IS_ERR(panel)) {
				TPD_INFO("tp matched lcd panel\n");
				panel_status = false;
				lcd_active_panel = panel;
				return 0;
			}
			msleep(5000);
			retry_count ++;
		}
		of_node_put(node);
		TPD_INFO("%s: error3\n", __func__);
	}

	return -ENODEV;
}

static void tp_load_lcd_node(struct work_struct *work)
{
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
			load_lcd_type_work);
	int ret = -1;
	int retry_count = 5;

	if (ts == NULL ||ts->s_client == NULL) {
		pr_err("[TP]: %s in %d NULL point\n", __func__, __LINE__);
		return;
	}

	ret = check_dt(ts->s_client->dev.of_node);

	while (ret != 0 && retry_count--) {
		TPD_INFO("check_dt fail, try again\n");
		usleep_range(3000, 3000);
		ret = check_dt(ts->s_client->dev.of_node);

		if (ret == 0)
			break;
	}
	ts->fb_drm_notif.notifier_call = fb_drm_notifier_callback;
	if (lcd_active_panel) {
		ret = drm_panel_notifier_register(lcd_active_panel, &ts->fb_drm_notif);
		if (ret) {
			TPD_INFO("Unable to register fb_drm_notif: %d\n", ret);
		}
	} else {
		TPD_INFO("register notifer failed\n");
	}
}

int register_common_touch_device(struct touchpanel_data *pdata)
{
	struct touchpanel_data *ts = pdata;
	char name[TP_NAME_SIZE_MAX];

	int ret = -1;

	TPD_INFO("%s  is gqc called\n", __func__);

	if (!ts->dev) {
		return -1;
	}

	/*step1 : dts parse*/
	ret = init_parse_dts(ts->dev, ts);

	if (ret < 0) {
		TP_INFO(ts->tp_index, "%s: dts init failed.\n", __func__);
		return -1;
	}

	/*step2 : initial health info parameter*/
	if (ts->health_monitor_support) {
		ret = tp_healthinfo_init(ts->dev, &ts->monitor_data);
		if (ret < 0) {
			TPD_INFO("health info init failed.\n");
		}

		ts->monitor_data.health_monitor_support = true;
		ts->monitor_data.chip_data = ts->chip_data;
		ts->monitor_data.debug_info_ops = ts->debug_info_ops;
	}

	/*step3 : interfaces init*/
	init_touch_interfaces(ts->dev, ts->register_is_16bit);

	/*step4 : mutex init*/
	mutex_init(&ts->mutex);
	mutex_init(&ts->report_mutex);
	init_completion(&ts->fw_complete);
	init_waitqueue_head(&ts->wait);
	ts->com_api_data.tp_irq_disable = 1;

	/*wake_lock_init(&ts->wakelock, WAKE_LOCK_SUSPEND, "tp_wakelock");*/
	/*step5 : power init*/
	ret = tp_power_init(ts);

	if (ret < 0) {
		return ret;
	}

	/*step6 : I2C function check*/
	if (!ts->is_noflash_ic) {
		if (!i2c_check_functionality(ts->client->adapter, I2C_FUNC_I2C)) {
			TP_INFO(ts->tp_index, "%s: need I2C_FUNC_I2C\n", __func__);
			ret = -ENODEV;
			goto err_check_functionality_failed;
		}
	}

	/*step7 : touch input dev init*/
	ret = init_input_device(ts);

	if (ret < 0) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "tp_input_init failed!\n");
		goto err_check_functionality_failed;
	}

	/*step8 : irq request setting*/
	if (ts->int_mode == UNBANNABLE) {
		ret = tp_register_irq_func(ts);

		if (ret < 0) {
			goto err_check_functionality_failed;
		}

		ts->bus_ready = true;
	}

	/*step9 : panel data init*/
	ret = tp_paneldata_init(ts);

	if (ret < 0) {
		goto err_check_functionality_failed;
	}

	/*step10 : FTM process*/
	ts->boot_mode = get_boot_mode();
	pr_err("[TP]:%s ts->boot_mode %d \n", __func__, ts->boot_mode);

	if (is_ftm_boot_mode(ts)) {
		ts->ts_ops->ftm_process(ts->chip_data);
		ret = -EFTM;

		if (ts->int_mode == UNBANNABLE) {
			devm_free_irq(ts->dev, ts->irq, ts);
		}

		g_tp[ts->tp_index] = ts;
		TP_INFO(ts->tp_index, "%s: not int normal mode, return.\n", __func__);
		return ret;
	}

	/*step11:get chip info*/
	if (!ts->ts_ops->get_chip_info) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "tp get_chip_info NULL!\n");
		goto err_check_functionality_failed;
	}

	ret = ts->ts_ops->get_chip_info(ts->chip_data);

	if (ret < 0) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "tp get_chip_info failed!\n");
		goto err_check_functionality_failed;
	}

	/*step12 : touchpanel Fw check*/
	if (!ts->is_noflash_ic) {           /*noflash don't have firmware before fw update*/
		if (!ts->ts_ops->fw_check) {
			ret = -EINVAL;
			TP_INFO(ts->tp_index, "tp fw_check NULL!\n");
			goto err_check_functionality_failed;
		}

		ret = ts->ts_ops->fw_check(ts->chip_data, &ts->resolution_info,
					   &ts->panel_data);

		if (ret == FW_ABNORMAL) {
			ts->force_update = 1;
			TP_INFO(ts->tp_index, "This FW need to be updated!\n");

		} else {
			ts->force_update = 0;
		}
	}

	/*step13 : enable touch ic irq output ability*/
	if (!ts->ts_ops->mode_switch) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "tp mode_switch NULL!\n");
		goto err_check_functionality_failed;
	}

	ret = ts->ts_ops->mode_switch(ts->chip_data, MODE_NORMAL, true);

	if (ret < 0) {
		ret = -EINVAL;
		TP_INFO(ts->tp_index, "%s:modem switch failed!\n", __func__);
		goto err_check_functionality_failed;
	}

	/*step14 : irq request setting*/
	if (ts->int_mode == BANNABLE) {
		ret = tp_register_irq_func(ts);

		if (ret < 0) {
			goto err_check_functionality_failed;
		}
	}

	/*step15 : suspend && resume fuction register*/

#if defined(CONFIG_DRM_MSM)
	ts->fb_notif.notifier_call = fb_notifier_callback;
	ret = msm_drm_register_client(&ts->fb_notif);

	if (ret) {
		TP_INFO(ts->tp_index, "Unable to register fb_notifier: %d\n", ret);
		goto err_check_functionality_failed;
	}

#elif defined(CONFIG_FB)
	ts->fb_notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);

	if (ret) {
		TP_INFO(ts->tp_index, "Unable to register fb_notifier: %d\n", ret);
		goto err_check_functionality_failed;
	}

#endif/*CONFIG_FB*/

	if (ts->headset_pump_support) {
		snprintf(name, TP_NAME_SIZE_MAX, "headset_pump%d", ts->tp_index);
		ts->headset_pump_wq = create_singlethread_workqueue(name);

		if (!ts->headset_pump_wq) {
			ret = -ENOMEM;
			goto error_fb_notif;
		}

		INIT_WORK(&ts->headset_pump_work, switch_headset_work);
	}

	if (ts->charger_pump_support) {
		snprintf(name, TP_NAME_SIZE_MAX, "charger_pump%d", ts->tp_index);
		ts->charger_pump_wq = create_singlethread_workqueue(name);

		if (!ts->charger_pump_wq) {
			ret = -ENOMEM;
			goto error_headset_pump;
		}

		INIT_WORK(&ts->charger_pump_work, switch_usb_state_work);
	}

	/*step16 : workqueue create(speedup_resume)*/
	snprintf(name, TP_NAME_SIZE_MAX, "sp_resume%d", ts->tp_index);
	ts->speedup_resume_wq = create_singlethread_workqueue(name);

	if (!ts->speedup_resume_wq) {
		ret = -ENOMEM;
		goto error_charger_pump;
	}

	INIT_WORK(&ts->speed_up_work, speedup_resume);
	INIT_WORK(&ts->fw_update_work, tp_fw_update_work);
	INIT_WORK(&ts->load_lcd_type_work, tp_load_lcd_node);
	//register notify call chain
	schedule_work(&ts->load_lcd_type_work);

	/*step 17:incell lcd trigger load tp fw  support*/
	if (ts->lcd_trigger_load_tp_fw_support) {
		snprintf(name, TP_NAME_SIZE_MAX, "lcd_trigger_load_tp_fw_wq%d", ts->tp_index);
		ts->lcd_trigger_load_tp_fw_wq = create_singlethread_workqueue(name);

		if (!ts->lcd_trigger_load_tp_fw_wq) {
			ret = -ENOMEM;
			goto error_speedup_resume_wq;
		}

		INIT_WORK(&ts->lcd_trigger_load_tp_fw_work, lcd_trigger_load_tp_fw);
	}

	/*step 18:esd recover support*/
	if (ts->esd_handle_support) {
		snprintf(name, TP_NAME_SIZE_MAX, "esd_workthread%d", ts->tp_index);
		ts->esd_info.esd_workqueue = create_singlethread_workqueue(name);

		if (!ts->esd_info.esd_workqueue) {
			ret = -ENOMEM;
			goto error_tp_fw_wq;
		}

		INIT_DELAYED_WORK(&ts->esd_info.esd_check_work, esd_handle_func);

		mutex_init(&ts->esd_info.esd_lock);

		ts->esd_info.esd_running_flag = 0;
		ts->esd_info.esd_work_time = 2 *
					     HZ; /* HZ: clock ticks in 1 second generated by system*/
		TP_INFO(ts->tp_index, "Clock ticks for an esd cycle: %d\n",
			ts->esd_info.esd_work_time);

		esd_handle_switch(&ts->esd_info, true);
	}

	/*step 19:frequency hopping simulate support*/
	if (ts->freq_hop_simulate_support) {
		snprintf(name, TP_NAME_SIZE_MAX, "syna_tcm_freq_hop%d", ts->tp_index);
		ts->freq_hop_info.freq_hop_workqueue = create_singlethread_workqueue(name);

		if (!ts->freq_hop_info.freq_hop_workqueue) {
			ret = -ENOMEM;
			goto error_esd_wq;
		}

		INIT_DELAYED_WORK(&ts->freq_hop_info.freq_hop_work, tp_freq_hop_work);
		ts->freq_hop_info.freq_hop_simulating = false;
		ts->freq_hop_info.freq_hop_freq = 0;
	}

	/*initial kernel grip parameter*/
	if (ts->kernel_grip_support) {
		ts->grip_info = kernel_grip_init(ts->dev);

		if (!ts->grip_info) {
			TP_INFO(ts->tp_index, "kernel grip init failed.\n");
		}
	}



	/*step 22 : createproc proc files interface*/
	init_touchpanel_proc(ts);

	/*step 23 : Other*****/
	if (ts->fw_edge_limit_support) {
		ts->limit_enable = 1;
	}

	ts->bus_ready = true;
	ts->loading_fw = false;
	ts->is_suspended = 0;
	ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;
	ts->gesture_enable = 0;
	ts->fd_enable = 0;
	ts->fp_enable = 0;
	ts->fp_info.touch_state = 0;
	ts->palm_enable = 1;
	ts->touch_count = 0;
	ts->view_area_touched = 0;
	ts->tp_suspend_order = LCD_TP_SUSPEND;
	ts->tp_resume_order = TP_LCD_RESUME;
	ts->skip_suspend_operate = false;
	ts->skip_reset_in_resume = false;
	ts->irq_slot = 0;
	ts->firmware_update_type = 0;

	if (ts->is_noflash_ic) {
		ts->irq = ts->s_client->irq;

	} else {
		ts->irq = ts->client->irq;
	}

	mutex_lock(&tp_core_lock);
	g_tp[ts->tp_index] = ts;
	mutex_unlock(&tp_core_lock);
	pr_err("[TP]:%s reg gesture enable flag\n", __func__);
	tp_gesture_enable_notifier = tp_gesture_enable_flag;
	TP_INFO(ts->tp_index, "Touch panel probe : normal end\n");
	return 0;

error_esd_wq:

	if (ts->esd_handle_support) {
		destroy_workqueue(ts->esd_info.esd_workqueue);
	}

error_tp_fw_wq:

	if (ts->lcd_trigger_load_tp_fw_support) {
		destroy_workqueue(ts->lcd_trigger_load_tp_fw_wq);
	}

error_speedup_resume_wq:

	if (ts->speedup_resume_wq) {
		destroy_workqueue(ts->speedup_resume_wq);
	}

error_charger_pump:

	if (ts->charger_pump_support) {
		destroy_workqueue(ts->charger_pump_wq);
	}

error_headset_pump:

	if (ts->headset_pump_support) {
		destroy_workqueue(ts->headset_pump_wq);
	}

error_fb_notif:
#ifdef CONFIG_DRM_MSM
    msm_drm_unregister_client(&ts->fb_notif);
#elif defined(CONFIG_FB)
    fb_unregister_client(&ts->fb_notif);
#endif

err_check_functionality_failed:
    ts->ts_ops->power_control(ts->chip_data, false);
	if (gpio_is_valid(ts->hw_res.cs_gpio)) {
		gpio_free(ts->hw_res.cs_gpio);
	}

    //wake_lock_destroy(&ts->wakelock);
    ret = -1;
    return ret;
}
EXPORT_SYMBOL(register_common_touch_device);

void unregister_common_touch_device(struct touchpanel_data *pdata)
{
    struct touchpanel_data *ts = pdata;
#if defined(CONFIG_FB) || defined(CONFIG_DRM_MSM)
    int ret;
#endif

	if (!pdata) {
		return;
	}

	/*step1 :free irq*/
	devm_free_irq(ts->dev, ts->irq, ts);
	/*step2 :free proc node*/
	remove_touchpanel_proc(ts);

	/*step3 :free the hw resource*/
	pdata->ts_ops->power_control(ts->chip_data, false);

	/*step4:frequency hopping simulate support*/
	if (ts->freq_hop_simulate_support) {
		if (ts->freq_hop_info.freq_hop_workqueue) {
			cancel_delayed_work_sync(&ts->freq_hop_info.freq_hop_work);
			flush_workqueue(ts->freq_hop_info.freq_hop_workqueue);
			destroy_workqueue(ts->freq_hop_info.freq_hop_workqueue);
		}
	}

	/*step5:esd recover support*/
	if (ts->esd_handle_support) {
		esd_handle_switch(&ts->esd_info, false);

		if (ts->esd_info.esd_workqueue) {
			cancel_delayed_work_sync(&ts->esd_info.esd_check_work);
			flush_workqueue(ts->esd_info.esd_workqueue);
			destroy_workqueue(ts->esd_info.esd_workqueue);
		}

		mutex_destroy(&ts->esd_info.esd_lock);
	}

	/*step6 : workqueue create(speedup_resume)*/
	if (ts->speedup_resume_wq) {
		cancel_work_sync(&ts->speed_up_work);
		flush_workqueue(ts->speedup_resume_wq);
		destroy_workqueue(ts->speedup_resume_wq);
	}

	if (ts->lcd_trigger_load_tp_fw_wq) {
		cancel_work_sync(&ts->lcd_trigger_load_tp_fw_work);
		flush_workqueue(ts->lcd_trigger_load_tp_fw_wq);
		destroy_workqueue(ts->lcd_trigger_load_tp_fw_wq);
	}

	/*step7 : suspend && resume fuction register*/
#if defined(CONFIG_DRM_MSM)
    if(ts->fb_notif.notifier_call) {
        ret = msm_drm_unregister_client(&ts->fb_notif);
        if (ret) {
            TP_INFO(ts->tp_index, "Unable to register fb_notifier: %d\n", ret);
        }
    }
#elif defined(CONFIG_FB)

	if (ts->fb_notif.notifier_call) {
		ret = fb_unregister_client(&ts->fb_notif);

		if (ret) {
			TP_INFO(ts->tp_index, "Unable to unregister fb_notifier: %d\n", ret);
		}
	}

#endif/*CONFIG_FB*/

	/*free regulator*/
	if (!IS_ERR_OR_NULL(ts->hw_res.avdd)) {
		regulator_put(ts->hw_res.avdd);
		ts->hw_res.avdd = NULL;
	}

	if (!IS_ERR_OR_NULL(ts->hw_res.vddi)) {
		regulator_put(ts->hw_res.vddi);
		ts->hw_res.vddi = NULL;
	}

	/*wake_lock_destroy(&ts->wakelock);*/
	/*step8 : mutex init*/
	mutex_destroy(&ts->mutex);
}
EXPORT_SYMBOL(unregister_common_touch_device);

struct touchpanel_data *common_touch_data_alloc(void)
{
	return tp_kzalloc(sizeof(struct touchpanel_data), GFP_KERNEL);
}
EXPORT_SYMBOL(common_touch_data_alloc);

int common_touch_data_free(struct touchpanel_data *pdata)
{
	if (pdata) {
		kfree(pdata);
	}

	return 0;
}
EXPORT_SYMBOL(common_touch_data_free);

/**
 * touchpanel_ts_suspend - touchpanel suspend function
 * @dev: i2c_client->dev using to get touchpanel_data resource
 *
 * suspend function bind to LCD on/off status
 * Returning zero(sucess) or negative errno(failed)
 */
int tp_suspend(struct device *dev)
{

	u64 start_time = 0;
	struct touchpanel_data *ts = dev_get_drvdata(dev);
	int ret;

	TP_INFO(ts->tp_index, "%s: start.\n", __func__);

	TP_INFO(ts->tp_index, "tp_suspend ts->bus_ready =%d\n", ts->bus_ready);

	/*step1:detect whether we need to do suspend*/
	if (ts->input_dev == NULL) {
		TP_INFO(ts->tp_index, "input_dev  registration is not complete\n");
		return 0;
	}

	if (ts->loading_fw) {
		TP_INFO(ts->tp_index, "FW is updating while suspending");
		return 0;
	}

	if (ts->health_monitor_support) {
		reset_healthinfo_time_counter(&start_time);
	}

	/* release all complete first */
	if (ts->tp_ic_type == TYPE_TDDI_TCM) {
		if (ts->ts_ops->reinit_device) {
			ts->ts_ops->reinit_device(ts->chip_data);
		}
	}

	/*step2:get mutex && start process suspend flow*/
	mutex_lock(&ts->mutex);

	if (!ts->is_suspended) {
		ts->is_suspended = 1;
		ts->suspend_state = TP_SUSPEND_COMPLETE;

	} else {
		TP_INFO(ts->tp_index, "%s: do not suspend twice.\n", __func__);
		goto EXIT;
	}
	pr_err("[TP]: %s ts->is_suspended %d \n", __func__, ts->is_suspended);

	/*step3:Release key && touch event before suspend*/
	tp_btnkey_release(ts);
	tp_touch_release(ts);

	/*step4:cancel esd test*/
	if (ts->esd_handle_support) {
		esd_handle_switch(&ts->esd_info, false);
	}

	ts->rate_ctrl_level = 0;

	if (!ts->is_incell_panel || (ts->black_gesture_support
				     && ts->gesture_enable > 0)) {
		/*step5:gamde mode support*/
		if (ts->game_switch_support) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_GAME, false);
		}

		if (ts->report_rate_white_list_support && ts->ts_ops->rate_white_list_ctrl) {
			ts->ts_ops->rate_white_list_ctrl(ts->chip_data, 0);
		}

		if (ts->face_detect_support && ts->fd_enable) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_FACE_DETECT, false);
		}
	}

	/*step6:finger print support handle*/
	if (ts->fingerprint_underscreen_support) {
		operate_mode_switch(ts);
		ts->fp_info.touch_state = 0;
        opticalfp_irq_handler(&ts->fp_info);
		goto EXIT;
	}

	/*step7:gesture mode status process*/
	if (ts->black_gesture_support) {
		if ((CHK_BIT(ts->gesture_enable, 0x01)) == 1) {
			ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, ts->gesture_enable);
			goto EXIT;
		}
	}

	/*step for suspend_gesture_cfg when ps is near ts->gesture_enable == 2*/
    if (ts->suspend_gesture_cfg && ts->black_gesture_support && ts->gesture_enable == 2) {
		ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, true);
		operate_mode_switch(ts);
		goto EXIT;
	}

	/*step8:skip suspend operate only when gesture_enable is 0*/
	if (ts->skip_suspend_operate && (!ts->gesture_enable)) {
		goto EXIT;
	}

	/*step9:switch mode to sleep*/
	ret = ts->ts_ops->mode_switch(ts->chip_data, MODE_SLEEP, true);

	if (ret < 0) {
		TP_INFO(ts->tp_index, "%s, Touchpanel operate mode switch failed\n", __func__);
	}

EXIT:
	sec_ts_pinctrl_configure(&ts->hw_res, false);
	if (ts->health_monitor_support) {
		tp_healthinfo_report(&ts->monitor_data, HEALTH_SUSPEND, &start_time);
	}

	TP_INFO(ts->tp_index, "%s: end.\n", __func__);
	mutex_unlock(&ts->mutex);
	return 0;
}

/**
 * touchpanel_ts_suspend - touchpanel resume function
 * @dev: i2c_client->dev using to get touchpanel_data resource
 *
 * resume function bind to LCD on/off status, this fuction start thread to speedup screen on flow.
 * Do not care the result: Return void type
 */
void tp_resume(struct device *dev)
{
	struct touchpanel_data *ts = dev_get_drvdata(dev);

	TP_INFO(ts->tp_index, "%s start.\n", __func__);

	if (!ts->is_suspended) {
		TP_INFO(ts->tp_index, "%s: do not resume twice.\n", __func__);
		goto NO_NEED_RESUME;
	}

	ts->is_suspended = 0;
	ts->suspend_state = TP_RESUME_COMPLETE;
	ts->disable_gesture_ctrl = false;
	if (ts->loading_fw) {
		goto NO_NEED_RESUME;
	}

	if (!ts->resume_no_freeirq) {
		/*free irq at first*/
		if (!(ts->tp_ic_type == TYPE_TDDI_TCM && ts->is_noflash_ic)) {
			free_irq(ts->irq, ts);
		}
	}

	if (ts->tp_ic_type == TYPE_TDDI_TCM) {
		if (ts->ts_ops->reinit_device) {
			ts->ts_ops->reinit_device(ts->chip_data);
		}
	}

	if (ts->kernel_grip_support) {
		if (ts->grip_info) {
			kernel_grip_reset(ts->grip_info);

		} else {
			ts->grip_info = kernel_grip_init(ts->dev);
			init_kernel_grip_proc(ts->prEntry_tp, ts->grip_info);
		}
	}

	queue_work(ts->speedup_resume_wq, &ts->speed_up_work);
	return;

NO_NEED_RESUME:
	ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;
}

/**
 * speedup_resume - speedup resume thread process
 * @work: work struct using for this thread
 *
 * do actully resume function
 * Do not care the result: Return void type
 */
static void speedup_resume(struct work_struct *work)
{
	int ret = 0;
	u64 start_time = 0;
	struct specific_resume_data specific_resume_data;
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     speed_up_work);

	TP_INFO(ts->tp_index, "%s is called\n", __func__);

	/*step1: get mutex for locking i2c acess flow*/
	mutex_lock(&ts->mutex);

	if (ts->health_monitor_support) {
		reset_healthinfo_time_counter(&start_time);
	}

	/*step2:before Resume clear All of touch/key event Reset some flag to default satus*/
	tp_btnkey_release(ts);
	tp_touch_release(ts);

	if (!(ts->tp_ic_type == TYPE_TDDI_TCM && ts->is_noflash_ic)) {
		if (ts->int_mode == UNBANNABLE) {
			tp_register_irq_func(ts);
		}
	}

	if (!ts->gesture_enable) {
		sec_ts_pinctrl_configure(&ts->hw_res, true);
	}

	/*step3:Reset IC && switch work mode, ft8006 is reset by lcd, no more reset needed*/
	if (!ts->skip_reset_in_resume && !ts->fp_info.touch_state) {
		if (!ts->lcd_trigger_load_tp_fw_support) {
			ts->ts_ops->reset(ts->chip_data);
		}
	}

	if (ts->ts_ops->specific_resume_operate && !ts->fp_info.touch_state) {
		specific_resume_data.suspend_state = ts->suspend_state;
		specific_resume_data.in_test_process = ts->in_test_process;
        ret =  ts->ts_ops->specific_resume_operate(ts->chip_data, &specific_resume_data);
        if(ret < 0)
            goto EXIT;
    }


	operate_mode_switch(ts);

	if (ts->esd_handle_support) {
		esd_handle_switch(&ts->esd_info, true);
	}

	/*step6:Request irq again*/
	if (!(ts->tp_ic_type == TYPE_TDDI_TCM && ts->is_noflash_ic)) {
		if (ts->int_mode == BANNABLE) {
			tp_register_irq_func(ts);
		}
	}

EXIT:
	ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;

	if (ts->health_monitor_support) {
		tp_healthinfo_report(&ts->monitor_data, HEALTH_RESUME, &start_time);
	}

	/*step7:Unlock  && exit*/
	TP_INFO(ts->tp_index, "%s: end!\n", __func__);
	mutex_unlock(&ts->mutex);
}


static void lcd_off_early_event(struct touchpanel_data *ts)
{

    ts->suspend_state = TP_SUSPEND_EARLY_EVENT;      //set suspend_resume_state

    if (ts->esd_handle_support && ts->is_incell_panel && (ts->tp_suspend_order == LCD_TP_SUSPEND)) {
		esd_handle_switch(&ts->esd_info, false);
		/*incell panel need cancel esd early*/
	}

	if (ts->tp_suspend_order == TP_LCD_SUSPEND) {
		tp_suspend(ts->dev);

	} else if (ts->tp_suspend_order == LCD_TP_SUSPEND) {
		if (!ts->gesture_enable && ts->is_incell_panel) {
			disable_irq_nosync(ts->irq);
		}
	}
};

static void lcd_off_event(struct touchpanel_data *ts)
{
	if (ts->tp_suspend_order == TP_LCD_SUSPEND) {
	} else if (ts->tp_suspend_order == LCD_TP_SUSPEND) {
		tp_suspend(ts->dev);
	}
};

static void lcd_on_early_event(struct touchpanel_data *ts)
{
	ts->suspend_state = TP_RESUME_EARLY_EVENT;        /*set suspend_resume_state*/

	if (ts->tp_resume_order == TP_LCD_RESUME) {
		tp_resume(ts->dev);

	} else if (ts->tp_resume_order == LCD_TP_RESUME) {
		if (!(ts->tp_ic_type == TYPE_TDDI_TCM && ts->is_noflash_ic)) {
			disable_irq_nosync(ts->irq);
		}
	}
};

static void lcd_on_event(struct touchpanel_data *ts)
{
	if (ts->tp_resume_order == TP_LCD_RESUME) {
	} else if (ts->tp_resume_order == LCD_TP_RESUME) {
		tp_resume(ts->dev);

		if (!(ts->tp_ic_type == TYPE_TDDI_TCM && ts->is_noflash_ic)) {
			enable_irq(ts->irq);
		}
	}
};

static void lcd_other_event(int *blank, struct touchpanel_data *ts)
{
	if (*blank == LCD_CTL_TP_LOAD_FW) {
		lcd_tp_load_fw(ts->tp_index);
	} else if (*blank == LCD_CTL_RST_ON) {
		tp_control_reset_gpio(1, ts->tp_index);
	} else if (*blank == LCD_CTL_RST_OFF) {
		tp_control_reset_gpio(0, ts->tp_index);
	} else if (*blank == LCD_CTL_TP_FTM) {
		tp_ftm_extra(ts->tp_index);
	} else if (*blank == LCD_CTL_CS_ON) {
		tp_control_cs_gpio(1, ts->tp_index);
	} else if (*blank == LCD_CTL_CS_OFF) {
		tp_control_cs_gpio(0, ts->tp_index);
	}

};

#if defined(CONFIG_FB) || defined(CONFIG_DRM_MSM)

static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
    int *blank;
#ifdef CONFIG_DRM_MSM
    struct msm_drm_notifier *evdata = data;
#else
    struct fb_event *evdata = data;
#endif

    struct touchpanel_data *ts = container_of(self, struct touchpanel_data, fb_notif);

    //to aviod some kernel bug (at fbmem.c some local veriable are not initialized)
#ifdef CONFIG_DRM_MSM
    if(event != MSM_DRM_EARLY_EVENT_BLANK && event != MSM_DRM_EVENT_BLANK)
#else
    if(event != FB_EARLY_EVENT_BLANK && event != FB_EVENT_BLANK)
#endif
        return 0;

    if (evdata && evdata->data && ts && ts->chip_data) {
        blank = evdata->data;
        TP_INFO(ts->tp_index, "%s: event = %ld, blank = %d\n", __func__, event, *blank);
#ifdef CONFIG_DRM_MSM
        if (*blank == MSM_DRM_BLANK_POWERDOWN) { //suspend
            if (event == MSM_DRM_EARLY_EVENT_BLANK) {    //early event
#else
        if (*blank == FB_BLANK_POWERDOWN) { //suspend
            if (event == FB_EARLY_EVENT_BLANK) {    //early event
#endif
                if (ts->speedup_resume_wq)
                    flush_workqueue(ts->speedup_resume_wq);//wait speedup_resume_wq done
                lcd_off_early_event(ts);
#ifdef CONFIG_DRM_MSM
            } else if (event == MSM_DRM_EVENT_BLANK) {   //event
#else
            } else if (event == FB_EVENT_BLANK) {   //event
#endif
                lcd_off_event(ts);
            }
#ifdef CONFIG_DRM_MSM
        } else if (*blank == MSM_DRM_BLANK_UNBLANK) { //resume
            if (event == MSM_DRM_EARLY_EVENT_BLANK) {    //early event
#else
        } else if (*blank == FB_BLANK_UNBLANK ) { //resume
            if (event == FB_EARLY_EVENT_BLANK) {    //early event
#endif
                lcd_on_early_event(ts);

#ifdef CONFIG_DRM_MSM
            } else if (event == MSM_DRM_EVENT_BLANK) {   //event
#else
            } else if (event == FB_EVENT_BLANK) {   //event
#endif
                lcd_on_event(ts);
            }
        }
    }

    return 0;
}
#endif

static int fb_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{

	int *blank;
	struct drm_panel_notifier *evdata = data;

	struct touchpanel_data *ts = container_of(self, struct touchpanel_data, fb_drm_notif);

	//to aviod some kernel bug (at fbmem.c some local veriable are not initialized)
	if(event != DRM_PANEL_EARLY_EVENT_BLANK && event != DRM_PANEL_EVENT_BLANK)
		return 0;

	if (evdata && evdata->data && ts && ts->chip_data) {
		blank = evdata->data;
		if (*blank == 4) { //DRM_PANEL_BLANK_POWERDOWN_CUST //suspend
			TP_INFO(ts->tp_index, "%s: event = %ld, blank = %d\n", __func__, event, *blank);
			if (event == DRM_PANEL_EARLY_EVENT_BLANK) {    //early event
				if (ts->speedup_resume_wq)
				flush_workqueue(ts->speedup_resume_wq);//wait speedup_resume_wq done
				lcd_off_early_event(ts);
			} else if (event == DRM_PANEL_EVENT_BLANK) {   //event
				lcd_off_event(ts);
			}
		} else if (*blank == 8) { //resume
			TP_INFO(ts->tp_index, "%s: event = %ld, blank = %d\n", __func__, event, *blank);
			if (event == DRM_PANEL_EARLY_EVENT_BLANK) {    //early event
				lcd_on_early_event(ts);
			} else if (event == DRM_PANEL_EVENT_BLANK) {   //event
				lcd_on_event(ts);
			}
		} else if (*blank == LCD_CTL_CS_ON || *blank == LCD_CTL_CS_OFF ||
			*blank == LCD_CTL_TP_LOAD_FW) {
			if (event == DRM_PANEL_EVENT_BLANK) {
				lcd_other_event(blank, ts);
			}
		}
	}

	return 0;
}

/*
 * tp_shutdown - touchpanel shutdown function
 * @ts: ts using to get touchpanel_data resource
 * shutdown function is called when power off or reboot
 * Returning void
 */
void tp_shutdown(struct touchpanel_data *ts)
{
	if (!ts) {
		return;
	}

	/*step1 :free the hw resource*/
	ts->ts_ops->power_control(ts->chip_data, false);
}
EXPORT_SYMBOL(tp_shutdown);

/*
 * tp_pm_suspend - touchpanel pm suspend function
 * @ts: ts using to get touchpanel_data resource
 * suspend function is called when system go to sleep
 * Returning void
 */
void tp_pm_suspend(struct touchpanel_data *ts)
{
	if (!ts) {
		return;
	}

	ts->bus_ready = false;

	if (TP_ALL_GESTURE_SUPPORT) {
		if (TP_ALL_GESTURE_ENABLE) {
			/*enable gpio wake system through interrupt*/
			pr_err("[TP]: %s enable_irq_wake\n", __func__);
			enable_irq_wake(ts->irq);
			return;
		}
	}

	disable_irq(ts->irq);
}
EXPORT_SYMBOL(tp_pm_suspend);

/*
 * tp_pm_resume - touchpanel pm resume function
 * @ts: ts using to get touchpanel_data resource
 * resume function is called when system wake up
 * Returning void
 */
void tp_pm_resume(struct touchpanel_data *ts)
{
	if (!ts) {
		return;
	}

	if (TP_ALL_GESTURE_SUPPORT) {
		if (TP_ALL_GESTURE_ENABLE) {
			/*disable gpio wake system through intterrupt*/
			disable_irq_wake(ts->irq);
			goto OUT;
		}
	}

	enable_irq(ts->irq);

OUT:
	ts->bus_ready = true;

	if ((ts->black_gesture_support || ts->fingerprint_underscreen_support)) {
        if ((ts->gesture_enable == 1 || ts->fp_enable == 1)) {
			wake_up_interruptible(&ts->wait);
		}
	}
}
EXPORT_SYMBOL(tp_pm_resume);

void clear_view_touchdown_flag(unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;

	if (tp_index >= TP_SUPPORT_MAX) {
		return;
	}

	ts = g_tp[tp_index];

	if (ts) {
		ts->view_area_touched = 0;
	}
}
EXPORT_SYMBOL(clear_view_touchdown_flag);

static oem_verified_boot_state oem_verifiedbootstate = OEM_VERIFIED_BOOT_STATE_LOCKED;
bool is_oem_unlocked(void)
{
    return (oem_verifiedbootstate == OEM_VERIFIED_BOOT_STATE_UNLOCKED);
}
EXPORT_SYMBOL(is_oem_unlocked);

int __init get_oem_verified_boot_state(void)
{
#if IS_BUILTIN(CONFIG_TOUCHPANEL_OPLUS)
    if (strstr(saved_command_line, "androidboot.verifiedbootstate=orange")) {
        oem_verifiedbootstate = OEM_VERIFIED_BOOT_STATE_UNLOCKED;
    } else {
        oem_verifiedbootstate = OEM_VERIFIED_BOOT_STATE_LOCKED;
    }
#endif
    return 0;
}
EXPORT_SYMBOL(get_oem_verified_boot_state);

/*******Part4:Extern Function  Area********************************/

/*
 * check_touchirq_triggered--used for stop system going sleep when touch irq is triggered
 * 1 if irq triggered, otherwise is 0
*/
int check_touchirq_triggered(unsigned int tp_index)
{
    int value = -1;
    struct touchpanel_data *ts = NULL;

    if (tp_index >= TP_SUPPORT_MAX)
        return 0;

    ts = g_tp[tp_index];

    if (!ts) {
        return 0;
    }
    if ((1 != (ts->gesture_enable & 0x01)) && (0 == ts->fp_enable)) {
        return 0;
    }

    value = gpio_get_value(ts->hw_res.irq_gpio);
    if ((0 == value) && (ts->irq_flags & IRQF_TRIGGER_LOW)) {
        TP_INFO(ts->tp_index, "touch irq is triggered.\n");
        return 1; //means irq is triggered
    }
    if ((1 == value) && (ts->irq_flags & IRQF_TRIGGER_HIGH)) {
        TP_INFO(ts->tp_index, "touch irq is triggered.\n");
        return 1; //means irq is triggered
    }

    return 0;
}
EXPORT_SYMBOL(check_touchirq_triggered);

static void lcd_trigger_load_tp_fw(struct work_struct *work)
{
	struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
				     lcd_trigger_load_tp_fw_work);
	static bool is_running = false;
	u64 start_time = 0;

	if (ts->lcd_trigger_load_tp_fw_support) {
		if (is_running) {
			TP_DEBUG(ts->tp_index, "%s is running, can not repeat\n", __func__);

		} else {
			TP_DEBUG(ts->tp_index, "%s start\n", __func__);

			if (ts->health_monitor_support) {
				reset_healthinfo_time_counter(&start_time);
			}

			is_running = true;
			mutex_lock(&ts->mutex);
			ts->ts_ops->reset(ts->chip_data);
			mutex_unlock(&ts->mutex);
			is_running = false;

			if (ts->health_monitor_support) {
				tp_healthinfo_report(&ts->monitor_data, HEALTH_FW_UPDATE_COST, &start_time);
			}
		}
	}
}

void lcd_tp_load_fw(unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;

	if (tp_index >= TP_SUPPORT_MAX) {
		return;
	}

	ts = g_tp[tp_index];

	if (!ts) {
		return;
	}

	TP_DEBUG(ts->tp_index, "%s\n", __func__);

	if (ts->irq_trigger_hdl_support) {
		free_irq(ts->irq, ts);
		tp_register_irq_func(ts);

	} else if (ts->lcd_trigger_load_tp_fw_support) {
		ts->disable_gesture_ctrl = true;

		if (ts->ts_ops) {
			if (ts->ts_ops->tp_queue_work_prepare) {
				mutex_lock(&ts->mutex);
				ts->ts_ops->tp_queue_work_prepare(ts->chip_data);
				mutex_unlock(&ts->mutex);
			}
		}

		queue_work(ts->lcd_trigger_load_tp_fw_wq, &(ts->lcd_trigger_load_tp_fw_work));
	}
}
EXPORT_SYMBOL(lcd_tp_load_fw);

/**
 * tp_gesture_enable_flag -   expose gesture control status for other module.
 * Return gesture_enable status.
 */
static int tp_gesture_enable_flag(unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;
	if (tp_index >= TP_SUPPORT_MAX) {
		return LCD_POWER_OFF;
	}

	ts = g_tp[tp_index];

	if (!ts || !ts->is_incell_panel) {
		return LCD_POWER_OFF;
	}

	TP_DEBUG(ts->tp_index, "gesture_enable is %d\n", ts->gesture_enable);

	return (ts->gesture_enable > 0) ? LCD_POWER_ON : LCD_POWER_OFF;
}

/*
*Interface for lcd to control reset pin
*/
int tp_control_reset_gpio(bool enable, unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;

	if (tp_index >= TP_SUPPORT_MAX) {
		return 0;
	}

	ts = g_tp[tp_index];

	if (!ts) {
		return 0;
	}

	if (gpio_is_valid(ts->hw_res.reset_gpio)) {
		if (ts->ts_ops->reset_gpio_control) {
			ts->ts_ops->reset_gpio_control(ts->chip_data, enable);
		}
	}

	return 0;
}
EXPORT_SYMBOL(tp_control_reset_gpio);

/*
*Interface for tp in ftm mode
*/
void tp_ftm_extra(unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;

	if (tp_index >= TP_SUPPORT_MAX) {
		return;
	}

	ts = g_tp[tp_index];

	if (!ts) {
		return;
	}

	if (ts->ts_ops) {
		if (ts->ts_ops->ftm_process_extra) {
			ts->ts_ops->ftm_process_extra(ts->chip_data);
		}
	}
	return;
}
EXPORT_SYMBOL(tp_ftm_extra);
static int tp_control_cs_gpio(bool enable, unsigned int tp_index)
{
	struct touchpanel_data *ts = NULL;
	int rc = 0;

	if (tp_index >= TP_SUPPORT_MAX) {
		return 0;
	}
	ts = g_tp[tp_index];
	if (!ts) {
		return 0;
	}

	if (gpio_is_valid(ts->hw_res.cs_gpio)) {
		rc = gpio_direction_output(ts->hw_res.cs_gpio, enable);
		if (rc) {
			TPD_INFO("unable to set dir for cs_gpio rc=%d", rc);
		}
		gpio_set_value(ts->hw_res.cs_gpio, enable);
		TPD_DEBUG("%s:set cs %d\n", __func__, enable);
	}

	return 0;
}

void sec_ts_pinctrl_configure(struct hw_resource *hw_res, bool enable)
{
	int ret;

	if (enable) {
		if (hw_res->pinctrl) {
			ret = pinctrl_select_state(hw_res->pinctrl, hw_res->active);
			if (ret)
				TPD_INFO("%s could not set active pinstate", __func__);
		}
	} else {
		if (hw_res->pinctrl) {
			ret = pinctrl_select_state(hw_res->pinctrl, hw_res->suspend);
			if (ret)
				TPD_INFO("%s could not set suspend pinstate", __func__);
		}
	}
}

MODULE_DESCRIPTION("Touchscreen common Driver");
MODULE_LICENSE("GPL");
