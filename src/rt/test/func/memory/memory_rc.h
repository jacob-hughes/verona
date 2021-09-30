// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "memory.h"

namespace memory_rc
{
  constexpr auto region_type = RegionType::Rc;
  using C = C1<region_type>;
  using F = F1<region_type>;
  using MC = MediumC2<region_type>;
  using MF = MediumF2<region_type>;
  using LC = LargeC2<region_type>;
  using LF = LargeF2<region_type>;
  using XC = XLargeC2<region_type>;
  using XF = XLargeF2<region_type>;

  using Cx = C3<region_type>;
  using Fx = F3<region_type>;

  void test_rc_stack() {
      auto& alloc = ThreadAlloc::get();
      auto* o = new (alloc) C;

      auto* o1 = new (alloc, o) C;
      auto* o2 = new (alloc, o) C;
      auto* o3 = new (alloc, o) C;
      UNUSED(o1);
      UNUSED(o2);
      UNUSED(o3);

      auto reg = (RegionRc*) Region::get(o);
      RcStack<Alloc>::iterator iter(reg->get_non_trivial_stack());
      for(auto object_count : iter) {
          assert(object_count->count == 0);
      }
  }

  void run_test()
  {
    test_rc_stack();
    /* test_basic(); */
    /* test_additional_roots(); */
    /* test_linked_list(); */
    /* test_freeze(); */
    /* test_cycles(); */
    /* test_merge(); */
    /* test_swap_root(); */
  }
}
