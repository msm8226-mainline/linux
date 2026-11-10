#ifndef _LINUX_CYTTSP4_CORE_H
#define _LINUX_CYTTSP4_CORE_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#define CYTTSP4_I2C_NAME "cyttsp4-i2c"
#define GET_NUM_TOUCH_RECORDS(x) ((x) & 0x1F)
#define IS_BAD_PKT(x) ((x) & 0x20)
#define CY_NUM_TCH_FIELDS 7
#define CY_NUM_EXT_TCH_FIELDS 3
#define CY_NUM_TOTAL_TCH_FIELDS CY_NUM_TCH_FIELDS + CY_NUM_EXT_TCH_FIELDS
#define CY_OFS_MASK GENMASK(4, 0)
#define CY_BOFS_MASK GENMASK(7, 5)
#define CY_RESOLUTION_MASK GENMASK(15, 0)

enum cyttsp4_tch_abs {	/* for ordering within the extracted touch data array */
	CY_TCH_X,	/* X */
	CY_TCH_Y,	/* Y */
	CY_TCH_P,	/* P (Z) */
	CY_TCH_T,	/* TOUCH ID */
	CY_TCH_E,	/* EVENT ID */
	CY_TCH_O,	/* OBJECT ID */
	CY_TCH_W,	/* SIZE */
	CY_TCH_MAJ,	/* TOUCH_MAJOR */
	CY_TCH_MIN,	/* TOUCH_MINOR */
	CY_TCH_OR,	/* ORIENTATION */
	CY_TCH_NUM_ABS
};

struct cyttsp4_touch {
	int abs[CY_TCH_NUM_ABS];
};

struct cyttsp4_sysinfo_offsets {
	__be16 hst_mode;
	__be16 map_sz;
	__be16 cydata_ofs;
	__be16 test_ofs;
	__be16 pcfg_ofs;
	__be16 opcfg_ofs;
	__be16 ddata_ofs;
	__be16 mdata_ofs;
} __packed;

struct cyttsp4_pcfg {
	u8 electrodes_x;
	u8 electrodes_y;
	__be16 len_x;
	__be16 len_y;
	__be16 res_x;
	__be16 res_y;
	__be16 max_z;
} __packed;

struct cyttsp4_bus_ops {
	u16 bustype;
	int (*write)(struct device *dev, u8 *xfer_buf, u16 addr, u8 length,
			const void *values);
	int (*read)(struct device *dev, u8 *xfer_buf, u16 addr, u8 length,
			void *values);
};

enum cyttsp4_hst_mode_bits {
	CY_HST_TOGGLE      = (1 << 7),
	CY_HST_MODE_CHANGE = (1 << 3),
	CY_HST_MODE        = (7 << 4),
	CY_HST_OPERATE     = (0 << 4),
	CY_HST_SYSINFO     = (1 << 4),
	CY_HST_CAT         = (2 << 4),
	CY_HST_LOWPOW      = (1 << 2),
	CY_HST_SLEEP       = (1 << 1),
	CY_HST_RESET       = (1 << 0),
};

struct cyttsp4_tch_abs_params {
	size_t ofs;	/* abs byte offset */
	size_t size;	/* size in bits */
	size_t max;	/* max value */
	size_t bofs;	/* bit offset */
};

/* TrueTouch Standard Product Gen4 interface definition */
enum cyttsp4_state {
	CY_ACTIVE_STATE,
	CY_BL_STATE,
};

struct cyttsp4_tch_rec_params {
	u8 loc;
	u8 size;
} __packed;

struct cyttsp4_opcfg {
	u8 cmd_ofs;
	u8 rep_ofs;
	u8 rep_szh;
	u8 rep_szl;
	u8 num_btns;
	u8 tt_stat_ofs;
	u8 obj_cfg0;
	u8 max_tchs;
	u8 tch_rec_size;
	struct cyttsp4_tch_rec_params tch_rec_old[CY_NUM_TCH_FIELDS];
	u8 btn_rec_size;/* btn record size (in bytes) */
	u8 btn_diff_ofs;/* btn data loc ,diff counts, (Op-Mode byte ofs) */
	u8 btn_diff_size;/* btn size of diff counts (in bits) */
	struct cyttsp4_tch_rec_params tch_rec_new[CY_NUM_EXT_TCH_FIELDS];
	u8 noise_data_ofs;
	u8 noise_data_sz;
} __packed;

struct cyttsp4 {
	struct device *dev;
	struct input_dev *input;
	struct gpio_desc *rst_gpio;
	const struct cyttsp4_bus_ops *bus_ops;
	struct completion bl_ready;
	struct cyttsp4_sysinfo_offsets sysinfo_offsets;
	struct cyttsp4_pcfg pcfg;
	struct cyttsp4_opcfg opcfg;
	struct cyttsp4_tch_abs_params tch_abs[CY_NUM_TOTAL_TCH_FIELDS];
	bool suspended;
	size_t max_tchs;
	size_t tch_rec_size;
	enum cyttsp4_state state;
	struct touchscreen_properties props;
	int num_prv_rec;
	size_t max_x;
	size_t max_y;
	size_t max_p;
	size_t pcfg_size;
	size_t opcfg_size;

	int irq;

	u16 pcfg_ofs;
	u16 opcfg_ofs;
	u8 rep_len_ofs;
	u8 rep_stat_ofs;
	u8 tt_stat_ofs;
	u8 tt_ofs;

	u8 *xy_mode;
	u8 *xy_data;
	u8 *xfer_buf;
};

extern struct cyttsp4 *cyttsp4_probe(const struct cyttsp4_bus_ops *ops,
		struct device *dev, u16 irq, size_t xfer_buf_size);
extern int cyttsp4_remove(struct cyttsp4 *ts);
int cyttsp_i2c_write_block_data(struct device *dev, u8 *xfer_buf, u16 addr,
		u8 length, const void *values);
int cyttsp_i2c_read_block_data(struct device *dev, u8 *xfer_buf, u16 addr,
		u8 length, void *values);
extern const struct dev_pm_ops cyttsp4_pm_ops;
#endif
