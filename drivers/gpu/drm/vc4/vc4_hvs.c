/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "linux/component.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

#define HVS_REG(reg) { reg, #reg }
static const struct {
	u32 reg;
	const char *name;
} hvs_regs[] = {
	HVS_REG(SCALER_DISPCTRL),
	HVS_REG(SCALER_DISPSTAT),
	HVS_REG(SCALER_DISPID),
	HVS_REG(SCALER_DISPECTRL),
	HVS_REG(SCALER_DISPPROF),
	HVS_REG(SCALER_DISPDITHER),
	HVS_REG(SCALER_DISPEOLN),
	HVS_REG(SCALER_DISPLIST0),
	HVS_REG(SCALER_DISPLIST1),
	HVS_REG(SCALER_DISPLIST2),
	HVS_REG(SCALER_DISPLSTAT),
	HVS_REG(SCALER_DISPLACT0),
	HVS_REG(SCALER_DISPLACT1),
	HVS_REG(SCALER_DISPLACT2),
	HVS_REG(SCALER_DISPCTRL0),
	HVS_REG(SCALER_DISPBKGND0),
	HVS_REG(SCALER_DISPSTAT0),
	HVS_REG(SCALER_DISPBASE0),
	HVS_REG(SCALER_DISPCTRL1),
	HVS_REG(SCALER_DISPBKGND1),
	HVS_REG(SCALER_DISPSTAT1),
	HVS_REG(SCALER_DISPBASE1),
	HVS_REG(SCALER_DISPCTRL2),
	HVS_REG(SCALER_DISPBKGND2),
	HVS_REG(SCALER_DISPSTAT2),
	HVS_REG(SCALER_DISPBASE2),
	HVS_REG(SCALER_DISPALPHA2),
};

void
vc4_hvs_dump_state(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	rmb();
	for (i = 0; i < ARRAY_SIZE(hvs_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 hvs_regs[i].reg, hvs_regs[i].name,
			 HVS_READ(hvs_regs[i].reg));
	}

	DRM_INFO("HVS ctx:\n");
	for (i = 0; i < 64; i += 4) {
		DRM_INFO("0x%08x (%s): 0x%08x 0x%08x 0x%08x 0x%08x\n",
			 i * 4, i < HVS_BOOTLOADER_DLIST_END ? "B" : "D",
			 ((uint32_t *)vc4->hvs->dlist)[i + 0],
			 ((uint32_t *)vc4->hvs->dlist)[i + 1],
			 ((uint32_t *)vc4->hvs->dlist)[i + 2],
			 ((uint32_t *)vc4->hvs->dlist)[i + 3]);
	}
}

static irqreturn_t vc4_hvs_irq_handler(int irq, void *data)
{
	struct vc4_dev *vc4 = data;
	u32 stat = HVS_READ(SCALER_DISPSTAT);
	u32 irq_bits = stat & (SCALER_DISPSTAT_IRQSCL |
			       SCALER_DISPSTAT_IRQDISP0 |
			       SCALER_DISPSTAT_IRQDISP1 |
			       SCALER_DISPSTAT_IRQDISP2 |
			       SCALER_DISPSTAT_IRQSLVWR |
			       SCALER_DISPSTAT_IRQSLVRD);
	irqreturn_t ret = IRQ_NONE;

	if (irq_bits != 0) {
		DRM_ERROR("HVS reported error: SCALER_DISPSTAT = 0x%08x\n",
			  stat);

		/* Mask out whichever of the interrupt enable generated our
		 * interrupt, so that the error only appears once for this
		 * type of error
		 */
		HVS_WRITE(SCALER_DISPCTRL,
			  HVS_READ(SCALER_DISPCTRL) & ~irq_bits);
		ret = IRQ_HANDLED;
	}

	return ret;
}

#ifdef CONFIG_DEBUG_FS
int vc4_hvs_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(hvs_regs); i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   hvs_regs[i].name, hvs_regs[i].reg,
			   HVS_READ(hvs_regs[i].reg));
	}

	return 0;
}
#endif

static int vc4_hvs_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = drm->dev_private;
	struct vc4_hvs *hvs = NULL;
	int ret;

	hvs = devm_kzalloc(&pdev->dev, sizeof(*hvs), GFP_KERNEL);
	if (!hvs)
		return -ENOMEM;

	hvs->pdev = pdev;

	hvs->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(hvs->regs))
		return PTR_ERR(hvs->regs);

	hvs->dlist = hvs->regs + SCALER_DLIST_START;

	vc4->hvs = hvs;

	/* Disable any interrupts set by the firmware. */
	HVS_WRITE(SCALER_DISPCTRL, SCALER_DISPCTRL_ENABLE);
	/* Clear any previous interrupts. */
	HVS_WRITE(SCALER_DISPSTAT, SCALER_DISPSTAT_IRQSCL);

	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_hvs_irq_handler, 0, "vc4 hvs", vc4);
	if (ret)
		return ret;

	/* Turn on a bunch of interrupts for catching HW setup errors. */
	HVS_WRITE(SCALER_DISPCTRL,
		  SCALER_DISPCTRL_ENABLE |
		  SCALER_DISPCTRL_DSP0EISLUR |
		  SCALER_DISPCTRL_DSP1EISLUR |
		  SCALER_DISPCTRL_DSP2EISLUR |
		  SCALER_DISPCTRL_SLVRDEIRQ |
		  SCALER_DISPCTRL_SLVWREIRQ |
		  SCALER_DISPCTRL_DMAEIRQ |
		  SCALER_DISPCTRL_DISP0EIRQ |
		  SCALER_DISPCTRL_DISP1EIRQ |
		  SCALER_DISPCTRL_DISP2EIRQ);

	return 0;
}

static void vc4_hvs_unbind(struct device *dev, struct device *master,
			   void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = drm->dev_private;

	HVS_WRITE(SCALER_DISPCTRL, SCALER_DISPCTRL_ENABLE);
	HVS_WRITE(SCALER_DISPSTAT, SCALER_DISPSTAT_IRQSCL);

	vc4->hvs = NULL;
}

static const struct component_ops vc4_hvs_ops = {
	.bind   = vc4_hvs_bind,
	.unbind = vc4_hvs_unbind,
};

static int vc4_hvs_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hvs_ops);
}

static int vc4_hvs_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hvs_ops);
	return 0;
}

static const struct of_device_id vc4_hvs_dt_match[] = {
	{ .compatible = "brcm,vc4-hvs" },
	{}
};

static struct platform_driver vc4_hvs_driver = {
	.probe = vc4_hvs_dev_probe,
	.remove = vc4_hvs_dev_remove,
	.driver = {
		.name = "vc4_hvs",
		.of_match_table = vc4_hvs_dt_match,
	},
};

void __init vc4_hvs_register(void)
{
	platform_driver_register(&vc4_hvs_driver);
}

void __exit vc4_hvs_unregister(void)
{
	platform_driver_unregister(&vc4_hvs_driver);
}
