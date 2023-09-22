// our own implementation of C++ memory management: disallow dynamic
// allocation (except where class-specific new/delete functions exist)
//
// more specialized memory allocation/deallocation functions follow
// below in the "Kmem" namespace

INTERFACE [ia32,amd64,ux]:

#include "globalconfig.h"
#include "initcalls.h"
#include "kip.h"
#include "mem_layout.h"
#include "paging.h"
#include "allocator.h"

class Cpu;
class Tss;

/**
 * The system's base facilities for kernel-memory management.
 * The kernel memory is a singleton object.  We access it through a
 * static class interface.
 */
EXTENSION class Kmem
{
  friend class Jdb;
  friend class Jdb_dbinfo;
  friend class Jdb_kern_info_misc;
  friend class Kdb;
  friend class Profile;
  friend class Vmem_alloc;

private:
  Kmem();			// default constructors are undefined
  Kmem (const Kmem&);

public:
  static Address kcode_start();
  static Address kcode_end();
  static Address virt_to_phys(const void *addr);

  /**
   * Allocator type for CPU structures (GDT, TSS, etc.).
   *
   * All allocator instances of this type need to be used strictly
   * non-concurrently to avoid the need of locking.
   *
   * This is guaranteed either by using the instances locally on a single CPU
   * or separately on each CPU during the initialization of the CPUs (which is
   * done sequentially).
   */
  using Lockless_alloc = Simple_alloc<Lockless_policy>;
};

typedef Kmem Kmem_space;


//----------------------------------------------------------------------------
INTERFACE [ia32, amd64]:

EXTENSION
class Kmem
{
  friend class Kernel_task;

public:
  static Address     user_max();

private:
  static Unsigned8   *io_bitmap_delimiter;
  static Address kphys_start, kphys_end;
};


//--------------------------------------------------------------------------
IMPLEMENTATION [ia32, amd64]:

#include "cpu.h"
#include "l4_types.h"
#include "kmem_alloc.h"
#include "mem_unit.h"
#include "panic.h"
#include "paging.h"
#include "pic.h"
#include "std_macros.h"
#include "paging_bits.h"

#include <cstdio>

enum { Print_info = 0 };

Unsigned8    *Kmem::io_bitmap_delimiter;
Address Kmem::kphys_start, Kmem::kphys_end;


PUBLIC static inline
Address
Kmem::io_bitmap_delimiter_page()
{
  return reinterpret_cast<Address>(io_bitmap_delimiter);
}


/**
 * Compute physical address from a kernel-virtual address.
 * @param addr a virtual address
 * @return corresponding physical address if a mappings exists.
 *         -1 otherwise.
 */
IMPLEMENT inline NEEDS["paging.h", "std_macros.h", "mem_layout.h"]
Address
Kmem::virt_to_phys(const void *addr)
{
  Address a = reinterpret_cast<Address>(addr);

  if (EXPECT_TRUE(Mem_layout::in_pmem(a)))
    return Mem_layout::pmem_to_phys(a);

  if (EXPECT_TRUE(Mem_layout::in_kernel_image(a)))
    return a - Mem_layout::Kernel_image_offset;

  return kdir->virt_to_phys(a);
}


// Only used for initialization and kernel debugger
PUBLIC static
Address
Kmem::map_phys_page_tmp(Address phys, Mword idx)
{
  unsigned long pte = cxx::mask_lsb(phys, Pdir::page_order_for_level(Pdir::Depth));
  Address virt;

  switch (idx)
    {
    case 0:  virt = Mem_layout::Kmem_tmp_page_1; break;
    case 1:  virt = Mem_layout::Kmem_tmp_page_2; break;
    default: return ~0UL;
    }

  static unsigned long tmp_phys_pte[2] = { ~0UL, ~0UL };

  if (pte != tmp_phys_pte[idx])
    {
      // map two consecutive pages as to be able to access
      map_phys_page(phys,          virt,          false, true);
      map_phys_page(phys + 0x1000, virt + 0x1000, false, true);
      tmp_phys_pte[idx] = pte;
    }

  return virt + phys - pte;
}

PUBLIC static inline
Address Kmem::kernel_image_start()
{
  return Pg::trunc(virt_to_phys(&Mem_layout::image_start));
}

IMPLEMENT inline Address Kmem::kcode_start()
{
  return Pg::trunc(virt_to_phys(&Mem_layout::start));
}

IMPLEMENT inline Address Kmem::kcode_end()
{
  return Pg::round(virt_to_phys(&Mem_layout::end));
}

IMPLEMENT_OVERRIDE inline NEEDS["mem_layout.h"]
bool
Kmem::is_io_bitmap_page_fault(Address addr)
{
  return addr >= Mem_layout::Io_bitmap &&
	 addr <= Mem_layout::Io_bitmap + Mem_layout::Io_port_max / 8;
}

IMPLEMENT inline NEEDS["mem_layout.h"]
bool
Kmem::is_kmem_page_fault(Address addr, Mword /*error*/)
{
  return addr > Mem_layout::User_max;
}


//
// helper functions
//

// Establish a 4k-mapping
// TODO: Implement this via ioremap
PUBLIC static
void
Kmem::map_phys_page(Address phys, Address virt,
                    bool cached, bool global, Address *offs = 0)
{
  auto i = kdir->walk(Virt_addr(virt), Pdir::Depth, false,
                      pdir_alloc(Kmem_alloc::allocator()));
  Mword pte = Pg::trunc(phys);

  assert(i.level == Pdir::Depth);

  i.set_page(pte, Pt_entry::Writable | Pt_entry::Referenced | Pt_entry::Dirty
                  | (cached ? 0 : (Pt_entry::Write_through | Pt_entry::Noncacheable))
                  | (global ? Pt_entry::global() : 0));
  Mem_unit::tlb_flush_kernel(virt);

  if (offs)
    *offs = phys - pte;
}

//--------------------------------------------------------------------------
IMPLEMENTATION [ia32 || (amd64 && !kernel_nx)]:

PRIVATE static FIASCO_INIT
void
Kmem::map_initial_ram()
{
  Kmem_alloc *const alloc = Kmem_alloc::allocator();

  // set up the kernel mapping for physical memory.  mark all pages as
  // referenced and modified (so when touching the respective pages
  // later, we save the CPU overhead of marking the pd/pt entries like
  // this)

  // we also set up a one-to-one virt-to-phys mapping for two reasons:
  // (1) so that we switch to the new page table early and re-use the
  //     segment descriptors set up by boot_cpu.cc.  (we'll set up our
  //     own descriptors later.) we only need the first 4MB for that.
  // (2) a one-to-one phys-to-virt mapping in the kernel's page directory
  //     sometimes comes in handy (mostly useful for debugging)

  // first 4MB page
  if (!kdir->map(0, Virt_addr(0UL), Virt_size(4 << 20),
                 Pt_entry::Dirty | Pt_entry::Writable | Pt_entry::Referenced,
                 Pt_entry::super_level(), false, pdir_alloc(alloc)))
    panic("Cannot map initial memory");
}

PRIVATE static FIASCO_INIT_CPU
void
Kmem::map_kernel_virt(Kpdir *dir)
{
  if (!dir->map(Mem_layout::Kernel_image_phys, Virt_addr(Kernel_image),
                Virt_size(Mem_layout::Kernel_image_size),
                Pt_entry::Dirty | Pt_entry::Writable | Pt_entry::Referenced
                | Pt_entry::global(),
                Pt_entry::super_level(), false, pdir_alloc(Kmem_alloc::allocator())))
    panic("Cannot map initial memory");
}

//--------------------------------------------------------------------------
IMPLEMENTATION [amd64 && kernel_nx]:

PRIVATE static FIASCO_INIT
void
Kmem::map_initial_ram()
{
  Kmem_alloc *const alloc = Kmem_alloc::allocator();

  // set up the kernel mapping for physical memory.  mark all pages as
  // referenced and modified (so when touching the respective pages
  // later, we save the CPU overhead of marking the pd/pt entries like
  // this)

  // we also set up a one-to-one virt-to-phys mapping for two reasons:
  // (1) so that we switch to the new page table early and re-use the
  //     segment descriptors set up by boot_cpu.cc.  (we'll set up our
  //     own descriptors later.) we only need the first 6MB for that.
  // (2) a one-to-one phys-to-virt mapping in the kernel's page directory
  //     sometimes comes in handy (mostly useful for debugging)

  bool ok = true;

  // first 2M

  // Beginning of physical memory up to the realmode trampoline code is RW
  ok &= kdir->map(0, Virt_addr(0), Virt_size(FIASCO_MP_TRAMP_PAGE),
                  Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                  | Pt_entry::Referenced,
                  Pdir::Depth, false, pdir_alloc(alloc));

  // Realmode trampoline code is RWX
  ok &= kdir->map(FIASCO_MP_TRAMP_PAGE, Virt_addr(FIASCO_MP_TRAMP_PAGE),
                  Virt_size(Config::PAGE_SIZE),
                  Pt_entry::Dirty | Pt_entry::Writable | Pt_entry::Referenced,
                  Pdir::Depth, false, pdir_alloc(alloc));

  // The rest of the first 2M is RW
  ok &= kdir->map(FIASCO_MP_TRAMP_PAGE + Config::PAGE_SIZE,
                  Virt_addr(FIASCO_MP_TRAMP_PAGE + Config::PAGE_SIZE),
                  Virt_size(Config::SUPERPAGE_SIZE - FIASCO_MP_TRAMP_PAGE
                            - Config::PAGE_SIZE),
                  Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                  | Pt_entry::Referenced,
                  Pdir::Depth, false, pdir_alloc(alloc));

  // Second 2M is RW
  ok &= kdir->map(Config::SUPERPAGE_SIZE, Virt_addr(Config::SUPERPAGE_SIZE),
                  Virt_size(Config::SUPERPAGE_SIZE),
                  Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                  | Pt_entry::Referenced,
                  Pt_entry::super_level(), false, pdir_alloc(alloc));

  if (!ok)
    panic("Cannot map initial memory");
}

PRIVATE static FIASCO_INIT_CPU
void
Kmem::map_kernel_virt(Kpdir *dir)
{
  extern char _kernel_text_start[];
  extern char _kernel_data_start[];
  extern char _initcall_end[];

  Address virt = Mem_layout::Kernel_image;
  Address text = (Address)&_kernel_text_start;
  Address data = Super_pg::trunc((Address)&_kernel_data_start);
  Address kend = Super_pg::round((Address)&_initcall_end);

  Kmem_alloc *const alloc = Kmem_alloc::allocator();
  bool ok = true;
  // Kernel text is RX
  ok &= dir->map(Mem_layout::Kernel_image_phys + (text - virt), Virt_addr(text),
                 Virt_size(data - text),
                 Pt_entry::Referenced | Pt_entry::global(), Pt_entry::super_level(),
                 false, pdir_alloc(alloc));

  // Kernel data is RW + XD
  ok &= dir->map(Mem_layout::Kernel_image_phys + (data - virt), Virt_addr(data),
                 Virt_size(kend - data),
                 Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                 | Pt_entry::Referenced | Pt_entry::global(),
                 Pt_entry::super_level(), false, pdir_alloc(alloc));

  if (!ok)
    panic("Cannot map initial memory");
}

//--------------------------------------------------------------------------
IMPLEMENTATION [ia32, amd64]:

#include "paging_bits.h"

PUBLIC static FIASCO_INIT
void
Kmem::init_mmu()
{
  Kmem_alloc *const alloc = Kmem_alloc::allocator();

  kdir = (Kpdir*)alloc->alloc(Config::page_order());
  memset (kdir, 0, Config::PAGE_SIZE);

  unsigned long cpu_features = Cpu::get_features();
  bool superpages = cpu_features & FEAT_PSE;

  printf("Superpages: %s\n", superpages ? "yes" : "no");

  Pt_entry::have_superpages(superpages);
  if (superpages)
    Cpu::set_cr4(Cpu::get_cr4() | CR4_PSE);

  if (cpu_features & FEAT_PGE)
    {
      Pt_entry::enable_global();
      Cpu::set_cr4 (Cpu::get_cr4() | CR4_PGE);
    }

  map_initial_ram();
  map_kernel_virt(kdir);

  bool ok = true;

  if (!Mem_layout::Adap_in_kernel_image)
    ok &= kdir->map(Mem_layout::Adap_image_phys, Virt_addr(Mem_layout::Adap_image),
                    Virt_size(Config::SUPERPAGE_SIZE),
                    Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                    | Pt_entry::Referenced | Pt_entry::global(),
                    Pt_entry::super_level(), false, pdir_alloc(alloc));

  // map the last 64MB of physical memory as kernel memory
  ok &= kdir->map(Mem_layout::pmem_to_phys(Mem_layout::Physmem),
                  Virt_addr(Mem_layout::Physmem), Virt_size(Mem_layout::pmem_size),
                  Pt_entry::XD | Pt_entry::Writable | Pt_entry::Referenced
                  | Pt_entry::global(),
                  Pt_entry::super_level(), false, pdir_alloc(alloc));

  if (!ok)
    panic("Cannot map initial memory");

  // The service page directory entry points to an universal usable
  // page table which is currently used for the Local APIC and the
  // jdb adapter page.
  assert(Super_pg::aligned(Mem_layout::Service_page));

  kdir->walk(Virt_addr(Mem_layout::Service_page), Pdir::Depth,
             false, pdir_alloc(alloc));

  // kernel mode should acknowledge write-protected page table entries
  Cpu::set_cr0(Cpu::get_cr0() | CR0_WP);

  // now switch to our new page table
  Cpu::set_pdbr(Mem_layout::pmem_to_phys(kdir));

  setup_global_cpu_structures(superpages);


  // did we really get the first byte ??
  assert(Pg::aligned(reinterpret_cast<Address>(io_bitmap_delimiter)));
  *io_bitmap_delimiter = 0xff;
}

PRIVATE static
void
Kmem::setup_cpu_structures(Cpu &cpu, Lockless_alloc *cpu_alloc,
                           Lockless_alloc *tss_alloc)
{
  // now initialize the global descriptor table
  void *gdt = cpu_alloc->alloc_bytes<void>(Gdt::gdt_max, 0x10);
  cpu.init_gdt((Address)gdt, user_max());

  // Allocate the task segment as the last thing from cpu_page_vm
  // because with IO protection enabled the task segment includes the
  // rest of the page and the following IO bitmap (2 pages).
  //
  // Allocate additional 256 bytes for emergency stack right beneath
  // the tss. It is needed if we get an NMI or debug exception at
  // entry_sys_fast_ipc/entry_sys_fast_ipc_c/entry_sys_fast_ipc_log.
  void *tss = tss_alloc->alloc_bytes<void>(sizeof(Tss) + 256, 0x10);
  Address tss_mem = (Address)tss;
  assert(tss_mem + sizeof(Tss) + 256 < Mem_layout::Io_bitmap);
  tss_mem += 256;
  assert(tss_mem >= Mem_layout::Io_bitmap - 0x100000);

  // this is actually tss_size + 1, including the io_bitmap_delimiter byte
  size_t tss_size;
  tss_size = Mem_layout::Io_bitmap + (Mem_layout::Io_port_max / 8) - tss_mem;

  assert(tss_size < 0x100000); // must fit into 20 Bits

  cpu.init_tss(tss_mem, tss_size);

  // force GDT... to memory before loading the registers
  asm volatile ( "" : : : "memory" );

  // set up the x86 CPU's memory model
  cpu.set_gdt();
  cpu.set_ldt(0);

  cpu.set_ds(Gdt::data_segment());
  cpu.set_es(Gdt::data_segment());
  cpu.set_ss(Gdt::gdt_data_kernel | Gdt::Selector_kernel);
  cpu.set_fs(Gdt::gdt_data_user   | Gdt::Selector_user);
  cpu.set_gs(Gdt::gdt_data_user   | Gdt::Selector_user);
  cpu.set_cs();

  // and finally initialize the TSS
  cpu.set_tss();

  init_cpu_arch(cpu, cpu_alloc);
}


//---------------------------------------------------------------------------
IMPLEMENTATION [ia32 || amd64]:

IMPLEMENT inline Address Kmem::user_max() { return ~0UL; }


//--------------------------------------------------------------------------
IMPLEMENTATION [ia32,ux,amd64]:

#include <cstdlib>
#include <cstddef>		// size_t
#include <cstring>		// memset

#include "config.h"
#include "cpu.h"
#include "gdt.h"
#include "globals.h"
#include "paging.h"
#include "regdefs.h"
#include "std_macros.h"
#include "tss.h"

// static class variables
Kpdir *Kmem::kdir;

/**
 * Compute a kernel-virtual address for a physical address.
 * This function always returns virtual addresses within the
 * physical-memory region.
 * @pre addr <= highest kernel-accessible RAM address
 * @param addr a physical address
 * @return kernel-virtual address.
 */
PUBLIC static inline
void *
Kmem::phys_to_virt(Address addr)
{
  return reinterpret_cast<void *>(Mem_layout::phys_to_pmem(addr));
}

/**
 * Return Global page directory.
 * This is the master copy of the kernel's page directory. Kernel-memory
 * allocations are kept here and copied to task page directories lazily
 * upon page fault.
 * @return kernel's global page directory
 */
PUBLIC static inline const Pdir* Kmem::dir() { return kdir; }


//--------------------------------------------------------------------------
INTERFACE [(ia32 || ux || amd64) && !cpu_local_map]:

#include "types.h"

EXTENSION class Kmem
{
  static unsigned long tss_mem_pm;
  static Static_object<Lockless_alloc> tss_mem_vm;
};

//--------------------------------------------------------------------------
IMPLEMENTATION [(ia32 || ux || amd64) && !cpu_local_map]:

unsigned long Kmem::tss_mem_pm;
Static_object<Kmem::Lockless_alloc> Kmem::tss_mem_vm;

//--------------------------------------------------------------------------
IMPLEMENTATION [realmode && amd64]:

/**
 * Get real mode startup page directory physical address.
 *
 * This page directory is used for the startup code of application CPUs until
 * the proper mapping is established. To avoid issues, a copy of the global
 * kernel mapping with a physical address below 4 GiB is provided.
 *
 * \note In case of CPU local mapping, this page directory must map all the
 *       memory that is needed until the CPU local mapping of the given
 *       application CPU is established.
 *
 * \return Real mode startup page directory physical address.
 */
PUBLIC
static Address
Kmem::get_realmode_startup_pdbr()
{
  // For amd64, we need to make sure that our boot-up page directory is below
  // 4 GiB in physical memory.
  static char _boot_pdir[Config::PAGE_SIZE] __attribute__((aligned(4096)));

  memcpy(_boot_pdir, kdir, sizeof(_boot_pdir));
  return Kmem::virt_to_phys(_boot_pdir);
}

/**
 * Get real mode startup Global Descriptor Table pseudo descriptor.
 *
 * This GDT pseudo descriptor is used for the startup code of application CPUs
 * until the proper GDT is established. To avoid issues, a copy of the
 * bootstrap CPU's GDT that is accessible via the \ref kdir mapping is
 * provided.
 *
 * \return Real mode startup Global Descriptor Table pseudo descriptor.
 */
PUBLIC
static Pseudo_descriptor
Kmem::get_realmode_startup_gdt_pdesc()
{
  // For amd64, we need to make sure that our boot-up Global Descriptor Table
  // is accessible via the kdir mapping.
  static char _boot_gdt[Gdt::gdt_max] __attribute__((aligned(0x10)));

  memcpy(_boot_gdt, Cpu::boot_cpu()->get_gdt(), sizeof(_boot_gdt));
  return Pseudo_descriptor(reinterpret_cast<Address>(&_boot_gdt),
                           Gdt::gdt_max - 1);
}

//--------------------------------------------------------------------------
IMPLEMENTATION [realmode && ia32]:

PUBLIC
static Address
Kmem::get_realmode_startup_pdbr()
{
  return Mem_layout::pmem_to_phys(Kmem::dir());
}

PUBLIC
static Pseudo_descriptor
Kmem::get_realmode_startup_gdt_pdesc()
{
  Gdt *_boot_gdt = Cpu::boot_cpu()->get_gdt();
  return Pseudo_descriptor(reinterpret_cast<Address>(_boot_gdt),
                           Gdt::gdt_max - 1);
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32 || ux) && !cpu_local_map]:

PUBLIC static inline
Kpdir *
Kmem::current_cpu_kdir()
{
  return kdir;
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && !cpu_local_map]:

#include "warn.h"

PRIVATE static inline
void
Kmem::setup_global_cpu_structures(bool superpages)
{
  auto *alloc = Kmem_alloc::allocator();
  assert(Super_pg::aligned(Mem_layout::Io_bitmap));

  enum { Tss_mem_size = 0x10 + Config::Max_num_cpus * cxx::ceil_lsb(sizeof(Tss) + 256, 4) };

  /* Per-CPU TSS required to use IO-bitmap for more CPUs */
  static_assert(Tss_mem_size < 0x10000, "Too many CPUs configured.");

  unsigned tss_mem_size = Tss_mem_size;

  if (tss_mem_size < Config::PAGE_SIZE)
    tss_mem_size = Config::PAGE_SIZE;

  tss_mem_pm = Mem_layout::pmem_to_phys(alloc->alloc(Bytes(tss_mem_size)));

  printf("Kmem:: TSS mem at %lx (%uBytes)\n", tss_mem_pm, tss_mem_size);

  if (superpages
      && Config::SUPERPAGE_SIZE - (Super_pg::offset(tss_mem_pm)) < 0x10000)
    {
      // can map as 4MB page because the cpu_page will land within a
      // 16-bit range from io_bitmap
      auto e = kdir->walk(Virt_addr(Mem_layout::Io_bitmap - Config::SUPERPAGE_SIZE),
                          Pdir::Super_level, false, pdir_alloc(alloc));

      e.set_page(Super_pg::trunc(tss_mem_pm),
                 Pt_entry::XD | Pt_entry::Writable | Pt_entry::Referenced
                 | Pt_entry::Dirty | Pt_entry::global());

      tss_mem_vm.construct(Super_pg::offset(tss_mem_pm)
                           + (Mem_layout::Io_bitmap - Config::SUPERPAGE_SIZE),
                           tss_mem_size);
    }
  else
    {
      unsigned i;
      for (i = 0; Pg::size(i) < tss_mem_size; ++i)
        {
          auto e = kdir->walk(Virt_addr(Mem_layout::Io_bitmap
                                        - Pg::size(i + 1)),
                              Pdir::Depth, false, pdir_alloc(alloc));

          e.set_page(tss_mem_pm + Pg::size(i),
                     Pt_entry::XD | Pt_entry::Writable | Pt_entry::Referenced
                     | Pt_entry::Dirty | Pt_entry::global());
        }

      tss_mem_vm.construct(Mem_layout::Io_bitmap - Pg::size(i), tss_mem_size);
    }

  // the IO bitmap must be followed by one byte containing 0xff
  // if this byte is not present, then one gets page faults
  // (or general protection) when accessing the last port
  // at least on a Pentium 133.
  //
  // Therefore we write 0xff in the first byte of the cpu_page
  // and map this page behind every IO bitmap
  io_bitmap_delimiter = tss_mem_vm->alloc<Unsigned8>();
}

PUBLIC static FIASCO_INIT_CPU
void
Kmem::init_cpu(Cpu &cpu)
{
  Lockless_alloc cpu_mem_vm(Kmem_alloc::allocator()->alloc(Bytes(1024)), 1024);
  if (Warn::is_enabled(Info))
    printf("Allocate cpu_mem @ %p\n", cpu_mem_vm.ptr());

  // now switch to our new page table
  Cpu::set_pdbr(Mem_layout::pmem_to_phys(kdir));

  setup_cpu_structures(cpu, &cpu_mem_vm, tss_mem_vm);
}

PUBLIC static inline
void
Kmem::resume_cpu(Cpu_number)
{
  Cpu::set_pdbr(pmem_to_phys(kdir));
}


//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && cpu_local_map && !kernel_isolation]:

EXTENSION class Kmem
{
  enum { Num_cpu_dirs = 1 };
};

PUBLIC static inline
Kpdir *
Kmem::current_cpu_udir()
{
  return reinterpret_cast<Kpdir *>(Kentry_cpu_pdir);
}

PRIVATE static inline
void
Kmem::setup_cpu_structures_isolation(Cpu &cpu, Kpdir *, Lockless_alloc *cpu_m)
{
  setup_cpu_structures(cpu, cpu_m, cpu_m);
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && kernel_isolation]:

EXTENSION class Kmem
{
  enum { Num_cpu_dirs = 2 };
};

PUBLIC static inline
Kpdir *
Kmem::current_cpu_udir()
{
  return reinterpret_cast<Kpdir *>(Kentry_cpu_pdir + 4096);
}

PRIVATE static
void
Kmem::setup_cpu_structures_isolation(Cpu &cpu, Kpdir *cpu_dir, Lockless_alloc *cpu_m)
{

  auto src = cpu_dir->walk(Virt_addr(Kentry_cpu_page), 0);
  auto dst = cpu_dir[1].walk(Virt_addr(Kentry_cpu_page), 0);
  write_now(dst.pte, *src.pte);

  // map kernel code to user space dir
  extern char _kernel_text_start[];
  extern char _kernel_text_entry_end[];
  Address ki_page = Pg::trunc((Address)_kernel_text_start);
  Address kie_page = Pg::round((Address)_kernel_text_entry_end);

  if (Print_info)
    printf("kernel code: %p(%lx)-%p(%lx)\n", _kernel_text_start,
           ki_page, _kernel_text_entry_end, kie_page);

  // FIXME: Make sure we can and do share level 1 to 3 among all CPUs
  if (!cpu_dir[1].map(ki_page - Kernel_image_offset, Virt_addr(ki_page),
                      Virt_size(kie_page - ki_page),
                      Pt_entry::Referenced | Pt_entry::global(),
                      Pdir::Depth,
                      false, pdir_alloc(Kmem_alloc::allocator())))
    panic("Cannot map initial memory");

  prepare_kernel_entry_points(cpu_m, cpu_dir);

  unsigned const estack_sz = 512;
  Unsigned8 *estack = cpu_m->alloc_bytes<Unsigned8>(estack_sz, 16);

  setup_cpu_structures(cpu, cpu_m, cpu_m);
  cpu.get_tss()->_rsp0 = (Address)(estack + estack_sz);
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && kernel_isolation && kernel_nx]:

PRIVATE static
void
Kmem::prepare_kernel_entry_points(Lockless_alloc *, Kpdir *cpu_dir)
{
  extern char _kernel_data_entry_start[];
  extern char _kernel_data_entry_end[];
  Address kd_page = Pg::trunc((Address)_kernel_data_entry_start);
  Address kde_page = Pg::round((Address)_kernel_data_entry_end);

  if (Print_info)
    printf("kernel entry data: %p(%lx)-%p(%lx)\n", _kernel_data_entry_start,
           kd_page, _kernel_data_entry_end, kde_page);

  bool ok = true;

  ok &= cpu_dir[1].map(kd_page - Kernel_image_offset, Virt_addr(kd_page),
                       Virt_size(kde_page - kd_page),
                       Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Referenced
                       | Pt_entry::global(),
                       Pdir::Depth, false, pdir_alloc(Kmem_alloc::allocator()));

  extern char const syscall_entry_code[];
  ok &= cpu_dir[1].map(virt_to_phys(syscall_entry_code),
                       Virt_addr(Kentry_cpu_page_text), Virt_size(Config::PAGE_SIZE),
                       Pt_entry::Referenced | Pt_entry::global(), Pdir::Depth, false,
                       pdir_alloc(Kmem_alloc::allocator()));

  if (!ok)
    panic("Cannot map initial memory");
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && kernel_isolation && !kernel_nx]:

PRIVATE static
void
Kmem::prepare_kernel_entry_points(Lockless_alloc *cpu_m, Kpdir *)
{
  extern char const syscall_entry_code[];
  extern char const syscall_entry_code_end[];

  void *sccode = cpu_m->alloc_bytes<void>(syscall_entry_code_end
                                          - syscall_entry_code, 16);
  assert((Address)sccode == Kentry_cpu_syscall_entry);

  memcpy(sccode, syscall_entry_code, syscall_entry_code_end
                                     - syscall_entry_code);
}

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && kernel_nx]:

PRIVATE static inline NEEDS["paging.h"]
Pte_ptr::Entry
Kmem::conf_xd()
{ return Pt_entry::XD; }

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && !kernel_nx]:

PRIVATE static inline NEEDS["paging.h"]
Pte_ptr::Entry
Kmem::conf_xd()
{ return 0; }

//--------------------------------------------------------------------------
IMPLEMENTATION [(amd64 || ia32) && cpu_local_map]:

#include "bitmap.h"

EXTENSION class Kmem
{
  static Bitmap<260> *_pte_map;
};

Bitmap<260> *Kmem::_pte_map;

DEFINE_PER_CPU static Per_cpu<Kpdir *> _per_cpu_dir;

PUBLIC static inline
Kpdir *
Kmem::current_cpu_kdir()
{
  return reinterpret_cast<Kpdir *>(Kentry_cpu_pdir);
}

PRIVATE static inline
void
Kmem::setup_global_cpu_structures(bool superpages)
{
  (void)superpages;
  io_bitmap_delimiter = (Unsigned8 *)Kmem_alloc::allocator()
                                        ->alloc(Config::page_order());
}

PUBLIC static FIASCO_INIT_CPU
void
Kmem::init_cpu(Cpu &cpu)
{
  Kmem_alloc *const alloc = Kmem_alloc::allocator();

  unsigned const cpu_dir_sz = sizeof(Kpdir) * Num_cpu_dirs;

  Kpdir *cpu_dir = (Kpdir*)alloc->alloc(Bytes(cpu_dir_sz));
  memset (cpu_dir, 0, cpu_dir_sz);

  auto src = kdir->walk(Virt_addr(0), 0);
  auto dst = cpu_dir->walk(Virt_addr(0), 0);
  write_now(dst.pte, *src.pte);

  static_assert ((Kglobal_area & ((1UL << 30) - 1)) == 0, "Kglobal area must be 1GB aligned");
  static_assert ((Kglobal_area_end & ((1UL << 30) - 1)) == 0, "Kglobal area must be 1GB aligned");

  for (unsigned i = 0; i < ((Kglobal_area_end - Kglobal_area) >> 30); ++i)
    {
      auto src = kdir->walk(Virt_addr(Kglobal_area + (((Address)i) << 30)), 1);
      auto dst = cpu_dir->walk(Virt_addr(Kglobal_area + (((Address)i) << 30)), 1,
                               false, pdir_alloc(alloc));

      if (dst.level != 1)
        panic("could not setup per-cpu page table: %d\n", __LINE__);

      if (src.level != 1)
        panic("could not setup per-cpu page table, invalid source mapping: %d\n", __LINE__);

      write_now(dst.pte, *src.pte);
    }

  static_assert(Super_pg::aligned(Physmem),
                "Physmem area must be superpage aligned");
  static_assert(Super_pg::aligned(Physmem_end),
                "Physmem_end area must be superpage aligned");

  for (unsigned i = 0; i < Super_pg::count(Physmem_end - Physmem);)
    {
      Address a = Physmem + Super_pg::size(i);
      if ((a & ((1UL << 30) - 1)) || ((Physmem_end - (1UL << 30)) < a))
        {
          // copy a superpage slot
          auto src = kdir->walk(Virt_addr(a), 2);

          if (src.level != 2)
            panic("could not setup per-cpu page table, invalid source mapping: %d\n", __LINE__);

          if (src.is_valid())
            {
              auto dst = cpu_dir->walk(Virt_addr(a), 2,
                                       false, pdir_alloc(alloc));

              if (dst.level != 2)
                panic("could not setup per-cpu page table: %d\n", __LINE__);

              if (dst.is_valid())
                {
                  assert (*dst.pte == *src.pte);
                  ++i;
                  continue;
                }

              if (Print_info)
                printf("physmem sync(2M): va:%16lx pte:%16lx\n", a, *src.pte);

              write_now(dst.pte, *src.pte);
            }
          ++i;
        }
      else
        {
          // copy a 1GB slot
          auto src = kdir->walk(Virt_addr(a), 1);
          if (src.level != 1)
            panic("could not setup per-cpu page table, invalid source mapping: %d\n", __LINE__);

          if (src.is_valid())
            {
              auto dst = cpu_dir->walk(Virt_addr(a), 1,
                                       false, pdir_alloc(alloc));

              if (dst.level != 1)
                panic("could not setup per-cpu page table: %d\n", __LINE__);

              if (dst.is_valid())
                {
                  assert (*dst.pte == *src.pte);
                  i += 512; // skip 512 2MB entries == 1G
                  continue;
                }

              if (Print_info)
                printf("physmem sync(1G): va:%16lx pte:%16lx\n", a, *src.pte);

              write_now(dst.pte, *src.pte);
            }

          i += 512; // skip 512 2MB entries == 1G
        }
    }

  map_kernel_virt(cpu_dir);

  bool ok = true;

  if (!Adap_in_kernel_image)
    ok &= cpu_dir->map(Adap_image_phys, Virt_addr(Adap_image),
                       Virt_size(Config::SUPERPAGE_SIZE),
                       Pt_entry::XD | Pt_entry::Dirty | Pt_entry::Writable
                       | Pt_entry::Referenced | Pt_entry::global(),
                       Pt_entry::super_level(), false, pdir_alloc(alloc));

  Address cpu_dir_pa = Mem_layout::pmem_to_phys(cpu_dir);
  ok &= cpu_dir->map(cpu_dir_pa, Virt_addr(Kentry_cpu_pdir), Virt_size(cpu_dir_sz),
                     Pt_entry::XD | Pt_entry::Writable | Pt_entry::Referenced
                     | Pt_entry::Dirty | Pt_entry::global(),
                     Pdir::Depth, false, pdir_alloc(alloc));

  unsigned const cpu_mx_sz = Config::PAGE_SIZE;
  void *cpu_mx = alloc->alloc(Bytes(cpu_mx_sz));
  auto cpu_mx_pa = Mem_layout::pmem_to_phys(cpu_mx);

  ok &= cpu_dir->map(cpu_mx_pa, Virt_addr(Kentry_cpu_page), Virt_size(cpu_mx_sz),
                     conf_xd() | Pt_entry::Writable
                     | Pt_entry::Referenced | Pt_entry::Dirty | Pt_entry::global(),
                     Pdir::Depth, false, pdir_alloc(alloc));

  if (!ok)
    panic("Cannot map initial CPU memory");

  _per_cpu_dir.cpu(cpu.id()) = cpu_dir;
  Cpu::set_pdbr(cpu_dir_pa);

  Lockless_alloc cpu_m(Kentry_cpu_page, Config::PAGE_SIZE);
  // [0] = CPU dir pa (PCID: + bit63 + ASID 0)
  // [1] = KSP
  // [2] = EXIT flags
  // [3] = CPU dir pa + 0x1000 (PCID: + bit63 + ASID)
  // [4] = entry scratch register
  // [5] = unused
  // [6] = here starts the syscall entry code (NX: unused)
  Mword *p = cpu_m.alloc<Mword>(6);
  // With PCID enabled set bit 63 to prevent flushing of any TLB entries or
  // paging-structure caches during the page table switch. In that case TLB
  // flushes are exclusively done by Mem_unit::tlb_flush() calls.
  Mword const flush_tlb_bit = Config::Pcid_enabled ? 1UL << 63 : 0;
  write_now(&p[0], cpu_dir_pa | flush_tlb_bit);
  write_now(&p[3], cpu_dir_pa | flush_tlb_bit |  0x1000);
  setup_cpu_structures_isolation(cpu, cpu_dir, &cpu_m);

  auto *pte_map = cpu_m.alloc<Bitmap<260> >(1, 0x20);

  pte_map->clear_all();
  // Sync pte_map bits for context switch optimization.
  // Slots > 255 are CPU local / kernel area.
  for (unsigned long i = 0; i < 256; ++i)
    if (cpu_dir->walk(Virt_addr(i << 39), 0).is_valid())
      pte_map->set_bit(i);

  if (!_pte_map)
    _pte_map = pte_map;
  else if (_pte_map != pte_map)
    panic("failed to allocate correct PTE map: %p expected %p\n",
          pte_map, _pte_map);
}

PUBLIC static inline
Bitmap<260> *
Kmem::pte_map()
{ return _pte_map; }

PUBLIC static
void
Kmem::resume_cpu(Cpu_number cpu)
{
  Cpu::set_pdbr(pmem_to_phys(_per_cpu_dir.cpu(cpu)));
}
