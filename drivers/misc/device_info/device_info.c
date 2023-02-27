#include "../../../../fs/proc/internal.h"
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/device_info.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVINFO_NAME "devinfo"
#define INFO_BUF_LEN 64
/**for definfo log**/
#define log_fmt(fmt) "[line:%d][module:%s][%s] " fmt

#define DEVINFO_ERR(a, arg...)                                           \
	do {                                                                 \
		printk(KERN_NOTICE log_fmt(a), __LINE__, DEVINFO_NAME, __func__, \
			   ##arg);                                                   \
	} while (0)

#define DEVINFO_MSG(a, arg...)                                                 \
	do {                                                                       \
		printk(KERN_INFO log_fmt(a), __LINE__, DEVINFO_NAME, __func__, ##arg); \
	} while (0)

/**definfo log end**/
static char fuse_flag[5];
static char devinfo_prj_name[10] = "unknow";
static char *devinfo_pcb_version;
char pcb_version[5];
char dtsi_name_char[10];
int dtsi_name = -1;
int temp_value = -1;
typedef struct{
	short board_id_num;
	char *pcba_type_name;
} BOARD_TYPE_TABLE;

enum{
	BOARD_ZERO,
	BOARD_ONE,
	BOARD_TWO,
	BOARD_THREE,
	BOARD_FOUR,
	BOARD_FIVE,
	BOARD_SIX,
	BOARD_SEVEN,
	BOARD_MAX,
};


static BOARD_TYPE_TABLE pcba_type_table_nico_go[] = {
		{ .board_id_num = BOARD_ZERO,  .pcba_type_name = "NULL"},
		{ .board_id_num = BOARD_ONE,   .pcba_type_name = "T0"},
		{ .board_id_num = BOARD_TWO,   .pcba_type_name = "EVT"},
		{ .board_id_num = BOARD_THREE, .pcba_type_name = "DVT"},
		{ .board_id_num = BOARD_FOUR,  .pcba_type_name = "PVT"},
		{ .board_id_num = BOARD_FIVE,  .pcba_type_name = "EVB"},
		{ .board_id_num = BOARD_SIX,   .pcba_type_name = "NULL"},
		{ .board_id_num = BOARD_SEVEN, .pcba_type_name = "NULL"},
		{ .board_id_num = BOARD_MAX,   .pcba_type_name = "NULL"},
};

static BOARD_TYPE_TABLE pcba_type_table_nico[] = {
		{ .board_id_num = BOARD_ZERO,  .pcba_type_name = "EVB"},
		{ .board_id_num = BOARD_ONE,   .pcba_type_name = "T0-1"},
		{ .board_id_num = BOARD_TWO,   .pcba_type_name = "T0-2"},
		{ .board_id_num = BOARD_THREE, .pcba_type_name = "EVT1"},
		{ .board_id_num = BOARD_FOUR,  .pcba_type_name = "DVT"},
		{ .board_id_num = BOARD_FIVE,  .pcba_type_name = "PVT"},
		{ .board_id_num = BOARD_SIX,   .pcba_type_name = "MP-0"},
		{ .board_id_num = BOARD_SEVEN, .pcba_type_name = "MP-1"},
		{ .board_id_num = BOARD_MAX,   .pcba_type_name = "NULL"},
};

static BOARD_TYPE_TABLE pcba_type_table_nicky[] = {
		{ .board_id_num = BOARD_ZERO,  .pcba_type_name = "EVB"},
		{ .board_id_num = BOARD_ONE,   .pcba_type_name = "T0-1"},
		{ .board_id_num = BOARD_TWO,   .pcba_type_name = "EVT1"},
		{ .board_id_num = BOARD_THREE, .pcba_type_name = "EVT2"},
		{ .board_id_num = BOARD_FOUR,  .pcba_type_name = "DVT"},
		{ .board_id_num = BOARD_FIVE,  .pcba_type_name = "PVT"},
		{ .board_id_num = BOARD_SIX,   .pcba_type_name = "MP-0"},
		{ .board_id_num = BOARD_SEVEN, .pcba_type_name = "MP-1"},
		{ .board_id_num = BOARD_MAX,   .pcba_type_name = "NULL"},
};

static struct of_device_id devinfo_id[] = {
	{
		.compatible = "oplus, device_info",
	},
	{},
};

struct devinfo_data {
	struct platform_device *devinfo;
	struct pinctrl *pinctrl;
	int hw_id1_gpio;
	int hw_id2_gpio;
	int hw_id3_gpio;
	int sub_hw_id1;
	int sub_hw_id2;
	int main_hw_id5;
	int main_hw_id6;
	int sub_board_id;
	int sdcard_gpio;
	struct pinctrl_state *hw_sub_id_sleep;
	struct pinctrl_state *hw_sub_id_active;
	struct pinctrl_state *hw_main_id5_active;
	struct pinctrl_state *hw_main_id6_active;
	int ant_select_gpio;
	struct manufacture_info sub_mainboard_info;
};

static struct devinfo_data *dev_info;
static struct proc_dir_entry *parent = NULL;
char buff[128];
char sensor_vendor[3][80];
char fuse_vendor[80];

struct sensor_devinfo sensordevinfo[] = {
	{"light_stk3a5x", "SensorTek"},
	{"light_mn78911", "mn"},
	{"accelerometer_mc3416", "MCUBE"},
	{"accelerometer_sc7a20", "SLANWEI"},
	{"magnetic_mmc5603", "MMC"},
	{"magnetic_akm09919", "AKM"},
};

static void hq_parse_sensor_devinfo(int type, char ic_name[], char vendor[])
{
	sprintf(sensor_vendor[type], "Device version:\t\t%s\nDevice manufacture:\t\t%s\n",
			ic_name, vendor);
}

void hq_register_sensor_info(int type, char ic_name[])
{
	int i;
	if (type >= 0 && type < 3) {
		for (i = 0; i < sizeof(sensordevinfo) / sizeof(struct sensor_devinfo); i++) {
			if (!strncmp(ic_name, sensordevinfo[i].ic_name, strlen(ic_name))) {
				hq_parse_sensor_devinfo(type, sensordevinfo[i].ic_name, sensordevinfo[i].vendor_name);
				break;
			}
		}
	}
	return;
}
EXPORT_SYMBOL(hq_register_sensor_info);

static const char *const devinfo_proc_list[] = {
	"Sensor_accel",
	"Sensor_alsps",
	"Sensor_gyro",
	"Sensor_msensor",
	"Fuse_flag",
};

HQ_DEVINFO_ATTR(Sensor_accel, "%s", sensor_vendor[ACCEL_HQ]);
HQ_DEVINFO_ATTR(Sensor_alsps, "%s", sensor_vendor[ALSPS_HQ]);
HQ_DEVINFO_ATTR(Sensor_gyro, "%s", sensor_vendor[ACCEL_HQ]);
HQ_DEVINFO_ATTR(Sensor_msensor, "%s", sensor_vendor[MSENSOR_HQ]);
HQ_DEVINFO_ATTR(Fuse_flag, "%s", fuse_vendor);

static const struct file_operations *proc_fops_list[] = {
	&Sensor_accel_fops,
	&Sensor_alsps_fops,
	&Sensor_gyro_fops,
	&Sensor_msensor_fops,
	&Fuse_flag_fops,
};

static unsigned int atoi(const char *str)
{
	unsigned int value = 0;

	while (*str >= '0' && *str <= '9') {
		value *= 10;
		value += *str - '0';
		str++;
	}
	return value;
}

static int fuse_flag_setup(void)
{
	struct device_node *cmdline_node;
	const char *cmd_line, *temp_name;
	int rc = 0;

	cmdline_node = of_find_node_by_path("/chosen");
	rc = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!rc) {
		temp_name = strstr(cmd_line, "isfuse=");
		if (temp_name) {
			sscanf(temp_name, "isfuse=%s", fuse_flag);
			pr_err("%s: isfuse=%s", __func__, fuse_flag);
		} else {
			pr_err("%s: isfuse read error", __func__);
		}
	}

	return rc;
}

static int op_prj_name_setup(void)
{
	struct device_node *cmdline_node;
	const char *cmd_line, *temp_name;
	int rc = 0;

	cmdline_node = of_find_node_by_path("/chosen");
	rc = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!rc) {
		temp_name = strstr(cmd_line, "prj_name=");
		if (temp_name) {
			sscanf(temp_name, "prj_name=%s", devinfo_prj_name);
			pr_err("%s: prj_name=%s", __func__, devinfo_prj_name);
		} else {
			pr_err("%s: prj_name read error", __func__);
		}
	}

	return rc;
}

static int op_pcb_version_setup(void)
{
	struct device_node *cmdline_node;
	const char *cmd_line, *temp_name;
	int rc = 0;

	cmdline_node = of_find_node_by_path("/chosen");
	rc = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!rc) {
		temp_name = strstr(cmd_line, "pcb_version=");
		if (temp_name) {
			sscanf(temp_name, "pcb_version=%s", pcb_version);
			temp_value = atoi(pcb_version);
			pr_err("%s: pcb_version=%s, temp_value=%d", __func__, pcb_version, temp_value);
		} else {
			pr_err("%s: pcb_version read error", __func__);
		}
	}

	return rc;
}

static int op_pcb_dtsi_name_setup(void)
{
	struct device_node *cmdline_node;
	const char *cmd_line, *temp_name;
	int rc = 0;

	cmdline_node = of_find_node_by_path("/chosen");
	rc = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!rc) {
		temp_name = strstr(cmd_line, "dtsi_name=");
		if (temp_name) {
			sscanf(temp_name, "dtsi_name=%s", dtsi_name_char);
			dtsi_name = atoi(dtsi_name_char);
			pr_err("%s: dtsi_name_char=%s, dtsi_name=%d", __func__, dtsi_name_char, dtsi_name);
		} else {
			pr_err("%s: dtsi_name read error", __func__);
		}
	}

	return rc;
}

static void *device_seq_start(struct seq_file *s, loff_t *pos)
{
	static unsigned long counter = 0;
	if (*pos == 0) {
		return &counter;
	} else {
		*pos = 0;
		return NULL;
	}
}

static void *device_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	return NULL;
}

static void device_seq_stop(struct seq_file *s, void *v) { return; }

static int device_seq_show(struct seq_file *s, void *v)
{
	struct proc_dir_entry *pde = s->private;
	struct manufacture_info *info = pde->data;
	if (info) {
		seq_printf(s, "Device version:\t\t%s\nDevice manufacture:\t\t%s\n",
				   info->version, info->manufacture);
		if (info->fw_path)
			seq_printf(s, "Device fw_path:\t\t%s\n", info->fw_path);
	}
	return 0;
}

static struct seq_operations device_seq_ops = {.start = device_seq_start,
											   .next = device_seq_next,
											   .stop = device_seq_stop,
											   .show = device_seq_show};

static int device_proc_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &device_seq_ops);
	pr_err("%s is called\n", __func__);

	if (!ret) {
		struct seq_file *sf = file->private_data;
		sf->private = PDE(inode);
	}

	return ret;
}

static const struct file_operations device_node_fops = {
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.open = device_proc_open,
	.owner = THIS_MODULE,
};

int register_device_proc(char *name, char *version, char *manufacture)
{
	struct proc_dir_entry *d_entry;
	struct manufacture_info *info;

	if (!parent) {
		parent = proc_mkdir("devinfo", NULL);
		if (!parent) {
			pr_err("can't create devinfo proc\n");
			return -ENOENT;
		}
	}

	info = kzalloc(sizeof *info, GFP_KERNEL);
	info->version = version;
	info->manufacture = manufacture;
	d_entry = proc_create_data(name, S_IRUGO, parent, &device_node_fops, info);
	if (!d_entry) {
		DEVINFO_ERR("create %s proc failed.\n", name);
		kfree(info);
		return -ENOENT;
	}
	return 0;
}

int register_devinfo(char *name, struct manufacture_info *info)
{
	struct proc_dir_entry *d_entry;
	if (!parent) {
		parent = proc_mkdir("devinfo", NULL);
		if (!parent) {
			pr_err("can't create devinfo proc\n");
			return -ENOENT;
		}
	}

	d_entry = proc_create_data(name, S_IRUGO, parent, &device_node_fops, info);
	if (!d_entry) {
		pr_err("create %s proc failed.\n", name);
		return -ENOENT;
	}
	return 0;
}

static int mainboard_init(void)
{
	if ((0 > temp_value) || (BOARD_MAX <= temp_value)) {
		pr_err("temp_value error %d\n", temp_value);
		return register_device_proc("mainboard", "UNKNOW", devinfo_prj_name);
	}
	if (dtsi_name == 21660) {
		devinfo_pcb_version = pcba_type_table_nico[temp_value].pcba_type_name;
	} else if (dtsi_name == 0x2171A || dtsi_name == 21723) {
		devinfo_pcb_version = pcba_type_table_nicky[temp_value].pcba_type_name;
	} else if ((dtsi_name == 21663) || (dtsi_name == 0x2173B)) {
		devinfo_pcb_version = pcba_type_table_nico_go[temp_value].pcba_type_name;
	} else {
		pr_err("dtsi_name error %d\n", dtsi_name);
		return register_device_proc("mainboard", "DTSI_NAME UNKNOW", devinfo_prj_name);
	}
	return register_device_proc("mainboard", devinfo_pcb_version, devinfo_prj_name);
}

static int subboard_init(struct devinfo_data *const devinfo_data)
{
	int ret = 0;
	struct device_node *np = NULL;
	np = devinfo_data->devinfo->dev.of_node;

	if ((strcmp(devinfo_pcb_version, "DVT") != 0) && (strcmp(devinfo_pcb_version, "PVT") != 0) && (strcmp(devinfo_pcb_version, "MP") != 0)) {
		DEVINFO_MSG("sub-match , before DVT\n");
		ret = register_device_proc("sub_mainboard", "SPRD", "sub-match");
		return -1;
	}

	devinfo_data->sub_board_id = of_get_named_gpio(np, "sub_board-gpio", 0);
	if (devinfo_data->sub_board_id < 0) {
		DEVINFO_ERR("devinfo_data->sub_board_id not specified\n");
		return -1;
	}
	DEVINFO_MSG("sub_board-gpio for match is %d \n", devinfo_data->sub_board_id);

	devinfo_data->sub_mainboard_info.manufacture = kzalloc(INFO_BUF_LEN, GFP_KERNEL);
	if (devinfo_data->sub_mainboard_info.manufacture == NULL) {
		kfree(devinfo_data->sub_mainboard_info.version);
		return -1;
	}

	ret = register_device_proc("sub_mainboard", "SPRD", devinfo_data->sub_mainboard_info.manufacture);
	return ret;
}

static int subboard_verify(struct devinfo_data *const devinfo_data)
{
	int ret = 0;
	int subboard_gpio_value = -1;
	char str_modem_id[5];
	int mainboard_modem_id = -1;
	int subboard_kind = -1;
	struct device_node *cmdline_node;
	const char *cmd_line, *temp_name;

	DEVINFO_MSG("Enter subboard_verify\n");
	if (IS_ERR_OR_NULL(devinfo_data)) {
		DEVINFO_ERR("devinfo_data is NULL\n");
		return -1;
	}

	DEVINFO_MSG("In subboard_verify, sub_board-gpio for match is %d \n", devinfo_data->sub_board_id);
	if (devinfo_data->sub_board_id >= 0) {
		ret = gpio_request(devinfo_data->sub_board_id, "sub_board_id");
		if (ret != 0) {
			DEVINFO_ERR("failed to request sub_board_id gpio\n");
			return -1;
		}
		subboard_gpio_value = gpio_get_value(devinfo_data->sub_board_id);
		DEVINFO_MSG("subboard_gpio_value before set is %d \n", subboard_gpio_value);
		if (subboard_gpio_value < 0) {
			DEVINFO_ERR("subboard_gpio_value before set is error\n");
			gpio_free(devinfo_data->sub_board_id);
			return -1;
		}
	} else {
		DEVINFO_ERR("devinfo_data->sub_board_id not specified\n");
		gpio_free(devinfo_data->sub_board_id);
		return -1;
	}

	if (subboard_gpio_value == 0) {
		subboard_kind = 0; //The asia-pacific version
	} else {
		subboard_kind = 1; //The other version
	}
	DEVINFO_MSG("subboard_kind is %d \n", subboard_kind);

	DEVINFO_ERR("%s: rfboard.id read start", __func__);
	cmdline_node = of_find_node_by_path("/chosen");
	ret = of_property_read_string(cmdline_node, "bootargs", &cmd_line);
	if (!ret) {
		temp_name = strstr(cmd_line, "rfboard.id=");
		if (temp_name) {
			sscanf(temp_name, "rfboard.id=%s", str_modem_id);
			DEVINFO_ERR("%s: rfboard.id=%s", __func__, str_modem_id);
		} else {
			DEVINFO_ERR("%s: rfboard.id read error", __func__);
		}
	}
	if (str_modem_id != 0) {
		mainboard_modem_id = simple_strtol(str_modem_id, NULL, 12);
	} else {
		mainboard_modem_id = 0;
	}
	DEVINFO_ERR("mainboard_modem_id is %d \n", mainboard_modem_id);
	/*
		modem_id = 0  亚太简版
		modem_id = 1  全频段
		modem_id = 2  全频段_越南
		modem_id = 3  全频段_NFC
		modem_id = 4  国际版
	*/

	if (subboard_kind == 0 && mainboard_modem_id == 0) {
		DEVINFO_MSG("sub-match , asia-pacific version\n");
		snprintf(devinfo_data->sub_mainboard_info.manufacture, INFO_BUF_LEN, "sub-match");
		gpio_free(devinfo_data->sub_board_id);
	} else if (subboard_kind == 1 && mainboard_modem_id != 0) {
		DEVINFO_MSG("sub-match , other version\n");
		snprintf(devinfo_data->sub_mainboard_info.manufacture, INFO_BUF_LEN, "sub-match");
		gpio_free(devinfo_data->sub_board_id);
	} else {
		DEVINFO_MSG("sub-unmatch\n");
		snprintf(devinfo_data->sub_mainboard_info.manufacture, INFO_BUF_LEN, "sub-unmatch");
		gpio_free(devinfo_data->sub_board_id);
	}

	return ret;
}

static int devinfo_probe(struct platform_device *pdev)
{
	int ret = 0;

	struct devinfo_data *const devinfo_data =
		devm_kzalloc(&pdev->dev, sizeof(struct devinfo_data), GFP_KERNEL);

	if (IS_ERR_OR_NULL(devinfo_data)) {
		printk("devinfo_data kzalloc failed\n");
		ret = -ENOMEM;
		return ret;
	}

	devinfo_data->devinfo = pdev;
	dev_info = devinfo_data;

	ret = fuse_flag_setup();
	if (ret < 0) {
		pr_err("%s: cmdline isfuse read error\n", __func__);
	}
	ret = op_prj_name_setup();
	if (ret < 0) {
		pr_err("%s: cmdline prj_name read error\n", __func__);
	}
	ret = op_pcb_version_setup();
	if (ret < 0) {
		pr_err("%s: cmdline pcb_version read error\n", __func__);
	}
	ret = op_pcb_dtsi_name_setup();
	if (ret < 0) {
		pr_err("%s: cmdline dtsi_name read error\n", __func__);
	}

	if (!parent) {
		parent = proc_mkdir("devinfo", NULL);
		if (!parent) {
			printk("can't create devinfo proc\n");
			ret = -ENOENT;
		}
	}
	proc_create("devinfo/Sensor_accel", 0444, NULL, proc_fops_list[0]);
	proc_create("devinfo/Sensor_alsps", 0444, NULL, proc_fops_list[1]);
	proc_create("devinfo/Sensor_gyro", 0444, NULL, proc_fops_list[2]);
	proc_create("devinfo/Sensor_msensor", 0444, NULL, proc_fops_list[3]);
	proc_create("devinfo/Fuse_flag", 0444, NULL, proc_fops_list[4]);
	ret = register_device_proc("audio_mainboard", "SPRD", "sub-match");
	if (ret < 0) {
		DEVINFO_ERR("register audio_mainboard failed\n");
	}

	ret = mainboard_init();
	if (ret < 0) {
		DEVINFO_ERR("register mainboard failed\n");
	}

	ret = subboard_init(devinfo_data);
	if (ret < 0) {
		DEVINFO_ERR("register subboard failed\n");
	} else {
		subboard_verify(devinfo_data);
	}

	return ret;
}

static int devinfo_remove(struct platform_device *dev)
{
	remove_proc_entry(DEVINFO_NAME, NULL);
	return 0;
}

static struct platform_driver devinfo_platform_driver = {
	.probe = devinfo_probe,
	.remove = devinfo_remove,
	.driver =
		{
			.name = DEVINFO_NAME,
			.of_match_table = devinfo_id,
		},
};

module_platform_driver(devinfo_platform_driver);

MODULE_DESCRIPTION("oplus device info");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("lzt<linzhengtao@vanyol.com>");
