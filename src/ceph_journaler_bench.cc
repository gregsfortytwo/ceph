// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/stat.h>
#include <iostream>
#include <string>

#include "common/config.h"

#include "common/async/context_pool.h"


#include "common/Timer.h"
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "common/pick_address.h"
#include "common/Finisher.h"

#include "msg/Messenger.h"
#include "osdc/Journaler.h"
#include "mon/MonClient.h"

#include <sys/types.h>

const static int THREADS = 2;
const static int TIME = 30;

class JournalLoader {
public:
  pthread_t thread_id;
  Journaler *journal;
  int buffer_size;
  utime_t start;
  utime_t end;
  int64_t total_appends;
  ceph::mutex lock;
  bool keep_running;
  char *zeros{nullptr};
  
  JournalLoader(Journaler *j, int buffersize) :
    journal(j), buffer_size(buffersize), total_appends(0), lock("JournalLoader::lock"), keep_running(true) {
    zeros = new char[buffer_size];
  }

  ~JournalLoader() {
    delete zeros;
  }

  static void *start_thread(void *ptr) {
    JournalLoader *jl = static_cast<JournalLoader*>(ptr);
    jl->load();
    return 0;
  }
  void load() {
    bufferlist bl;
    bl.append(ceph::buffer::create_static(buffer_size, zeros));
    start = ceph_clock_now();
    lock.lock();
    while (keep_running) {
      lock.unlock();
      for (int i = 0; i < 50; ++i) {
	journal->append_entry(bl);
	++total_appends;
      }
      lock.lock();
    }
    lock.unlock();
    end = ceph_clock_now();
  }
  void start_thread() {
    pthread_create(&thread_id, NULL, start_thread, this);
  }
  void stop_thread() {
    keep_running = false;
  }
  void join_thread() {
    void *rv;
    pthread_join(thread_id, &rv);
  }
};

int main(int argc, const char **argv, char *envp[])
{
  //cerr << "ceph-journaler-bench starting" << std::endl;
  vector<const char*> args;
  argv_to_vec(argc, argv, args);

  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  // parse_bench_options(args);
  string name = "ceph_journaler_bench_journal";
  inodeno_t ino(0);
  int64_t pool(1);
  const char *magic = "ceph_journaler_bench_magic";
  int latency_key = 5001;
  int thread_count = 2;
  int buffer_size = 163840;
  int time = 30;
  
  pick_addresses(g_ceph_context, CEPH_PICK_ADDRESS_PUBLIC);

  // get monmap
  ceph::async::io_context_pool  poolctx(1);
  MonClient mc(g_ceph_context, poolctx);
  if (mc.build_initial_monmap() < 0)
    return -1;

  Messenger *messenger = Messenger::create_client_messenger(g_ceph_context, "journaler_bench");
  messenger->set_default_policy(Messenger::Policy::lossy_client(0));
  messenger->start();

  Objecter objecter(g_ceph_context, messenger, &mc, poolctx);

  PerfCountersBuilder plb(g_ceph_context, "journaler_bench", 5000, 5002);
  plb.add_time_avg(latency_key, "jlat", "Journaler flush latency");
  PerfCounters *logger = plb.create_perf_counters();
  g_ceph_context->get_perfcounters_collection()->add(logger);

  Finisher finisher(g_ceph_context, "ceph_journaler_bench", "bench_finisher");
  
  cout << "ceph-journaler-bench: starting" << std::endl;

  messenger->add_dispatcher_tail(&objecter);
  mc.set_messenger(messenger);
  mc.set_want_keys(CEPH_ENTITY_TYPE_OSD);
  mc.init();
  objecter.init();
  objecter.start();

  Journaler journaler(name, ino, pool, magic, &objecter, logger, latency_key, &finisher);

  list<JournalLoader*> loaders;
  for (int i = 0; i < thread_count; ++i) {
    loaders.push_back(new JournalLoader(&journaler, buffer_size));
  }


  cout << "ceph-journaler-bench: created " << loaders.size() << " loaders" << std::endl;

  file_layout_t log_layout = file_layout_t::get_default();
  log_layout.pool_id = pool;
  
  journaler.set_writeable();
  journaler.create(&log_layout, g_conf()->mds_journal_format);

  ceph::mutex timer_mutex("timer_mutex");
  std::unique_lock<ceph::mutex> timer_lock(timer_mutex);
  ceph::condition_variable cond;
  C_SaferCond safer_cond;
  bool header_done = false;
  int rval = -242;
  journaler.write_head(new C_SafeCond(timer_mutex, cond, &header_done, &rval));

  cout << "waiting for journal header write" << std::endl;
  while (!header_done) {
    cond.wait(timer_lock);
  }
  if (rval != 0) {
    cerr << "Error writing journal header: " << rval << std::endl;
    return rval;
  }
  
  for (auto i : loaders) {
    i->start_thread();
  }

  cout << "ceph-journaler-bench: started " << loaders.size() << " loaders; waiting for " << time << std::endl;

  utime_t start = ceph_clock_now();
  utime_t finish(start);
  finish += time;
  do {
    cond.wait_for(timer_lock, std::chrono::seconds(1));
    journaler.flush();
  } while (ceph_clock_now() < finish);

  cout << "timer expired" << std::endl;

  for (auto i : loaders) {
    i->stop_thread();
  }

  utime_t flush_start = ceph_clock_now();
  bool flush_done = false;
  rval = -242;
  journaler.flush(new C_SafeCond(timer_mutex, cond, &flush_done, &rval));
  
  for (auto i : loaders) {
    i->join_thread();
  }

  while (!flush_done) {
    cond.wait(timer_lock);
  }
  utime_t flush_end = ceph_clock_now();

  cout << "flush and thread joins took " << (double(flush_end) - flush_start)
       << " secs" << " with r " << rval << std::endl;

  int total_appends = 0;
  int loader_i = 0;
  for (auto i : loaders) {
    cout << "loader " << loader_i << ": " << i->total_appends << ", "
	 << i->end - i->start << " seconds" << std::endl;
    total_appends += i->total_appends;
    ++loader_i;
  }
  cout << "total appends: " << total_appends << std::endl;
  cout << "average appends/thread: " << (double(total_appends) / thread_count) << std::endl;
  cout << "total appends/s: " << (double(total_appends) / time) << std::endl;
  cout << "average appends/thread/s: " << ((double(total_appends) / thread_count) / time) << std::endl;
  
  for (auto i : loaders) {
    delete i;
  }
  objecter.shutdown();
  mc.shutdown();
  messenger->shutdown();
  messenger->wait();
  delete messenger;
  poolctx.stop();

  cout << "ceph-journaler-bench: done" << std::endl;

  return 0;
}
