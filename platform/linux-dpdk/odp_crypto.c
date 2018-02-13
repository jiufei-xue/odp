/* Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp/api/crypto.h>
#include <odp_internal.h>
#include <odp/api/atomic.h>
#include <odp/api/spinlock.h>
#include <odp/api/sync.h>
#include <odp/api/debug.h>
#include <odp/api/align.h>
#include <odp/api/shared_memory.h>
#include <odp_crypto_internal.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>
#include <odp/api/random.h>
#include <odp_packet_internal.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>

#include <string.h>
#include <math.h>

#include <openssl/rand.h>

/* default number supported by DPDK crypto */
#define MAX_SESSIONS 2048
#define NB_MBUF  8192

enum crypto_chain_order {
	CRYPTO_CHAIN_ONLY_CIPHER,
	CRYPTO_CHAIN_ONLY_AUTH,
	CRYPTO_CHAIN_CIPHER_AUTH,
	CRYPTO_CHAIN_AUTH_CIPHER,
	CRYPTO_CHAIN_NOT_SUPPORTED
};

typedef struct crypto_session_entry_s crypto_session_entry_t;
struct crypto_session_entry_s {
		struct crypto_session_entry_s *next;
		odp_crypto_session_param_t p;
		uint64_t rte_session;
		odp_bool_t do_cipher_first;
		struct rte_crypto_sym_xform cipher_xform;
		struct rte_crypto_sym_xform auth_xform;
		struct {
			uint8_t *data;
			uint16_t length;
		} iv;
};

struct crypto_global_s {
	odp_spinlock_t                lock;
	uint8_t enabled_crypto_devs;
	uint8_t enabled_crypto_dev_ids[RTE_CRYPTO_MAX_DEVS];
	crypto_session_entry_t *free;
	crypto_session_entry_t sessions[MAX_SESSIONS];
	int is_crypto_dev_initialized;
	struct rte_mempool *crypto_op_pool;
};

typedef struct crypto_global_s crypto_global_t;
static crypto_global_t *global;
static odp_shm_t crypto_global_shm;

static inline int is_valid_size(uint16_t length, uint16_t min,
				uint16_t max, uint16_t increment)
{
	uint16_t supp_size = min;

	if (length < supp_size)
		return -1;

	for (; supp_size <= max; supp_size += increment) {
		if (length == supp_size)
			return 0;
	}

	return -1;
}

static int cipher_alg_odp_to_rte(odp_cipher_alg_t cipher_alg,
				 struct rte_crypto_sym_xform *cipher_xform)
{
	int rc = 0;

	switch (cipher_alg) {
	case ODP_CIPHER_ALG_NULL:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_NULL;
		break;
	case ODP_CIPHER_ALG_DES:
	case ODP_CIPHER_ALG_3DES_CBC:
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_3DES_CBC;
		break;
	case ODP_CIPHER_ALG_AES_CBC:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_CBC:
#endif
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_AES_CBC;
		break;
	case ODP_CIPHER_ALG_AES_GCM:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_GCM:
#endif
		cipher_xform->cipher.algo = RTE_CRYPTO_CIPHER_AES_GCM;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static int auth_alg_odp_to_rte(odp_auth_alg_t auth_alg,
			       struct rte_crypto_sym_xform *auth_xform,
			       uint32_t auth_digest_len)
{
	int rc = 0;

	/* Process based on auth */
	switch (auth_alg) {
	case ODP_AUTH_ALG_NULL:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_NULL;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_MD5_96:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_MD5_HMAC;
		auth_xform->auth.digest_length = 12;
		break;
#endif
	case ODP_AUTH_ALG_MD5_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_MD5_HMAC;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_SHA256_128:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA256_HMAC;
		auth_xform->auth.digest_length = 16;
		break;
#endif
	case ODP_AUTH_ALG_SHA256_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA256_HMAC;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
	case ODP_AUTH_ALG_SHA1_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA1_HMAC;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
	case ODP_AUTH_ALG_SHA512_HMAC:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_SHA512_HMAC;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_AES128_GCM:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_AES_GCM;
		auth_xform->auth.digest_length = 16;
		break;
#endif
	case ODP_AUTH_ALG_AES_GCM:
		auth_xform->auth.algo = RTE_CRYPTO_AUTH_AES_GCM;
		auth_xform->auth.digest_length = auth_digest_len;
		break;
	default:
		rc = -1;
	}

	return rc;
}

static crypto_session_entry_t *alloc_session(void)
{
	crypto_session_entry_t *session = NULL;

	odp_spinlock_lock(&global->lock);
	session = global->free;
	if (session) {
		global->free = session->next;
		session->next = NULL;
	}
	odp_spinlock_unlock(&global->lock);

	return session;
}

static void free_session(crypto_session_entry_t *session)
{
	odp_spinlock_lock(&global->lock);
	session->next = global->free;
	global->free = session;
	odp_spinlock_unlock(&global->lock);
}

int odp_crypto_init_global(void)
{
	size_t mem_size;
	int idx;
	int16_t cdev_id, cdev_count;
	int rc = -1;
	unsigned cache_size = 0;
	unsigned nb_queue_pairs = 0, queue_pair;

	/* Calculate the memory size we need */
	mem_size  = sizeof(*global);
	mem_size += (MAX_SESSIONS * sizeof(crypto_session_entry_t));

	/* Allocate our globally shared memory */
	crypto_global_shm = odp_shm_reserve("crypto_pool", mem_size,
					    ODP_CACHE_LINE_SIZE, 0);

	if (crypto_global_shm != ODP_SHM_INVALID) {
		global = odp_shm_addr(crypto_global_shm);

		if (global == NULL) {
			ODP_ERR("Failed to find the reserved shm block");
			return -1;
		}
	} else {
		ODP_ERR("Shared memory reserve failed.\n");
		return -1;
	}

	/* Clear it out */
	memset(global, 0, mem_size);

	/* Initialize free list and lock */
	for (idx = 0; idx < MAX_SESSIONS; idx++) {
		global->sessions[idx].next = global->free;
		global->free = &global->sessions[idx];
	}

	global->enabled_crypto_devs = 0;
	odp_spinlock_init(&global->lock);

	odp_spinlock_lock(&global->lock);
	if (global->is_crypto_dev_initialized)
		return 0;

	if (RTE_MEMPOOL_CACHE_MAX_SIZE > 0) {
		unsigned j;

		j = ceil((double)NB_MBUF / RTE_MEMPOOL_CACHE_MAX_SIZE);
		j = RTE_MAX(j, 2UL);
		for (; j <= (NB_MBUF / 2); ++j)
			if ((NB_MBUF % j) == 0) {
				cache_size = NB_MBUF / j;
				break;
			}
		if (odp_unlikely(cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE ||
				 (uint32_t)cache_size * 1.5 > NB_MBUF)) {
			ODP_ERR("cache_size calc failure: %d\n", cache_size);
			cache_size = 0;
		}
	}

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		printf("No crypto devices available\n");
		return 0;
	}

	for (cdev_id = cdev_count - 1; cdev_id >= 0; cdev_id--) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		nb_queue_pairs = odp_cpu_count();
		if (nb_queue_pairs > dev_info.max_nb_queue_pairs)
			nb_queue_pairs = dev_info.max_nb_queue_pairs;

		struct rte_cryptodev_qp_conf qp_conf;

		struct rte_cryptodev_config conf = {
			.nb_queue_pairs = nb_queue_pairs,
			.socket_id = SOCKET_ID_ANY,
			.session_mp = {
				.nb_objs = NB_MBUF,
				.cache_size = cache_size
			}
		};

		rc = rte_cryptodev_configure(cdev_id, &conf);
		if (rc < 0) {
			ODP_ERR("Failed to configure cryptodev %u", cdev_id);
			return -1;
		}

		qp_conf.nb_descriptors = NB_MBUF;

		for (queue_pair = 0; queue_pair < nb_queue_pairs;
							queue_pair++) {
			rc = rte_cryptodev_queue_pair_setup(cdev_id,
							    queue_pair,
							    &qp_conf,
							    SOCKET_ID_ANY);
			if (rc < 0) {
				ODP_ERR("Fail to setup queue pair %u on dev %u",
					queue_pair, cdev_id);
				return -1;
			}
		}

		rc = rte_cryptodev_start(cdev_id);
		if (rc < 0) {
			ODP_ERR("Failed to start device %u: error %d\n",
				cdev_id, rc);
			return -1;
		}

		global->enabled_crypto_devs++;
		global->enabled_crypto_dev_ids[
				global->enabled_crypto_devs - 1] = cdev_id;
	}

	/* create crypto op pool */
	global->crypto_op_pool = rte_crypto_op_pool_create("crypto_op_pool",
						   RTE_CRYPTO_OP_TYPE_SYMMETRIC,
						   NB_MBUF, cache_size, 0,
						   rte_socket_id());

	if (global->crypto_op_pool == NULL) {
		ODP_ERR("Cannot create crypto op pool\n");
		return -1;
	}

	global->is_crypto_dev_initialized = 1;
	odp_spinlock_unlock(&global->lock);

	return 0;
}

int odp_crypto_capability(odp_crypto_capability_t *capability)
{
	uint8_t i, cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	enum rte_crypto_auth_algorithm cap_auth_algo;
	enum rte_crypto_cipher_algorithm cap_cipher_algo;

	if (NULL == capability)
		return -1;

	/* Initialize crypto capability structure */
	memset(capability, 0, sizeof(odp_crypto_capability_t));

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		i = 0;
		cap = &dev_info.capabilities[i];
		if ((dev_info.feature_flags &
			RTE_CRYPTODEV_FF_HW_ACCELERATED)) {
			odp_crypto_cipher_algos_t *hw_ciphers;

			hw_ciphers = &capability->hw_ciphers;
			while (cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED) {
				cap_cipher_algo = cap->sym.cipher.algo;
				if (cap->sym.xform_type ==
					RTE_CRYPTO_SYM_XFORM_CIPHER) {
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_NULL) {
						hw_ciphers->bit.null = 1;
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_3DES_CBC) {
						hw_ciphers->bit.trides_cbc = 1;
						hw_ciphers->bit.des = 1;
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_AES_CBC) {
						hw_ciphers->bit.aes_cbc = 1;
#if ODP_DEPRECATED_API
						hw_ciphers->bit.aes128_cbc = 1;
#endif
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_AES_GCM) {
						hw_ciphers->bit.aes_gcm = 1;
#if ODP_DEPRECATED_API
						hw_ciphers->bit.aes128_gcm = 1;
#endif
					}
				}

				cap_auth_algo = cap->sym.auth.algo;
				if (cap->sym.xform_type ==
				    RTE_CRYPTO_SYM_XFORM_AUTH) {
					odp_crypto_auth_algos_t *hw_auths;

					hw_auths = &capability->hw_auths;
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_NULL) {
						hw_auths->bit.null = 1;
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_AES_GCM) {
						hw_auths->bit.aes_gcm = 1;
#if ODP_DEPRECATED_API
						hw_auths->bit.aes128_gcm = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_MD5_HMAC) {
						hw_auths->bit.md5_hmac = 1;
#if ODP_DEPRECATED_API
						hw_auths->bit.md5_96 = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA256_HMAC) {
						hw_auths->bit.sha256_hmac = 1;
#if ODP_DEPRECATED_API
						hw_auths->bit.sha256_128 = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA1_HMAC) {
						hw_auths->bit.sha1_hmac = 1;
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA512_HMAC) {
						hw_auths->bit.sha512_hmac = 1;
					}
				}
				cap = &dev_info.capabilities[++i];
			}
		} else {
			while (cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED) {
				odp_crypto_cipher_algos_t *ciphers;

				ciphers = &capability->ciphers;
				cap_cipher_algo = cap->sym.cipher.algo;
				if (cap->sym.xform_type ==
				    RTE_CRYPTO_SYM_XFORM_CIPHER) {
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_NULL) {
						ciphers->bit.null = 1;
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_3DES_CBC) {
						ciphers->bit.trides_cbc = 1;
						ciphers->bit.des = 1;
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_AES_CBC) {
						ciphers->bit.aes_cbc = 1;
#if ODP_DEPRECATED_API
						ciphers->bit.aes128_cbc = 1;
#endif
					}
					if (cap_cipher_algo ==
						RTE_CRYPTO_CIPHER_AES_GCM) {
						ciphers->bit.aes_gcm = 1;
#if ODP_DEPRECATED_API
						ciphers->bit.aes128_gcm = 1;
#endif
					}
				}

				cap_auth_algo = cap->sym.auth.algo;
				if (cap->sym.xform_type ==
				    RTE_CRYPTO_SYM_XFORM_AUTH) {
					odp_crypto_auth_algos_t *auths;

					auths = &capability->auths;
					if (cap_auth_algo ==
					    RTE_CRYPTO_AUTH_NULL) {
						auths->bit.null = 1;
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_AES_GCM) {
						auths->bit.aes_gcm = 1;
#if ODP_DEPRECATED_API
						auths->bit.aes128_gcm = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_MD5_HMAC) {
						auths->bit.md5_hmac = 1;
#if ODP_DEPRECATED_API
						auths->bit.md5_96 = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA256_HMAC) {
						auths->bit.sha256_hmac = 1;
#if ODP_DEPRECATED_API
						auths->bit.sha256_128 = 1;
#endif
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA1_HMAC) {
						auths->bit.sha1_hmac = 1;
					}
					if (cap_auth_algo ==
						RTE_CRYPTO_AUTH_SHA512_HMAC) {
						auths->bit.sha512_hmac = 1;
					}
				}
				cap = &dev_info.capabilities[++i];
			}
		}

		/* Read from the device with the lowest max_nb_sessions */
		if (capability->max_sessions > dev_info.sym.max_nb_sessions)
			capability->max_sessions = dev_info.sym.max_nb_sessions;

		if (capability->max_sessions == 0)
			capability->max_sessions = dev_info.sym.max_nb_sessions;
	}

	/* Make sure the session count doesn't exceed MAX_SESSIONS */
	if (capability->max_sessions > MAX_SESSIONS)
		capability->max_sessions = MAX_SESSIONS;

	return 0;
}

int odp_crypto_cipher_capability(odp_cipher_alg_t cipher,
				 odp_crypto_cipher_capability_t dst[],
				 int num_copy)
{
	odp_crypto_cipher_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_cipher_capability_t);

	uint8_t i, cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	enum rte_crypto_cipher_algorithm cap_cipher_algo;
	struct rte_crypto_sym_xform cipher_xform;

	rc = cipher_alg_odp_to_rte(cipher, &cipher_xform);

	/* Check result */
	if (rc)
		return -1;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		i = 0;
		cap = &dev_info.capabilities[i];
		while (cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED) {
			if (cap->sym.xform_type ==
			    RTE_CRYPTO_SYM_XFORM_CIPHER) {
				cap_cipher_algo = cap->sym.cipher.algo;
				if (cap_cipher_algo == cipher_xform.cipher.algo)
						break;
			}
					cap = &dev_info.capabilities[++i];
		}

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		uint32_t key_size_min = cap->sym.cipher.key_size.min;
		uint32_t key_size_max = cap->sym.cipher.key_size.max;
		uint32_t key_inc = cap->sym.cipher.key_size.increment;
		uint32_t iv_size_max = cap->sym.cipher.iv_size.max;
		uint32_t iv_size_min = cap->sym.cipher.iv_size.min;
		uint32_t iv_inc = cap->sym.cipher.iv_size.increment;

		for (uint32_t key_len = key_size_min; key_len <= key_size_max;
							   key_len += key_inc) {
			for (uint32_t iv_size = iv_size_min;
				iv_size <= iv_size_max; iv_size += iv_inc) {
				src[idx].key_len = key_len;
				src[idx].iv_len = iv_size;
				idx++;
				if (iv_inc == 0)
					break;
			}

			if (key_inc == 0)
				break;
		}
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

int odp_crypto_auth_capability(odp_auth_alg_t auth,
			       odp_crypto_auth_capability_t dst[],
			       int num_copy)
{
	odp_crypto_auth_capability_t src[num_copy];
	int idx = 0, rc = 0;
	int size = sizeof(odp_crypto_auth_capability_t);

	uint8_t i, cdev_id, cdev_count;
	const struct rte_cryptodev_capabilities *cap;
	enum rte_crypto_auth_algorithm cap_auth_algo;
	struct rte_crypto_sym_xform auth_xform;

	rc = auth_alg_odp_to_rte(auth, &auth_xform, 0);

	/* Check result */
	if (rc)
		return -1;

	cdev_count = rte_cryptodev_count();
	if (cdev_count == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	for (cdev_id = 0; cdev_id < cdev_count; cdev_id++) {
		struct rte_cryptodev_info dev_info;

		rte_cryptodev_info_get(cdev_id, &dev_info);
		i = 0;
		cap = &dev_info.capabilities[i];
		while (cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED) {
			cap_auth_algo = cap->sym.auth.algo;
			if (cap->sym.xform_type ==
			    RTE_CRYPTO_SYM_XFORM_AUTH) {
				if (cap_auth_algo == auth_xform.auth.algo)
						break;
			}
					cap = &dev_info.capabilities[++i];
		}

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		uint8_t key_size_min = cap->sym.auth.key_size.min;
		uint8_t key_size_max = cap->sym.auth.key_size.max;
		uint8_t increment = cap->sym.auth.key_size.increment;
		uint8_t digest_size_max = cap->sym.auth.digest_size.max;

		if (key_size_min == key_size_max) {
			src[idx].key_len = key_size_min;
			src[idx].digest_len = digest_size_max;
			src[idx].aad_len.min = cap->sym.auth.aad_size.min;
			src[idx].aad_len.max = cap->sym.auth.aad_size.max;
			src[idx].aad_len.inc = cap->sym.auth.aad_size.increment;
			idx++;
		} else {
			for (uint8_t key_len = key_size_min;
				key_len <= key_size_max;
				key_len += increment) {
				idx = (key_len - key_size_min) / increment;
				src[idx].key_len = key_len;
				src[idx].digest_len = digest_size_max;
				src[idx].aad_len.min =
						cap->sym.auth.aad_size.min;
				src[idx].aad_len.max =
						cap->sym.auth.aad_size.max;
				src[idx].aad_len.inc =
					       cap->sym.auth.aad_size.increment;
				idx++;
			}
		}
	}

	if (idx < num_copy)
		num_copy = idx;

	memcpy(dst, src, num_copy * size);

	return idx;
}

static int get_crypto_dev(struct rte_crypto_sym_xform *first_xform,
			  enum crypto_chain_order order,
			  uint16_t iv_length, uint8_t *dev_id)
{
	uint8_t cdev_id, id;
	const struct rte_cryptodev_capabilities *cap;
	struct rte_crypto_sym_xform *auth_xform = NULL;
	struct rte_crypto_sym_xform *cipher_xform = NULL;
	enum rte_crypto_cipher_algorithm cap_cipher_algo;
	enum rte_crypto_auth_algorithm cap_auth_algo;
	enum rte_crypto_cipher_algorithm app_cipher_algo;
	enum rte_crypto_auth_algorithm app_auth_algo;

	switch (order) {
	case CRYPTO_CHAIN_ONLY_CIPHER:
		cipher_xform = first_xform;
		break;
	case CRYPTO_CHAIN_ONLY_AUTH:
		auth_xform = first_xform;
		break;
	case CRYPTO_CHAIN_CIPHER_AUTH:
		cipher_xform = first_xform;
		auth_xform = first_xform->next;
		break;
	case CRYPTO_CHAIN_AUTH_CIPHER:
		auth_xform = first_xform;
		cipher_xform = first_xform->next;
		break;
	default:
		return -1;
	}

	for (id = 0; id < global->enabled_crypto_devs; id++) {
		struct rte_cryptodev_info dev_info;
		int i = 0;

		cdev_id = global->enabled_crypto_dev_ids[id];
		rte_cryptodev_info_get(cdev_id, &dev_info);
		cap = &dev_info.capabilities[i];
		while (cipher_xform && cap->op !=
			RTE_CRYPTO_OP_TYPE_UNDEFINED) {
			if (cap->sym.xform_type ==
			    RTE_CRYPTO_SYM_XFORM_CIPHER) {
				app_cipher_algo = cipher_xform->cipher.algo;
				cap_cipher_algo = cap->sym.cipher.algo;
				if (cap_cipher_algo == app_cipher_algo)
						break;
			}
			cap = &dev_info.capabilities[++i];
		}

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		if (cipher_xform) {
			/* Check if key size is supported by the algorithm. */
			if (cipher_xform->cipher.key.length) {
				if (is_valid_size(
						cipher_xform->cipher.key.length,
						cap->sym.cipher.key_size.min,
						cap->sym.cipher.key_size.max,
						cap->sym.cipher.key_size.
						increment) != 0) {
					ODP_ERR("Invalid cipher key length\n");
					return -1;
				}
			/* No size provided, use minimum size. */
			} else
				cipher_xform->cipher.key.length =
						cap->sym.cipher.key_size.min;

			/* Check if iv length is supported by the algorithm. */
			if (is_valid_size(iv_length,
					  cap->sym.cipher.iv_size.min,
					  cap->sym.cipher.iv_size.max,
					  cap->sym.cipher.iv_size.
					  increment) != 0) {
				ODP_ERR("Invalid iv length\n");
				return -1;
			}
		}

		if (cipher_xform && !auth_xform) {
			memcpy(dev_id, &cdev_id, sizeof(cdev_id));
			return 0;
		}

		i = 0;
		cap = &dev_info.capabilities[i];
		while (auth_xform && cap->op != RTE_CRYPTO_OP_TYPE_UNDEFINED) {
			if ((cap->sym.xform_type ==
			    RTE_CRYPTO_SYM_XFORM_AUTH)) {
				app_auth_algo = auth_xform->auth.algo;
				cap_auth_algo = cap->sym.auth.algo;
				if (cap_auth_algo == app_auth_algo)
					break;
			}
			cap = &dev_info.capabilities[++i];
		}

		if (cap->op == RTE_CRYPTO_OP_TYPE_UNDEFINED)
			continue;

		/* Check if key size is supported by the algorithm. */
		if (auth_xform->auth.key.length) {
			if (is_valid_size(auth_xform->auth.key.length,
					  cap->sym.auth.key_size.min,
					  cap->sym.auth.key_size.max,
					  cap->sym.auth.key_size.
					  increment) != 0) {
				ODP_ERR("Unsupported auth key length\n");
				return -1;
			}
		/* No size provided, use minimum size. */
		} else
			auth_xform->auth.key.length =
					cap->sym.auth.key_size.min;

		/* Check if digest size is supported by the algorithm. */
		if (auth_xform->auth.digest_length) {
			if (is_valid_size(auth_xform->auth.digest_length,
					  cap->sym.auth.digest_size.min,
					  cap->sym.auth.digest_size.max,
					  cap->sym.auth.digest_size.
					  increment) != 0) {
				ODP_ERR("Unsupported digest length\n");
				return -1;
			}
		/* No size provided, use minimum size. */
		} else
			auth_xform->auth.digest_length =
					cap->sym.auth.digest_size.min;

		memcpy(dev_id, &cdev_id, sizeof(cdev_id));
		return 0;
	}

	return -1;
}

static enum crypto_chain_order set_chain_order(
				      odp_bool_t do_cipher_first,
				      struct rte_crypto_sym_xform **first_xform,
				      struct rte_crypto_sym_xform *auth_xform,
				      struct rte_crypto_sym_xform *cipher_xform)
{
	/* Process based on cipher */
	/* Derive order */
	if (do_cipher_first) {
		if (auth_xform->auth.algo != RTE_CRYPTO_AUTH_NULL &&
		    cipher_xform->cipher.algo != RTE_CRYPTO_CIPHER_NULL) {
			*first_xform = cipher_xform;
			(*first_xform)->next = auth_xform;
			return CRYPTO_CHAIN_CIPHER_AUTH;
		}
	} else {
		if (auth_xform->auth.algo != RTE_CRYPTO_AUTH_NULL &&
		    cipher_xform->cipher.algo != RTE_CRYPTO_CIPHER_NULL) {
			*first_xform = auth_xform;
			(*first_xform)->next = cipher_xform;
			return CRYPTO_CHAIN_AUTH_CIPHER;
		}
	}

	if (auth_xform->auth.algo == RTE_CRYPTO_AUTH_NULL) {
		*first_xform = cipher_xform;
		(*first_xform)->next = NULL;
		 return CRYPTO_CHAIN_ONLY_CIPHER;
	} else if (cipher_xform->cipher.algo == RTE_CRYPTO_CIPHER_NULL) {
		*first_xform = auth_xform;
		(*first_xform)->next = NULL;
		return CRYPTO_CHAIN_ONLY_AUTH;
	}

	return CRYPTO_CHAIN_NOT_SUPPORTED;
}

int odp_crypto_session_create(odp_crypto_session_param_t *param,
			      odp_crypto_session_t *session_out,
			      odp_crypto_ses_create_err_t *status)
{
	int rc = 0;
	uint8_t cdev_id = 0;
	struct rte_crypto_sym_xform cipher_xform;
	struct rte_crypto_sym_xform auth_xform;
	struct rte_crypto_sym_xform *first_xform = NULL;
	struct rte_cryptodev_sym_session *session;
	enum crypto_chain_order order = CRYPTO_CHAIN_NOT_SUPPORTED;
	crypto_session_entry_t *entry;

	*session_out = ODP_CRYPTO_SESSION_INVALID;

	if (rte_cryptodev_count() == 0) {
		ODP_ERR("No crypto devices available\n");
		return -1;
	}

	/* Allocate memory for this session */
	entry = alloc_session();
	if (entry == NULL) {
		ODP_ERR("Failed to allocate a session entry");
		return -1;
	}

	/* Copy parameters */
	entry->p = *param;

	/* Default to successful result */
	*status = ODP_CRYPTO_SES_CREATE_ERR_NONE;

	cipher_xform.type = RTE_CRYPTO_SYM_XFORM_CIPHER;
	cipher_xform.next = NULL;
	rc = cipher_alg_odp_to_rte(param->cipher_alg, &cipher_xform);

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_INV_CIPHER;
		/* remove the crypto_session_entry_t */
		memset(entry, 0, sizeof(*entry));
		free_session(entry);
		return -1;
	}

	if (param->cipher_key.length) {
		/* Cipher Data */
		cipher_xform.cipher.key.data = rte_malloc("crypto key",
						   param->cipher_key.length, 0);
		if (cipher_xform.cipher.key.data == NULL) {
			ODP_ERR("Failed to allocate memory for cipher key\n");
			/* remove the crypto_session_entry_t */
			memset(entry, 0, sizeof(*entry));
			free_session(entry);
			return -1;
		}

		cipher_xform.cipher.key.length = param->cipher_key.length;
		memcpy(cipher_xform.cipher.key.data,
		       param->cipher_key.data,
		       param->cipher_key.length);
	} else {
		cipher_xform.cipher.key.length = 0;
		cipher_xform.cipher.key.data = 0;
	}

	auth_xform.type = RTE_CRYPTO_SYM_XFORM_AUTH;
	auth_xform.next = NULL;
	rc = auth_alg_odp_to_rte(param->auth_alg,
				 &auth_xform,
				 param->auth_digest_len);

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_INV_AUTH;
		/* remove the crypto_session_entry_t */
		memset(entry, 0, sizeof(*entry));
		free_session(entry);
		return -1;
	}

	if (param->auth_key.length) {
		/* Authentication Data */
		auth_xform.auth.key.data = rte_malloc("auth key",
						     param->auth_key.length, 0);
		if (auth_xform.auth.key.data == NULL) {
			ODP_ERR("Failed to allocate memory for auth key\n");
			/* remove the crypto_session_entry_t */
			memset(entry, 0, sizeof(*entry));
			free_session(entry);
			return -1;
		}
		auth_xform.auth.key.length = param->auth_key.length;
		memcpy(auth_xform.auth.key.data,
		       param->auth_key.data,
		       param->auth_key.length);
	} else {
		auth_xform.auth.key.data = 0;
		auth_xform.auth.key.length = 0;
	}


	/* Derive order */
	if (ODP_CRYPTO_OP_ENCODE == param->op) {
		cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_ENCRYPT;
		auth_xform.auth.op = RTE_CRYPTO_AUTH_OP_GENERATE;
		entry->do_cipher_first =  param->auth_cipher_text;
	} else {
		cipher_xform.cipher.op = RTE_CRYPTO_CIPHER_OP_DECRYPT;
		auth_xform.auth.op = RTE_CRYPTO_AUTH_OP_VERIFY;
		entry->do_cipher_first = !param->auth_cipher_text;
	}

	order = set_chain_order(entry->do_cipher_first,
				&first_xform,
				&auth_xform,
				&cipher_xform);
	if (order == CRYPTO_CHAIN_NOT_SUPPORTED) {
		ODP_ERR("Couldn't set chain order");
		/* remove the crypto_session_entry_t */
		memset(entry, 0, sizeof(*entry));
		free_session(entry);
		return -1;
	}

	rc = get_crypto_dev(first_xform,
			    order,
			    param->iv.length,
			    &cdev_id);

	if (rc) {
		ODP_ERR("Couldn't find a crypto device");
		/* remove the crypto_session_entry_t */
		memset(entry, 0, sizeof(*entry));
		free_session(entry);
		return -1;
	}

	/* Setup session */
	session = rte_cryptodev_sym_session_create(cdev_id, first_xform);

	if (session == NULL) {
		/* remove the crypto_session_entry_t */
		memset(entry, 0, sizeof(*entry));
		free_session(entry);
		return -1;
	}

	entry->rte_session  = (intptr_t)session;
	entry->cipher_xform = cipher_xform;
	entry->auth_xform = auth_xform;
	entry->iv.length = param->iv.length;
	entry->iv.data = param->iv.data;

	/* We're happy */
	*session_out = (intptr_t)entry;

	return 0;
}

int odp_crypto_session_destroy(odp_crypto_session_t session)
{
	struct rte_cryptodev_sym_session *rte_session = NULL;
	crypto_session_entry_t *entry;

	entry = (crypto_session_entry_t *)session;

	rte_session =
		(struct rte_cryptodev_sym_session *)
						(intptr_t)entry->rte_session;

	rte_session = rte_cryptodev_sym_session_free(rte_session->dev_id,
						     rte_session);

	if (rte_session != NULL)
		return -1;

	/* remove the crypto_session_entry_t */
	memset(entry, 0, sizeof(*entry));
	free_session(entry);

	return 0;
}

int odp_crypto_operation(odp_crypto_op_param_t *param,
			 odp_bool_t *posted,
			 odp_crypto_op_result_t *result)
{
	odp_crypto_packet_op_param_t packet_param;
	odp_packet_t out_pkt = param->out_pkt;
	odp_crypto_packet_result_t packet_result;
	odp_crypto_op_result_t local_result;
	int rc;

	packet_param.session = param->session;
	packet_param.override_iv_ptr = param->override_iv_ptr;
	packet_param.hash_result_offset = param->hash_result_offset;
	packet_param.aad.ptr = param->aad.ptr;
	packet_param.aad.length = param->aad.length;
	packet_param.cipher_range = param->cipher_range;
	packet_param.auth_range = param->auth_range;

	rc = odp_crypto_op(&param->pkt, &out_pkt, &packet_param, 1);
	if (rc < 0)
		return rc;

	rc = odp_crypto_result(&packet_result, out_pkt);
	if (rc < 0)
		return rc;

	/* Indicate to caller operation was sync */
	*posted = 0;

	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_BASIC);

	/* Fill in result */
	local_result.ctx = param->ctx;
	local_result.pkt = out_pkt;
	local_result.cipher_status = packet_result.cipher_status;
	local_result.auth_status = packet_result.auth_status;
	local_result.ok = packet_result.ok;

	/*
	 * Be bug-to-bug compatible. Return output packet also through params.
	 */
	param->out_pkt = out_pkt;

	*result = local_result;

	return 0;
}

int odp_crypto_term_global(void)
{
	int rc = 0;
	int ret;
	int count = 0;
	crypto_session_entry_t *session;

	odp_spinlock_init(&global->lock);
	odp_spinlock_lock(&global->lock);
	for (session = global->free; session != NULL; session = session->next)
		count++;
	if (count != MAX_SESSIONS) {
		ODP_ERR("crypto sessions still active\n");
		rc = -1;
	}

	if (global->crypto_op_pool != NULL)
		rte_mempool_free(global->crypto_op_pool);

	odp_spinlock_unlock(&global->lock);

	ret = odp_shm_free(crypto_global_shm);
	if (ret < 0) {
		ODP_ERR("shm free failed for crypto_pool\n");
		rc = -1;
	}

	return rc;
}

odp_random_kind_t odp_random_max_kind(void)
{
	return ODP_RANDOM_CRYPTO;
}

int32_t odp_random_data(uint8_t *buf, uint32_t len, odp_random_kind_t kind)
{
	int rc;

	switch (kind) {
	case ODP_RANDOM_BASIC:
	case ODP_RANDOM_CRYPTO:
		rc = RAND_bytes(buf, len);
		return (1 == rc) ? (int)len /*success*/: -1 /*failure*/;

	case ODP_RANDOM_TRUE:
	default:
		return -1;
	}
}

int32_t odp_random_test_data(uint8_t *buf, uint32_t len, uint64_t *seed)
{
	union {
		uint32_t rand_word;
		uint8_t rand_byte[4];
	} u;
	uint32_t i = 0, j;
	uint32_t seed32 = (*seed) & 0xffffffff;

	while (i < len) {
		u.rand_word = rand_r(&seed32);

		for (j = 0; j < 4 && i < len; j++, i++)
			*buf++ = u.rand_byte[j];
	}

	*seed = seed32;
	return len;
}

odp_crypto_compl_t odp_crypto_compl_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	if (odp_event_type(ev) != ODP_EVENT_CRYPTO_COMPL)
		ODP_ABORT("Event not a crypto completion");
	return (odp_crypto_compl_t)ev;
}

odp_event_t odp_crypto_compl_to_event(odp_crypto_compl_t completion_event)
{
	return (odp_event_t)completion_event;
}

void odp_crypto_compl_result(odp_crypto_compl_t completion_event,
			     odp_crypto_op_result_t *result)
{
	(void)completion_event;
	(void)result;

	/* We won't get such events anyway, so there can be no result */
	ODP_ASSERT(0);
}

void odp_crypto_compl_free(odp_crypto_compl_t completion_event)
{
	odp_event_t ev = odp_crypto_compl_to_event(completion_event);

	odp_buffer_free(odp_buffer_from_event(ev));
}

void odp_crypto_session_param_init(odp_crypto_session_param_t *param)
{
	memset(param, 0, sizeof(odp_crypto_session_param_t));
}

uint64_t odp_crypto_session_to_u64(odp_crypto_session_t hdl)
{
	return (uint64_t)hdl;
}

uint64_t odp_crypto_compl_to_u64(odp_crypto_compl_t hdl)
{
	return _odp_pri(hdl);
}

odp_packet_t odp_crypto_packet_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	ODP_ASSERT(odp_event_type(ev) == ODP_EVENT_PACKET);
	ODP_ASSERT(odp_event_subtype(ev) == ODP_EVENT_PACKET_CRYPTO);

	return odp_packet_from_event(ev);
}

odp_event_t odp_crypto_packet_to_event(odp_packet_t pkt)
{
	return odp_packet_to_event(pkt);
}

static
odp_crypto_packet_result_t *get_op_result_from_packet(odp_packet_t pkt)
{
	odp_packet_hdr_t *hdr = odp_packet_hdr(pkt);

	return &hdr->crypto_op_result;
}

int odp_crypto_result(odp_crypto_packet_result_t *result,
		      odp_packet_t packet)
{
	odp_crypto_packet_result_t *op_result;

	ODP_ASSERT(odp_event_subtype(odp_packet_to_event(packet)) ==
		   ODP_EVENT_PACKET_CRYPTO);

	op_result = get_op_result_from_packet(packet);

	memcpy(result, op_result, sizeof(*result));

	return 0;
}

static
int odp_crypto_int(odp_packet_t pkt_in,
		   odp_packet_t *pkt_out,
		   const odp_crypto_packet_op_param_t *param)
{
	crypto_session_entry_t *entry;
	odp_crypto_packet_result_t local_result;
	odp_crypto_alg_err_t rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
	odp_crypto_alg_err_t rc_auth = ODP_CRYPTO_ALG_ERR_NONE;
	struct rte_crypto_sym_xform cipher_xform;
	struct rte_crypto_sym_xform auth_xform;
	struct rte_cryptodev_sym_session *rte_session = NULL;
	uint8_t *data_addr, *aad_head;
	struct rte_crypto_op *op;
	uint32_t aad_len;
	odp_bool_t allocated = false;
	odp_packet_t out_pkt = *pkt_out;
	odp_crypto_packet_result_t *op_result;
	uint16_t rc;

	entry = (crypto_session_entry_t *)(intptr_t)param->session;
	if (entry == NULL)
		return -1;

	rte_session =
		(struct rte_cryptodev_sym_session *)
						(intptr_t)entry->rte_session;

	if (rte_session == NULL)
		return -1;

	cipher_xform = entry->cipher_xform;
	auth_xform = entry->auth_xform;

	/* Resolve output buffer */
	if (ODP_PACKET_INVALID == out_pkt &&
	    ODP_POOL_INVALID != entry->p.output_pool) {
		out_pkt = odp_packet_alloc(entry->p.output_pool,
					   odp_packet_len(pkt_in));
		allocated = true;
	}

	if (pkt_in != out_pkt) {
		if (odp_unlikely(ODP_PACKET_INVALID == out_pkt))
			ODP_ABORT();
		int ret;

		ret = odp_packet_copy_from_pkt(out_pkt,
					       0,
					       pkt_in,
					       0,
					       odp_packet_len(pkt_in));
		if (odp_unlikely(ret < 0))
			goto err;

		_odp_packet_copy_md_to_packet(pkt_in, out_pkt);
		odp_packet_free(pkt_in);
		pkt_in = ODP_PACKET_INVALID;
	}

	data_addr = odp_packet_data(out_pkt);

	odp_spinlock_init(&global->lock);
	odp_spinlock_lock(&global->lock);
	op = rte_crypto_op_alloc(global->crypto_op_pool,
				 RTE_CRYPTO_OP_TYPE_SYMMETRIC);
	if (op == NULL) {
		ODP_ERR("Failed to allocate crypto operation");
		goto err;
	}

	op->sym->auth.aad.data = NULL;
	op->sym->cipher.iv.data = NULL;

	odp_spinlock_unlock(&global->lock);

	/* Set crypto operation data parameters */
	rte_crypto_op_attach_sym_session(op, rte_session);
	op->sym->auth.digest.data = data_addr + param->hash_result_offset;
	op->sym->auth.digest.phys_addr =
		rte_pktmbuf_mtophys_offset((struct rte_mbuf *)out_pkt,
					   odp_packet_len(out_pkt) -
					   auth_xform.auth.digest_length);
	op->sym->auth.digest.length = auth_xform.auth.digest_length;

	/* For SNOW3G algorithms, offset/length must be in bits */
	if (auth_xform.auth.algo == RTE_CRYPTO_AUTH_SNOW3G_UIA2) {
		op->sym->auth.data.offset = param->auth_range.offset << 3;
		op->sym->auth.data.length = param->auth_range.length << 3;
	} else {
		op->sym->auth.data.offset = param->auth_range.offset;
		op->sym->auth.data.length = param->auth_range.length;
	}

	aad_head = param->aad.ptr;
	aad_len = param->aad.length;

	if (aad_len > 0) {
		op->sym->auth.aad.data = rte_malloc("aad", aad_len, 0);
		if (op->sym->auth.aad.data == NULL) {
			ODP_ERR("Failed to allocate memory for AAD");
			goto err_op_free;
		}

		memcpy(op->sym->auth.aad.data, aad_head, aad_len);
		op->sym->auth.aad.phys_addr =
				rte_malloc_virt2phy(op->sym->auth.aad.data);
		op->sym->auth.aad.length = aad_len;
	}


	if (entry->iv.length) {
		op->sym->cipher.iv.data = rte_malloc("iv", entry->iv.length, 0);
		if (op->sym->cipher.iv.data == NULL) {
			ODP_ERR("Failed to allocate memory for IV");
			goto err_op_free;
		}
	}

	if (param->override_iv_ptr) {
		memcpy(op->sym->cipher.iv.data,
		       param->override_iv_ptr,
		       entry->iv.length);
		op->sym->cipher.iv.phys_addr =
				rte_malloc_virt2phy(op->sym->cipher.iv.data);
		op->sym->cipher.iv.length = entry->iv.length;
	} else if (entry->iv.data) {
		memcpy(op->sym->cipher.iv.data,
		       entry->iv.data,
		       entry->iv.length);
		op->sym->cipher.iv.phys_addr =
				rte_malloc_virt2phy(op->sym->cipher.iv.data);
		op->sym->cipher.iv.length = entry->iv.length;
	} else {
		rc_cipher = ODP_CRYPTO_ALG_ERR_IV_INVALID;
	}

	/* For SNOW3G algorithms, offset/length must be in bits */
	if (cipher_xform.cipher.algo == RTE_CRYPTO_CIPHER_SNOW3G_UEA2) {
		op->sym->cipher.data.offset = param->cipher_range.offset << 3;
		op->sym->cipher.data.length = param->cipher_range.length << 3;

	} else {
		op->sym->cipher.data.offset = param->cipher_range.offset;
		op->sym->cipher.data.length = param->cipher_range.length;
	}

	if (rc_cipher == ODP_CRYPTO_ALG_ERR_NONE &&
	    rc_auth == ODP_CRYPTO_ALG_ERR_NONE) {
		int queue_pair = odp_cpu_id();

		op->sym->m_src = (struct rte_mbuf *)out_pkt;
		rc = rte_cryptodev_enqueue_burst(rte_session->dev_id,
						 queue_pair, &op, 1);
		if (rc == 0) {
			ODP_ERR("Failed to enqueue packet");
			goto err_op_free;
		}

		rc = rte_cryptodev_dequeue_burst(rte_session->dev_id,
						 queue_pair, &op, 1);

		if (rc == 0) {
			ODP_ERR("Failed to dequeue packet");
			goto err_op_free;
		}

		out_pkt = (odp_packet_t)op->sym->m_src;
	}

	/* Fill in result */
	local_result.cipher_status.alg_err = rc_cipher;
	local_result.cipher_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	local_result.auth_status.alg_err = rc_auth;
	local_result.auth_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	local_result.ok =
		(rc_cipher == ODP_CRYPTO_ALG_ERR_NONE) &&
		(rc_auth == ODP_CRYPTO_ALG_ERR_NONE);

	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_CRYPTO);
	op_result = get_op_result_from_packet(out_pkt);
	*op_result = local_result;

	rte_free(op->sym->cipher.iv.data);
	rte_free(op->sym->auth.aad.data);
	rte_crypto_op_free(op);

	/* Synchronous, simply return results */
	*pkt_out = out_pkt;

	return 0;

err_op_free:
	rte_free(op->sym->cipher.iv.data);
	rte_free(op->sym->auth.aad.data);
	rte_crypto_op_free(op);

err:
	if (allocated) {
		odp_packet_free(out_pkt);
		out_pkt = ODP_PACKET_INVALID;
	}

	return -1;
}

int odp_crypto_op(const odp_packet_t pkt_in[],
		  odp_packet_t pkt_out[],
		  const odp_crypto_packet_op_param_t param[],
		  int num_pkt)
{
	crypto_session_entry_t *entry;
	int i, rc;

	entry = (crypto_session_entry_t *)(intptr_t)param->session;
	ODP_ASSERT(ODP_CRYPTO_SYNC == entry->p.op_mode);

	for (i = 0; i < num_pkt; i++) {
		rc = odp_crypto_int(pkt_in[i], &pkt_out[i], &param[i]);
		if (rc < 0)
			break;
	}

	return i;
}

int odp_crypto_op_enq(const odp_packet_t pkt_in[],
		      const odp_packet_t pkt_out[],
		      const odp_crypto_packet_op_param_t param[],
		      int num_pkt)
{
	odp_packet_t pkt;
	odp_event_t event;
	crypto_session_entry_t *entry;
	int i, rc;

	entry = (crypto_session_entry_t *)(intptr_t)param->session;
	ODP_ASSERT(ODP_CRYPTO_ASYNC == entry->p.op_mode);
	ODP_ASSERT(ODP_QUEUE_INVALID != entry->p.compl_queue);

	for (i = 0; i < num_pkt; i++) {
		pkt = pkt_out[i];
		rc = odp_crypto_int(pkt_in[i], &pkt, &param[i]);
		if (rc < 0)
			break;

		event = odp_packet_to_event(pkt);
		if (odp_queue_enq(entry->p.compl_queue, event)) {
			odp_event_free(event);
			break;
		}
	}

	return i;
}