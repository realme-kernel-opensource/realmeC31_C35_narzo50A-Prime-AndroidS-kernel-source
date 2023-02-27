// SPDX-License-Identifier: GPL-2.0
//
// Unisoc APCPU HW DVFS driver
//
// Copyright (C) 2020 Unisoc, Inc.
// Author: Jack Liu <jack.liu@unisoc.com>

#include <linux/arch_topology.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/of_platform.h>
#include "sprd-hwdvfs-cpufreq.h"

static struct cpufreq_driver sprd_hardware_cpufreq_driver;
static unsigned long boot_done_timestamp;
static int boost_mode_flag = 1;
static struct sprd_cpufreq_info *global_cpufreq_info;

static const struct of_device_id sprd_hardware_cpufreq_of_match[] = {
	{.compatible = "sprd,hardware-cpufreq",},
	{},
};

static inline struct sprd_cpufreq_info *sprd_cpufreq_info_lookup(u32 cpu)
{
	if (cpu >= nr_cpu_ids) {
		pr_err("invalid cpu id\n");
		return ERR_PTR(-EINVAL);
	}

	if (!global_cpufreq_info) {
		pr_err("the cpufreq information for all cpus is null\n");
		return ERR_PTR(-ENODEV);
	}

	return &global_cpufreq_info[cpu];
}

static int sprd_related_cpus_cluster_alloc(struct sprd_cpufreq_info *info,
					   struct cpufreq_policy *policy)
{
	struct sprd_cpufreq_info *temp;
	int cpu;

	if (!info || !policy)
		return -ENODEV;

	for_each_cpu(cpu, policy->cpus) {
		if (cpu == policy->cpu)
			continue;
		temp = sprd_cpufreq_info_lookup(cpu);
		if (IS_ERR(temp))
			return PTR_ERR(temp);

		temp->pcluster = info->pcluster;
	}

	return 0;
}

static
int sprd_nvmem_info_read(struct device_node *node, const char *name, u32 *value)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len = 0;

	cell = of_nvmem_cell_get(node, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf)) {
		nvmem_cell_put(cell);
		return PTR_ERR(buf);
	}

	if (len > sizeof(u32))
		len = sizeof(u32);

	memcpy(value, buf, len);

	kfree(buf);
	nvmem_cell_put(cell);

	return 0;
}

static int sprd_cpufreq_arch_update(struct sprd_cpufreq_info *info)
{
	struct sprd_cpu_cluster_info *clu;
	struct cpudvfs_phy_ops *driver;
	struct dev_pm_opp *opp;
	unsigned long rate, volt, freq;
	int opp_num, i, ret = 0;

	if (!info) {
		pr_err("the info for cpu is null\n");
		return -EINVAL;
	}

	if (!info->cpufreq_np || !info->cpu_dev || !info->pcluster ||
	    !info->parchdev) {
		dev_err(info->pdev, "cpu or cluster information is missing\n");
		return -EINVAL;
	}

	clu = info->pcluster;
	driver = info->parchdev->phy_ops;
	if (!driver) {
		dev_err(info->pdev, "the way to operate dvfs device is null\n");
		return -EINVAL;
	}

	if (!driver->udelay_update || !driver->idle_pd_volt_update ||
	    !driver->index_map_table_update) {
		dev_err(info->pdev,
			"no way to update cpu hw dvfs device parameters\n");
		return -EINVAL;
	}

	opp_num = dev_pm_opp_get_opp_count(info->cpu_dev);
	if (opp_num < 0) {
		dev_err(info->pdev,
			"failed to get entry num of opp tbl(%d)\n", opp_num);
		return opp_num;
	}

	for (i = 0, rate = 0; i < opp_num; i++, rate++) {
		opp = dev_pm_opp_find_freq_ceil(info->cpu_dev, &rate);
		if (IS_ERR(opp))
			return PTR_ERR(opp);

		volt = dev_pm_opp_get_voltage(opp);
		freq = dev_pm_opp_get_freq(opp);		/* in HZ */

		dev_pm_opp_put(opp);

		ret = driver->volt_grade_table_update(info->parchdev,
						      info->clu_id, freq,
						      volt, i);
		if (ret) {
			dev_err(info->pdev, "failed to update vol grade val\n");
			return ret;
		}
	}

	/*
	 * Because the opp table is changed, some dvfs hardare parameters need
	 * to be changed meanwhile
	 */
	ret = driver->udelay_update(info->parchdev, info->clu_id);
	if (ret) {
		dev_err(info->pdev, "failed to update hw delay time\n");
		return ret;
	}

	ret = driver->index_map_table_update(info->parchdev, clu->opp_name,
					     info->clu_id, clu->cpu_diff_ver,
					     clu->cpubin_str,
					     clu->curr_temp_threshold);
	if (ret) {
		dev_err(info->pdev, "failed to update dvfs index table\n");
		return ret;
	}

	ret = driver->idle_pd_volt_update(info->parchdev, info->clu_id);
	if (ret)
		dev_err(info->pdev, "failed to set idle pd voltage\n");

	return ret;
}

static int sprd_dev_pm_opp_table_update(struct sprd_cpufreq_info *info)
{
	struct sprd_cpu_cluster_info *clu;
	struct property *prop;
	const __be32 *value;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int ret, num;

	if (!info) {
		pr_err("the info for cpu is null\n");
		return -EINVAL;
	}

	if (!info->cpufreq_np || !info->cpu_dev || !info->pcluster ||
	    !info->parchdev) {
		dev_err(info->pdev, "cpu or cluster information is missing\n");
		return -EINVAL;
	}

	clu = info->pcluster;
	if (!strcmp(clu->pre_opp_name, clu->opp_name))
		return 0;

	prop = of_find_property(info->cpufreq_np, clu->opp_name, &num);
	if (!prop || !prop->value) {
		dev_err(info->pdev, "no %s property found\n", clu->opp_name);
		return -ENOENT;
	}

	num = num / sizeof(u32);
	if (num % 2) {
		dev_err(info->pdev, "invalid opp table list\n");
		return -EINVAL;
	}
	value = prop->value;

	dev_dbg(info->pdev, "the final opp table name: %s opp num: %d\n",
		clu->opp_name, num / 2);

	clu->temp_max_freq = 0;
	while (num) {
		unsigned long freq = be32_to_cpup(value++) * 1000; /*in HZ*/
		unsigned long volt = be32_to_cpup(value++);	   /*in uV */

		ret = dev_pm_opp_add(info->cpu_dev, freq, volt);
		if (ret) {
			dev_pm_opp_remove(info->cpu_dev, freq);
			ret = dev_pm_opp_add(info->cpu_dev, freq, volt);
			if (ret) {
				dev_err(info->pdev, "failed to add opp: %luHZ-%luUV\n",
					freq, volt);
				return ret;
			}
		}
		if (freq / 1000 > clu->temp_max_freq)
			clu->temp_max_freq = freq / 1000;

		num -= 2;
		dev_dbg(info->pdev, "add opp: %luHZ-%luUV\n", freq, volt);
	}

	rate = (unsigned long)clu->temp_max_freq * 1000 + 1;
	while (!IS_ERR(opp = dev_pm_opp_find_freq_ceil(info->cpu_dev, &rate))) {
		dev_pm_opp_put(opp);
		dev_pm_opp_remove(info->cpu_dev, rate);
		rate++;
	}

	ret = sprd_cpufreq_arch_update(info);
	if (ret) {
		dev_err(info->pdev, "failed to update hw dvfs parameters\n");
		return ret;
	}

	strcpy(clu->pre_opp_name, clu->opp_name);

	return 0;
}

static
int sprd_locate_opp_table_by_temp(struct sprd_cpufreq_info *info,
				  int temp_now, char *temp_opp_name)
{
	struct sprd_cpu_cluster_info *clu;
	int num, temp_threshold, temp_index = -1;

	if (!info) {
		pr_err("cpufreq info is not initialized\n");
		return -EINVAL;
	}

	clu = info->pcluster;
	if (!clu) {
		dev_err(info->pdev, "cpu cluster info for cpu%d is not initialized\n",
			info->cpu_id);
		return -EINVAL;
	}

	for (num = 0; num < clu->temp_threshold_max; num++) {
		temp_threshold = clu->temp_list[num];

		if (temp_now >= temp_threshold) {
			temp_index = num;
			clu->curr_temp_threshold = temp_threshold;
			sprintf(temp_opp_name, "-%d", temp_threshold);
		} else {
			/* Use the default opp table */
			strcpy(temp_opp_name, "");
			clu->curr_temp_threshold = 0;
		}
	}

	dev_dbg(info->pdev, "get the highest themperature threshold: %d\n",
		clu->curr_temp_threshold);

	clu->temp_bottom = temp_index < 0 ?  clu->temp_min :
		clu->temp_list[temp_index];
	clu->temp_top = (temp_index + 1) >= clu->temp_threshold_max ?
		clu->temp_max : clu->temp_list[temp_index + 1];

	dev_dbg(info->pdev, "temp_threshold_num: %d, temp_bottom: %d temp top: %d\n",
		clu->temp_threshold_max, clu->temp_bottom, clu->temp_top);

	return 0;
}

static int sprd_opp_select(struct sprd_cpufreq_info *info, int temp_now)
{
	struct sprd_cpu_cluster_info *clu;
	char temp_opp_name[8] = "";
	int ret;

	if (!info) {
		pr_err("cpufreq info is not initialized\n");
		return -EINVAL;
	}

	clu = info->pcluster;
	if (!clu) {
		dev_err(info->pdev, "cpu cluster info for cpu%d is not initialized\n",
			info->cpu_id);
		return -EINVAL;
	}

	ret = sprd_locate_opp_table_by_temp(info, temp_now, temp_opp_name);
	if (ret) {
		dev_err(info->pdev, "error in updating opp table\n");
		return ret;
	}

	if (!strcmp(temp_opp_name, ""))
		strcpy(clu->opp_name, clu->default_opp_name);
	else
		strcat(clu->opp_name, temp_opp_name);

	dev_dbg(info->pdev, "opp name after locating by current temp: %s\n",
		clu->opp_name);

	ret = sprd_dev_pm_opp_table_update(info);
	if (ret)
		dev_err(info->pdev, "failed to update opp table by temp\n");

	return ret;
}

unsigned int sprd_cpufreq_update_opp_normal(int cpu, int temp_now)
{
	struct sprd_cpufreq_info *info;
	struct sprd_cpu_cluster_info *clu;
	int ret = 0;

	info = sprd_cpufreq_info_lookup(cpu);
	if (!info) {
		pr_err("cpufreq info for cpu%d is not initialized\n", cpu);
		return -EINVAL;
	}

	clu = info->pcluster;
	if (!clu) {
		dev_err(info->pdev, "cpu cluster info for cpu%d is not initialized\n",
			cpu);
		return -EINVAL;
	}

	if (clu->cpu_bin == INVALID_CPU_BIN)
		return 0;

	if (!clu->temp_threshold_defined) /* ignore temperature influence */
		return 0;

	if (!mutex_trylock(&clu->opp_mutex))
		return 0;

	dev_dbg(info->pdev, "current cpu: %d current_temp: %d\n",
		info->cpu_id, temp_now);

	clu->temp_now = temp_now;

	if (!(clu->online && clu->temp_threshold_max > 0))
		goto out;

	temp_now = temp_now / 1000;
	if (temp_now <= clu->temp_min || temp_now >= clu->temp_max)
		goto out;

	if (temp_now < clu->temp_bottom && !clu->temp_fall_time)
		clu->temp_fall_time = jiffies + clu->freq_temp_fall_hz;

	if (temp_now >= clu->temp_bottom)
		clu->temp_fall_time = 0;

	if (!(temp_now >= clu->temp_top ||
	    (clu->temp_fall_time && time_after(jiffies, clu->temp_fall_time))))
		goto out;

	if (temp_now >= clu->temp_top)
		dev_dbg(info->pdev, "temp_now(%d) is higher than temp_top(%d),so switch OPP table to high temp table\n",
			clu->temp_now, clu->temp_top * 1000);

	if (clu->temp_fall_time && time_after(jiffies, clu->temp_fall_time))
		dev_dbg(info->pdev, "temp_now(%d) is lower than temp_bottom(%d), so switch OPP table to low temp table\n",
			clu->temp_now, clu->temp_bottom * 1000);

	clu->temp_fall_time = 0;

	ret = sprd_opp_select(info, temp_now);
	if (ret)
		ret = 0;
	else
		ret = clu->temp_max_freq;

out:
	mutex_unlock(&clu->opp_mutex);

	return ret;
}

static int sprd_cpu_soc_version_parse(struct sprd_cpufreq_info *info)
{
	struct sprd_cpu_cluster_info *clu;
	struct device_node *hwf;
	const char *value;
	char str[30];

	if (!info || !info->pcluster) {
		pr_err("invalid inputted parameters\n");
		return -EINVAL;
	}

	clu = info->pcluster;

	if (!info->cpufreq_np) {
		dev_err(info->pdev, "cpu device or cluster info is missing\n");
		return -EINVAL;
	}

	if (!of_property_read_bool(info->cpufreq_np, "sprd,multi-version")) {
		dev_info(info->pdev, "cpu cluster has no different versions\n");
		strcpy(clu->cpu_diff_ver, "");
		return 0;
	}

	/*
	 * Note: We distinguish cpu as 3 kinds in UMS512, t610/t618/t610s
	 * you can distinguish other types here
	 */
	hwf = of_find_node_by_path("/hwfeature/auto");
	if (IS_ERR_OR_NULL(hwf)) {
		pr_err("NO hwfeature/auto node found\n");
		return PTR_ERR(hwf);
	}

	value = of_get_property(hwf, "efuse", NULL);
	if (strcmp(value, "T610") && strcmp(value, "T618") &&
	    strcmp(value, "T700") && strcmp(value, "T610S") &&
	    strcmp(value, "T606") && strcmp(value, "T612") &&
	    strcmp(value, "T616")) {
		dev_err(info->pdev, "the cpu version defined is error(%s)\n",
			clu->cpu_diff_ver);
		return 0;
	}

	strcpy(clu->cpu_diff_ver, value);

	dev_info(info->pdev, "the cpu version: %s\n", clu->cpu_diff_ver);

	sprintf(str, "-%s", clu->cpu_diff_ver);
	strcat(clu->opp_name, str);
	strcat(clu->default_opp_name, str);

	return 0;
}

static int sprd_cpu_binning_parse(struct sprd_cpufreq_info *info)
{
	struct sprd_cpu_cluster_info *clu;
	char str[30], *temp;
	int ret = 0;

	if (!info || !info->pcluster) {
		pr_err("invalid inputted parameters\n");
		return -EINVAL;
	}

	clu = info->pcluster;

	if (!info->cpufreq_np || !clu->bin_prop_name) {
		dev_err(info->pdev, "cpu device or cluster info is missing\n");
		return -EINVAL;
	}

	dev_dbg(info->pdev, "opp name before cpu bin parsed: %s\n",
		clu->opp_name);

	ret = of_property_match_string(info->cpufreq_np, "nvmem-cell-names",
				       clu->bin_prop_name);
	if (ret == -EINVAL) { /* No definition is allowed */
		dev_warn(info->pdev, "Warning: no '%s' appointed\n",
			 clu->bin_prop_name);
		clu->cpu_bin = INVALID_CPU_BIN;
		clu->cpubin_str = "";
		ret = 0;
	} else {
		ret = sprd_nvmem_info_read(info->cpufreq_np, clu->bin_prop_name,
					   &clu->cpu_bin);
		if (ret) {
			dev_err(info->pdev, "error in reading cpu bin value\n");
			goto out;
		}

		if (clu->cpu_bin > clu->max_cpu_bin) {
			dev_err(info->pdev, "invalid cpu bin value(%d)\n",
				clu->cpu_bin);
			ret = -EINVAL;
			goto out;
		}

		if (!clu->cpu_bin) {
			dev_warn(info->pdev, "Warning: no cpu bin value set\n");
			clu->cpu_bin = INVALID_CPU_BIN;
			clu->cpubin_str = "";
			ret = 0;
		} else {
			dev_dbg(info->pdev, "cpu bin: %d\n", clu->cpu_bin);

			ret = cpubin2str(clu->cpu_bin, &temp);
			if (ret)
				goto out;
			clu->cpubin_str = temp;
			sprintf(str, "-%s", temp);
			strcat(clu->opp_name, str);
			strcat(clu->default_opp_name, str);
		}
	}

	dev_dbg(info->pdev, "opp name after cpu bin parsed: %s\n",
		clu->opp_name);

out:
	of_node_put(info->cpufreq_np);

	return ret;
}

static int sprd_temp_threshold_parse(struct sprd_cpufreq_info *info)
{
	struct sprd_cpu_cluster_info *clu;
	const char *name;
	int num, i;
	int ret = 0;

	if (!info) {
		pr_err("invalid inputted parameters\n");
		return -EINVAL;
	}

	clu = info->pcluster;
	if (!clu || !clu->temp_threshold_name) {
		dev_err(info->pdev, "cpu cluster info is not initialized\n");
		return -EINVAL;
	}

	name = clu->temp_threshold_name;

	if (!of_find_property(info->cpufreq_np, name, &num)) {
		dev_warn(info->pdev, "warning: no %s temp thresholds found\n",
			 name);
		ret = 0;	/* No definition is allowed */
		clu->temp_threshold_defined = false;
		goto out;
	}

	num = num / sizeof(u32);
	if (num > TEMP_THRESHOLD_NUM) {
		dev_err(info->pdev, "the temp threshold num is overflowing\n");
		ret = -EINVAL;
		goto out;
	}

	clu->temp_threshold_max = 0;
	clu->temp_threshold_defined = true;
	clu->temp_min = SPRD_CPUFREQ_TEMP_MIN;
	clu->temp_max = SPRD_CPUFREQ_TEMP_MAX;
	clu->curr_temp_threshold = 0;

	dev_dbg(info->pdev, "temperature threshold number: %d\n", num);

	for (i = 0; i < num; i++) {
		ret = of_property_read_u32_index(info->cpufreq_np, name, i,
						 &clu->temp_list[i]);
		if (ret) {
			dev_err(info->pdev, "failed to get temp threshold\n");
			goto out;
		}

		clu->temp_threshold_max++;
		dev_dbg(info->pdev, "threshold[%d]:%d\n", i, clu->temp_list[i]);
	}

out:
	of_node_put(info->cpufreq_np);

	return ret;
}

static
int sprd_hardware_cpufreq_set_target_index(struct cpufreq_policy *policy,
					   unsigned int index)
{
	struct sprd_cpufreq_info *info;
	struct cpudvfs_device *pdev;
	struct cpudvfs_phy_ops *driver;
	int ret;

	info = policy->driver_data;
	if (IS_ERR(info))
		return PTR_ERR(info);

	pdev = info->parchdev;
	if (!pdev) {
		dev_err(info->pdev, "no dvfs device found\n");
		return -ENODEV;
	}

	driver = pdev->phy_ops;
	if (!driver || !driver->target_set) {
		dev_err(info->pdev, "no way to operate dvfs device\n");
		return -EINVAL;
	}

	if (!info->pcluster) {
		dev_err(info->pdev, "cluster info for cpu%d is missing\n",
			policy->cpu);
		return -EINVAL;
	}

	/*
	 * Should to set frequency after boot_done_timestamp ticks since the
	 * cpufreq device has been probed
	 */

	if (boost_mode_flag) {
		if (!time_after(jiffies, boot_done_timestamp))
			return 0;
		boost_mode_flag = 0;
		pr_info("Disables boost it is %lu seconds after boot up\n",
			SPRD_CPUFREQ_BOOST_DURATION / HZ);
	}

	mutex_lock(&info->pcluster->opp_mutex);
	ret = driver->target_set(pdev, info->clu_id, index);
	mutex_unlock(&info->pcluster->opp_mutex);

	if (ret) {
		dev_err(info->pdev, "failed to set target index%d for cpu%d\n",
			index, info->cpu_id);
		return ret;
	}

	arch_set_freq_scale(policy->related_cpus,
			    policy->freq_table[index].frequency,
			    policy->cpuinfo.max_freq);
	return 0;
}

static
int sprd_hw_dvfs_enable(struct device *dev, struct cpudvfs_device *pdev,
			u32 clu_id)
{
	struct cpudvfs_phy_ops *driver;

	if (!pdev) {
		dev_err(dev, "no cpu dvfs device found\n");
		return -ENODEV;
	}

	driver = pdev->phy_ops;
	if (!driver || !driver->dvfs_enable) {
		dev_err(dev, "no way to operate cpu dvfs device\n");
		return -EINVAL;
	}

	return driver->dvfs_enable(pdev, clu_id, true);
}

static int sprd_cpu_cluster_static_init(struct device *pdev,
					struct sprd_cpu_cluster_info *clu)
{
	if (!clu) {
		dev_err(pdev, "the cluster to access is null\n");
		return -EINVAL;
	}

	clu->bin_prop_name = "dvfs_bin";
	clu->temp_threshold_name = "temp-threshold";
	clu->max_cpu_bin = 4;
	clu->freq_temp_fall_hz = 2 * HZ;
	strcpy(clu->opp_name, VENDOR_NAME);
	strcpy(clu->default_opp_name, VENDOR_NAME);
	strcpy(clu->pre_opp_name, "");
	strcpy(clu->cpu_diff_ver, "");
	mutex_init(&clu->opp_mutex);

	return 0;
}

static int sprd_hardware_cpufreq_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *freq_table;
	struct sprd_cpufreq_info *info;
	int ret;
	unsigned int cpumask;
	cpumask_t cluster_cpumask;

	info = sprd_cpufreq_info_lookup(policy->cpu);
	if (IS_ERR(info))
		return PTR_ERR(info);

	/* CPUs in the same cluster share same clock and power domain */
	if (!info->cpufreq_np) {
		dev_err(info->pdev, "cpu device or cluster info is missing\n");
		return -EINVAL;
	}
	ret = of_property_read_u32(info->cpufreq_np, "cpufreq-cluster-cpumask",
			     &cpumask);
	if (ret) {
		dev_err(info->pdev, "cpufreq cluster cpumask read fail");
		return ret;
	}
	cluster_cpumask.bits[0] = (unsigned long)cpumask;
	cpumask_or(policy->cpus, policy->cpus, &cluster_cpumask);
	cpumask_copy(&info->cpus, policy->cpus);

	if (!info->pcluster) { /* Just do once for every policy */
		struct sprd_cpu_cluster_info *clu;

		clu = kzalloc(sizeof(*clu), GFP_KERNEL);
		if (!clu)
			return -ENOMEM;

		info->pcluster = clu;

		ret = sprd_cpu_cluster_static_init(info->pdev, clu);
		if (ret)
			return ret;

		ret = sprd_cpu_soc_version_parse(info);
		if (ret)
			return ret;

		ret = sprd_cpu_binning_parse(info);
		if (ret)
			return ret;

		ret = sprd_temp_threshold_parse(info);
		if (ret)
			return ret;

		ret = sprd_related_cpus_cluster_alloc(info, policy);
		if (ret)
			return ret;
	}

	if (info->policy_trans)
		policy->transition_delay_us = info->policy_trans;

	mutex_lock(&info->pcluster->opp_mutex);
	ret = sprd_dev_pm_opp_table_update(info);
	if (ret) {
		dev_err(info->pdev, "failed to update opp table\n");
		mutex_unlock(&info->pcluster->opp_mutex);
		return ret;
	}

	ret = dev_pm_opp_init_cpufreq_table(info->cpu_dev, &freq_table);
	if (ret) {
		dev_err(info->pdev, "error in initializing frequency table (%d)\n",
			ret);
		mutex_unlock(&info->pcluster->opp_mutex);
		goto free_opp;
	}

	policy->freq_table = freq_table;
	policy->suspend_freq = freq_table[0].frequency;
	info->pcluster->online = true;
	mutex_unlock(&info->pcluster->opp_mutex);

	policy->dvfs_possible_from_any_cpu = true;
	dev_pm_opp_of_register_em(policy->cpus);
	policy->driver_data = info;

#if defined(CONFIG_DEBUG_FS)
	ret = sprd_cpufreq_debugfs_add(info);
	if (ret) {
		dev_err(info->pdev, "error in adding debug nodes\n");
		goto free_table;
	}
#endif

	if (!info->pcluster->arch_dvfs_enabled) {
		ret = sprd_hw_dvfs_enable(info->pdev, info->parchdev,
					  info->clu_id);
		if (ret)
			goto free_table;
		info->pcluster->arch_dvfs_enabled = true;
	}

	return 0;

free_table:
	if (policy->freq_table)
		dev_pm_opp_free_cpufreq_table(info->cpu_dev,
					      &policy->freq_table);
free_opp:
	dev_pm_opp_of_remove_table(info->cpu_dev);
	return ret;
}

static int sprd_hardware_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct sprd_cpufreq_info *info = policy->driver_data;
	__maybe_unused int ret;

	if (IS_ERR(info))
		return PTR_ERR(info);

	mutex_lock(&info->pcluster->opp_mutex);
	dev_pm_opp_free_cpufreq_table(info->cpu_dev, &policy->freq_table);
	dev_pm_opp_of_remove_table(info->cpu_dev);

	policy->driver_data = NULL;
	info->pcluster->online = false;
	strcpy(info->pcluster->pre_opp_name, "");
	mutex_unlock(&info->pcluster->opp_mutex);

#if defined(CONFIG_DEBUG_FS)
	ret = sprd_cpufreq_debugfs_remove(info);
	if (ret) {
		dev_err(info->pdev, "failed to remove debugfs node\n");
		return ret;
	}
#endif

	return 0;
}

static int sprd_hardware_cpufreq_table_verify(struct cpufreq_policy_data
					      *policy_data)
{
	return cpufreq_generic_frequency_table_verify(policy_data);
}

static unsigned int sprd_hardware_cpufreq_get(unsigned int cpu)
{
	struct sprd_cpufreq_info *info;
	struct cpudvfs_device *pdev;
	struct cpudvfs_phy_ops *driver;

	info = sprd_cpufreq_info_lookup(cpu);
	if (IS_ERR(info))
		return 0;

	pdev = info->parchdev;
	if (!pdev) {
		dev_err(info->pdev, "no cpu dvfs device found.\n");
		return  0;
	}
	driver = pdev->phy_ops;
	if (!driver || !driver->freq_get) {
		dev_err(info->pdev, "no way to access cpu dvfs device\n");
		return 0;
	}

	return driver->freq_get(pdev, info->clu_id);
}

static int sprd_hardware_cpufreq_suspend(struct cpufreq_policy *policy)
{
	struct sprd_cpufreq_info *info;
	struct cpudvfs_device *pdev;
	struct cpudvfs_phy_ops *driver;
	int cpu;

	if (!strcmp(policy->governor->name, "userspace")) {
		pr_info("Do nothing for governor-%s\n",
			policy->governor->name);
		return 0;
	}

	/*
	 * If suspend occus during the boost, cancel the boost and
	 * actively switch frequency to suspend freq.
	 */
	if (boost_mode_flag) {
		boost_mode_flag = 0;

		/* Current policy switch to suspend freq */
		info = policy->driver_data;
		pdev = info->parchdev;
		driver = pdev->phy_ops;
		mutex_lock(&info->pcluster->opp_mutex);
		driver->target_set(pdev, info->clu_id, 0);
		mutex_unlock(&info->pcluster->opp_mutex);

		/* The other policy switch to suspend freq */
		cpu = cpumask_next_zero(-1, policy->cpus);
		info = sprd_cpufreq_info_lookup(cpu);
		mutex_lock(&info->pcluster->opp_mutex);
		driver->target_set(pdev, info->clu_id, 0);
		mutex_unlock(&info->pcluster->opp_mutex);
	}

	return cpufreq_generic_suspend(policy);
}

static int sprd_hardware_cpufreq_resume(struct cpufreq_policy *policy)
{
	if (!strcmp(policy->governor->name, "userspace")) {
		pr_info("Do nothing for governor-%s\n", policy->governor->name);
		return 0;
	}

	return cpufreq_generic_suspend(policy);
}

static struct cpufreq_driver sprd_hardware_cpufreq_driver = {
	.name = "sprd-cpufreq",
	.flags = CPUFREQ_STICKY
			| CPUFREQ_NEED_INITIAL_FREQ_CHECK
			| CPUFREQ_HAVE_GOVERNOR_PER_POLICY
			| CPUFREQ_IS_COOLING_DEV,
	.init = sprd_hardware_cpufreq_init,
	.exit = sprd_hardware_cpufreq_exit,
	.verify = sprd_hardware_cpufreq_table_verify,
	.target_index = sprd_hardware_cpufreq_set_target_index,
	.get = sprd_hardware_cpufreq_get,
	.suspend = sprd_hardware_cpufreq_suspend,
	.resume = sprd_hardware_cpufreq_resume,
	.attr = cpufreq_generic_attr,
};

static int sprd_cpufreq_info_init(struct sprd_cpufreq_info *info, int cpu)
{
	struct device_node *cpu_np, *dev_np, *cpufreq_np;
	struct device *cpu_dev;
	struct nvmem_cell *cell;
	struct platform_device *pdev;
	int ret;
	unsigned int policy_trans_delay;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		dev_err(info->pdev, "failed to get cpu%d device\n", cpu);
		return -ENODEV;
	}

	cpu_np = of_node_get(cpu_dev->of_node);
	if (!cpu_np) {
		dev_err(info->pdev, "failed to get cpu%d node\n", cpu);
		return -ENODEV;
	}

	cpufreq_np = of_parse_phandle(cpu_np, "cpufreq-data-v1", 0);
	if (!cpufreq_np) {
		dev_err(info->pdev, "no cpufreq-data-v1 node found\n");
		ret = -EINVAL;
		goto cpu_np_put;
	}

	if (!of_property_read_u32(cpufreq_np, "transition_delay_us",
			     &policy_trans_delay))
		info->policy_trans = policy_trans_delay;

	dev_np = of_parse_phandle(cpufreq_np, "sprd,hw-dvfs-device", 0);
	if (!dev_np) {
		dev_err(info->pdev, "no associated hw dvfs device appointed\n");
		ret = -EINVAL;
		goto cpufreq_np_put;
	}

	pdev = of_find_device_by_node(dev_np);
	if (IS_ERR_OR_NULL(pdev)) {
		ret = -EPROBE_DEFER;
		goto dev_np_put;
	}

	info->cpudvfs_dev_np = dev_np;
	info->cpu_dev = cpu_dev;
	info->cpu_np = cpu_np;
	info->cpufreq_np = cpufreq_np;
	info->clu_id = topology_physical_package_id(cpu);
	info->cpu_id = cpu;

	info->parchdev = (struct cpudvfs_device *)platform_get_drvdata(pdev);
	if (!info->parchdev) {
		ret = -EPROBE_DEFER;
		goto dev_np_put;
	}

	cell = of_nvmem_cell_get(cpufreq_np, "dvfs_bin");
	if (IS_ERR(cell)) {
		if (PTR_ERR(cell) == -EPROBE_DEFER)
			ret = -EPROBE_DEFER;
		else
			ret = 0;	/* No definition is allowed */
	} else {
		nvmem_cell_put(cell);
		ret = 0;
	}

	info->update_opp = sprd_cpufreq_update_opp_normal;

dev_np_put:
	of_node_put(dev_np);
cpufreq_np_put:
	of_node_put(cpufreq_np);
cpu_np_put:
	of_node_put(cpu_np);

	return ret;
}

static int sprd_hardware_cpufreq_probe(struct platform_device *pdev)
{
	struct sprd_cpufreq_info *info;
	int ret, cpu;

	boot_done_timestamp = jiffies + SPRD_CPUFREQ_BOOST_DURATION;

	global_cpufreq_info = devm_kcalloc(&pdev->dev, nr_cpu_ids,
					   sizeof(struct sprd_cpufreq_info),
					   GFP_KERNEL);
	if (!global_cpufreq_info)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		info = sprd_cpufreq_info_lookup(cpu);
		if (IS_ERR(info))
			return PTR_ERR(info);

		info->pdev = &pdev->dev;
		ret = sprd_cpufreq_info_init(info, cpu);
		if (ret)
			return ret;
	}

	ret = cpufreq_register_driver(&sprd_hardware_cpufreq_driver);
	if (ret)
		dev_err(&pdev->dev, "Failed to reigister cpufreq driver\n");
	else
		dev_info(&pdev->dev, "Succeeded to register cpufreq driver\n");

	return 0;
}

static int sprd_hardware_cpufreq_remove(struct platform_device *pdev)
{
	return cpufreq_unregister_driver(&sprd_hardware_cpufreq_driver);
}

static struct platform_driver sprd_hardware_cpufreq_platdrv = {
	.driver = {
		.name	= "sprd-hw-cpufreq",
		.of_match_table = sprd_hardware_cpufreq_of_match,
	},
	.probe		= sprd_hardware_cpufreq_probe,
	.remove		= sprd_hardware_cpufreq_remove,
};

static int __init sprd_hardware_cpufreq_common_init(void)
{
	return platform_driver_register(&sprd_hardware_cpufreq_platdrv);
}

static void __exit sprd_hardware_cpufreq_common_exit(void)
{
	return platform_driver_unregister(&sprd_hardware_cpufreq_platdrv);
}

device_initcall(sprd_hardware_cpufreq_common_init);
module_exit(sprd_hardware_cpufreq_common_exit);

MODULE_DESCRIPTION("sprd hardware cpufreq driver");
MODULE_LICENSE("GPL v2");
