// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Unisoc Inc.
 */

#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <video/of_display_timing.h>
#include <video/mipi_display.h>
#include <video/videomode.h>
#include <linux/notifier.h>

#include <drm/drm_atomic_helper.h>
#include <linux/reboot.h>

#include "sprd_crtc.h"
#include "sprd_dpu.h"
#include "sprd_dsi_panel.h"
#include "dsi/sprd_dsi_api.h"
#include "sysfs/sysfs_display.h"
#include "oplus_display_private_api.h"

#define SPRD_MIPI_DSI_FMT_DSC 0xff

#define host_to_dsi(host) \
	container_of(host, struct sprd_dsi, host)
	
/*add by licheng@huaqin.com for lcm bias set 2021.12.1*/
#define LCM_BIAS_MAXVAL 3

/*add by licheng@huaqin.com for lcm bias set 2021.12.1*/
static bool first_turnoff_screen = true;
static u8 bias_lcd[LCM_BIAS_MAXVAL] = {0x0f,0x0f,0x03};

int sprd_oled_set_brightness(struct backlight_device *bdev);
static int last_brightness = 0;

int touch_black_test = 0;
EXPORT_SYMBOL(touch_black_test);
unsigned long tp_gesture = 0;
EXPORT_SYMBOL(tp_gesture);

extern int set_lcm_bias_voltage_parameter(u8 *val ,int size);
extern int set_lcm_bias_voltage_for_sprd(void);

extern int oplus_display_private_api_init(void);
extern void oplus_display_private_api_exit(void);

/* liusiyu silence mode */
const char *bootmode="normal";;
/* liusiyu silence mode */

//extern void fts_tp_reset_pin(int enable);


static BLOCKING_NOTIFIER_HEAD(esd_tp_reset_notifier_list);
int esd_tp_reset_notifier_register(struct notifier_block *nb)
{
        return blocking_notifier_chain_register(&esd_tp_reset_notifier_list, nb);
}
EXPORT_SYMBOL(esd_tp_reset_notifier_register);

int esd_tp_reset_notifier_unregister(struct notifier_block *nb)
{
        return blocking_notifier_chain_unregister(&esd_tp_reset_notifier_list, nb);
}
EXPORT_SYMBOL(esd_tp_reset_notifier_unregister);

int esd_tp_reset_notifier_call_chain(unsigned long val,void *v)
{
        return blocking_notifier_call_chain(&esd_tp_reset_notifier_list,val, v);
}
EXPORT_SYMBOL(esd_tp_reset_notifier_call_chain);

static inline struct sprd_panel *to_sprd_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sprd_panel, base);
}

static int sprd_panel_send_cmds(struct mipi_dsi_device *dsi,
				const void *data, int size)
{
	struct sprd_panel *panel;
	const struct dsi_cmd_desc *cmds = data;
	u16 len;

	if ((cmds == NULL) || (dsi == NULL))
		return -EINVAL;

	panel = mipi_dsi_get_drvdata(dsi);

	while (size > 0) {
		len = (cmds->wc_h << 8) | cmds->wc_l;

		if (panel->info.use_dcs)
			mipi_dsi_dcs_write_buffer(dsi, cmds->payload, len);
		else
			mipi_dsi_generic_write(dsi, cmds->payload, len);

		if (cmds->wait)
			mdelay(cmds->wait);
		cmds = (const struct dsi_cmd_desc *)(cmds->payload + len);
		size -= (len + 4);
	}

	return 0;
}

static int sprd_panel_unprepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct gpio_timing *timing;
	int items, i;

	DRM_INFO("%s()\n", __func__);
	
	/*add by licheng@huaqin.com for lcm bias set 2021.12.1*/
	if(first_turnoff_screen){
		set_lcm_bias_voltage_parameter(bias_lcd,LCM_BIAS_MAXVAL);
		first_turnoff_screen = false;
	}

	DRM_INFO("%s(),touch_black_test or tp_gesture = 1 won't turn off power.touch_black_test = %d ,tp_gesture = %d\n", __func__,touch_black_test,tp_gesture);
	if(touch_black_test != 1 && tp_gesture != 1){

		if (panel->info.reset_gpio) {
			items = panel->info.rst_off_seq.items;
			timing = panel->info.rst_off_seq.timing;
			for (i = 0; i < items; i++) {
				gpiod_direction_output(panel->info.reset_gpio,
							timing[i].level);
				mdelay(timing[i].delay);
			}
		}

		if (panel->info.avee_gpio) {
			gpiod_direction_output(panel->info.avee_gpio, 0);
			mdelay(panel->info.power_gpio_delay);
		}

		if (panel->info.avdd_gpio) {
			gpiod_direction_output(panel->info.avdd_gpio, 0);
			mdelay(5);
		}

		regulator_disable(panel->supply);
	} else {
		DRM_ERROR("%s(), not turn off lcd power touch_black_test = %d ,tp_gesture = %d\n", __func__,touch_black_test,tp_gesture);
	}
	

	return 0;
}

static int sprd_panel_prepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct gpio_timing *timing;
	int items, i, ret;

	DRM_INFO("%s()\n", __func__);

	ret = regulator_enable(panel->supply);
	if (ret < 0)
		DRM_ERROR("enable lcd regulator failed\n");
	
	if(strstr(panel->lcd_name, "NT36672C")) {
		if (panel->info.avdd_gpio) {
			gpiod_direction_output(panel->info.avdd_gpio, 1);
			mdelay(panel->info.power_gpio_delay);
		}

		if (panel->info.avee_gpio) {
			gpiod_direction_output(panel->info.avee_gpio, 1);
			mdelay(15);
		}

		if (panel->info.reset_gpio) {
			items = panel->info.rst_on_seq.items;
			timing = panel->info.rst_on_seq.timing;
			for (i = 0; i < items; i++) {
				gpiod_direction_output(panel->info.reset_gpio,
							timing[i].level);
				mdelay(timing[i].delay);
			}
		}
	}
    else if (strstr(panel->lcd_name, "lcd_ft8006s")){
		
		if (panel->info.reset_gpio) {
			esd_tp_reset_notifier_call_chain(FTS_TP_RESET_0,NULL);
			//fts_tp_reset_pin(0);
			mdelay(7);
			gpiod_direction_output(panel->info.reset_gpio,0);
			mdelay(4);
		}
        
		if (panel->info.avdd_gpio) {
			gpiod_direction_output(panel->info.avdd_gpio, 1);
			mdelay(panel->info.power_gpio_delay);
        }

        if (panel->info.avee_gpio) {
			gpiod_direction_output(panel->info.avee_gpio, 1);
			mdelay(5);
        }        
        
        if (panel->info.reset_gpio) {
			gpiod_direction_output(panel->info.reset_gpio,1);
			mdelay(3);
			gpiod_direction_output(panel->info.reset_gpio,0);
			mdelay(5);
			esd_tp_reset_notifier_call_chain(FTS_TP_RESET_1,NULL);
			//fts_tp_reset_pin(1);
			mdelay(4);
			gpiod_direction_output(panel->info.reset_gpio,1);
			mdelay(36);
        }
    }
	else if(strstr(panel->lcd_name, "icnl9911C")) {
		if (panel->info.avdd_gpio) {
			gpiod_direction_output(panel->info.avdd_gpio, 1);
			mdelay(panel->info.power_gpio_delay);
		}

		if (panel->info.avee_gpio) {
			gpiod_direction_output(panel->info.avee_gpio, 1);
			mdelay(15);
		}

		if (panel->info.reset_gpio) {
			items = panel->info.rst_on_seq.items;
			timing = panel->info.rst_on_seq.timing;
			for (i = 0; i < items; i++) {
				gpiod_direction_output(panel->info.reset_gpio,
						timing[i].level);
				mdelay(timing[i].delay);
			}
		}
	}
	else {
		if (panel->info.avdd_gpio) {
			gpiod_direction_output(panel->info.avdd_gpio, 1);
			mdelay(panel->info.power_gpio_delay);
		}

		if (panel->info.avee_gpio) {
			gpiod_direction_output(panel->info.avee_gpio, 1);
			mdelay(5);
		}

		if (panel->info.reset_gpio) {
			items = panel->info.rst_on_seq.items;
			timing = panel->info.rst_on_seq.timing;
			for (i = 0; i < items; i++) {
				gpiod_direction_output(panel->info.reset_gpio,
							timing[i].level);
				mdelay(timing[i].delay);
			}
		}
	}

	set_lcm_bias_voltage_for_sprd();

	return 0;
}

static int sprd_panel_disable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	DRM_INFO("%s()\n", __func__);

	/*
	 * FIXME:
	 * The cancel work should be executed before DPU stop,
	 * otherwise the esd check will be failed if the DPU
	 * stopped in video mode and the DSI has not change to
	 * CMD mode yet. Since there is no VBLANK timing for
	 * LP cmd transmission.
	 */
	if (panel->esd_work_pending) {
		cancel_delayed_work_sync(&panel->esd_work);
		panel->esd_work_pending = false;
	}

	mutex_lock(&panel->lock);
	if (panel->backlight) {
		panel->backlight->props.power = FB_BLANK_POWERDOWN;
		panel->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(panel->backlight);
	}

	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_SLEEP_IN],
			     panel->info.cmds_len[CMD_CODE_SLEEP_IN]);

		if(strstr(panel->lcd_name, "lcd_ft8006s"))
			mdelay(30);
	panel->enabled = false;
	mutex_unlock(&panel->lock);

	return 0;
}

static int sprd_panel_enable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	DRM_INFO("%s()\n", __func__);

	mutex_lock(&panel->lock);
	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_INIT],
			     panel->info.cmds_len[CMD_CODE_INIT]);

	if (panel->backlight) {
		panel->backlight->props.power = FB_BLANK_UNBLANK;
		panel->backlight->props.state &= ~BL_CORE_FBBLANK;
		backlight_update_status(panel->backlight);
	}

	if (panel->info.esd_check_en) {
		schedule_delayed_work(&panel->esd_work,
				      msecs_to_jiffies(1000));
		panel->esd_work_pending = true;
		panel->esd_work_backup = false;
	}

	panel->enabled = true;
	mutex_unlock(&panel->lock);

	return 0;
}

static struct sprd_panel *p;

int sprd_panel_cabc(unsigned int level)
{

	DRM_INFO("%s()\n", __func__);
	mutex_lock(&p->lock);
	if (level == 1) {
		sprd_panel_send_cmds(p->slave,
			     p->info.cmds[CMD_CODE_CABC_UI],
			     p->info.cmds_len[CMD_CODE_CABC_UI]);
    } else if (level == 2) {
		sprd_panel_send_cmds(p->slave,
			     p->info.cmds[CMD_CODE_CABC_STILL_IMAGE],
			     p->info.cmds_len[CMD_CODE_CABC_STILL_IMAGE]);
    } else if (level == 3) {
		sprd_panel_send_cmds(p->slave,
			     p->info.cmds[CMD_CODE_CABC_VIDEO],
			     p->info.cmds_len[CMD_CODE_CABC_VIDEO]);
    } else {
		sprd_panel_send_cmds(p->slave,
			     p->info.cmds[CMD_CODE_CABC_OFF],
			     p->info.cmds_len[CMD_CODE_CABC_OFF]);
    }
	p->enabled = true;
	mutex_unlock(&p->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(sprd_panel_cabc);


static int sprd_panel_get_modes(struct drm_panel *p)
{
	struct drm_display_mode *mode;
	struct sprd_panel *panel = to_sprd_panel(p);
	struct sprd_dsi *dsi =
		container_of(p->connector, struct sprd_dsi, connector);
	struct device_node *np = panel->slave->dev.of_node;
	u32 surface_width = 0, surface_height = 0;
	int i, mode_count = 0;

	DRM_INFO("%s()\n", __func__);
	mode = drm_mode_duplicate(p->drm, &panel->info.mode);
	if (!mode) {
		DRM_ERROR("failed to alloc mode %s\n", panel->info.mode.name);
		return 0;
	}
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(p->connector, mode);
	mode_count++;

	for (i = 0; i < panel->info.num_buildin_modes - 1; i++) {
		mode = drm_mode_duplicate(p->drm,
			&(panel->info.buildin_modes[i]));
		if (!mode) {
			DRM_ERROR("failed to alloc mode %s\n",
				panel->info.buildin_modes[i].name);
			return 0;
		}

		mode->type = DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(p->connector, mode);
		mode_count++;
	}

	of_property_read_u32(np, "sprd,surface-width", &surface_width);
	of_property_read_u32(np, "sprd,surface-height", &surface_height);
	if (surface_width && surface_height) {
		struct videomode vm = {};

		vm.hactive = surface_width;
		vm.vactive = surface_height;
		vm.pixelclock = surface_width * surface_height * 60;

		mode = drm_mode_create(p->drm);

		mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_USERDEF;
		mode->vrefresh = 60;
		drm_display_mode_from_videomode(&vm, mode);
		drm_mode_probed_add(p->connector, mode);
		mode_count++;
		dsi->ctx.surface_mode = true;
	}

	p->connector->display_info.width_mm = panel->info.mode.width_mm;
	p->connector->display_info.height_mm = panel->info.mode.height_mm;

	return mode_count;
}

static const struct drm_panel_funcs sprd_panel_funcs = {
	.get_modes = sprd_panel_get_modes,
	.enable = sprd_panel_enable,
	.disable = sprd_panel_disable,
	.prepare = sprd_panel_prepare,
	.unprepare = sprd_panel_unprepare,
	//.cabc = sprd_panel_cabc,
};

static int sprd_panel_esd_check(struct sprd_panel *panel)
{
	struct mipi_dsi_host *host = panel->slave->host;
	struct sprd_dsi *dsi = host_to_dsi(host);
	struct drm_connector *connector = &dsi->connector;
	struct panel_info *info = &panel->info;
	struct sprd_dpu *dpu;
	u8 read_val = 0;

	if (connector->dpms != DRM_MODE_DPMS_ON) {
		DRM_INFO("skip esd when display suspend\n");
		return 0;
	}

	if (!dsi->ctx.enabled) {
		DRM_WARN("dsi is not initialized，skip esd check\n");
		return 0;
	}

	mutex_lock(&panel->lock);
	if (!panel->enabled) {
		DRM_INFO("panel is not enabled, skip esd check");
		mutex_unlock(&panel->lock);
		return 0;
	}

	dpu = dsi->dpu;

	mutex_lock(&dpu->ctx.vrr_lock);

	/* FIXME: we should enable HS cmd tx here */
	mipi_dsi_set_maximum_return_packet_size(panel->slave, 1);
	mipi_dsi_dcs_read(panel->slave, info->esd_check_reg,
			  &read_val, 1);

	mutex_unlock(&dpu->ctx.vrr_lock);

	/*
	 * TODO:
	 * Should we support multi-registers check in the future?
	 */
	if (read_val != info->esd_check_val) {
		DRM_ERROR("esd check failed, read value = 0x%02x\n",
			  read_val);
		mutex_unlock(&panel->lock);
		return -EINVAL;
	}
	mutex_unlock(&panel->lock);

	return 0;
}

static int sprd_panel_te_check(struct sprd_panel *panel)
{
	struct mipi_dsi_host *host = panel->slave->host;
	struct sprd_dsi *dsi = host_to_dsi(host);
	struct drm_connector *connector = &dsi->connector;
	struct sprd_dpu *dpu;
	int ret;
	bool irq_occur = false;

	if (connector->dpms != DRM_MODE_DPMS_ON) {
		DRM_INFO("skip esd when display suspend\n");
		return 0;
	}

	if (!dsi->ctx.enabled) {
		DRM_WARN("dsi is not initialized，skip esd check\n");
		return 0;
	}

	dpu = dsi->dpu;

	/* DPU TE irq maybe enabled in kernel */
	if (!dpu->ctx.enabled)
		return 0;

	dpu->ctx.te_check_en = true;

	/* wait for TE interrupt */
	ret = wait_event_interruptible_timeout(dpu->ctx.te_wq,
		dpu->ctx.evt_te, msecs_to_jiffies(500));
	if (!ret) {
		/* double check TE interrupt through dpu_int_raw register */
		if (dpu->core && dpu->core->check_raw_int) {
			down(&dpu->ctx.lock);
			if (dpu->ctx.enabled)
				irq_occur = dpu->core->check_raw_int(&dpu->ctx,
					BIT_DPU_INT_TE);
			up(&dpu->ctx.lock);
			if (!irq_occur) {
				DRM_ERROR("TE esd timeout.\n");
				ret = -ETIMEDOUT;
			} else
				DRM_WARN("TE occur, but isr schedule delay\n");
		} else {
			DRM_ERROR("TE esd timeout.\n");
			ret = -ETIMEDOUT;
		}
	}

	dpu->ctx.te_check_en = false;
	dpu->ctx.evt_te = false;

	return ret < 0 ? ret : 0;
}

static int sprd_panel_mix_check(struct sprd_panel *panel)
{
	int ret;

	ret = sprd_panel_esd_check(panel);
	if (ret) {
		DRM_ERROR("mix check use read reg with error\n");
		return ret;
	}

	ret = sprd_panel_te_check(panel);
	if (ret) {
		DRM_ERROR("mix check use te signal with error\n");
		return ret;
	}

	return 0;
}

static void sprd_panel_esd_work_func(struct work_struct *work)
{
	struct sprd_panel *panel = container_of(work, struct sprd_panel,
						esd_work.work);
	struct panel_info *info = &panel->info;
	struct mipi_dsi_host *host = panel->slave->host;
	struct sprd_dsi *dsi = host_to_dsi(host);
	int ret;

	if ((!dsi->ctx.enabled) || (!panel->enabled)) {
		DRM_ERROR("dsi is suspended, skip esd work\n");
		return;
	}

	if (info->esd_check_mode == ESD_MODE_REG_CHECK)
		ret = sprd_panel_esd_check(panel);
	else if (info->esd_check_mode == ESD_MODE_TE_CHECK)
		ret = sprd_panel_te_check(panel);
	else if (info->esd_check_mode == ESD_MODE_MIX_CHECK)
		ret = sprd_panel_mix_check(panel);
	else {
		DRM_ERROR("unknown esd check mode:%d\n", info->esd_check_mode);
		return;
	}

	if (ret && panel->base.connector && panel->base.connector->encoder) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;

		encoder = panel->base.connector->encoder;
		funcs = encoder->helper_private;
		panel->esd_work_pending = false;

		if (!encoder->crtc || (encoder->crtc->state &&
		    !encoder->crtc->state->active)) {
			DRM_INFO("skip esd recovery during panel suspend\n");
			panel->esd_work_backup = true;
			return;
		}

		DRM_INFO("====== esd recovery start ========\n");
		esd_tp_reset_notifier_call_chain(ESD_EVENT_TP_SUSPEND,NULL);
		funcs->disable(encoder);
		funcs->enable(encoder);
		if(panel->oled_bdev){
			DRM_INFO("======= esd recovery backlight =========\n");
			if(panel->oled_bdev->props.brightness){
				sprd_oled_set_brightness(panel->oled_bdev);
			}else{
				panel->oled_bdev->props.brightness = last_brightness;
				sprd_oled_set_brightness(panel->oled_bdev);
			}
		}
		esd_tp_reset_notifier_call_chain(ESD_EVENT_TP_RESUME,NULL);

		if (!panel->esd_work_pending && panel->enabled)
			schedule_delayed_work(&panel->esd_work,
					msecs_to_jiffies(info->esd_check_period));
		DRM_INFO("======= esd recovery end =========\n");
	} else
		schedule_delayed_work(&panel->esd_work,
			msecs_to_jiffies(info->esd_check_period));
}

static int sprd_panel_gpio_request(struct device *dev,
			struct sprd_panel *panel)
{
	panel->info.avdd_gpio = devm_gpiod_get_optional(dev,
					"avdd", GPIOD_ASIS);
	if (IS_ERR_OR_NULL(panel->info.avdd_gpio))
		DRM_WARN("can't get panel avdd gpio: %ld\n",
				 PTR_ERR(panel->info.avdd_gpio));

	panel->info.avee_gpio = devm_gpiod_get_optional(dev,
					"avee", GPIOD_ASIS);
	if (IS_ERR_OR_NULL(panel->info.avee_gpio))
		DRM_WARN("can't get panel avee gpio: %ld\n",
				 PTR_ERR(panel->info.avee_gpio));

	panel->info.reset_gpio = devm_gpiod_get_optional(dev,
					"reset", GPIOD_ASIS);
	if (IS_ERR_OR_NULL(panel->info.reset_gpio))
		DRM_WARN("can't get panel reset gpio: %ld\n",
				 PTR_ERR(panel->info.reset_gpio));

	return 0;
}

static int of_parse_reset_seq(struct device_node *np,
				struct panel_info *info)
{
	struct property *prop;
	int bytes, rc;
	u32 *p;

	prop = of_find_property(np, "sprd,reset-on-sequence", &bytes);
	if (!prop) {
		DRM_ERROR("sprd,reset-on-sequence property not found\n");
		return -EINVAL;
	}

	p = kzalloc(bytes, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	rc = of_property_read_u32_array(np, "sprd,reset-on-sequence",
					p, bytes / 4);
	if (rc) {
		DRM_ERROR("parse sprd,reset-on-sequence failed\n");
		kfree(p);
		return rc;
	}

	info->rst_on_seq.items = bytes / 8;
	info->rst_on_seq.timing = (struct gpio_timing *)p;

	prop = of_find_property(np, "sprd,reset-off-sequence", &bytes);
	if (!prop) {
		DRM_ERROR("sprd,reset-off-sequence property not found\n");
		return -EINVAL;
	}

	p = kzalloc(bytes, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	rc = of_property_read_u32_array(np, "sprd,reset-off-sequence",
					p, bytes / 4);
	if (rc) {
		DRM_ERROR("parse sprd,reset-off-sequence failed\n");
		kfree(p);
		return rc;
	}

	info->rst_off_seq.items = bytes / 8;
	info->rst_off_seq.timing = (struct gpio_timing *)p;

	return 0;
}

static int of_parse_buildin_modes(struct panel_info *info,
	struct device_node *lcd_node)
{
	int i, rc, num_timings;
	char timing_check_ct = 0;
	struct device_node *timings_np;

	timings_np = of_get_child_by_name(lcd_node, "display-timings");
	if (!timings_np) {
		DRM_ERROR("%s: can not find display-timings node\n",
			lcd_node->name);
		return -ENODEV;
	}

	num_timings = of_get_child_count(timings_np);
	if (num_timings == 0) {
		/* should never happen, as entry was already found above */
		DRM_ERROR("%s: no timings specified\n", lcd_node->name);
		goto done;
	}

	info->buildin_modes = kzalloc(sizeof(struct drm_display_mode) *
				num_timings, GFP_KERNEL);

	for (i = 0; i < num_timings; i++) {
		rc = of_get_drm_display_mode(lcd_node,
			&info->buildin_modes[i], NULL, i);
		if (rc) {
			DRM_ERROR("get display timing failed\n");
			goto entryfail;
		}

		info->buildin_modes[i].width_mm = info->mode.width_mm;
		info->buildin_modes[i].height_mm = info->mode.height_mm;
		info->mode.vrefresh = drm_mode_vrefresh(&info->buildin_modes[i]);
		info->buildin_modes[i].vrefresh = info->mode.vrefresh;
	}
	info->num_buildin_modes = num_timings;

	if (info->num_buildin_modes > 1) {
		for (i = 1; i < num_timings; i++)
			if (info->buildin_modes[0].htotal
					== info->buildin_modes[i].htotal)
				timing_check_ct++;
		if (timing_check_ct == num_timings - 1)
			vrr_mode = true;
	}
	DRM_INFO("num_timings = %d, vrr_mode = %d\n", num_timings, vrr_mode);

	goto done;

entryfail:
	kfree(info->buildin_modes);
done:
	of_node_put(timings_np);

	return 0;
}

static int of_parse_oled_cmds(struct sprd_oled *oled,
		const void *data, int size)
{
	const struct dsi_cmd_desc *cmds = data;
	struct dsi_cmd_desc *p;
	u16 len;
	int i, total;

	if (cmds == NULL)
		return -EINVAL;

	/*
	 * TODO:
	 * Currently, we only support the same length cmds
	 * for oled brightness level. So we take the first
	 * cmd payload length as all.
	 */
	len = (cmds->wc_h << 8) | cmds->wc_l;
	total =  size / (len + 4);

	p = (struct dsi_cmd_desc *)kzalloc(size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	memcpy(p, cmds, size);
	for (i = 0; i < total; i++) {
		oled->cmds[i] = p;
		p = (struct dsi_cmd_desc *)(p->payload + len);
	}

	oled->cmds_total = total;
	oled->cmd_len = len + 4;

	return 0;
}

unsigned int flag_bl = 0;
EXPORT_SYMBOL_GPL(flag_bl);

int sprd_oled_set_brightness(struct backlight_device *bdev)
{
	int brightness;
	static bool first_mdelay_flag = true;
	struct sprd_oled *oled = bl_get_data(bdev);
	struct sprd_panel *panel = oled->panel;

	mutex_lock(&panel->lock);
	if (!panel->enabled) {
		mutex_unlock(&panel->lock);
		DRM_WARN("oled panel has been powered off\n");
		return 0;
	}
	
	if(strstr(panel->lcd_name,"icnl9911C")||strstr(panel->lcd_name,"ft8006s")||strstr(panel->lcd_name,"hx83102d")){
		if(first_mdelay_flag)
		{
			mdelay(34);
			DRM_WARN("lsy delay34\n");
		}
	}

	brightness = bdev->props.brightness;
	last_brightness = brightness;

	DRM_INFO("%s brightness: %d\n", __func__, brightness);
	flag_bl = brightness;


	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_OLED_REG_LOCK],
			     panel->info.cmds_len[CMD_OLED_REG_LOCK]);

	if (oled->cmds_total == 1) {
		if (oled->cmds[0]->wc_l == 3) {
			if (strstr(panel->lcd_name,"lcd_ft8006s")) {
						oled->cmds[0]->payload[1] = brightness >> 4;
						oled->cmds[0]->payload[2] = brightness & 0xF;
			} else {
			oled->cmds[0]->payload[1] = brightness >> 8;
			oled->cmds[0]->payload[2] = brightness & 0xFF;
			}
		} else
			oled->cmds[0]->payload[1] = brightness;

		sprd_panel_send_cmds(panel->slave,
			     oled->cmds[0],
			     oled->cmd_len);
	} else {
		if (brightness >= oled->cmds_total)
			brightness = oled->cmds_total - 1;

		sprd_panel_send_cmds(panel->slave,
			     oled->cmds[brightness],
			     oled->cmd_len);
	}

	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_OLED_REG_UNLOCK],
			     panel->info.cmds_len[CMD_OLED_REG_UNLOCK]);
				 
	if(0==brightness)
	{
		first_mdelay_flag = true;
		DRM_INFO("first_mdelay_flag = true\n");
	}else{
		first_mdelay_flag = false;
		DRM_INFO("first_mdelay_flag = false\n");
	}


	mutex_unlock(&panel->lock);
	
    if(0==brightness)
    DRM_INFO("liang : send 51 00 \n");
	return 0;
}

static const struct backlight_ops sprd_oled_backlight_ops = {
	.update_status = sprd_oled_set_brightness,
};

static int sprd_oled_backlight_init(struct sprd_panel *panel)
{
	struct sprd_oled *oled;
	struct device_node *oled_node;
	struct panel_info *info = &panel->info;
	const void *p;
	int bytes, rc;
	u32 temp;

	oled_node = of_get_child_by_name(info->of_node,
				"oled-backlight");
	if (!oled_node)
		return 0;

	oled = devm_kzalloc(&panel->dev,
			sizeof(struct sprd_oled), GFP_KERNEL);
	if (!oled)
		return -ENOMEM;

	oled->bdev = devm_backlight_device_register(&panel->dev,
			"sprd_backlight", &panel->dev, oled,
			&sprd_oled_backlight_ops, NULL);
	if (IS_ERR(oled->bdev)) {
		DRM_ERROR("failed to register oled backlight ops\n");
		return PTR_ERR(oled->bdev);
	}
	panel->oled_bdev = oled->bdev;
	p = of_get_property(oled_node, "brightness-levels", &bytes);
	if (p) {
		info->cmds[CMD_OLED_BRIGHTNESS] = p;
		info->cmds_len[CMD_OLED_BRIGHTNESS] = bytes;
	} else
		DRM_ERROR("can't find brightness-levels property\n");

	p = of_get_property(oled_node, "sprd,reg-lock", &bytes);
	if (p) {
		info->cmds[CMD_OLED_REG_LOCK] = p;
		info->cmds_len[CMD_OLED_REG_LOCK] = bytes;
	} else
		DRM_INFO("can't find sprd,reg-lock property\n");

	p = of_get_property(oled_node, "sprd,reg-unlock", &bytes);
	if (p) {
		info->cmds[CMD_OLED_REG_UNLOCK] = p;
		info->cmds_len[CMD_OLED_REG_UNLOCK] = bytes;
	} else
		DRM_INFO("can't find sprd,reg-unlock property\n");

	rc = of_property_read_u32(oled_node, "default-brightness-level", &temp);
	if (!rc)
		oled->bdev->props.brightness = temp;
	else
		oled->bdev->props.brightness = 25;

	rc = of_property_read_u32(oled_node, "sprd,max-level", &temp);
	if (!rc)
		oled->max_level = temp;
	else
		oled->max_level = 255;

	oled->bdev->props.max_brightness = oled->max_level;
	oled->panel = panel;
	of_parse_oled_cmds(oled,
			panel->info.cmds[CMD_OLED_BRIGHTNESS],
			panel->info.cmds_len[CMD_OLED_BRIGHTNESS]);

	DRM_INFO("%s() ok\n", __func__);

	return 0;
}

int sprd_panel_parse_lcddtb(struct device_node *lcd_node,
	struct sprd_panel *panel)
{
	struct panel_info *info = &panel->info;
	int bytes, rc;
	const void *p;
	const char *str;
	u32 val;

	if (!lcd_node) {
		DRM_ERROR("lcd node from dtb is null\n");
		return -ENODEV;
	}
	info->of_node = lcd_node;

	rc = of_property_read_u32(lcd_node, "sprd,dsi-work-mode", &val);
	if (!rc) {
		if (val == SPRD_DSI_MODE_CMD)
			info->mode_flags = 0;
		else if (val == SPRD_DSI_MODE_VIDEO_BURST)
			info->mode_flags = MIPI_DSI_MODE_VIDEO |
					   MIPI_DSI_MODE_VIDEO_BURST;
		else if (val == SPRD_DSI_MODE_VIDEO_SYNC_PULSE)
			info->mode_flags = MIPI_DSI_MODE_VIDEO |
					   MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
		else if (val == SPRD_DSI_MODE_VIDEO_SYNC_EVENT)
			info->mode_flags = MIPI_DSI_MODE_VIDEO;
	} else {
		DRM_ERROR("dsi work mode is not found! use video mode\n");
		info->mode_flags = MIPI_DSI_MODE_VIDEO |
				   MIPI_DSI_MODE_VIDEO_BURST;
	}

	if (of_property_read_bool(lcd_node, "sprd,dsi-non-continuous-clock"))
		info->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;

	rc = of_property_read_u32(lcd_node, "sprd,dsi-lane-number", &val);
	if (!rc)
		info->lanes = val;
	else
		info->lanes = 4;

	rc = of_property_read_string(lcd_node, "sprd,dsi-color-format", &str);
	if (rc)
		info->format = MIPI_DSI_FMT_RGB888;
	else if (!strcmp(str, "rgb888"))
		info->format = MIPI_DSI_FMT_RGB888;
	else if (!strcmp(str, "rgb666"))
		info->format = MIPI_DSI_FMT_RGB666;
	else if (!strcmp(str, "rgb666_packed"))
		info->format = MIPI_DSI_FMT_RGB666_PACKED;
	else if (!strcmp(str, "rgb565"))
		info->format = MIPI_DSI_FMT_RGB565;
	else if (!strcmp(str, "dsc"))
		info->format = SPRD_MIPI_DSI_FMT_DSC;
	else
		DRM_ERROR("dsi-color-format (%s) is not supported\n", str);

	rc = of_property_read_u32(lcd_node, "sprd,phy-bit-clock", &val);
	if (!rc)
		info->hs_rate = val;
	else
		info->hs_rate = 500000;

	rc = of_property_read_u32(lcd_node, "sprd,phy-escape-clock", &val);
	if (!rc)
		info->lp_rate = val > 20000 ? 20000 : val;
	else
		info->lp_rate = 20000;

	rc = of_property_read_u32(lcd_node, "sprd,width-mm", &val);
	if (!rc)
		info->mode.width_mm = val;
	else
		info->mode.width_mm = 68;

	rc = of_property_read_u32(lcd_node, "sprd,height-mm", &val);
	if (!rc)
		info->mode.height_mm = val;
	else
		info->mode.height_mm = 121;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-enable", &val);
	if (!rc)
		info->esd_check_en = val;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-mode", &val);
	if (!rc)
		info->esd_check_mode = val;
	else
		info->esd_check_mode = 1;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-period", &val);
	if (!rc)
		info->esd_check_period = val;
	else
		info->esd_check_period = 1000;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-register", &val);
	if (!rc)
		info->esd_check_reg = val;
	else
		info->esd_check_reg = 0x0A;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-value", &val);
	if (!rc)
		info->esd_check_val = val;
	else
		info->esd_check_val = 0x9C;

        rc = of_property_read_u32(lcd_node, "sprd,power-gpio-delay", &val);
        if (!rc)
                info->power_gpio_delay = val;
        else
                info->power_gpio_delay = 5;

	if (of_property_read_bool(lcd_node, "sprd,use-dcs-write"))
		info->use_dcs = true;
	else
		info->use_dcs = false;

	rc = of_parse_reset_seq(lcd_node, info);
	if (rc)
		DRM_ERROR("parse lcd reset sequence failed\n");
	
	/*add by licheng@huaqin.com for lcm bias set 2021.12.1*/
	p = of_get_property(lcd_node, "sprd,power-i2c-val", &bytes);
	if (p) {
		of_property_read_u8_array(lcd_node,"sprd,power-i2c-val",bias_lcd,bytes);
		DRM_ERROR("sprd,power-i2c-val bias_lcd[0]=0x%02x;bias_lcd[1]=0x%02x;bias_lcd[2]=0x%02x;bytes = %d\n",bias_lcd[0],bias_lcd[1],bias_lcd[2],bytes);
	} else
		DRM_ERROR("can't find sprd,power-i2c-val property\n");
	//endif

	p = of_get_property(lcd_node, "sprd,initial-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_INIT] = p;
		info->cmds_len[CMD_CODE_INIT] = bytes;
	} else
		DRM_ERROR("can't find sprd,init-command property\n");

	p = of_get_property(lcd_node, "sprd,sleep-in-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_IN] = p;
		info->cmds_len[CMD_CODE_SLEEP_IN] = bytes;
	} else
		DRM_ERROR("can't find sprd,sleep-in-command property\n");

	p = of_get_property(lcd_node, "sprd,sleep-out-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_OUT] = p;
		info->cmds_len[CMD_CODE_SLEEP_OUT] = bytes;
	} else
		DRM_ERROR("can't find sprd,sleep-out-command property\n");
	
	p = of_get_property(lcd_node, "sprd,cabc-off-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_CABC_OFF] = p;
		info->cmds_len[CMD_CODE_CABC_OFF] = bytes;
	} else
		DRM_ERROR("can't find sprd,cabc-off-command property\n");

	p = of_get_property(lcd_node, "sprd,cabc-ui-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_CABC_UI] = p;
		info->cmds_len[CMD_CODE_CABC_UI] = bytes;
	} else
		DRM_ERROR("can't find sprd,cabc-ui-command property\n");

	p = of_get_property(lcd_node, "sprd,cabc-still-image-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_CABC_STILL_IMAGE] = p;
		info->cmds_len[CMD_CODE_CABC_STILL_IMAGE] = bytes;
	} else
		DRM_ERROR("can't find sprd,cabc-still-image-command property\n");

	p = of_get_property(lcd_node, "sprd,cabc-video-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_CABC_VIDEO] = p;
		info->cmds_len[CMD_CODE_CABC_VIDEO] = bytes;
	} else
		DRM_ERROR("can't find sprd,cabc-video-command property\n");

	rc = of_get_drm_display_mode(lcd_node, &info->mode, 0,
				     OF_USE_NATIVE_MODE);
	if (rc) {
		DRM_ERROR("get display timing failed\n");
		return rc;
	}

	info->mode.vrefresh = drm_mode_vrefresh(&info->mode);
	of_parse_buildin_modes(info, lcd_node);

	return 0;
}

static int sprd_panel_parse_dt(struct device_node *np, struct sprd_panel *panel)
{
	struct device_node *lcd_node, *cmdline_node;
	const char *cmd_line, *lcd_name_p;
	char lcd_path[60];
	int rc;

	cmdline_node = of_find_node_by_path("/chosen");
	rc = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!rc) {
		lcd_name_p = strstr(cmd_line, "lcd_name=");
		bootmode = strstr(cmd_line, "androidboot.mode=");
		if (lcd_name_p) {
			sscanf(lcd_name_p, "lcd_name=%s", panel->lcd_name);
			DRM_INFO("lcd name: %s,bootmoade:%s\n", panel->lcd_name, bootmode);
		}
	} else {
		DRM_ERROR("can't not parse bootargs property\n");
		return rc;
	}

	sprintf(lcd_path, "/lcds/%s", panel->lcd_name);
	lcd_node = of_find_node_by_path(lcd_path);
	if (!lcd_node) {
		DRM_ERROR("%pOF: could not find %s node\n", np, panel->lcd_name);
		return -ENODEV;
	}
	rc = sprd_panel_parse_lcddtb(lcd_node, panel);
	if (rc)
		return rc;

	return 0;
}

static int sprd_panel_device_create(struct device *parent,
				    struct sprd_panel *panel)
{
	panel->dev.class = display_class;
	panel->dev.parent = parent;
	panel->dev.of_node = panel->info.of_node;
	dev_set_name(&panel->dev, "panel0");
	dev_set_drvdata(&panel->dev, panel);

	return device_register(&panel->dev);
}

static int sprd_panel_panic_event(struct notifier_block *self, unsigned long val, void *reason)
{
	struct sprd_panel *panel = container_of(self, struct sprd_panel, panic_nb);
	DRM_ERROR("%s() satrt.\n", __func__);
	mutex_lock(&panel->lock);
	if (!panel->enabled) {
		DRM_ERROR("%s() return\n", __func__);
		mutex_unlock(&panel->lock);
		return 0;
	}
	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_SLEEP_IN],
			     panel->info.cmds_len[CMD_CODE_SLEEP_IN]);
	mutex_unlock(&panel->lock);
	DRM_ERROR("%s()\n", __func__);
	return 0;
}

static struct notifier_block sprd_panel_panic_event_nb = {
    .notifier_call = sprd_panel_panic_event,
    .priority = INT_MAX,
};

static int sprd_panel_probe(struct mipi_dsi_device *slave)
{
	struct sprd_panel *panel;
	struct device_node *bl_node;
	int ret;

	panel = devm_kzalloc(&slave->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;
	
	p = panel;

	bl_node = of_parse_phandle(slave->dev.of_node,
					"sprd,backlight", 0);
	if (bl_node) {
		panel->backlight = of_find_backlight_by_node(bl_node);
		of_node_put(bl_node);

		if (panel->backlight) {
			panel->backlight->props.state &= ~BL_CORE_FBBLANK;
			panel->backlight->props.power = FB_BLANK_UNBLANK;
			backlight_update_status(panel->backlight);
		} else {
			DRM_WARN("backlight is not ready, panel probe deferred\n");
			return -EPROBE_DEFER;
		}
	} else
		DRM_WARN("backlight node not found\n");

	panel->supply = devm_regulator_get(&slave->dev, "power");
	if (IS_ERR(panel->supply)) {
		if (PTR_ERR(panel->supply) == -EPROBE_DEFER)
			DRM_ERROR("regulator driver not initialized, probe deffer\n");
		else
			DRM_ERROR("can't get regulator: %ld\n", PTR_ERR(panel->supply));

		return PTR_ERR(panel->supply);
	}

	INIT_DELAYED_WORK(&panel->esd_work, sprd_panel_esd_work_func);

	ret = sprd_panel_parse_dt(slave->dev.of_node, panel);
	if (ret) {
		DRM_ERROR("parse panel info failed\n");
		return ret;
	}

	ret = sprd_panel_gpio_request(&slave->dev, panel);
	if (ret) {
		DRM_WARN("gpio is not ready, panel probe deferred\n");
		return -EPROBE_DEFER;
	}

	ret = sprd_panel_device_create(&slave->dev, panel);
	if (ret)
		return ret;

	ret = sprd_oled_backlight_init(panel);
	if (ret)
		return ret;

	panel->base.dev = &panel->dev;
	panel->base.funcs = &sprd_panel_funcs;
	drm_panel_init(&panel->base);

	ret = drm_panel_add(&panel->base);
	if (ret) {
		DRM_ERROR("drm_panel_add() failed\n");
		return ret;
	}

	slave->lanes = panel->info.lanes;
	slave->format = panel->info.format;
	slave->hs_rate = panel->info.hs_rate;
	slave->lp_rate = panel->info.lp_rate;
	slave->mode_flags = panel->info.mode_flags;

	ret = mipi_dsi_attach(slave);
	if (ret) {
		DRM_ERROR("failed to attach dsi panel to host\n");
		drm_panel_remove(&panel->base);
		return ret;
	}
	panel->slave = slave;

	ret = sprd_panel_sysfs_init(&panel->dev);
	if (ret)
		return ret;

	mipi_dsi_set_drvdata(slave, panel);
	
	ret = oplus_display_private_api_init();
	if (ret) {
		DRM_ERROR("failed to oplus_display_private_api_init\n");
	}
	/*
	 * FIXME:
	 * The esd check work should not be scheduled in probe
	 * function. It should be scheduled in the enable()
	 * callback function. But the dsi encoder will not call
	 * drm_panel_enable() the first time in encoder_enable().
	 */
	if (panel->info.esd_check_en) {
		schedule_delayed_work(&panel->esd_work,
				      msecs_to_jiffies(2000));
		panel->esd_work_pending = true;
	}

	/* liusiyu silence mode */
	//panel->enabled = true;
	if(!strcmp(bootmode, "silent")){
        panel->enabled = false;
    }else{
        panel->enabled = true;
    }

	panel->panic_nb = sprd_panel_panic_event_nb;
	atomic_notifier_chain_register(&panic_notifier_list, &panel->panic_nb);

	/* liusiyu silence mode */
	get_hardware_info_data(HWID_LCM, panel->lcd_name);
	mutex_init(&panel->lock);

	return 0;
}

static int sprd_panel_remove(struct mipi_dsi_device *slave)
{
	struct sprd_panel *panel = mipi_dsi_get_drvdata(slave);
	int ret;

	DRM_INFO("%s()\n", __func__);
	
	oplus_display_private_api_exit();

	atomic_notifier_chain_unregister(&panic_notifier_list, &panel->panic_nb);
	sprd_panel_disable(&panel->base);
	sprd_panel_unprepare(&panel->base);

	ret = mipi_dsi_detach(slave);
	if (ret < 0)
		DRM_ERROR("failed to detach from DSI host: %d\n", ret);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);

	return 0;
}

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "sprd,generic-mipi-panel", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_of_match);

struct mipi_dsi_driver sprd_panel_driver = {
	.driver = {
		.name = "sprd-mipi-panel-drv",
		.of_match_table = panel_of_match,
	},
	.probe = sprd_panel_probe,
	.remove = sprd_panel_remove,
};

MODULE_AUTHOR("Leon He <leon.he@unisoc.com>");
MODULE_AUTHOR("Kevin Tang <kevin.tang@unisoc.com>");
MODULE_DESCRIPTION("UNISOC MIPI DSI Panel Driver");
MODULE_LICENSE("GPL v2");
