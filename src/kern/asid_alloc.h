#pragma once

#include <atomic.h>
#include <types.h>
#include <bitmap.h>
#include <spin_lock.h>
#include <config.h>
#include <cpu.h>
#include <lock_guard.h>
#include <per_cpu_data.h>

/** Asid storage format:
 * 63                        X      0
 * +------------------------+--------+
 * |   generation count     | ASID   |
 * +------------------------+--------+
 * X = Asid_bits - 1
 *
 * As the generation count increases it might happen that it wraps
 * around and starts at 0 again. If we have address spaces which are
 * not active for a "long time" we might see <generation, asid>
 * tuples of the same value for different spaces after a wrap
 * around. To decrease the likelyhood that this actually happens we
 * use a large generation count. Under worst case assumptions
 * (working set constantly generating new ASIDs every 100 cycles on
 * a 1GHz processor) a wrap around happens after 429 seconds with 32
 * bits and after 58494 years with 64 bit. We use 64bit to be on the
 * save side.
 */
template<typename ASID_TYPE, unsigned ASID_BITS>
class Asid_t
{
public:
  using Value_type = ASID_TYPE;

  enum
  {
    Generation_inc = ((ASID_TYPE)1) << ASID_BITS,
    Mask           = Generation_inc - 1,
    Invalid        = (ASID_TYPE)(~0ULL),
  };

  Value_type a;

  Asid_t() = default;
  Asid_t(Value_type a) : a(a) {}

  bool is_valid() const
  {
    if (sizeof(a) == sizeof(Mword))
      return a != Invalid;
    else
      return ((Unsigned32)(a >> 32) & (Unsigned32)a) != Unsigned32(~0);
  }

  bool is_invalid_generation() const
  {
    return a == (Invalid & ~Mask);
  }

  Value_type asid() const { return a & Mask; }

  bool is_same_generation(Asid_t generation) const
  { return (a & ~Mask) == generation.a; }

  bool operator == (Asid_t o) const
  { return a == o.a; }

  bool operator != (Asid_t o) const
  { return a != o.a; }
};

/**
 * Keep track of reserved Asids
 *
 * If a generation roll over happens we have to keep track of ASIDs
 * active on other CPUs. These ASIDs are marked as reserved in the
 * bitmap.
 */
template<unsigned ASID_BITS, unsigned long ASID_BASE>
class Asid_bitmap_t : public Bitmap<(1UL << ASID_BITS)>
{
public:
  enum
  {
    Asid_base = ASID_BASE,
    Asid_num = 1UL << ASID_BITS
  };

  /**
   * Reset all bits and set first available ASID to Asid_base
   */
  void reset()
  {
    this->clear_all();
    _current_idx = Asid_base;
  }

  /**
   * Find next free ASID
   *
   * \return First free ASID or Asid_num if no ASID is available
   */
  unsigned find_next()
  {
    // assume a sparsely populated bitmap - the next free bit is
    // normally found during first iteration
    for (unsigned i = _current_idx; i < Asid_num; ++i)
      if ((*this)[i] == 0)
        {
          _current_idx = i + 1;
          return i;
        }

    return Asid_num;
  }

  Asid_bitmap_t()
  {
    reset();
  }

private:
  unsigned _current_idx;
};

template<typename ASID_TYPE, unsigned ASID_BITS>
class Asids_per_cpu_t
{
public:
  using Asid = Asid_t<ASID_TYPE, ASID_BITS>;
  /**
   * Currently active ASID on a CPU.
   *
   * written using atomic_xchg outside of spinlock and
   * atomic_write under protection of spinlock
   */
  Asid active = Asid::Invalid;

  /**
   * reserved ASID on a CPU, active during last generation change.
   *
   * written under protection of spinlock
   */
  Asid reserved = Asid::Invalid;

  bool check_and_update_reserved(Asid asid, Asid update)
  {
    if (reserved == asid)
      {
        reserved = update;
        return true;
      }
    else
      return false;
  }
};


template<typename ASID_TYPE, unsigned ASID_BITS, unsigned ASID_BASE>
class Asid_alloc_t
{
public:
  using Asid = Asid_t<ASID_TYPE, ASID_BITS>;
  using Asids_per_cpu = Asids_per_cpu_t<ASID_TYPE, ASID_BITS>;
  using Asids = Per_cpu_ptr<Asids_per_cpu>;
  using Asid_bitmap = Asid_bitmap_t<ASID_BITS, ASID_BASE>;

private:
  bool check_and_update_reserved(Asid asid, Asid update)
  {
    bool res = false;

    for (Cpu_number cpu = Cpu_number::first(); cpu < Config::max_num_cpus();
         ++cpu)
      {
        if (!Cpu::online(cpu))
          continue;

        res |= _asids.cpu(cpu).check_and_update_reserved(asid, update);
      }
    return res;
  }

  /**
   * Reset allocation data structures, reserve currently active ASIDs,
   * mark TLB flush pending for all CPUs
   *
   * \pre
   *   * _lock held
   *
   * \post
   *   * _asids.cpu(x).reserved == ASID currently used on cpu x
   *   * _asids.cpu(x).active   == Mem_unit::Asid_invalid
   *   * _asid_bitmap[x]   != 0 for x in { _asdis.cpu(cpu).reserved }
   *   * _lock held
   *
   */
  void roll_over()
  {
    _reserved.reset();

    // update reserved asids
    for (Cpu_number cpu = Cpu_number::first(); cpu < Config::max_num_cpus();
         ++cpu)
      {
        if (!Cpu::online(cpu))
          continue;

        auto &a = _asids.cpu(cpu);
        Asid asid = atomic_exchange(&a.active, Asid::Invalid);

        // keep reserved asid, if there already was a roll over
        if (asid.is_valid())
          a.reserved = asid;
        else
          asid = a.reserved;

        if (asid.is_valid())
          _reserved.set_bit(asid.asid());
      }

    _tlb_flush_pending = Cpu::online_mask();
  }

  /**
   * Get a new ASID
   *
   * Check whether the ASID is a reserved one (was in use on any cpu
   * during roll over). If it is, update generation and return.
   * Otherwise allocate a new one and handle generation roll over if
   * necessary.
   *
   * \pre
   *   * _lock held
   *
   * \post
   *   * if a generation roll over happens, generation increased by Generation_inc
   *   * returned ASID is marked in _asid_bitmap and has current generation
   *   * _lock held
   *
   */
  Asid FIASCO_FLATTEN new_asid(Asid asid, Asid generation)
  {
    if (asid.is_valid() && _reserved[asid.asid()])
      {
        Asid update = asid.asid() | generation.a;
        if (EXPECT_TRUE(check_and_update_reserved(asid, update)))
          {
            // This ASID was active during a roll over and therefore is
            // still valid. Return the asid with its updated generation.
            return update;
          }
      }

    // Get a new ASID
    unsigned new_asid = _reserved.find_next();
    if (EXPECT_FALSE(new_asid == Asid_bitmap::Asid_num))
      {
        generation = atomic_add_fetch(&_gen, Asid::Generation_inc);

        if (EXPECT_FALSE(generation.is_invalid_generation()))
          {
            // Skip problematic generation value
            generation = atomic_add_fetch(&_gen, Asid::Generation_inc);
          }

        roll_over();
        new_asid = _reserved.find_next();
      }

    return new_asid | generation.a;
  }

public:
  Asid_alloc_t(Asids const &asids) : _asids(asids) {}

  bool can_use_asid(Asid *asid, Asid *active_asid)
  {
    Asid a = atomic_load(asid);
    // is_same_generation implicitely checks for asid != Asid_invalid
    return EXPECT_TRUE(a.is_same_generation(atomic_load(&_gen)))
           && EXPECT_TRUE(atomic_exchange(active_asid, a).is_valid());
  }

  bool alloc_asid(Asid *asid, Asid *active_asid)
  {
    auto guard = lock_guard(_lock);

    // Re-read data
    Asid a = atomic_load(asid);
    Asid generation = atomic_load(&_gen);

    // We either have an older generation or a roll over happened on
    // another cpu - find out which one it was
    if (!a.is_same_generation(generation))
      {
        // We have an asid from an older generation - get a fresh one
        a = new_asid(a, generation);
        atomic_store(asid, a);
      }

    // Set active asid, needs to be atomic since this value is written
    // above using atomic_xchg()
    atomic_store(active_asid, a);

    // Is a tlb flush pending?
    return _tlb_flush_pending.atomic_get_and_clear(current_cpu());
  }

  Asid *get_active_asid() { return &_asids.current().active; }

private:
  /// current ASID generation, protected by _lock
  Asid _gen = Asid::Generation_inc;
  /// Protect elements changed during generation roll over
  Spin_lock<> _lock;
  /// active/reserved ASID (per CPU)
  Asids _asids;
  /// keep track of pending TLB flush operations, protected by _lock
  Cpu_mask _tlb_flush_pending;
  /// keep track of reserved ASIDs, protected by _lock
  Asid_bitmap _reserved;
};

