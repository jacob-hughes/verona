// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#pragma once
#ifdef __FreeBSD__
#  define SNMALLOC_USE_THREAD_CLEANUP 1
#endif
#define SNMALLOC_PLATFORM_HAS_GETENTROPY 1
#include <backend/backend.h>
#include <mem/commonconfig.h>
#include <mem/corealloc.h>
#include <mem/localalloc.h>
#include <mem/pool.h>
#include <mem/slaballocator.h>
#include <process_sandbox/helpers.h>
#include <process_sandbox/sandbox_fd_numbers.h>

namespace snmalloc
{
  class Superslab;
  class Mediumslab;
  class Largeslab;
}

namespace sandbox
{
  struct SnmallocBackend
  {
    /**
     * Expose a PAL that doesn't do allocation.
     */
    using Pal = snmalloc::PALNoAlloc<snmalloc::DefaultPal>;

    /**
     * Proxy that requests memory from the parent process.
     */
    class ProxyAddressSpaceManager
    {
      /**
       * Non-templated version of reserve_with_left_over.  Calls the parent
       * process to request a new chunk of memory.
       */
      snmalloc::CapPtr<void, snmalloc::CBChunk> reserve(size_t size);

      /**
       * Lock that protects the RPC channel.
       */
      std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    public:
      /**
       * Public interface.
       */
      template<bool>
      snmalloc::CapPtr<void, snmalloc::CBChunk>
      reserve_with_left_over(size_t size)
      {
        return reserve(size);
      }
    };

    /**
     * Thread-local state.  Currently not used.
     */
    struct LocalState
    {};

    /**
     * Global state, shared among all allocators.
     */
    class GlobalState
    {
      /**
       * The back-end accesses some fields of this directly.
       */
      friend struct SnmallocBackend;

      /**
       * Private address-space manager.  Used to manage allocations that are
       * not shared with the parent.
       */
      snmalloc::AddressSpaceManager<snmalloc::DefaultPal> private_asm;

      /**
       * Proxy to access the address-space manager in the parent.
       */
      ProxyAddressSpaceManager heap_asm;

    public:
      /**
       * The pagemap that spans the entire address space.
       */
      snmalloc::FlatPagemap<
        snmalloc::MIN_CHUNK_BITS,
        snmalloc::MetaEntry,
        snmalloc::DefaultPal,
        /*fixed range*/ false>
        pagemap;
    };

    /**
     * Return the metadata associated with an address.  This reads the
     * read-only mapping of the pagemap directly.
     */
    template<bool potentially_out_of_range = false>
    static const snmalloc::MetaEntry&
    get_meta_data(GlobalState& h, snmalloc::address_t p)
    {
      return h.pagemap.template get<potentially_out_of_range>(p);
    }

    /**
     * Set the metadata associated with an address range.  Sends an RPC to the
     * parent, which validates the entry.
     */
    static void set_meta_data(
      GlobalState& h,
      snmalloc::address_t p,
      size_t size,
      snmalloc::MetaEntry t);

    /**
     * Allocate a chunk of memory and install its metadata in the pagemap.
     * This performs a single RPC that validates the metadata and then
     * allocates and installs the entry.
     */
    static std::
      pair<snmalloc::CapPtr<void, snmalloc::CBChunk>, snmalloc::Metaslab*>
      alloc_chunk(
        GlobalState& h,
        LocalState* local_state,
        size_t size,
        snmalloc::RemoteAllocator* remote,
        snmalloc::sizeclass_t sizeclass);

    /**
     * Allocate metadata.  This allocates non-shared memory for metaslabs and
     * shared memory for allocators.
     */
    template<typename T>
    static snmalloc::CapPtr<void, snmalloc::CBChunk>
    alloc_meta_data(GlobalState& h, LocalState*, size_t size);
  };

  /**
   * The snmalloc configuration used for the child process.
   */
  class SnmallocGlobals : public snmalloc::CommonConfig
  {
    /**
     * The allocator pool type used to allocate per-thread allocators.
     */
    using AllocPool =
      snmalloc::PoolState<snmalloc::CoreAllocator<SnmallocGlobals>>;

    /**
     * The state associated with the chunk allocator.
     */
    inline static snmalloc::ChunkAllocatorState chunk_alloc_state;

    /**
     * The concrete instance of the pool allocator.
     */
    inline static AllocPool alloc_pool;

  public:
    /**
     * The type of the back end.  This is used directly from snmalloc.
     */
    using Backend = SnmallocBackend;

    /**
     * Returns the singleton back-end global state object.
     */
    static Backend::GlobalState& get_backend_state()
    {
      static SnmallocBackend::GlobalState backend_state;
      return backend_state;
    }

    /**
     * Returns the allocation pool.
     */
    static AllocPool& pool()
    {
      return alloc_pool;
    }

    /**
     * Ensure that all of the early bootstrapping is done.
     */
    static void ensure_init() noexcept;

    /**
     * Returns true if the system has bootstrapped, false otherwise.
     */
    static bool is_initialised();

    /**
     * Message queues are currently always allocated inline for
     * in-sandbox allocators.  When we move to dynamically creating
     * shared memory objects one per chunk then they will move to a
     * separate place.
     */
    constexpr static snmalloc::Flags Options{};

    /**
     * Register per-thread cleanup.
     */
    static void register_clean_up()
    {
#ifndef SNMALLOC_USE_THREAD_CLEANUP
      snmalloc::register_clean_up();
#endif
    }

    /**
     * Returns the singleton instance of the chunk allocator state.
     */
    static snmalloc::ChunkAllocatorState& get_slab_allocator_state(void*)
    {
      return chunk_alloc_state;
    }
  };
}

#define SNMALLOC_PROVIDE_OWN_CONFIG
namespace snmalloc
{
  /**
   * The standard allocator type that we provide.
   */
  using Alloc = LocalAllocator<sandbox::SnmallocGlobals>;
}

#include <override/malloc.cc>
