//-------------------------------------------------------------------
IMPLEMENTATION [arm && pf_arm_virt]:

#include "irq_mgr_msi.h"
#include "irq_mgr_multi_chip.h"
#include "gic_v2.h"
#include "gic_v3.h"
#include "panic.h"


PUBLIC static
void Pic::init_ap(Cpu_number cpu, bool resume)
{
  gic->init_ap(cpu, resume);
}

PUBLIC static FIASCO_INIT
void Pic::init()
{

  Mmio_register_block dist(Kmem::mmio_remap(Mem_layout::Gic_dist_phys_base,
                                            Gic_dist::Size));
  if ((dist.read<Unsigned32>(0xfe8) & 0x0f0) == 0x20)
    {
      // assume GICv2
      printf("GICv2\n");
      gic = new Boot_object<Gic_v2>(Kmem::mmio_remap(Mem_layout::Gic_cpu_phys_base,
                                                     Gic_cpu_v2::Size),
                                    dist.get_mmio_base());

      typedef Irq_mgr_multi_chip<9> Mgr;
      Mgr *m = new Boot_object<Mgr>(1);
      m->add_chip(0, gic, gic->nr_irqs());
      Irq_mgr::mgr = m;
    }
  else if ((dist.read<Unsigned32>(0xffe8) & 0x0f0) == 0x30)
    {
      // assume GICv3
      printf("GICv3\n");
      auto redist = Kmem::mmio_remap(Mem_layout::Gic_redist_phys_base,
                                     Mem_layout::Gic_redist_size);
      auto gic_v3 = new Boot_object<Gic_v3>(dist.get_mmio_base(), redist);

      gic_v3->add_its(Kmem::mmio_remap(Mem_layout::Gic_its_phys_base,
                                       Mem_layout::Gic_its_size));

      gic = gic_v3;

      typedef Irq_mgr_msi<Gic_v3, Gic_msi> Mgr;
      Irq_mgr::mgr = new Boot_object<Mgr>(gic_v3, gic_v3->msi_chip());
    }
  else
    panic("GIC not found or not supported");
}

