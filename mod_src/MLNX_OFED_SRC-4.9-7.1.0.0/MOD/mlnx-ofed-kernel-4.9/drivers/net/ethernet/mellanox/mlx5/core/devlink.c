/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2019, Mellanox Technologies */

#include <devlink.h>

#include "mlx5_core.h"
#include "fs_core.h"
#include "eswitch.h"
#include "meddev/sf.h"

#if defined(HAVE_DEVLINK_HAS_INFO_GET) && defined(HAVE_DEVLINK_INFO_VERSION_FIXED_PUT)
int
mlx5_devlink_info_get(struct devlink *devlink, struct devlink_info_req *req,
		      struct netlink_ext_ack *extack);
#endif

#ifdef HAVE_DEVLINK_HAS_FLASH_UPDATE
static int mlx5_devlink_flash_update(struct devlink *devlink,
#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
				     struct devlink_flash_update_params *params,
#else
				     const char *file_name,
				     const char *component,
#endif
				     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
#ifdef HAVE_DEVLINK_FLASH_UPDATE_PARAMS_HAS_STRUCT_FW
	return mlx5_firmware_flash(dev, params->fw, extack);
#else
	const struct firmware *fw;
	int err;

#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
	if (params->component)
#else
	if (component)
#endif
		return -EOPNOTSUPP;

	err = request_firmware_direct(&fw, 
#ifdef HAVE_FLASH_UPDATE_GET_3_PARAMS
			params->file_name,
#else
			file_name,
#endif
			&dev->pdev->dev);
	if (err)
		return err;

	err = mlx5_firmware_flash(dev, fw, extack);
	release_firmware(fw);

	return err;
#endif /* HAVE_DEVLINK_FLASH_UPDATE_PARAMS_HAS_STRUCT_FW */
}
#endif

#ifdef HAVE_DEVLINK_HAS_RELOAD_UP_DOWN
static int mlx5_devlink_reload_down(struct devlink *devlink,
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
				    bool netns_change,
				    enum devlink_reload_action action,
				    enum devlink_reload_limit limit,
#elif defined(HAVE_DEVLINK_RELOAD_DOWN_HAS_3_PARAMS)
			     	    bool netns_change,
#endif
				    struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	return mlx5_unload_one(dev, false);
}

static int mlx5_devlink_reload_up(struct devlink *devlink,
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
				  enum devlink_reload_action action,
				  enum devlink_reload_limit limit, u32 *actions_performed,
#endif
				  struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	*actions_performed = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT);
#endif
	return mlx5_load_one(dev, false);
}
#endif /* HAVE_DEVLINK_HAS_RELOAD_UP_DOWN */

static const struct devlink_ops mlx5_devlink_ops = {
#ifdef CONFIG_MLX5_ESWITCH
#ifdef HAVE_DEVLINK_HAS_ESWITCH_MODE_GET_SET
	.eswitch_mode_set = mlx5_devlink_eswitch_mode_set,
	.eswitch_mode_get = mlx5_devlink_eswitch_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_MODE_GET_SET */
#ifdef HAVE_DEVLINK_HAS_ESWITCH_INLINE_MODE_GET_SET
	.eswitch_inline_mode_set = mlx5_devlink_eswitch_inline_mode_set,
	.eswitch_inline_mode_get = mlx5_devlink_eswitch_inline_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_INLINE_MODE_GET_SET */
#ifdef HAVE_DEVLINK_HAS_ESWITCH_ENCAP_MODE_SET
	.eswitch_encap_mode_set = mlx5_devlink_eswitch_encap_mode_set,
	.eswitch_encap_mode_get = mlx5_devlink_eswitch_encap_mode_get,
#endif /* HAVE_DEVLINK_HAS_ESWITCH_ENCAP_MODE_SET */
#endif
#ifdef HAVE_DEVLINK_HAS_FLASH_UPDATE
	.flash_update = mlx5_devlink_flash_update,
#endif /* HAVE_DEVLINK_HAS_FLASH_UPDATE */
#ifdef HAVE_DEVLINK_HAS_RELOAD_UP_DOWN
#ifdef HAVE_DEVLINK_RELOAD_DOWN_SUPPORT_RELOAD_ACTION
	.reload_actions = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT),
#endif
	.reload_down = mlx5_devlink_reload_down,
	.reload_up = mlx5_devlink_reload_up,
#endif /* HAVE_DEVLINK_HAS_RELOAD_UP_DOWN */
#if defined(HAVE_DEVLINK_HAS_INFO_GET) && defined(HAVE_DEVLINK_INFO_VERSION_FIXED_PUT)
	.info_get = mlx5_devlink_info_get,
#endif /* HAVE_DEVLINK_HAS_INFO_GET */
};

struct devlink *mlx5_devlink_alloc(struct device *dev)
{
#ifdef HAVE_DEVLINK_ALLOC_GET_3_PARAMS
	return devlink_alloc(&mlx5_devlink_ops, sizeof(struct mlx5_core_dev), dev);
#else
	return devlink_alloc(&mlx5_devlink_ops, sizeof(struct mlx5_core_dev));
#endif
}

void mlx5_devlink_free(struct devlink *devlink)
{
	devlink_free(devlink);
}

#if defined(HAVE_DEVLINK_PARAM) && (defined(HAVE_DEVLINK_PARAMS_PUBLISHED) || defined(HAVE_DEVLINK_REGISTER_GET_1_PARAMS))

static int mlx5_devlink_fs_mode_validate(struct devlink *devlink, u32 id,
					 union devlink_param_value val,
					 struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	char *value = val.vstr;
	int err = 0;

	if (!strcmp(value, "dmfs")) {
		return 0;
	} else if (!strcmp(value, "smfs")) {
		u8 eswitch_mode;
		bool smfs_cap;

		eswitch_mode = mlx5_eswitch_mode(dev->priv.eswitch);
		smfs_cap = mlx5_fs_dr_is_supported(dev);

		if (!smfs_cap) {
			err = -EOPNOTSUPP;
			NL_SET_ERR_MSG_MOD(extack,
					   "Software managed steering is not supported by current device");
		}

		else if (eswitch_mode == MLX5_ESWITCH_OFFLOADS) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Software managed steering is not supported when eswitch offlaods enabled.");
			err = -EOPNOTSUPP;
		}
	} else {
		NL_SET_ERR_MSG_MOD(extack,
				   "Bad parameter: supported values are [\"dmfs\", \"smfs\"]");
		err = -EINVAL;
	}

	return err;
}

static int mlx5_devlink_fs_mode_set(struct devlink *devlink, u32 id,
				    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	enum mlx5_flow_steering_mode mode;

	if (!strcmp(ctx->val.vstr, "smfs"))
		mode = MLX5_FLOW_STEERING_MODE_SMFS;
	else
		mode = MLX5_FLOW_STEERING_MODE_DMFS;
	dev->priv.steering->mode = mode;

	return 0;
}

static int mlx5_devlink_fs_mode_get(struct devlink *devlink, u32 id,
				    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (dev->priv.steering->mode == MLX5_FLOW_STEERING_MODE_SMFS)
		strcpy(ctx->val.vstr, "smfs");
	else
		strcpy(ctx->val.vstr, "dmfs");
	return 0;
}

enum mlx5_devlink_param_id {
	MLX5_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	MLX5_DEVLINK_PARAM_FLOW_STEERING_MODE,
};

#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
static int mlx5_devlink_enable_roce_validate(struct devlink *devlink, u32 id,
					     union devlink_param_value val,
					     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	bool new_state = val.vbool;

	if (new_state && !MLX5_CAP_GEN(dev, roce)) {
		NL_SET_ERR_MSG_MOD(extack, "Device doesn't support RoCE");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

static const struct devlink_param mlx5_devlink_params[] = {
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_FLOW_STEERING_MODE,
			     "flow_steering_mode", DEVLINK_PARAM_TYPE_STRING,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     mlx5_devlink_fs_mode_get, mlx5_devlink_fs_mode_set,
			     mlx5_devlink_fs_mode_validate),
#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
	DEVLINK_PARAM_GENERIC(ENABLE_ROCE, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_enable_roce_validate),
#endif
};

static void mlx5_devlink_set_params_init_values(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;

	if (dev->priv.steering->mode == MLX5_FLOW_STEERING_MODE_DMFS)
		strcpy(value.vstr, "dmfs");
	else
		strcpy(value.vstr, "smfs");
	devlink_param_driverinit_value_set(devlink,
					   MLX5_DEVLINK_PARAM_FLOW_STEERING_MODE,
					   value);

#ifdef HAVE_DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE
	value.vbool = MLX5_CAP_GEN(dev, roce);
	devlink_param_driverinit_value_set(devlink,
					   DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE,
					   value);
#endif
}

#endif /* HAVE_DEVLINK_PARAM && HAVE_DEVLINK_PARAMS_PUBLISHED */

int mlx5_devlink_register(struct devlink *devlink, struct device *dev)
{
	int err = 0;

#ifndef HAVE_DEVLINK_REGISTER_GET_1_PARAMS
	err = devlink_register(devlink, dev);
	if (err)
		return err;
#endif
#if defined(HAVE_DEVLINK_PARAM) && (defined(HAVE_DEVLINK_PARAMS_PUBLISHED) || defined(HAVE_DEVLINK_REGISTER_GET_1_PARAMS))
	err = devlink_params_register(devlink, mlx5_devlink_params,
				      ARRAY_SIZE(mlx5_devlink_params));
	if (err) {
#ifndef HAVE_DEVLINK_REGISTER_GET_1_PARAMS
		devlink_unregister(devlink);
#endif
		return err;
	}
	mlx5_devlink_set_params_init_values(devlink);

#ifdef HAVE_DEVLINK_PARAMS_PUBLISHED
	devlink_params_publish(devlink);
#endif /* HAVE_DEVLINK_PARAMS_PUBLISHED */
#ifdef HAVE_DEVLINK_SET_FEATURES
        devlink_set_features(devlink, DEVLINK_F_RELOAD);
#endif
#endif
	return err;
}

void mlx5_devlink_unregister(struct devlink *devlink)
{
#if defined(HAVE_DEVLINK_PARAM) && (defined(HAVE_DEVLINK_PARAMS_PUBLISHED) || defined(HAVE_DEVLINK_REGISTER_GET_1_PARAMS))
#ifdef HAVE_DEVLINK_PARAMS_PUBLISHED
	devlink_params_unpublish(devlink);
#endif /* HAVE_DEVLINK_PARAMS_PUBLISHED */
	devlink_params_unregister(devlink, mlx5_devlink_params,
				  ARRAY_SIZE(mlx5_devlink_params));
#endif
#ifndef HAVE_DEVLINK_REGISTER_GET_1_PARAMS
	devlink_unregister(devlink);
#endif
}

#if defined(HAVE_DEVLINK_HAS_INFO_GET) && defined(HAVE_DEVLINK_INFO_VERSION_FIXED_PUT)
static u8 mlx5_fw_ver_major(u32 version)
{
	return (version >> 24) & 0xff;
}

static u8 mlx5_fw_ver_minor(u32 version)
{
	return (version >> 16) & 0xff;
}

static u16 mlx5_fw_ver_subminor(u32 version)
{
	return version & 0xffff;
}

#define DEVLINK_FW_STRING_LEN 32

int
mlx5_devlink_info_get(struct devlink *devlink, struct devlink_info_req *req,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	char version_str[DEVLINK_FW_STRING_LEN];
	u32 running_fw, stored_fw;
	int err;

	err = devlink_info_driver_name_put(req, DRIVER_NAME);
	if (err)
		return err;

	err = devlink_info_version_fixed_put(req, "fw.psid", dev->board_id);
	if (err)
		return err;

	err = mlx5_fw_version_query(dev, &running_fw, &stored_fw);
	if (err)
		return err;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(running_fw), mlx5_fw_ver_minor(running_fw),
		 mlx5_fw_ver_subminor(running_fw));
	err = devlink_info_version_running_put(req, "fw.version", version_str);
	if (err)
		return err;

	/* no pending version, return running (stored) version */
	if (stored_fw == 0)
		stored_fw = running_fw;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(stored_fw), mlx5_fw_ver_minor(stored_fw),
		 mlx5_fw_ver_subminor(stored_fw));
	err = devlink_info_version_stored_put(req, "fw.version", version_str);
	if (err)
		return err;

	return 0;
}
#endif /* HAVE_DEVLINK_HAS_INFO_GET && HAVE_DEVLINK_INFO_VERSION_FIXED_PUT */

