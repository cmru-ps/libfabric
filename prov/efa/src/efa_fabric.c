/*
 * Copyright (c) 2014-2016, Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <netdb.h>
#include <inttypes.h>

#include <infiniband/efadv.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include "ofi_prov.h"
#include <ofi_util.h>

#include "efa.h"
#include "efa_prov_info.h"
#if HAVE_EFA_DL
#include <ofi_shm.h>
#endif

#ifdef EFA_PERF_ENABLED
const char *efa_perf_counters_str[] = {
	EFA_PERF_FOREACH(OFI_STR)
};
#endif

static int efa_check_hints(uint32_t version, const struct fi_info *hints,
			   const struct fi_info *info)
{
	uint64_t prov_mode;
	size_t size;
	int ret;

	if (hints->caps & ~(info->caps)) {
		EFA_INFO(FI_LOG_CORE, "Unsupported capabilities\n");
		OFI_INFO_CHECK(&efa_prov, info, hints, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	prov_mode = ofi_mr_get_prov_mode(version, hints, info);

	if ((hints->mode & prov_mode) != prov_mode) {
		EFA_INFO(FI_LOG_CORE, "Required hints mode bits not set\n");
		OFI_INFO_MODE(&efa_prov, prov_mode, hints->mode);
		return -FI_ENODATA;
	}

	if (hints->fabric_attr) {
		ret = ofi_check_fabric_attr(&efa_prov, info->fabric_attr,
					    hints->fabric_attr);

		if (ret)
			return ret;
	}

	switch (hints->addr_format) {
	case FI_FORMAT_UNSPEC:
	case FI_ADDR_EFA:
		size = EFA_EP_ADDR_LEN;
		break;
	default:
		EFA_INFO(FI_LOG_CORE,
			 "Address format not supported: hints[%u], supported[%u,%u]\n",
			 hints->addr_format, FI_FORMAT_UNSPEC, FI_ADDR_EFA);
		return -FI_ENODATA;
	}

	if (hints->src_addr && hints->src_addrlen < size)
		return -FI_ENODATA;

	if (hints->dest_addr && hints->dest_addrlen < size)
		return -FI_ENODATA;

	if (hints->domain_attr) {
		ret = ofi_check_domain_attr(&efa_prov, version, info->domain_attr, hints);
		if (ret)
			return ret;
	}

	if (hints->ep_attr) {
		ret = ofi_check_ep_attr(&efa_util_prov, info->fabric_attr->api_version, info, hints);
		if (ret)
			return ret;
	}

	if (hints->rx_attr) {
		ret = ofi_check_rx_attr(&efa_prov, info, hints->rx_attr, hints->mode);
		if (ret)
			return ret;
	}

	if (hints->tx_attr) {
		ret = ofi_check_tx_attr(&efa_prov, info->tx_attr, hints->tx_attr, hints->mode);
		if (ret)
			return ret;
	}

	return 0;
}

static int efa_str_to_ep_addr(const char *node, const char *service, struct efa_ep_addr *addr)
{
	int ret;

	if (!node)
		return -FI_EINVAL;

	memset(addr, 0, sizeof(*addr));

	ret = inet_pton(AF_INET6, node, addr->raw);
	if (ret != 1)
		return -FI_EINVAL;
	if (service)
		addr->qpn = atoi(service);

	return 0;
}

static int efa_node_matches_addr(struct efa_ep_addr *addr, const char *node)
{
	struct efa_ep_addr eaddr;

	efa_str_to_ep_addr(node, NULL, &eaddr);
	return memcmp(&eaddr.raw, &addr->raw, sizeof(addr->raw));
}

static int efa_get_matching_info(uint32_t version, const char *node, uint64_t flags,
				 const struct fi_info *hints, struct fi_info **info)
{
	const struct fi_info *check_info;
	struct fi_info *fi, *tail;
	int ret;

	*info = tail = NULL;

	for (check_info = efa_util_prov.info; check_info; check_info = check_info->next) {
		ret = 0;
		if (flags & FI_SOURCE) {
			if (node)
				ret = efa_node_matches_addr(check_info->src_addr, node);
		} else if (hints && hints->src_addr) {
			ret = memcmp(check_info->src_addr, hints->src_addr, EFA_EP_ADDR_LEN);
		}

		if (ret)
			continue;
		EFA_INFO(FI_LOG_FABRIC, "found match for interface %s %s\n", node, check_info->fabric_attr->name);
		if (hints) {
			ret = efa_check_hints(version, hints, check_info);
			if (ret)
				continue;
		}

		fi = fi_dupinfo(check_info);
		if (!fi) {
			ret = -FI_ENOMEM;
			goto err_free_info;
		}

		fi->fabric_attr->api_version = version;

		if (!*info)
			*info = fi;
		else
			tail->next = fi;
		tail = fi;
	}

	if (!*info)
		return -FI_ENODATA;

	return 0;

err_free_info:
	fi_freeinfo(*info);
	*info = NULL;
	return ret;
}

static int efa_set_fi_address(const char *node, const char *service, uint64_t flags,
			      const struct fi_info *hints, struct fi_info *fi)
{
	struct efa_ep_addr tmp_addr;
	void *dest_addr = NULL;
	int ret = FI_SUCCESS;
	struct fi_info *cur;

	if (flags & FI_SOURCE) {
		if (hints && hints->dest_addr)
			dest_addr = hints->dest_addr;
	} else {
		if (node || service) {
			ret = efa_str_to_ep_addr(node, service, &tmp_addr);
			if (ret)
				return ret;
			dest_addr = &tmp_addr;
		} else if (hints && hints->dest_addr) {
			dest_addr = hints->dest_addr;
		}
	}

	if (dest_addr) {
		for (cur = fi; cur; cur = cur->next) {
			cur->dest_addr = malloc(EFA_EP_ADDR_LEN);
			if (!cur->dest_addr) {
				for (; fi->dest_addr; fi = fi->next)
					free(fi->dest_addr);
				return -FI_ENOMEM;
			}
			memcpy(cur->dest_addr, dest_addr, EFA_EP_ADDR_LEN);
			cur->dest_addrlen = EFA_EP_ADDR_LEN;
		}
	}
	return ret;
}

int efa_getinfo(uint32_t version, const char *node, const char *service,
		uint64_t flags, const struct fi_info *hints, struct fi_info **info)
{
	int ret;

	if (!(flags & FI_SOURCE) && hints && hints->src_addr &&
	    hints->src_addrlen != EFA_EP_ADDR_LEN)
		return -FI_ENODATA;

	if (((!node && !service) || (flags & FI_SOURCE)) &&
	    hints && hints->dest_addr &&
	    hints->dest_addrlen != EFA_EP_ADDR_LEN)
		return -FI_ENODATA;

	ret = efa_get_matching_info(version, node, flags, hints, info);
	if (ret)
		goto out;

	ret = efa_set_fi_address(node, service, flags, hints, *info);
	if (ret)
		goto out;

	ofi_alter_info(*info, hints, version);

out:
	if (!ret || ret == -FI_ENOMEM || ret == -FI_ENODEV) {
		return ret;
	} else {
		fi_freeinfo(*info);
		*info = NULL;
		return -FI_ENODATA;
	}
}

static int efa_fabric_close(fid_t fid)
{
	struct efa_fabric *efa_fabric;
	int ret;

	efa_fabric = container_of(fid, struct efa_fabric, util_fabric.fabric_fid.fid);
	ret = ofi_fabric_close(&efa_fabric->util_fabric);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_FABRIC,
			"Unable to close fabric: %s\n",
			fi_strerror(-ret));
		return ret;
	}

	if (efa_fabric->shm_fabric) {
		ret = fi_close(&efa_fabric->shm_fabric->fid);
		if (ret) {
			FI_WARN(&rxr_prov, FI_LOG_FABRIC,
				"Unable to close fabric: %s\n",
				fi_strerror(-ret));
			return ret;
		}
	}

#ifdef EFA_PERF_ENABLED
	ofi_perfset_log(&efa_fabric->perf_set, efa_perf_counters_str);
	ofi_perfset_close(&efa_fabric->perf_set);
#endif
	free(efa_fabric);

	return 0;
}

static struct fi_ops efa_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = efa_fabric_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_fabric efa_ops_fabric = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = efa_domain_open,
	.passive_ep = fi_no_passive_ep,
	.eq_open = ofi_eq_create,
	.wait_open = ofi_wait_fd_open,
	.trywait = ofi_trywait
};

int efa_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric_fid,
	       void *context)
{
	const struct fi_info *info;
	struct efa_fabric *efa_fabric;
	int ret = 0, retv;

	efa_fabric = calloc(1, sizeof(*efa_fabric));
	if (!efa_fabric)
		return -FI_ENOMEM;

	for (info = efa_util_prov.info; info; info = info->next) {
		ret = ofi_fabric_init(&efa_prov, info->fabric_attr, attr,
				      &efa_fabric->util_fabric, context);
		if (ret != -FI_ENODATA)
			break;
	}

	if (ret)
		goto err_free_fabric;

	/* Open shm provider's fabric domain */
	if (shm_info) {
		assert(!strcmp(shm_info->fabric_attr->name, "shm"));
		ret = fi_fabric(shm_info->fabric_attr,
				&efa_fabric->shm_fabric, context);
		if (ret)
			goto err_close_util_fabric;
	} else {
		efa_fabric->shm_fabric = NULL;
	}

#ifdef EFA_PERF_ENABLED
	ret = ofi_perfset_create(&rxr_prov, &efa_fabric->perf_set,
				 efa_perf_size, perf_domain, perf_cntr,
				 perf_flags);

	if (ret)
		FI_WARN(&rxr_prov, FI_LOG_FABRIC,
			"Error initializing EFA perfset: %s\n",
			fi_strerror(-ret));
#endif


	*fabric_fid = &efa_fabric->util_fabric.fabric_fid;
	(*fabric_fid)->fid.fclass = FI_CLASS_FABRIC;
	(*fabric_fid)->fid.ops = &efa_fi_ops;
	(*fabric_fid)->ops = &efa_ops_fabric;
	(*fabric_fid)->api_version = attr->api_version;

	return 0;

err_close_util_fabric:
	retv = ofi_fabric_close(&efa_fabric->util_fabric);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_FABRIC,
			"Unable to close fabric: %s\n",
			fi_strerror(-retv));
err_free_fabric:
	free(efa_fabric);

	return ret;
}

#ifndef _WIN32


void efa_win_lib_finalize(void)
{
	// Nothing to do when we are not compiling for Windows
}

int efa_win_lib_initialize(void)
{
	return 0;
}

#else // _WIN32

#include "efawin.h"

/**
 * @brief open efawin.dll and load the symbols on windows platform
 *
 * This function is a no-op on windows
 */
int efa_win_lib_initialize(void)
{
	/* On Windows we need to load efawin dll to interact with
 	* efa device as there is no built-in verbs integration in the OS.
	* efawin dll provides all the ibv_* functions on Windows.
	* efa_load_efawin_lib function will replace stub ibv_* functions with
	* functions from efawin dll
	*/
	return efa_load_efawin_lib();
}

/**
 * @brief close efawin.dll on windows
 *
 * This function is a no-op on windows
 */
void efa_win_lib_finalize(void) {
	efa_free_efawin_lib();
}

#endif // _WIN32

/**
 * @brief initialize global variable: util_prov
 *
 * util_prov is the util_provider with its
 * info pointing a linked list of device info
 */
static int efa_util_prov_initialize()
{
	int i;

	for (i = 0; i < g_device_cnt; i++) {
		assert(g_device_list[i].rdm_info);
		assert(g_device_list[i].dgram_info);
		g_device_list[i].rdm_info->next = g_device_list[i].dgram_info;
		if (i < g_device_cnt - 1)
			g_device_list[i].dgram_info->next = g_device_list[i+1].rdm_info;
		else
			g_device_list[i].dgram_info->next = NULL;
	}

	efa_util_prov.info = g_device_list[0].rdm_info;
	return 0;
}

/**
 * @brief reset util_prov.info
 */
static void efa_util_prov_finalize()
{
	efa_util_prov.info = NULL;
}

/**
 * @brief initialize global variables use by EFA provider.
 *
 * This function call various functions to initialize
 * device_list, pd_list, win_lib and util_prov. All
 * of them are global variables.
 */
int efa_prov_initialize(void)
{
	int ret = 0, err;

	err = efa_device_list_initialize();
	if (err)
		return err;

	if (g_device_cnt <= 0)
		return -FI_ENODEV;

	err = efa_win_lib_initialize();
	if (err) {
		ret = err;
		goto err_free;
	}

	err = efa_util_prov_initialize();
	if (err) {
		ret = err;
		goto err_free;
	}

	return 0;

err_free:
	efa_win_lib_finalize();
	efa_device_list_finalize();
	return ret;
}

/**
 * @brief release the resources of global variables of provider
 *
 * This function calls various functions to release
 * util_prov, device_list, pd_list, win_lib
 */
void efa_prov_finalize(void)
{
	efa_util_prov_finalize();

	efa_device_list_finalize();

	efa_win_lib_finalize();

#if HAVE_EFA_DL
	smr_cleanup();
#endif
}

struct fi_provider efa_prov = {
	.name = EFA_PROV_NAME,
	.version = OFI_VERSION_DEF_PROV,
	.fi_version = OFI_VERSION_LATEST,
	.getinfo = efa_getinfo,
	.fabric = efa_fabric,
	.cleanup = efa_prov_finalize
};

struct util_prov efa_util_prov = {
	.prov = &efa_prov,
	.info = NULL,
	.flags = 0,
};

