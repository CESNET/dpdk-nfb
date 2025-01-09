/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Cesnet
 */

#include <rte_kvargs.h>
#include <ethdev_vdev.h>

#include "nfb.h"

#define VDEV_NFB_DRIVER net_vdev_nfb
#define VDEV_NFB_ARG_DEV "dev"


static int
vdev_nfb_vdev_probe(struct rte_vdev_device *dev)
{
	int i;
	int ret = 0;

	struct nfb_init_params params;

	const char *vdev_name = rte_vdev_device_name(dev);
	const char *vdev_args = rte_vdev_device_args(dev);
	char *dev_params, *dev_params_mod;
	struct rte_kvargs *kvargs;

	kvargs = rte_kvargs_parse(vdev_args, NULL);
	if (kvargs == NULL) {
		RTE_LOG(ERR, PMD, "Failed to parse device arguments %s\n", vdev_args);
		ret = -EINVAL;
		goto err_parse_args;
	}

	dev_params = strdup(vdev_args);
	if (dev_params == NULL) {
		ret = -ENOMEM;
		goto err_strdup_params;
	}

	params.args = dev_params;
	params.path = nfb_default_dev_path();
	params.nfb_id = 0;

	dev_params_mod = dev_params;

	dev_params_mod[0] = 0;

	/* Parse parameters for virtual device */
	for (i = 0; i != (signed) kvargs->count; ++i) {
		const struct rte_kvargs_pair *pair = &kvargs->pairs[i];

		if (!strcmp(pair->key, VDEV_NFB_ARG_DEV)) {
			params.path = pair->value;
		} else {
			dev_params_mod += sprintf(dev_params_mod, "%s%s=%s",
					dev_params_mod == dev_params ? "" : ",",
					pair->key, pair->value);
		}
	}

	strcpy(params.name, vdev_name);

	ret = nfb_eth_common_probe(&dev->device, NULL, NULL, &params, -1);
	if (ret)
		goto err_nfb_common_probe;

	free(dev_params);
	rte_kvargs_free(kvargs);

	return ret;

err_nfb_common_probe:
	free(dev_params);
err_strdup_params:
	rte_kvargs_free(kvargs);
err_parse_args:
	return ret;
}

static int
vdev_nfb_vdev_remove(struct rte_vdev_device *dev)
{
	return nfb_eth_common_remove(&dev->device);
}

/** Virtual device descriptor. */
static struct rte_vdev_driver vdev_nfb_vdev = {
	.probe = vdev_nfb_vdev_probe,
	.remove = vdev_nfb_vdev_remove,
};

RTE_PMD_REGISTER_VDEV(VDEV_NFB_DRIVER, vdev_nfb_vdev);
RTE_PMD_REGISTER_ALIAS(VDEV_NFB_DRIVER, eth_vdev_nfb);
RTE_PMD_REGISTER_PARAM_STRING(net_vdev_nfb,
		VDEV_NFB_ARG_DEV "=<string> "
		NFB_COMMON_ARGS
		);
