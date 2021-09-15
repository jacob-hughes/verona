// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../object/object.h"
#include "region_arena.h"
#include "region_trace.h"
#include "region_base.h"

namespace verona::rt
{
  using namespace snmalloc;

  class RegionRc : public RegionTrace
  {
    friend class Freeze;
    friend class Region;

  public:
    explicit RegionRc() : RegionTrace()
    {}

    inline static RegionRc* get(Object* o)
    {
      assert(o->debug_is_iso());
      assert(is_trace_region(o->get_region()));
      return (RegionRc*)o->get_region();
    }

    inline static bool is_rc_region(Object* o)
    {
      return o->is_type(desc());
    }

    template<size_t size = 0>
    static Object* create(Alloc& alloc, const Descriptor* desc)
    {
      void* p = alloc.alloc<vsizeof<RegionRc>>();
      Object* o = Object::register_object(p, RegionRc::desc());
      auto reg = new (o) RegionRc();
      reg->use_memory(desc->size);

      if constexpr (size == 0)
        p = alloc.alloc(desc->size);
      else
        p = alloc.alloc<size>();
      o = Object::register_object(p, desc);

      reg->init_next(o);
      o->init_iso();
      o->set_region(reg);

      assert(Object::debug_is_aligned(o));
      return o;
    }

    /**
     * Allocates an object `o` of type `desc` in the region represented by the
     * Iso object `in`, and adds it to the appropriate ring. Returns a pointer
     * to `o`.
     *
     * The default template parameter `size = 0` is to avoid writing two
     * definitions which differ only in one line. This overload works because
     * every object must contain a descriptor, so 0 is not a valid size.
     **/
    template<size_t size = 0>
    static Object* alloc(Alloc& alloc, Object* in, const Descriptor* desc)
    {
      assert((size == 0) || (size == desc->size));
      RegionRc* reg = get(in);

      assert(reg != nullptr);

      void* p = nullptr;
      if constexpr (size == 0)
        p = alloc.alloc(desc->size);
      else
        p = alloc.alloc<size>();

      // Pre-increment the count for the object before allocating
      /* desc |=  1UL; */

      auto o = (Object*)Object::register_object(p, desc);
      incref(o);
      assert(o->get_ref_count() == 1);
      assert(Object::debug_is_aligned(o));

      // Add to the ring.
      reg->append(o);

      // GC heuristics.
      reg->use_memory(desc->size);
      return o;
    }

  static bool incref(Object* o) {
      return o->incref_mut();
  }

  static bool decref(Alloc& alloc, Object* o, Object* in) {
      if (o->decref_mut()) {
          dealloc_object(alloc, o, in);
          return true;
      }
      return false;
  }


  private:
    static void dealloc_object(
      Alloc& alloc,
      Object* o,
      Object* region
      )
    {
        // We first need to decref -- and potentially deallocate -- any object
        // pointed to through `o`'s fields.
        ObjectStack dfs(alloc);
        ObjectStack sub_regions(alloc);
        o->trace(dfs);

        while (!dfs.empty()) {
            Object* p = dfs.pop();
            switch (p->get_class()) {
                case Object::ISO:
                    sub_regions.push(p);
                    break;
                case Object::MARKED:
                case Object::UNMARKED:
                case Object::SCC_PTR:
                case Object::RC:
                case Object::COWN:
                    decref(alloc, p, region);
                    break;
                default:
                    assert(0);
            }
        }

        if (!o->is_trivial()) {
            o->finalise(region, sub_regions);

        }

        // Unlike traced regions, we can deallocate this immediately, as
        // other finalizers will only look at it if the ref count > 0.
        o->dealloc(alloc);

        // Finally, we release any regions which were held by ISO pointers from
        // this object.
        while (!sub_regions.empty())
        {
            o = sub_regions.pop();
            assert(o->debug_is_iso());
            Systematic::cout() << "Region GC: releasing unreachable subregion: "
                << o << Systematic::endl;

            // Note that we need to dispatch because `r` is a different region
            // metadata object.
            RegionBase* r = o->get_region();
            assert(r != region);

            // Unfortunately, we can't use Region::release_internal because of a
            // circular dependency between header files.
            /* if (RegionTrace::is_trace_region(r)) */
            /*     ((RegionTrace*)r)->release_internal(alloc, o, sub_regions); */
            /* else if (RegionArena::is_arena_region(r)) */
            /*     ((RegionArena*)r)->release_internal(alloc, o, sub_regions); */
            /* else */
            /*     abort(); */
        }
    }
    bool debug_is_in_region(Object* o)
    {
      for (auto p : *this)
      {
        if (p == o)
          return true;
      }
      return false;
    }
  };
} // namespace verona::rt
