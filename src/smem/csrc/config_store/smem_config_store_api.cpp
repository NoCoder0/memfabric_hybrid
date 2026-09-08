/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "smem_config_store_api.h"
#include <cstdio>
#include <new>
#include "smem_bm_def.h"
#include "smem_define.h"
#include "smem_ha_config_store.h"
#include "smem_store_factory.h"

namespace {
using namespace ock;
using StorePtr = smem::StorePtr;
StorePtr *AsStore(smem_config_store_t value)
{
    return static_cast<StorePtr *>(value);
}

template<size_t N>
void CopyString(char (&target)[N], const char *source)
{
    std::snprintf(target, N, "%s", source == nullptr ? "" : source);
}
int32_t SetTls(const smem_tls_options_t *tls, const char *library, const char *decrypter)
{
    smem_tls_config config{};
    if (tls != nullptr) {
        config.tlsEnable = tls->enable_tls != 0;
        CopyString(config.caPath, tls->tls_ca_file);
        CopyString(config.crlPath, tls->tls_crl_file);
        CopyString(config.certPath, tls->tls_cert);
        CopyString(config.keyPath, tls->tls_private_key);
        CopyString(config.keyPassPath, tls->tls_private_key_password);
    }
    CopyString(config.packagePath, library);
    CopyString(config.decrypterLibPath, decrypter);
    smem::StoreFactory::SetTlsInfo(config);
    return 0;
}

smem_config_store_t Create(const char *url)
{
    if (url == nullptr) {
        return nullptr;
    }
    auto store = smem::StoreFactory::CreateStoreByUrl(url, smem::ConfigStoreModel::CSM_SERVER);
    return store == nullptr ? nullptr : new (std::nothrow) StorePtr(store);
}

void Destroy(smem_config_store_t store, const char *url)
{
    auto *native = AsStore(store);
    if (native != nullptr && *native != nullptr) {
        (*native)->RegisterServerBrokenHandler(nullptr);
    }
    delete AsStore(store);
    if (url != nullptr) {
        smem::StoreFactory::DestroyStore(url);
    }
}

void SetBrokenHandler(smem_config_store_t store, smem_config_store_broken_cb callback, void *context)
{
    auto *native = AsStore(store);
    if (native == nullptr || *native == nullptr) {
        return;
    }
    if (callback == nullptr) {
        (*native)->RegisterServerBrokenHandler(nullptr);
        return;
    }
    StorePtr retained = *native;
    (*native)->RegisterServerBrokenHandler([callback, context, retained](uint32_t linkId, smem::StoreBackendPtr) {
        return callback(context, linkId, retained->GetRankIdByLinkId(linkId));
    });
}

int32_t SetLeaderHandler(smem_config_store_t store, smem_config_store_leader_change_cb callback, void *context)
{
    auto *native = AsStore(store);
    if (native == nullptr || *native == nullptr) {
        return -1;
    }
    auto *ha = (*native)->AsHaConfigStore();
    if (ha == nullptr) {
        return 1;
    }
    if (callback == nullptr) {
        ha->RegisterLeaderChangeCallback(nullptr);
    } else {
        ha->RegisterLeaderChangeCallback([callback, context](const smem::HaConfigStore::LeaderAddresses &addresses) {
            smem_config_store_leader_info_t info{static_cast<uint8_t>(addresses.isLeader),
                                                 addresses.metaServiceAddr.c_str()};
            callback(context, &info);
        });
    }
    return 0;
}
} // namespace

extern "C" {
SMEM_API int32_t smem_config_store_set_tls(const smem_tls_options_t *tls, const char *tlsLibrary,
                                           const char *decrypterLibrary)
{
    return SetTls(tls, tlsLibrary, decrypterLibrary);
}

SMEM_API smem_config_store_t smem_config_store_create_server(const char *url)
{
    return Create(url);
}

SMEM_API void smem_config_store_destroy_server(smem_config_store_t store, const char *url)
{
    Destroy(store, url);
}

SMEM_API void smem_config_store_set_server_broken_handler(smem_config_store_t store,
                                                          smem_config_store_broken_cb callback, void *context)
{
    SetBrokenHandler(store, callback, context);
}

SMEM_API int32_t smem_config_store_set_leader_handler(smem_config_store_t store,
                                                      smem_config_store_leader_change_cb callback, void *context)
{
    return SetLeaderHandler(store, callback, context);
}
}
