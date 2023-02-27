// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Unisoc Inc.
 */

#include <linux/backlight.h>
#include <linux/dma-buf.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "sprd_bl.h"
#include "sprd_dpu.h"
#include "dpu_enhance_param.h"
#include "sprd_dsi.h"
#include "sprd_crtc.h"
#include "sprd_plane.h"

#define XFBC8888_HEADER_SIZE(w, h) (ALIGN((ALIGN((w), 16)) * \
				(ALIGN((h), 16)) / 16, 128))
#define XFBC8888_PAYLOAD_SIZE(w, h) (ALIGN((w), 16) * ALIGN((h), 16) * 4)
#define XFBC8888_BUFFER_SIZE(w, h) (XFBC8888_HEADER_SIZE(w, h) \
				+ XFBC8888_PAYLOAD_SIZE(w, h))

#define SLP_BRIGHTNESS_THRESHOLD 0x20

/* DPU registers size, 4 Bytes(32 Bits) */
#define DPU_REG_SIZE					0x04
/* Layer registers offset */
#define DPU_LAY_REG_OFFSET				0x10

#define DPU_MAX_REG_OFFSET				0x19AC

#define DPU_REG_RD(reg) readl_relaxed(reg)

#define DPU_REG_WR(reg, mask) writel_relaxed(mask, reg)

#define DPU_REG_SET(reg, mask) \
		writel_relaxed(readl_relaxed(reg) | mask, reg)

#define DPU_REG_CLR(reg, mask) \
		writel_relaxed(readl_relaxed(reg) & ~mask, reg)

#define DPU_LAY_REG(reg, index) \
		(reg + index * DPU_LAY_REG_OFFSET * DPU_REG_SIZE)

#define DPU_LAY_PLANE_ADDR(reg, index, plane) \
		(reg + index * DPU_LAY_REG_OFFSET * DPU_REG_SIZE + plane * DPU_REG_SIZE)

/*Global control registers */
#define REG_DPU_CTRL					0x08
#define REG_DPU_CFG0					0x0C
#define REG_DPU_CFG1					0x10
#define REG_PANEL_SIZE					0x18
#define REG_BLEND_SIZE					0x1C
#define REG_BG_COLOR					0x24

/* Layer enable */
#define REG_LAYER_ENABLE				0x2c

/* Layer0 control registers */
#define REG_LAY_BASE_ADDR				0x30
#define REG_LAY_CTRL					0x40
#define REG_LAY_DES_SIZE				0x44
#define REG_LAY_SRC_SIZE				0x48
#define REG_LAY_PITCH					0x4C
#define REG_LAY_POS					0x50
#define REG_LAY_ALPHA					0x54
#define REG_LAY_CK					0x58
#define REG_LAY_PALLETE					0x5C
#define REG_LAY_CROP_START				0x60

/* Write back config registers */
#define REG_WB_BASE_ADDR				0x230
#define REG_WB_CTRL					0x234
#define REG_WB_CFG					0x238
#define REG_WB_PITCH					0x23C

/* Interrupt control registers */
#define REG_DPU_INT_EN					0x250
#define REG_DPU_INT_CLR					0x254
#define REG_DPU_INT_STS					0x258
#define REG_DPU_INT_RAW					0x25C

/* DPI control registers */
#define REG_DPI_CTRL					0x260
#define REG_DPI_H_TIMING				0x264
#define REG_DPI_V_TIMING				0x268

/* DPU STS */
#define REG_DPU_STS_21					0x754
#define REG_DPU_STS_22					0x758

/* Global control bits */
#define BIT_DPU_RUN					BIT(0)
#define BIT_DPU_STOP					BIT(1)
#define BIT_DPU_ALL_UPDATE				BIT(2)
#define BIT_DPU_REG_UPDATE				BIT(3)
#define BIT_LAY_REG_UPDATE				BIT(4)
#define BIT_DPU_IF_EDPI					BIT(0)

/* Layer control bits */
// #define BIT_DPU_LAY_EN				BIT(0)
#define BIT_DPU_LAY_LAYER_ALPHA				(0x01 << 2)
#define BIT_DPU_LAY_COMBO_ALPHA				(0x01 << 3)
#define BIT_DPU_LAY_FORMAT_YUV422_2PLANE		(0x00 << 4)
#define BIT_DPU_LAY_FORMAT_YUV420_2PLANE		(0x01 << 4)
#define BIT_DPU_LAY_FORMAT_YUV420_3PLANE		(0x02 << 4)
#define BIT_DPU_LAY_FORMAT_ARGB8888			(0x03 << 4)
#define BIT_DPU_LAY_FORMAT_RGB565			(0x04 << 4)
#define BIT_DPU_LAY_FORMAT_XFBC_ARGB8888		(0x08 << 4)
#define BIT_DPU_LAY_FORMAT_XFBC_RGB565			(0x09 << 4)
#define BIT_DPU_LAY_FORMAT_XFBC_YUV420			(0x0A << 4)
#define BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3		(0x00 << 8)
#define BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0		(0x01 << 8)
#define BIT_DPU_LAY_DATA_ENDIAN_B2B3B0B1		(0x02 << 8)
#define BIT_DPU_LAY_DATA_ENDIAN_B1B0B3B2		(0x03 << 8)
#define BIT_DPU_LAY_NO_SWITCH				(0x00 << 10)
#define BIT_DPU_LAY_RGB888_RB_SWITCH			(0x01 << 10)
#define BIT_DPU_LAY_RGB565_RB_SWITCH			(0x01 << 12)
#define BIT_DPU_LAY_PALLETE_EN				(0x01 << 13)
#define BIT_DPU_LAY_MODE_BLEND_NORMAL			(0x01 << 16)
#define BIT_DPU_LAY_MODE_BLEND_PREMULT			(0x01 << 16)

/*Interrupt control & status bits */
#define BIT_DPU_INT_DONE				BIT(0)
#define BIT_DPU_INT_TE					BIT(1)
#define BIT_DPU_INT_ERR					BIT(2)
#define BIT_DPU_INT_VSYNC_EN				BIT(4)
#define BIT_DPU_INT_WB_DONE_EN				BIT(5)
#define BIT_DPU_INT_WB_ERR_EN				BIT(6)
#define BIT_DPU_INT_FBC_PLD_ERR				BIT(7)
#define BIT_DPU_INT_FBC_HDR_ERR				BIT(8)
#define BIT_DPU_INT_DPU_ALL_UPDATE_DONE			BIT(16)
#define BIT_DPU_INT_DPU_REG_UPDATE_DONE			BIT(17)
#define BIT_DPU_INT_LAY_REG_UPDATE_DONE			BIT(18)
#define BIT_DPU_INT_PQ_REG_UPDATE_DONE			BIT(19)

/* DPI control bits */
#define BIT_DPU_EDPI_TE_EN				BIT(8)
#define BIT_DPU_EDPI_FROM_EXTERNAL_PAD			BIT(10)
#define BIT_DPU_DPI_HALT_EN				BIT(16)

#define BIT_DPU_STS_RCH_DPU_BUSY			BIT(15)

struct wb_region {
	u32 index;
	u16 pos_x;
	u16 pos_y;
	u16 size_w;
	u16 size_h;
};

struct dpu_cfg1 {
	u8 arqos_low;
	u8 arqos_high;
	u8 awqos_low;
	u8 awqos_high;
};

struct layer_reg {
	u32 addr[4];
	u32 ctrl;
	u32 dst_size;
	u32 src_size;
	u32 pitch;
	u32 pos;
	u32 alpha;
	u32 ck;
	u32 pallete;
	u32 crop_start;
	u32 reserved[3];
};

static struct dpu_cfg1 qos_cfg = {
	.arqos_low = 0x01,
	.arqos_high = 0x07,
	.awqos_low = 0x01,
	.awqos_high = 0x07,
};


struct dpu_dsc_cfg {
	char name[128];
	bool dual_dsi_en;
	bool dsc_en;
	int  dsc_mode;
};

static void dpu_clean_all(struct dpu_context *ctx);
static void dpu_layer(struct dpu_context *ctx,
		    struct sprd_layer_state *hwlayer);


static void dpu_version(struct dpu_context *ctx)
{
	ctx->version = "dpu-r6p0";
}

static int dpu_parse_dt(struct dpu_context *ctx,
				struct device_node *np)
{
	int ret = 0;
	struct device_node *qos_np;

	qos_np = of_parse_phandle(np, "sprd,qos", 0);
	if (!qos_np)
		pr_warn("can't find dpu qos cfg node\n");

	ret = of_property_read_u8(qos_np, "arqos-low",
					&qos_cfg.arqos_low);
	if (ret)
		pr_warn("read arqos-low failed, use default\n");

	ret = of_property_read_u8(qos_np, "arqos-high",
					&qos_cfg.arqos_high);
	if (ret)
		pr_warn("read arqos-high failed, use default\n");

	ret = of_property_read_u8(qos_np, "awqos-low",
					&qos_cfg.awqos_low);
	if (ret)
		pr_warn("read awqos_low failed, use default\n");

	ret = of_property_read_u8(qos_np, "awqos-high",
					&qos_cfg.awqos_high);
	if (ret)
		pr_warn("read awqos-high failed, use default\n");

	return 0;
}

static u32 dpu_isr(struct dpu_context *ctx)
{
	u32 reg_val, int_mask = 0;

	reg_val = DPU_REG_RD(ctx->base + REG_DPU_INT_STS);

	/* disable err interrupt */
	if (reg_val & BIT_DPU_INT_ERR)
		int_mask |= BIT_DPU_INT_ERR;

	/* dpu vsync isr */
	if (reg_val & BIT_DPU_INT_VSYNC_EN) {
		/* write back feature */
		if ((ctx->vsync_count == ctx->max_vsync_count) && ctx->wb_en)
			schedule_work(&ctx->wb_work);

		ctx->vsync_count++;
	}

	/* dpu update done isr */
	if (reg_val & BIT_DPU_INT_LAY_REG_UPDATE_DONE) {
		ctx->evt_update = true;
		wake_up_interruptible_all(&ctx->wait_queue);
	}

	if (reg_val & BIT_DPU_INT_DPU_ALL_UPDATE_DONE) {
		ctx->evt_all_update = true;
		wake_up_interruptible_all(&ctx->wait_queue);
	}

	// if (reg_val & DPU_INT_PQ_REG_UPDATE_DONE_MASK) {
		// ctx->evt_pq_update = true;
		// wake_up_interruptible_all(&wait_queue);
	// }

	/* dpu stop done isr */
	if (reg_val & BIT_DPU_INT_DONE) {
		ctx->evt_stop = true;
		wake_up_interruptible_all(&ctx->wait_queue);
	}

	/* dpu write back done isr */
	if (reg_val & BIT_DPU_INT_WB_DONE_EN) {
		/*
		 * The write back is a time-consuming operation. If there is a
		 * flip occurs before write back done, the write back buffer is
		 * no need to display. Otherwise the new frame will be covered
		 * by the write back buffer, which is not what we wanted.
		 */
		if (ctx->wb_en && (ctx->vsync_count > ctx->max_vsync_count)) {
			ctx->wb_en = false;
			schedule_work(&ctx->wb_work);
			/*reg_val |= DPU_INT_FENCE_SIGNAL_REQUEST;*/
		}

		pr_debug("wb done\n");
	}

	/* dpu write back error isr */
	if (reg_val & BIT_DPU_INT_WB_ERR_EN) {
		pr_err("dpu write back fail\n");
		/*give a new chance to write back*/
		// if (ctx->max_vsync_count > 0) {
		ctx->wb_en = true;
		ctx->vsync_count = 0;
		// }
	}

	/* dpu afbc payload error isr */
	if (reg_val & BIT_DPU_INT_FBC_PLD_ERR) {
		int_mask |= BIT_DPU_INT_FBC_PLD_ERR;
		pr_err("dpu afbc payload error\n");
	}

	/* dpu afbc header error isr */
	if (reg_val & BIT_DPU_INT_FBC_HDR_ERR) {
		int_mask |= BIT_DPU_INT_FBC_HDR_ERR;
		pr_err("dpu afbc header error\n");
	}

	DPU_REG_WR(ctx->base + REG_DPU_INT_CLR, reg_val);
	DPU_REG_CLR(ctx->base + REG_DPU_INT_EN, int_mask);

	return reg_val;
}

static int dpu_wait_stop_done(struct dpu_context *ctx)
{
	int rc, i;
	u32 dpu_sts_21, dpu_sts_22;

	if (ctx->stopped)
		return 0;

	/* wait for stop done interrupt */
	rc = wait_event_interruptible_timeout(ctx->wait_queue, ctx->evt_stop,
					       msecs_to_jiffies(500));
	ctx->evt_stop = false;

	ctx->stopped = true;

	if (!rc) {
		/* time out */
		pr_err("dpu wait for stop done time out!\n");
		return -1;
	}

	for (i = 1; i <= 3000; i++) {
		dpu_sts_21 = DPU_REG_RD(ctx->base + REG_DPU_STS_21);
		dpu_sts_22 = DPU_REG_RD(ctx->base + REG_DPU_STS_22);
		if ((dpu_sts_21 & BIT(15)) ||
		  (dpu_sts_22 & BIT(15)))
			mdelay(1);
		else {
			pr_info("dpu is idle now\n");
			break;
		}

		if (i == 3000)
			pr_err("wait for dpu idle timeout\n");
	}

	return 0;
}

static int dpu_wait_update_done(struct dpu_context *ctx)
{
	int rc;

	/* clear the event flag before wait */
	if (!ctx->stopped)
		ctx->evt_update = false;

	/* wait for reg update done interrupt */
	rc = wait_event_interruptible_timeout(ctx->wait_queue, ctx->evt_update,
					       msecs_to_jiffies(500));

	if (!rc) {
		/* time out */
		pr_err("dpu wait for reg update done time out!\n");
		return -1;
	}

	return 0;
}

static void dpu_stop(struct dpu_context *ctx)
{
	if (ctx->if_type == SPRD_DPU_IF_DPI)
		DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_DPU_STOP);

	dpu_wait_stop_done(ctx);
	ctx->evt_update = false;
	pr_info("dpu stop\n");
}

static void dpu_run(struct dpu_context *ctx)
{
	DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_DPU_RUN);
	DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_LAY_REG_UPDATE);
	ctx->stopped = false;

	pr_info("dpu run\n");

	if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		/*
		 * If the panel read GRAM speed faster than
		 * DSI write GRAM speed, it will display some
		 * mass on screen when backlight on. So wait
		 * a TE period after flush the GRAM.
		 */
		if (!ctx->panel_ready) {
			dpu_wait_stop_done(ctx);
			/* wait for TE again */
			mdelay(20);
			ctx->panel_ready = true;
		}
	}
}


static void dpu_wb_trigger(struct dpu_context *ctx, u8 count, bool debug)
{
	int mode_width  = DPU_REG_RD(ctx->base + REG_BLEND_SIZE) & 0xFFFF;
	int mode_height = DPU_REG_RD(ctx->base + REG_BLEND_SIZE) >> 16;

	ctx->wb_layer.dst_w = mode_width;
	ctx->wb_layer.dst_h = mode_height;
	ctx->wb_layer.src_w = mode_width;
	ctx->wb_layer.src_h = mode_height;
	ctx->wb_layer.pitch[0] = ALIGN(mode_width, 16) * 4;
	ctx->wb_layer.fbc_hsize_r = XFBC8888_HEADER_SIZE(mode_width,
						mode_height) / 128;
	DPU_REG_WR(ctx->base + REG_WB_PITCH, ALIGN((mode_width), 16));

	ctx->wb_layer.xfbc = ctx->wb_xfbc_en;

	if (ctx->wb_xfbc_en) {
		DPU_REG_WR(ctx->base + REG_WB_CFG, (ctx->wb_layer.fbc_hsize_r << 16) | BIT(0));
		DPU_REG_WR(ctx->base + REG_WB_BASE_ADDR, ctx->wb_layer.addr[0] +
				ctx->wb_layer.fbc_hsize_r);
		}
	else {
		DPU_REG_WR(ctx->base + REG_WB_CFG, 0);
		DPU_REG_WR(ctx->base + REG_WB_BASE_ADDR, ctx->wb_layer.addr[0]);
		}

	DPU_REG_WR(ctx->base + REG_WB_PITCH, ctx->vm.hactive);

	if (debug)
		/* writeback debug trigger */
		DPU_REG_WR(ctx->base + REG_WB_CTRL, BIT(1));
	else
		DPU_REG_SET(ctx->base + REG_WB_CTRL, BIT(0));

	/* update trigger */
	DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT(4));
	dpu_wait_update_done(ctx);

	pr_debug("write back trigger\n");
}

static void dpu_wb_flip(struct dpu_context *ctx)
{
	dpu_clean_all(ctx);
	dpu_layer(ctx, &ctx->wb_layer);

	DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT(4));
	dpu_wait_update_done(ctx);
	pr_debug("write back flip\n");
}

static void dpu_wb_work_func(struct work_struct *data)
{
	struct dpu_context *ctx =
		container_of(data, struct dpu_context, wb_work);

	down(&ctx->lock);

	if (!ctx->enabled) {
		up(&ctx->lock);
		pr_err("dpu is not initialized\n");
		return;
	}

	if (ctx->flip_pending) {
		up(&ctx->lock);
		pr_warn("dpu flip is disabled\n");
		return;
	}

	if (ctx->wb_en && (ctx->vsync_count > ctx->max_vsync_count))
		dpu_wb_trigger(ctx, 1, false);
	else if (!ctx->wb_en)
		dpu_wb_flip(ctx);

	up(&ctx->lock);
}

static int dpu_write_back_config(struct dpu_context *ctx)
{
	static int need_config = 1;
	size_t wb_buf_size;
	struct sprd_dpu *dpu =
		(struct sprd_dpu *)container_of(ctx, struct sprd_dpu, ctx);
	struct drm_device *drm = dpu->crtc->base.dev;
	int mode_width  = DPU_REG_RD(ctx->base + REG_BLEND_SIZE) & 0xFFFF;

	if (!need_config) {
		DPU_REG_WR(ctx->base + REG_WB_BASE_ADDR, ctx->wb_addr_p);
		DPU_REG_WR(ctx->base + REG_WB_PITCH, ALIGN((mode_width), 16));
		if (ctx->wb_xfbc_en)
			DPU_REG_WR(ctx->base + REG_WB_CFG, ((ctx->wb_layer.fbc_hsize_r << 16) | BIT(0)));
		else
			DPU_REG_WR(ctx->base + REG_WB_CFG, 0);
		pr_debug("write back has configed\n");
		return 0;
	}

	wb_buf_size = XFBC8888_BUFFER_SIZE(dpu->mode->hdisplay,
						dpu->mode->vdisplay);
	pr_info("use wb_reserved memory for writeback, size:0x%zx\n", wb_buf_size);
	ctx->wb_addr_v = dma_alloc_wc(drm->dev, wb_buf_size, &ctx->wb_addr_p, GFP_KERNEL);
	if (!ctx->wb_addr_p) {
		ctx->max_vsync_count = 0;
		return -ENOMEM;
	}

	// ctx->wb_xfbc_en = 1;
	ctx->wb_layer.index = 7;
	ctx->wb_layer.planes = 1;
	ctx->wb_layer.alpha = 0xff;
	ctx->wb_layer.format = DRM_FORMAT_ABGR8888;
	ctx->wb_layer.addr[0] = ctx->wb_addr_p;
	DPU_REG_WR(ctx->base + REG_WB_BASE_ADDR, ctx->wb_addr_p);
	DPU_REG_WR(ctx->base + REG_WB_PITCH, ALIGN((mode_width), 16));
	if (ctx->wb_xfbc_en) {
		ctx->wb_layer.xfbc = ctx->wb_xfbc_en;
		DPU_REG_WR(ctx->base + REG_WB_CFG, ((ctx->wb_layer.fbc_hsize_r << 16) | BIT(0)));
	}

	ctx->max_vsync_count = 0;
	need_config = 0;

	INIT_WORK(&ctx->wb_work, dpu_wb_work_func);

	return 0;
}

static int dpu_init(struct dpu_context *ctx)
{
	u32 reg_val, size;

	/* set bg color */
	DPU_REG_WR(ctx->base + REG_BG_COLOR, 0x00);

	/* set dpu output size */
	size = (ctx->vm.vactive << 16) | ctx->vm.hactive;
	DPU_REG_WR(ctx->base + REG_PANEL_SIZE, size);
	DPU_REG_WR(ctx->base + REG_BLEND_SIZE, size);

	DPU_REG_WR(ctx->base + REG_DPU_CFG0, 0x00);

	reg_val = (qos_cfg.awqos_high << 12) |
		(qos_cfg.awqos_low << 8) |
		(qos_cfg.arqos_high << 4) |
		(qos_cfg.arqos_low) | BIT(18) | BIT(22) | BIT(23);
	DPU_REG_WR(ctx->base + REG_DPU_CFG1, reg_val);;
	if (ctx->stopped)
		dpu_clean_all(ctx);

	DPU_REG_WR(ctx->base + REG_DPU_INT_CLR, 0xffff);

	dpu_write_back_config(ctx);

	ctx->base_offset[0] = 0x0;
	ctx->base_offset[1] = DPU_MAX_REG_OFFSET;

	return 0;
}

static void dpu_fini(struct dpu_context *ctx)
{
	DPU_REG_WR(ctx->base + REG_DPU_INT_EN, 0x00);
	DPU_REG_WR(ctx->base + REG_DPU_INT_CLR, 0xff);

	ctx->panel_ready = false;
}

enum {
	DPU_LAYER_FORMAT_YUV422_2PLANE,
	DPU_LAYER_FORMAT_YUV420_2PLANE,
	DPU_LAYER_FORMAT_YUV420_3PLANE,
	DPU_LAYER_FORMAT_ARGB8888,
	DPU_LAYER_FORMAT_RGB565,
	DPU_LAYER_FORMAT_XFBC_ARGB8888 = 8,
	DPU_LAYER_FORMAT_XFBC_RGB565,
	DPU_LAYER_FORMAT_XFBC_YUV420,
	DPU_LAYER_FORMAT_MAX_TYPES,
};

enum {
	DPU_LAYER_ROTATION_0,
	DPU_LAYER_ROTATION_90,
	DPU_LAYER_ROTATION_180,
	DPU_LAYER_ROTATION_270,
	DPU_LAYER_ROTATION_0_M,
	DPU_LAYER_ROTATION_90_M,
	DPU_LAYER_ROTATION_180_M,
	DPU_LAYER_ROTATION_270_M,
};

static u32 to_dpu_rotation(u32 angle)
{
	u32 rot = DPU_LAYER_ROTATION_0;

	switch (angle) {
	case 0:
	case DRM_MODE_ROTATE_0:
		rot = DPU_LAYER_ROTATION_0;
		break;
	case DRM_MODE_ROTATE_90:
		rot = DPU_LAYER_ROTATION_90;
		break;
	case DRM_MODE_ROTATE_180:
		rot = DPU_LAYER_ROTATION_180;
		break;
	case DRM_MODE_ROTATE_270:
		rot = DPU_LAYER_ROTATION_270;
		break;
	case DRM_MODE_REFLECT_Y:
		rot = DPU_LAYER_ROTATION_180_M;
		break;
	case (DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90):
		rot = DPU_LAYER_ROTATION_90_M;
		break;
	case DRM_MODE_REFLECT_X:
		rot = DPU_LAYER_ROTATION_0_M;
		break;
	case (DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90):
		rot = DPU_LAYER_ROTATION_270_M;
		break;
	default:
		pr_err("rotation convert unsupport angle (drm)= 0x%x\n", angle);
		break;
	}

	return rot;
}

static u32 dpu_img_ctrl(u32 format, u32 blending, u32 compression, u32 y2r_coef,
		u32 rotation)
{
	int reg_val = 0;
	/* layer enable */
	// reg_val |= BIT_DPU_LAY_EN;

	switch (format) {
	case DRM_FORMAT_BGRA8888:
		/* BGRA8888 -> ARGB8888 */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		if (compression)
			/* XFBC-ARGB8888 */
			reg_val |= (BIT_DPU_LAY_FORMAT_XFBC_ARGB8888);
		else
			reg_val |= (BIT_DPU_LAY_FORMAT_ARGB8888);
		break;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		/* RGBA8888 -> ABGR8888 */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		/* FALLTHRU */
	case DRM_FORMAT_ABGR8888:
		/* rb switch */
		reg_val |= BIT_DPU_LAY_RGB888_RB_SWITCH;
		/* FALLTHRU */
	case DRM_FORMAT_ARGB8888:
		if (compression)
			/* XFBC-ARGB8888 */
			reg_val |= (BIT_DPU_LAY_FORMAT_XFBC_ARGB8888);
		else
			reg_val |= (BIT_DPU_LAY_FORMAT_ARGB8888);
		break;
	case DRM_FORMAT_XBGR8888:
		/* rb switch */
		reg_val |= BIT_DPU_LAY_RGB888_RB_SWITCH;
		/* FALLTHRU */
	case DRM_FORMAT_XRGB8888:
		if (compression)
			/* XFBC-ARGB8888 */
			reg_val |= (BIT_DPU_LAY_FORMAT_XFBC_ARGB8888);
		else
			reg_val |= (BIT_DPU_LAY_FORMAT_ARGB8888);
		break;
	case DRM_FORMAT_BGR565:
		/* rb switch */
		reg_val |= BIT_DPU_LAY_RGB565_RB_SWITCH;
		/* FALLTHRU */
	case DRM_FORMAT_RGB565:
		if (compression)
			/* XFBC-RGB565 */
			reg_val |= (BIT_DPU_LAY_FORMAT_XFBC_RGB565);
		else
			reg_val |= (BIT_DPU_LAY_FORMAT_RGB565);
		break;
	case DRM_FORMAT_NV12:
		if (compression)
			/*2-Lane: Yuv420 */
			reg_val |= BIT_DPU_LAY_FORMAT_XFBC_YUV420;
		else
			reg_val |= BIT_DPU_LAY_FORMAT_YUV420_2PLANE;
		/*Y endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/*UV endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		break;
	case DRM_FORMAT_NV21:
		if (compression)
			/*2-Lane: Yuv420 */
			reg_val |= BIT_DPU_LAY_FORMAT_XFBC_YUV420;
		else
			reg_val |= BIT_DPU_LAY_FORMAT_YUV420_2PLANE;
		/*Y endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/*UV endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0 << 2;
		break;
	case DRM_FORMAT_NV16:
		/*2-Lane: Yuv422 */
		reg_val |= BIT_DPU_LAY_FORMAT_YUV422_2PLANE;
		/*Y endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0;
		/*UV endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B3B2B1B0 << 2;
		break;
	case DRM_FORMAT_NV61:
		/*2-Lane: Yuv422 */
		reg_val |= BIT_DPU_LAY_FORMAT_YUV422_2PLANE;
		/*Y endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/*UV endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		break;
	case DRM_FORMAT_YUV420:
		reg_val |= BIT_DPU_LAY_FORMAT_YUV420_3PLANE;
		/*Y endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		/*UV endian */
		reg_val |= BIT_DPU_LAY_DATA_ENDIAN_B0B1B2B3;
		break;
	default:
		pr_err("error: invalid format %c%c%c%c\n", format,
						format >> 8,
						format >> 16,
						format >> 24);
		break;
	}

	switch (blending) {
	case DRM_MODE_BLEND_PIXEL_NONE:
		/* don't do blending, maybe RGBX */
		/* alpha mode select - layer alpha */
		reg_val |= BIT_DPU_LAY_LAYER_ALPHA;
		break;
	case DRM_MODE_BLEND_COVERAGE:
		/* alpha mode select - combo alpha */
		reg_val |= BIT_DPU_LAY_COMBO_ALPHA;
		/* blending mode select - normal mode */
		reg_val &= (~BIT_DPU_LAY_MODE_BLEND_NORMAL);
		break;
	case DRM_MODE_BLEND_PREMULTI:
		/* alpha mode select - combo alpha */
		reg_val |= BIT_DPU_LAY_COMBO_ALPHA;
		/* blending mode select - pre-mult mode */
		reg_val |= BIT_DPU_LAY_MODE_BLEND_PREMULT;
		break;
	default:
		/* alpha mode select - layer alpha */
		reg_val |= BIT_DPU_LAY_LAYER_ALPHA;
		break;
	}

	reg_val |= y2r_coef << 28;

	return reg_val;
}

static void dpu_clean_all(struct dpu_context *ctx)
{
	int i;

	for (i = 0; i < 8; i++)
		DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_CTRL, i), 0x00);

	DPU_REG_WR(ctx->base + REG_LAYER_ENABLE, 0);
}

static void dpu_bgcolor(struct dpu_context *ctx, u32 color)
{

	if (ctx->if_type == SPRD_DPU_IF_EDPI)
		dpu_wait_stop_done(ctx);

	DPU_REG_WR(ctx->base + REG_BG_COLOR, color);

	dpu_clean_all(ctx);

	if ((ctx->if_type == SPRD_DPU_IF_DPI) && !ctx->stopped) {
		DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_LAY_REG_UPDATE);
		dpu_wait_update_done(ctx);
	} else if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_DPU_RUN);
		ctx->stopped = false;
	}
}

static void dpu_layer(struct dpu_context *ctx,
		    struct sprd_layer_state *hwlayer)
{
	const struct drm_format_info *info;
	struct layer_reg tmp = {};
	u32 dst_size, src_size, offset, wd, rot, layer_enable_sts;
	int i;

	layer_enable_sts = DPU_REG_RD(ctx->base + REG_LAYER_ENABLE);

	offset = (hwlayer->dst_x & 0xffff) | ((hwlayer->dst_y) << 16);
	src_size = (hwlayer->src_w & 0xffff) | ((hwlayer->src_h) << 16);
	dst_size = (hwlayer->dst_w & 0xffff) | ((hwlayer->dst_h) << 16);

	if (hwlayer->pallete_en) {
		tmp.pos = offset;
		tmp.src_size = src_size;
		tmp.dst_size = dst_size;
		tmp.alpha = hwlayer->alpha;
		tmp.pallete = hwlayer->pallete_color;

		/* pallete layer enable */
		tmp.ctrl = 0x2004;
		pr_debug("dst_x = %d, dst_y = %d, dst_w = %d, dst_h = %d, pallete:%d\n",
				hwlayer->dst_x, hwlayer->dst_y,
				hwlayer->dst_w, hwlayer->dst_h, tmp.pallete);
	} else {
		if (src_size != dst_size) {
			rot = to_dpu_rotation(hwlayer->rotation);
			if ((rot == DPU_LAYER_ROTATION_90) || (rot == DPU_LAYER_ROTATION_270) ||
					(rot == DPU_LAYER_ROTATION_90_M) || (rot == DPU_LAYER_ROTATION_270_M))
				dst_size = (hwlayer->dst_h & 0xffff) | ((hwlayer->dst_w) << 16);
			tmp.ctrl = BIT(24);
		}

		for (i = 0; i < hwlayer->planes; i++) {
			if (hwlayer->addr[i] % 16)
				pr_err("layer addr[%d] is not 16 bytes align, it's 0x%08x\n",
						i, hwlayer->addr[i]);
			tmp.addr[i] = hwlayer->addr[i];
		}

		tmp.pos = offset;
		tmp.src_size = src_size;
		tmp.dst_size = dst_size;
		tmp.crop_start = (hwlayer->src_y << 16) | hwlayer->src_x;
		tmp.alpha = hwlayer->alpha;

		info = drm_format_info(hwlayer->format);
		wd = info->cpp[0];
		if (wd == 0) {
			pr_err("layer[%d] bytes per pixel is invalid\n", hwlayer->index);
			return;
		}

		if (hwlayer->planes == 3)
			/* UV pitch is 1/2 of Y pitch*/
			tmp.pitch = (hwlayer->pitch[0] / wd) |
				(hwlayer->pitch[0] / wd << 15);
		else
			tmp.pitch = hwlayer->pitch[0] / wd;

		tmp.ctrl |= dpu_img_ctrl(hwlayer->format, hwlayer->blending,
				hwlayer->xfbc, hwlayer->y2r_coef, hwlayer->rotation);
	}

	for (i = 0; i < hwlayer->planes; i++)
		DPU_REG_WR(ctx->base + DPU_LAY_PLANE_ADDR(REG_LAY_BASE_ADDR,
					hwlayer->index, i), tmp.addr[i]);

	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_POS,
			hwlayer->index), tmp.pos);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_SRC_SIZE,
			hwlayer->index), tmp.src_size);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_DES_SIZE,
			hwlayer->index), tmp.dst_size);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_CROP_START,
			hwlayer->index), tmp.crop_start);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_ALPHA,
			hwlayer->index), tmp.alpha);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_PITCH,
			hwlayer->index), tmp.pitch);
	DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_CTRL,
			hwlayer->index), tmp.ctrl);

	layer_enable_sts |= (1 << hwlayer->index);
	DPU_REG_WR(ctx->base + REG_LAYER_ENABLE, layer_enable_sts);
	// DPU_REG_WR(ctx->base + DPU_LAY_REG(REG_LAY_PALLETE,
				// hwlayer->index), tmp.pallete);

	pr_debug("dst_x = %d, dst_y = %d, dst_w = %d, dst_h = %d\n",
				hwlayer->dst_x, hwlayer->dst_y,
				hwlayer->dst_w, hwlayer->dst_h);
	pr_debug("start_x = %d, start_y = %d, start_w = %d, start_h = %d\n",
				hwlayer->src_x, hwlayer->src_y,
				hwlayer->src_w, hwlayer->src_h);
}

static void dpu_flip(struct dpu_context *ctx,
		     struct sprd_plane planes[], u8 count)
{
	int i;
	u32 reg_val;
	struct sprd_plane_state *state;

	ctx->vsync_count = 0;
	if (ctx->max_vsync_count > 0 && count > 1)
		ctx->wb_en = true;
	/*
	 * Make sure the dpu is in stop status. DPU_r6p0 has no shadow
	 * registers in EDPI mode. So the config registers can only be
	 * updated in the rising edge of DPU_RUN bit.
	 */
	if (ctx->if_type == SPRD_DPU_IF_EDPI)
		dpu_wait_stop_done(ctx);

	/* reset the bgcolor to black */
	DPU_REG_WR(ctx->base + REG_BG_COLOR, 0x00);

	/* disable all the layers */
	dpu_clean_all(ctx);

	/* start configure dpu layers */
	for (i = 0; i < count; i++) {
		state = to_sprd_plane_state(planes[i].base.state);
		dpu_layer(ctx, &state->layer);
	}
	/* update trigger and wait */
	if (ctx->if_type == SPRD_DPU_IF_DPI) {
		if (!ctx->stopped) {
			DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_LAY_REG_UPDATE);
			dpu_wait_update_done(ctx);
		}

		DPU_REG_SET(ctx->base + REG_DPU_INT_EN, BIT_DPU_INT_ERR);
	} else if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		DPU_REG_SET(ctx->base + REG_DPU_CTRL, BIT_DPU_RUN);
		ctx->stopped = false;
	}

	/*
	 * If the following interrupt was disabled in isr,
	 * re-enable it.
	 */
	reg_val = BIT_DPU_INT_FBC_PLD_ERR |
	BIT_DPU_INT_FBC_HDR_ERR;
	DPU_REG_SET(ctx->base + REG_DPU_INT_EN, reg_val);
}

static void dpu_dpi_init(struct dpu_context *ctx)
{
	u32 int_mask = 0;
	u32 reg_val;

	if (ctx->if_type == SPRD_DPU_IF_DPI) {
		/* use dpi as interface */
		DPU_REG_CLR(ctx->base + REG_DPU_CFG0, BIT_DPU_IF_EDPI);

		/* enable Halt function for SPRD DSI */
		DPU_REG_SET(ctx->base + REG_DPI_CTRL, BIT_DPU_DPI_HALT_EN);

		/* set dpi timing */
		reg_val = ctx->vm.hsync_len << 0 |
			  ctx->vm.hback_porch << 8 |
			  ctx->vm.hfront_porch << 20;
		DPU_REG_WR(ctx->base + REG_DPI_H_TIMING, reg_val);

		reg_val = ctx->vm.vsync_len << 0 |
			  ctx->vm.vback_porch << 8 |
			  ctx->vm.vfront_porch << 20;
		DPU_REG_WR(ctx->base + REG_DPI_V_TIMING, reg_val);

		if (ctx->vm.vsync_len + ctx->vm.vback_porch < 32)
			pr_warn("Warning: (vsync + vbp) < 32, "
				"underflow risk!\n");

		/* enable dpu update done INT */
		int_mask |= BIT_DPU_INT_DPU_ALL_UPDATE_DONE;
		int_mask |= BIT_DPU_INT_DPU_REG_UPDATE_DONE;
		int_mask |= BIT_DPU_INT_LAY_REG_UPDATE_DONE;
		int_mask |= BIT_DPU_INT_PQ_REG_UPDATE_DONE;
		/* enable dpu DONE  INT */
		int_mask |= BIT_DPU_INT_DONE;
		/* enable dpu dpi vsync */
		int_mask |= BIT_DPU_INT_VSYNC_EN;
		/* enable dpu TE INT */
		int_mask |= BIT_DPU_INT_TE;
		/* enable underflow err INT */
		int_mask |= BIT_DPU_INT_ERR;
		/* enable write back done INT */
		int_mask |= BIT_DPU_INT_WB_DONE_EN;
		/* enable write back fail INT */
		int_mask |= BIT_DPU_INT_WB_ERR_EN;

	} else if (ctx->if_type == SPRD_DPU_IF_EDPI) {
		/* use edpi as interface */
		DPU_REG_SET(ctx->base + REG_DPU_CFG0, BIT_DPU_IF_EDPI);

		/* use external te */
		DPU_REG_SET(ctx->base + REG_DPI_CTRL, BIT_DPU_EDPI_FROM_EXTERNAL_PAD);;

		/* enable te */
		DPU_REG_SET(ctx->base + REG_DPI_CTRL, BIT_DPU_EDPI_TE_EN);

		/* enable stop DONE INT */
		int_mask |= BIT_DPU_INT_DONE;
		/* enable TE INT */
		int_mask |= BIT_DPU_INT_TE;
	}

	/* enable ifbc payload error INT */
	int_mask |= BIT_DPU_INT_FBC_PLD_ERR;
	/* enable ifbc header error INT */
	int_mask |= BIT_DPU_INT_FBC_HDR_ERR;

	DPU_REG_WR(ctx->base + REG_DPU_INT_EN, int_mask);
}

static void enable_vsync(struct dpu_context *ctx)
{
	DPU_REG_SET(ctx->base + REG_DPU_INT_EN, BIT_DPU_INT_VSYNC_EN);
}

static void disable_vsync(struct dpu_context *ctx)
{
	DPU_REG_CLR(ctx->base + REG_DPU_INT_EN, BIT_DPU_INT_VSYNC_EN);
}

static int dpu_context_init(struct dpu_context *ctx)
{
	ctx->base_offset[0] = 0x0;
	ctx->base_offset[1] = DPU_MAX_REG_OFFSET;

	return 0;
}

static const u32 primary_fmts[] = {
	DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGBX8888, DRM_FORMAT_BGRX8888,
	DRM_FORMAT_RGB565, DRM_FORMAT_BGR565,
	DRM_FORMAT_NV12, DRM_FORMAT_NV21,
	DRM_FORMAT_NV16, DRM_FORMAT_NV61,
	DRM_FORMAT_YUV420,
};

static void dpu_capability(struct dpu_context *ctx,
			struct sprd_crtc_capability *cap)
{
	cap->max_layers = 6;
	cap->fmts_ptr = primary_fmts;
	cap->fmts_cnt = ARRAY_SIZE(primary_fmts);
}

const struct dpu_core_ops dpu_r6p0_core_ops = {
	.version = dpu_version,
	.parse_dt = dpu_parse_dt,
	.init = dpu_init,
	.fini = dpu_fini,
	.run = dpu_run,
	.stop = dpu_stop,
	.isr = dpu_isr,
	.ifconfig = dpu_dpi_init,
	.capability = dpu_capability,
	.bg_color = dpu_bgcolor,
	.flip = dpu_flip,
	.enable_vsync = enable_vsync,
	.disable_vsync = disable_vsync,
	.context_init = dpu_context_init,
	.write_back = dpu_wb_trigger,
};
