// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 * Copyright (C) 2016 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "gtest/gtest.h"
#include "include/cephfs/libcephfs.h"
#include "client/UserPerm.h"

static int update_root_mode()
{
  struct ceph_mount_info *admin;
  int r = ceph_create(&admin, NULL);
  if (r < 0)
    return r;
  ceph_conf_read_file(admin, NULL);
  ceph_conf_parse_env(admin, NULL);
  ceph_conf_set(admin, "client_permissions", "false");
  r = ceph_mount(admin, "/");

  // Horrifying hack: we need to chmod as the root user, but don't
  // have a great way of doing so.
  UserPerm *mount_perms = ceph_mount_perms(admin);
  UserPerm existing_perms = *mount_perms;
  UserPerm root_perms(0, 0);

  if (r < 0)
    goto out;
  
  *mount_perms = root_perms;
  r = ceph_chmod(admin, "/", 01777);
  *mount_perms = existing_perms;
out:
  ceph_shutdown(admin);
  return r;
}


int main(int argc, char **argv)
{
  int r = update_root_mode();
  if (r < 0)
    exit(1);

  ::testing::InitGoogleTest(&argc, argv);

  srand(getpid());

  return RUN_ALL_TESTS();
}
