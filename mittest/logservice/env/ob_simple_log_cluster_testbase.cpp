/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define private public
#define protected public
#include "logservice/ob_log_io_adapter.h"
#undef protected
#undef private
#include "ob_simple_log_cluster_testbase.h"
#include "common/ob_member_list.h"
#include "share/allocator/ob_tenant_mutil_allocator_mgr.h"
#include "ob_mittest_utils.h"
#include "lib/alloc/memory_dump.h"
#include "lib/alloc/alloc_func.cpp"
#include "lib/allocator/ob_mem_leak_checker.h"
#include "lib/allocator/ob_libeasy_mem_pool.h"
#include "share/io/ob_io_define.h"
#include "src/share/ob_device_manager.h"
#include <algorithm>
#include "share/resource_manager/ob_resource_manager.h"       // ObResourceManager

namespace oceanbase
{
using namespace logservice;
namespace unittest
{
// int64_t ObSimpleLogClusterTestBase::member_cnt_ = 3;
// int64_t ObSimpleLogClusterTestBase::node_cnt_ = 7;
std::vector<ObISimpleLogServer*> ObSimpleLogClusterTestBase::cluster_;
bool ObSimpleLogClusterTestBase::is_started_ = false;
common::ObMemberList ObSimpleLogClusterTestBase::member_list_ = ObMemberList();
common::hash::ObHashMap<common::ObAddr, common::ObRegion> ObSimpleLogClusterTestBase::member_region_map_;
common::ObMemberList ObSimpleLogClusterTestBase::node_list_ = ObMemberList();
int64_t ObSimpleLogClusterTestBase::node_idx_base_ = 1002;
char ObSimpleLogClusterTestBase::sig_buf_[sizeof(ObSignalWorker) + sizeof(observer::ObSignalHandle)];
ObSignalWorker *ObSimpleLogClusterTestBase::sig_worker_ = new (sig_buf_) ObSignalWorker();
observer::ObSignalHandle *ObSimpleLogClusterTestBase::signal_handle_ = new (sig_worker_ + 1) observer::ObSignalHandle();
bool ObSimpleLogClusterTestBase::disable_hot_cache_ = false;
int64_t ObSimpleLogClusterTestBase::tenant_id_ = ObISimpleLogServer::DEFAULT_TENANT_ID;
ObTenantIOManager *ObSimpleLogClusterTestBase::tio_manager_ = nullptr;
ObAddr ObSimpleLogClusterTestBase::remote_log_store_addr_ = ObAddr(ObAddr::VER::IPV4, "127.0.0.1", 50051);

void ObSimpleLogClusterTestBase::SetUpTestCase()
{
  SERVER_LOG(INFO, "SetUpTestCase", K(member_cnt_), K(node_cnt_));
  int ret = OB_SUCCESS;
  if (!is_started_) {
    ret = start();
  }
  ASSERT_EQ(ret, OB_SUCCESS);
}

void ObSimpleLogClusterTestBase::TearDownTestCase()
{
  SERVER_LOG(INFO, "TearDownTestCase", K(member_cnt_), K(node_cnt_));
  int ret = OB_SUCCESS;

  if (cluster_.size() != 0) {
    ret = close();
    ASSERT_EQ(ret, OB_SUCCESS);
  }
  for (auto svr : cluster_) {
    if (OB_NOT_NULL(svr)) {
      ob_delete(svr);
    }
  }
}

int ObSimpleLogClusterTestBase::init_log_store(const ObAddr &addr)
{
  int ret = OB_SUCCESS;
  remote_log_store_addr_ = addr;
  if (ObSimpleLogClusterTestBase::node_cnt_ != 1 && ObSimpleLogClusterTestBase::member_cnt_ != 1) {
    ret = OB_NOT_SUPPORTED;
    CLOG_LOG(ERROR, "not supported ObLogStore in multi replica");
  }
  CLOG_LOG(INFO, "init_log_store", K(addr));
  ObLogIOInfo io_info; io_info.io_mode_ = ObLogIOMode::REMOTE; io_info.log_store_addr_ = addr; io_info.cluster_id_ = 1;
  return LOG_IO_ADAPTER.switch_log_io_mode(io_info);
}

int ObSimpleLogClusterTestBase::start()
{
  int ret = OB_SUCCESS;
  int64_t member_cnt = 0;
  ObTenantMutilAllocatorMgr::get_instance().init();
  auto malloc = ObMallocAllocator::get_instance();
  malloc->create_and_add_tenant_allocator(tenant_id_);
  ObMemoryDump::get_instance().init();
  // set easy allocator for watching easy memory holding
  easy_pool_set_allocator(ob_easy_realloc);
  ev_set_allocator(ob_easy_realloc);
  lib::set_memory_limit(10L * 1000L * 1000L * 1000L);
  const uint64_t mittest_memory = 6L * 1024L * 1024L * 1024L;
  ObTenantBase *tmp_base = OB_NEW(ObTenantBase, "mittest", tenant_id_);
  share::ObTenantEnv::set_tenant(tmp_base);
  const std::string clog_dir = test_name_;
  const int64_t disk_io_thread_count = 256;
  const int64_t max_io_depth = 8;
  ObLogIOInfo log_io_info;
  ObLogIOMode mode = (ObSimpleLogClusterTestBase::need_remote_log_store_ ? ObLogIOMode::REMOTE : ObLogIOMode::LOCAL);
  oceanbase::lib::reload_diagnose_info_config(true);
  log_io_info.io_mode_ = mode; log_io_info.cluster_id_ = 1; log_io_info.log_store_addr_ = remote_log_store_addr_;
  if (sig_worker_ != nullptr && OB_FAIL(sig_worker_->start())) {
    SERVER_LOG(ERROR, "Start signal worker error", K(ret));
  } else if (signal_handle_ != nullptr && OB_FAIL(signal_handle_->start())) {
    SERVER_LOG(ERROR, "Start signal handle error", K(ret));
  } else if (OB_FAIL(member_region_map_.create(OB_MAX_MEMBER_NUMBER,
      ObMemAttr(MTL_ID(), ObModIds::OB_HASH_NODE, ObCtxIds::DEFAULT_CTX_ID)))) {
  } else if (OB_FAIL(generate_sorted_server_list_(node_cnt_))) {
  } else if (OB_FAIL(G_RES_MGR.init())) {
    SERVER_LOG(ERROR, "init ObResourceManager failed", K(ret));
  } else if (OB_FAIL(ObDeviceManager::get_instance().init_devices_env())) {
    STORAGE_LOG(WARN, "init device manager failed", KR(ret));
  } else if (OB_FAIL(ObIOManager::get_instance().init(mittest_memory))) {
    SERVER_LOG(ERROR, "init ObIOManager failed");
  } else if (OB_FAIL(ObIOManager::get_instance().start())) {
    SERVER_LOG(ERROR, "start ObIOManager failed");
  } else if (OB_FAIL(ObTenantIOManager::mtl_new(tio_manager_))) {
    SERVER_LOG(ERROR, "new tenant io manager failed", K(ret));
  } else if (OB_FAIL(ObTenantIOManager::mtl_init(tio_manager_))) {
    SERVER_LOG(ERROR, "init tenant io manager failed", K(ret));
  } else if (OB_FAIL(tio_manager_->start())) {
    SERVER_LOG(ERROR, "start tenant io manager failed", K(ret));
  } else if (OB_FAIL(LOG_IO_ADAPTER.init(clog_dir.c_str(), disk_io_thread_count, max_io_depth, log_io_info, &OB_IO_MANAGER, &ObDeviceManager::get_instance()))) {
    SERVER_LOG(ERROR, "LOG_IO_ADAPTER init failed", K(ret), K(log_io_info));
  } else {
    ObTenantIOConfig io_config;
    io_config.unit_config_.max_iops_ = 10000000;
    io_config.unit_config_.min_iops_ = 10000000;
    io_config.unit_config_.weight_ = 10000000;
    tio_manager_->update_basic_io_config(io_config);
    // 如果需要新增arb server，将其作为memberlist最后一项
    // TODO by runlin, 这个是暂时的解决方法，以后可以走加减成员的流程
    const int64_t arb_idx = member_cnt_ - 1;
    int64_t node_id = node_idx_base_;
    for (int i = 0; OB_SUCC(ret) && i < node_cnt_; i++) {
      ObISimpleLogServer *svr = NULL;
      if (i == arb_idx && true == need_add_arb_server_) {
        svr = OB_NEW(ObSimpleArbServer, "TestBase");
      } else {
        svr = OB_NEW(ObSimpleLogServer, "TestBase");
      }
      common::ObAddr server;
      if (OB_FAIL(node_list_.get_server_by_index(i, server))) {
      } else if (OB_FAIL(svr->simple_init(test_name_, server, node_id, tio_manager_, &member_region_map_, true))) {
        SERVER_LOG(WARN, "simple_init failed", K(ret), K(i), K_(node_list));
      } else if (OB_FAIL(svr->simple_start(true))) {
        SERVER_LOG(WARN, "simple_start failed", K(ret), K(i), K_(node_list));
      } else {
        node_id += 2;
        cluster_.push_back(svr);
      }
      if (i < ObSimpleLogClusterTestBase::member_cnt_ && OB_SUCC(ret)) {
        common::ObMember member;
        if (OB_FAIL(member_list_.add_member(ObMember(server, 1)))) {
        } else if (OB_FAIL(member_region_map_.set_refactored(server, DEFAULT_REGION_NAME))) {
          SERVER_LOG(WARN, "member_region_map_.insert failed", K(ret), K(server), K_(node_list));
        }
      }
      usleep(500);
      SERVER_LOG(INFO, "ObSimpleLogClusterTestBase start success", K(node_id), K(server));
    }
    if (OB_SUCC(ret)) {
      is_started_ = true;
    }
    SERVER_LOG(INFO, "ObSimpleLogClusterTestBase started", K(ret), K_(member_cnt), K_(node_cnt), K_(node_list), K(member_list_));
  }
  return ret;
}

int ObSimpleLogClusterTestBase::close()
{
  int ret = OB_SUCCESS;
  for (auto svr : cluster_) {
    ret = svr->simple_close(true);
    if (OB_FAIL(ret)) {
      SERVER_LOG(WARN, "simple_close failed", K(ret));
      break;
    }
  }

  LOG_IO_ADAPTER.destroy();
  ObIOManager::get_instance().stop();
  ObIOManager::get_instance().destroy();
  ObDeviceManager::get_instance().destroy();
  return ret;
}

int ObSimpleLogClusterTestBase::generate_sorted_server_list_(const int64_t node_cnt)
{
  int ret = OB_SUCCESS;
  // each get_rpc_port calling will get two available ports,
  // so just get node_cnt / 2 + 1 times
  const int64_t get_port_cnt = node_cnt / 2 + 1;
  for (int i = 0; i < get_port_cnt; i++) {
    int server_fd = 0;
    const std::string local_ip = get_local_ip();
    const int64_t port = get_rpc_port(server_fd);
    common::ObAddr addr;
    if (0 == port) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(ERROR, "get_rpc_port failed", K(ret), K(port));
      break;
    } else if (local_ip == "") {
      ret = OB_ERR_UNEXPECTED;
    } else if (false == addr.set_ip_addr(local_ip.c_str(), port)) {
      SERVER_LOG(ERROR, "set_ip_addr failed", K(local_ip.c_str()), K(port), K(addr));
    } else if (OB_FAIL(node_list_.add_server(addr))) {
      PALF_LOG(WARN, "add_server failed", K(ret));
    } else if (false == addr.set_ip_addr(local_ip.c_str(), port + 1)) {
      SERVER_LOG(ERROR, "set_ip_addr failed", K(local_ip.c_str()), K(port), K(addr));
    } else if (node_list_.get_member_number() < node_cnt && OB_FAIL(node_list_.add_server(addr))) {
      PALF_LOG(WARN, "add_server failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    SERVER_LOG(INFO, "simple log cluster node_list", K_(node_list));
  }
  return ret;
}

} // end unittest
} // end oceanbase
