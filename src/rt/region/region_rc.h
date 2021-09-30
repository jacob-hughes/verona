// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../object/object.h"
#include "region_arena.h"
#include "region_base.h"

namespace verona::rt
{
  using namespace snmalloc;

  struct ObjectCount {
      Object* object;
      uintptr_t count;
  };

  /**
   * A Region Vector is used to track all allocations in a region. It is a vector-like data structure 
   * optimised for constant time insertion and removal. 
   * 
   * Removal is O(1) because a removed item will leave a hole in the vector rather than 
   * shifting remaining items down. To reduce fragmentation which can occur with deallocation churn,
   * the region vector maintains a freelist which is threaded through the holes left in the vector.
   * Insertion of new items will first query the freelist to see if a hole can be reused, 
   * otherwise items are bump allocated.
   * 
   * To maintain an internal freelist with no additional space requirements, the item `T` must be at
   * least 1 machine word in size.
   */
  template<class T, class Alloc>
  class RegionVector {
    static constexpr size_t POINTER_COUNT = 64;
    static_assert(
      snmalloc::bits::next_pow2_const(POINTER_COUNT) == POINTER_COUNT,
      "Should be power of 2 for alignment.");

    static constexpr size_t STACK_COUNT = POINTER_COUNT - 1;
    static constexpr uintptr_t EMPTY_MASK = 1 << 0;

    struct alignas(POINTER_COUNT * sizeof(T)) Block
    {
      T* prev;
      T data[STACK_COUNT];
    };

      private:
    // Used to thread a freelist pointer through the stack.
    T* next_free;

      public:

    RegionVector<T, Alloc>() : next_free(nullptr) {
      static_assert(
        sizeof(*this) == sizeof(void*),
        "Stack should contain only the index pointer");
    }

    /// Deallocate the linked blocks for this stack.
    void dealloc(Alloc& alloc)
    {
      auto local_block = get_block(index);
      while (local_block != &null_block)
      {
        auto prev = get_block(local_block->prev);
        alloc.template dealloc<sizeof(Block)>(local_block);
        local_block = prev;
      }
    }

    /// returns true if this stack is empty
    ALWAYSINLINE bool empty()
    {
      return index == null_index;
    }

    /// Return the top element of the stack without removing it.
    ALWAYSINLINE T* peek()
    {
      assert(!empty());
      return *index;
    }

    /// Call this to push an element onto the stack.
    ALWAYSINLINE T* push(T item, Alloc& alloc)
    {
      if (!is_full(index))
      {
        index++;
        *index = item;
        return index;
      }

      return push_slow(item, alloc);
    }

  private:
    /// Slow path for push, performs a push, when allocation is required.
    T* push_slow(T item, Alloc& alloc)
    {
      assert(is_full(index));

      Block* next = (Block*)alloc.template alloc<sizeof(Block)>();
      assert(((uintptr_t)next) % alignof(Block) == 0);
      next->prev = index;
      index = &(next->data[0]);
      *index = item;
      return index;
    }

    /// Put an element on the stack.
    ALWAYSINLINE T* push(T item, Alloc& alloc)
    {
        // if (next_free != nullptr) {
        //     T* prev = (T*) ((uintptr_t) next_free->object | EMPTY_MASK);
        //     *next_free = item;
        //     T* cur = next_free;
        //     next_free = prev;
        //     return cur;
        // }
        return push_slow(oc, alloc);
    }

    ALWAYSINLINE void remove(T* index)
    {
        index->object = (T*) ((uintptr_t) index | EMPTY_MASK);
        next_free = index;
    }

  public:
    class iterator
    {
      friend class RegionVector;

        public:
      iterator(RegionVector<T, Alloc>* stack) : stack(stack)
      {
        // If the stack is empty then there is nothing to iterate over.
        if (stack->empty()) {
          ptr = nullptr;
          return;
        }

        ptr = stack->peek();
      }

      iterator(RegionVector<T, Alloc>* stack, T* p) : stack(stack), ptr(p) {}

      iterator operator++()
      {
        if (!stack->is_empty(ptr)) {
            ptr--;
            return *this;
        }

        if (ptr != stack->null_index) {
            ptr = stack->get_block(ptr)->prev;
            return *this;
        }

        ptr = nullptr;
        return *this;
      }

      inline bool operator!=(const iterator& other) const
      {
        return ptr != other.ptr;
      }

      inline bool operator==(const iterator& other) const
      {
        return ptr == other.ptr;
      }

      inline ObjectCount* operator*() const
      {
        return ptr;
      }
    inline iterator begin()
    {
      return {stack};
    }

    inline iterator end()
    {
      return {stack, nullptr};
    }

    private:
      RegionVector<T, Alloc>* stack;
      T* ptr;
    };


  };

  class RegionRc: public RegionBase
  {
    friend class Freeze;
    friend class Region;


  private:
    enum StackKind
    {
      TrivialStack,
      NonTrivialStack,
    };

    RegionVector<ObjectCount, Alloc> trivial_counts{};
    RegionVector<ObjectCount, Alloc> non_trivial_counts{};

    // Memory usage in the region.
    size_t current_memory_used = 0;

    // Compact representation of previous memory used as a sizeclass.
    snmalloc::sizeclass_t previous_memory_used = 0;

    // Stack of stack based entry points into the region.
    StackThin<Object, Alloc> additional_entry_points{};

    RegionRc() : RegionBase() {}

    static const Descriptor* desc()
    {
      static constexpr Descriptor desc = {
        vsizeof<RegionRc>, nullptr, nullptr, nullptr};

      return &desc;
    }

  public:
    inline static RegionRc* get(Object* o)
    {
      assert(o->debug_is_iso());
      assert(is_rc_region(o->get_region()));
      return (RegionRc*)o->get_region();
    }

    inline static bool is_rc_region(Object* o)
    {
      return o->is_type(desc());
    }

    inline RegionVector<ObjectCount, Alloc>* get_trivial_stack() {
        return &trivial_counts;
    }

    inline RegionVector<ObjectCount, Alloc>* get_non_trivial_stack() {
        return &non_trivial_counts;
    }

    /**
     * Creates a new trace region by allocating Object `o` of type `desc`. The
     * object is initialised as the Iso object for that region, and points to a
     * newly created Region metadata object. Returns a pointer to `o`.
     *
     * The default template parameter `size = 0` is to avoid writing two
     * definitions which differ only in one line. This overload works because
     * every object must contain a descriptor, so 0 is not a valid size.
     **/
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

      auto o = (Object*)Object::register_object(p, desc);
      assert(Object::debug_is_aligned(o));

      // Add to the stack.

      Object* stack_loc = (Object*) reg->get_non_trivial_stack()->push(o, alloc);

      // And add back pointer to the object header
      o->set_next(stack_loc);

      // GC heuristics.
      reg->use_memory(desc->size);
      return o;
    }

    static void incref(Object* o) {
        ObjectCount* oc = (ObjectCount*) o->get_next();
        oc->count += 1;
    }

    static bool decref(Alloc& alloc, Object* o, Object* in) {
        if (decref_inner(o)) {
            dealloc_object(alloc, o, in);
            return true;
        }
        return false;
    }

    /**
     * Run a garbage collection on the region represented by the Object `o`.
     * Only `o`'s region will be GC'd; we ignore pointers to Immutables and
     * other regions.
     **/
    static void gc(Alloc& alloc, Object* o)
    {
      Systematic::cout() << "Region GC called for: " << o << Systematic::endl;
      assert(o->debug_is_iso());
      assert(is_rc_region(o->get_region()));

      RegionRc* reg = get(o);
      ObjectStack f(alloc);
      ObjectStack collect(alloc);

      // Copy additional roots into f.
      reg->additional_entry_points.forall([&f](Object* o) {
        Systematic::cout() << "Additional root: " << o << Systematic::endl;
        f.push(o);
      });

      reg->mark(alloc, o, f);
      reg->sweep(alloc, o, collect);

      // `collect` contains all the iso objects to unreachable subregions.
      // Since they are unreachable, we can just release them.
      while (!collect.empty())
      {
        o = collect.pop();
        assert(o->debug_is_iso());
        Systematic::cout() << "Region GC: releasing unreachable subregion: "
                           << o << Systematic::endl;

        // Note that we need to dispatch because `r` is a different region
        // metadata object.
        RegionBase* r = o->get_region();
        assert(r != reg);

        // Unfortunately, we can't use Region::release_internal because of a
        // circular dependency between header files.
        if (RegionTrace::is_trace_region(r))
          ((RegionTrace*)r)->release_internal(alloc, o, collect);
        else if (RegionArena::is_arena_region(r))
          ((RegionArena*)r)->release_internal(alloc, o, collect);
        else
          abort();
      }
    }

  private:

    inline static bool decref_inner(
      Object* o
    ) {
        ObjectCount* oc = (ObjectCount*) o->get_next();
        if (oc->count == 1) {
            return true;
        }
        oc->count -= 1;
        return false;
    }

    static void dealloc_object(
      Alloc& alloc,
      Object* o,
      Object* in
      )
    {
        // We first need to decref -- and potentially deallocate -- any object
        // pointed to through `o`'s fields.
        ObjectStack dfs(alloc);
        ObjectStack sub_regions(alloc);
        o->trace(dfs);
        ObjectStack flzr_q(alloc);

        RegionRc* reg = get(in);

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
                    if (decref_inner(p)) {
                        flzr_q.push(p);
                    }
                    break;
                default:
                    assert(0);
            }
        }

        while (!flzr_q.empty()) {
            o = flzr_q.pop();
            ObjectCount* entry = (ObjectCount*) o->get_next();
            if (!o->is_trivial()) {
                reg->non_trivial_counts.remove(entry);
                o->finalise(in, sub_regions);
            } else {
                reg->trivial_counts.remove(entry);
            }
            // Unlike traced regions, we can deallocate this immediately after
            // finalization.
            o->dealloc(alloc);
        }



        // Finally, we release any regions which were held by ISO pointers from
        // this object.
        /* while (!sub_regions.empty()) */
        /* { */
        /*     o = sub_regions.pop(); */
        /*     assert(o->debug_is_iso()); */
        /*     Systematic::cout() << "Region GC: releasing unreachable subregion: " */
        /*         << o << Systematic::endl; */

        /*     // Note that we need to dispatch because `r` is a different region */
        /*     // metadata object. */
        /*     RegionBase* r = o->get_region(); */
        /*     assert(r != region); */

        /*     // Unfortunately, we can't use Region::release_internal because of a */
        /*     // circular dependency between header files. */
        /*     if (RegionTrace::is_trace_region(r)) */
        /*         ((RegionTrace*)r)->release_internal(alloc, o, sub_regions); */
        /*     else if (RegionArena::is_arena_region(r)) */
        /*         ((RegionArena*)r)->release_internal(alloc, o, sub_regions); */
        /*     else */
        /*         abort(); */
        /* } */
    }

    /**
     * Scan through the region and mark all objects reachable from the iso
     * object `o`. We don't follow pointers to subregions. Also will trace
     * from anything already in `dfs`.
     **/
    void mark(Alloc& alloc, Object* o, ObjectStack& dfs)
    {
      o->trace(dfs);
      while (!dfs.empty())
      {
        Object* p = dfs.pop();
        switch (p->get_class())
        {
          case Object::ISO:
          case Object::MARKED:
            break;

          case Object::UNMARKED:
            Systematic::cout() << "Mark" << p << Systematic::endl;
            p->mark();
            p->trace(dfs);
            break;

          case Object::SCC_PTR:
            p = p->immutable();
            RememberedSet::mark(alloc, p);
            break;

          case Object::RC:
          case Object::COWN:
            RememberedSet::mark(alloc, p);
            break;

          default:
            assert(0);
        }
      }
    }

    enum class SweepAll
    {
      Yes,
      No
    };

    /**
     * Sweep and deallocate all unmarked objects in the region. If we find an
     * unmarked object that points to a subregion, we add it to `collect` so we
     * can release it later.
     *
     * If sweep_all is Yes, it is assumed the entire region is being released
     * and the Iso object is collected as well.
     **/
    template<SweepAll sweep_all = SweepAll::No>
    void sweep(Alloc& alloc, Object* o, ObjectStack& collect)
    {
      current_memory_used = 0;
      UNUSED(collect);
      UNUSED(o);
      UNUSED(alloc);



      previous_memory_used = size_to_sizeclass(current_memory_used);
    }

    /* template<StackKind stack, SweepAll sweep_all> */
    /* void sweep_stack( */
    /*   Alloc& alloc, Object* o, RingKind primary_ring, ObjectStack& collect) */
    /* { */
    /*   LinkedObjectStack gc; */


    /*   // Use iterator because we don't remove in place so it won't invalidate */

    /*   // Note: we don't use the iterator because we need to remove and */
    /*   // deallocate objects from the rings. */
    /*   while (p != this) */
    /*   { */
    /*     switch (p->get_class()) */
    /*     { */
    /*       case Object::ISO: */
    /*       { */
    /*         // An iso is always the root, and the last thing in the ring. */
    /*         assert(p->get_next_any_mark() == this); */
    /*         assert(p->get_region() == this); */

    /*         // The ISO is considered marked, unless we're releasing the */
    /*         // entire region anyway. */
    /*         if constexpr (sweep_all == SweepAll::Yes) */
    /*         { */
    /*           sweep_object<ring>(alloc, p, o, &gc, collect); */
    /*         } */
    /*         else */
    /*         { */
    /*           use_memory(p->size()); */
    /*         } */

    /*         p = this; */
    /*         break; */
    /*       } */

    /*       case Object::MARKED: */
    /*       { */
    /*         assert(sweep_all == SweepAll::No); */
    /*         use_memory(p->size()); */
    /*         p->unmark(); */
    /*         prev = p; */
    /*         p = p->get_next(); */
    /*         break; */
    /*       } */

    /*       case Object::UNMARKED: */
    /*       { */
    /*         Object* q = p->get_next(); */
    /*         Systematic::cout() << "Sweep " << p << Systematic::endl; */
    /*         sweep_object<ring>(alloc, p, o, &gc, collect); */

    /*         if (ring != primary_ring && prev == this) */
    /*           next_not_root = q; */
    /*         else */
    /*           prev->set_next(q); */

    /*         if (ring != primary_ring && last_not_root == p) */
    /*           last_not_root = prev; */

    /*         p = q; */
    /*         break; */
    /*       } */

    /*       default: */
    /*         assert(0); */
    /*     } */
    /*   } */

    /**
     * Release and deallocate all objects within the region represented by the
     * Iso Object `o`.
     *
     * Note: this does not release subregions. Use Region::release instead.
     **/
    /* void release_internal(Alloc& alloc, Object* o, ObjectStack& collect) */
    /* { */
    /*   assert(o->debug_is_iso()); */

    /*   // It is an error if this region has additional roots. */
    /*   if (!additional_entry_points.empty()) */
    /*   { */
    /*     Systematic::cout() << "Region release failed due to additional roots" */
    /*                        << Systematic::endl; */
    /*     additional_entry_points.forall([](Object* o) { */
    /*       Systematic::cout() << " root" << o << Systematic::endl; */
    /*     }); */
    /*     abort(); */
    /*   } */

    /*   Systematic::cout() << "Region release: rc region: " << o */
    /*                      << Systematic::endl; */

    /*   // Sweep everything, including the entrypoint. */
    /*   sweep<SweepAll::Yes>(alloc, o, collect); */

    /*   dealloc(alloc); */
    /* } */

    void use_memory(size_t size)
    {
      current_memory_used += size;
    }

  /* public: */
  /*   template<IteratorType type = AllObjects> */
  /*   class iterator */
  /*   { */
  /*     friend class RegionRc; */

  /*     static_assert( */
  /*       type == Trivial || type == NonTrivial || type == AllObjects); */

  /*     iterator(RegionRc* r) : reg(r) */
  /*     { */
  /*       trivial = RcStack<Alloc>::iterator(r->get_trivial_stack()); */
  /*       non_trivial = RcStack<Alloc>::iterator(r->get_non_trivial_stack()); */
  /*     } */

  /*     iterator(RegionRc* r, Object* p) : reg(r), ptr(p) */
  /*     { */
  /*       trivial = RcStack<Alloc>::iterator(r->get_trivial_stack()); */
  /*       non_trivial = RcStack<Alloc>::iterator(r->get_non_trivial_stack()); */
  /*     } */

  /*     iterator(RegionTrace* r, RcStack<Alloc>::iterator trivial, RcStack<Alloc>::iterator non_trivial, Object* p) */
  /*         : reg(r), trivial(trivial), non_trivial(non_trivial), ptr(p) {} */

  /*   private: */
  /*     RegionRc* reg; */
  /*     RcStack<Alloc>::iterator trivial; */
  /*     RcStack<Alloc>::iterator non_trivial; */
  /*     Object* ptr; */

  /*   public: */
  /*     iterator operator++() */
  /*     { */
  /*       if constexpr (type == AllObjects) */
  /*       { */
  /*           if (non_trivial != non_trivial.end()) { */
  /*               ObjectCount* p = *++non_trivial; */
  /*               ptr = p->object; */
  /*               return *this; */
  /*           } */

  /*           if (trivial != trivial.end()) { */
  /*               ObjectCount* p = *++trivial; */
  /*               ptr = p->object; */
  /*               return *this; */
  /*           } */

  /*           ptr = nullptr; */
  /*           return *this; */
  /*       } */
  /*       else if constexpr (type == NonTrivial) */
  /*       { */
  /*           ObjectCount* p = *++non_trivial; */
  /*           if (non_trivial == non_trivial.end()) { */
  /*               ptr = nullptr; */
  /*               return *this; */
  /*           } */

  /*           ptr = p->object; */
  /*           return *this; */
  /*       } */
  /*       else if constexpr (type == Trivial) */
  /*       { */
  /*           ObjectCount* p = *++trivial; */
  /*           if (trivial == trivial.end()) { */
  /*               ptr = nullptr; */
  /*               return *this; */
  /*           } */

  /*           ptr = p->object; */
  /*           return *this; */
  /*       } */
  /*     } */

  /*     inline bool operator!=(const iterator& other) const */
  /*     { */
  /*       assert(reg == other.reg); */
  /*       return ptr != other.ptr; */
  /*     } */

  /*     inline Object* operator*() const */
  /*     { */
  /*       return ptr; */
  /*     } */
  /*   }; */

  /*   template<IteratorType type = AllObjects> */
  /*   inline iterator<type> begin() */
  /*   { */
  /*     return {this}; */
  /*   } */

  /*   template<IteratorType type = AllObjects> */
  /*   inline iterator<type> end() */
  /*   { */
  /*     return {this, nullptr}; */
  /*   } */

  /* private: */
  /*   bool debug_is_in_region(Object* o) */
  /*   { */
  /*     for (auto p : *this) */
  /*     { */
  /*       if (p == o) */
  /*         return true; */
  /*     } */
  /*     return false; */
  /*   } */
  };

} // namespace verona::rt
