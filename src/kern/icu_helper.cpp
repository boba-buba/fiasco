INTERFACE:

#include "irq.h"
#include "irq_mgr.h"
#include "kobject_helper.h"
#include "kobject_rpc.h"

/**
 * Helper IRQ chip for virtual ICUs.
 *
 * This IRQ chip provides a simple software-only IRQ chip that can bind up to
 * NIRQS interrupts and can be used with the Icu_h<> template.
 */
template<unsigned NIRQS = 1>
class Irq_chip_virt : public Irq_chip_soft
{
public:
  struct Ic_res
  {
    Ic_res() = default;
    Ic_res(Irq_chip_virt *c, unsigned p) : chip(c), pin(p) {}

    Irq_chip_virt *chip = nullptr;
    unsigned pin;

    Irq_base *irq() const
    { return chip->_irqs[pin]; }
  };

  unsigned nr_irqs() const { return NIRQS; }

  Irq_base *icu_get_irq(unsigned pin) const
  {
    if (pin < NIRQS)
      return _irqs[pin];
    return nullptr;
  }

  Ic_res icu_get_chip(unsigned pin)
  {
    if (pin < NIRQS)
      return Ic_res(this, pin);

    return Ic_res();
  }

  int icu_bind_irq(Mword pin, Irq_base *irq)
  {
    if (pin >= NIRQS)
      return -L4_err::EInval;

    if (_irqs[pin])
      return -L4_err::EInval;

    bind(irq, pin);
    if (cas(&_irqs[pin], (Irq_base *)nullptr, irq))
      return 0;

    irq->unbind();
    return -L4_err::EInval;
  }

  int icu_get_info(Mword *features, Mword *num_irqs, Mword *num_msis)
  {
    *features = 0; // supported features (only normal irqs)
    *num_irqs = NIRQS;
    *num_msis = 0;
    return 0;
  }

  int icu_msi_info(Mword, Unsigned64, Irq_mgr::Msi_info *)
  { return -L4_err::ENosys; }

  void unbind(Irq_base *irq) override
  {
    if (irq->chip() != this)
      return;

    unsigned pin = irq->pin();
    if (pin >= NIRQS)
      return;

    if (_irqs[pin] != irq)
      return;

    if (cas(&_irqs[pin], irq, (Irq_base *)nullptr))
      Irq_chip_soft::unbind(irq);
  }

protected:
  Irq_base *const *icu_irq_ptr(Mword pin)
  { return &_irqs[pin]; }

private:
  Irq_base *_irqs[NIRQS];
};


class Icu_h_base
{
public:
  enum Op
  {
    Op_bind       = 0,
    Op_unbind     = 1,
    Op_info       = 2,
    Op_msi_info   = 3,
    Op_eoi        = Irq::Op_eoi_2, // 4
    Op_unmask     = Op_eoi,
    Op_mask       = 5,
    Op_set_mode   = 6,
  };

  enum Feature
  {
    Msi_bit = 0x80000000
  };
};


template< typename REAL_ICU >
class Icu_h : public Kobject_h<REAL_ICU>, public Icu_h_base
{
protected:
  typedef Irq_mgr::Msi_info Msi_info;

  REAL_ICU const *this_icu() const
  { return nonull_static_cast<REAL_ICU const *>(this); }

  REAL_ICU *this_icu()
  { return nonull_static_cast<REAL_ICU *>(this); }

  L4_RPC(Op_bind,     icu_bind,     (Mword irqnum, Ko::Cap<Irq> irq));
  L4_RPC(Op_unbind,   icu_unbind,   (Mword irqnum, Ko::Cap<Irq> irq));
  L4_RPC(Op_set_mode, icu_set_mode, (Mword irqnum, Irq_chip::Mode mode));
  L4_RPC(Op_info,     icu_get_info, (Mword *features, Mword *num_irqs, Mword *num_msis));
  L4_RPC(Op_msi_info, icu_msi_info, (Mword msinum, Unsigned64 src_id, Msi_info *info));
};


IMPLEMENTATION:

PUBLIC inline
template<typename REAL_ICU>
void
Icu_h<REAL_ICU>::icu_mask_irq(bool mask, unsigned irqnum)
{
  Irq_base *i = this_icu()->icu_get_irq(irqnum);

  if (EXPECT_FALSE(!i))
    return;

  if (mask)
    i->mask();
  else
    i->unmask();
}

PUBLIC inline
template<typename REAL_ICU>
L4_msg_tag
Icu_h<REAL_ICU>::op_icu_bind(unsigned irqnum, Ko::Cap<Irq> const &irq)
{
  if (!Ko::check_rights(irq.rights, Ko::Rights::CW()))
    return Kobject_iface::commit_result(-L4_err::EPerm);

  auto g = lock_guard(irq.obj->irq_lock());
  irq.obj->unbind();

  return Kobject_iface::commit_result(this_icu()->icu_bind_irq(irqnum, irq.obj));
}

PUBLIC inline
template<typename REAL_ICU>
L4_msg_tag
Icu_h<REAL_ICU>::op_icu_unbind(unsigned irqnum, Ko::Cap<Irq> const &)
{
  Irq_base *irq = this_icu()->icu_get_irq(irqnum);

  if (irq)
    irq->unbind();

  return Kobject_iface::commit_result(0);
}

PUBLIC inline
template<typename REAL_ICU>
L4_msg_tag
Icu_h<REAL_ICU>::op_icu_set_mode(Mword irqnum, Irq_chip::Mode mode)
{
  auto i = this_icu()->icu_get_chip(irqnum);

  if (!i.chip)
    return Kobject_iface::commit_result(-L4_err::EInval);

  if (i.pin >= i.chip->nr_irqs())
    return Kobject_iface::commit_result(-L4_err::EInval);

  int r = i.chip->set_mode(i.pin, mode);

  Irq_base *irq = i.irq();
  if (irq)
    {
      auto g = lock_guard(irq->irq_lock());
      if (irq->chip() == i.chip && irq->pin() == i.pin)
        irq->switch_mode(i.chip->is_edge_triggered(i.pin));
    }

  return Kobject_iface::commit_result(r);
}

PUBLIC inline
template<typename REAL_ICU>
L4_msg_tag
Icu_h<REAL_ICU>::op_icu_get_info(Mword *features, Mword *num_irqs, Mword *num_msis)
{
  return Kobject_iface::commit_result(
    this_icu()->icu_get_info(features, num_irqs, num_msis));
}

PUBLIC inline
template<typename REAL_ICU>
L4_msg_tag
Icu_h<REAL_ICU>::op_icu_msi_info(Mword msi, Unsigned64 source, Irq_mgr::Msi_info *out)
{
  return Kobject_iface::commit_result(
    this_icu()->icu_msi_info(msi, source, out));
}

PUBLIC template< typename REAL_ICU >
inline
L4_msg_tag
Icu_h<REAL_ICU>::icu_invoke(L4_obj_ref, L4_fpage::Rights /*rights*/,
                            Syscall_frame *f,
                            Utcb const *utcb, Utcb *out)
{
  L4_msg_tag tag = f->tag();

  switch (utcb->values[0])
    {
    case Op_bind:
      return Msg_icu_bind::call(this_icu(), tag, utcb, out);

    case Op_unbind:
      return Msg_icu_unbind::call(this_icu(), tag, utcb, out);

    case Op_info:
      return Msg_icu_get_info::call(this_icu(), tag, utcb, out);

    case Op_msi_info:
      return Msg_icu_msi_info::call(this_icu(), tag, utcb, out);

    case Op_unmask:
    case Op_mask:
      if (tag.words() < 2)
        return Kobject_h<REAL_ICU>::no_reply();

      this_icu()->icu_mask_irq(utcb->values[0] == Op_mask, utcb->values[1]);
      return Kobject_h<REAL_ICU>::no_reply();

    case Op_set_mode:
      return Msg_icu_set_mode::call(this_icu(), tag, utcb, out);

    default:
      return Kobject_iface::commit_result(-L4_err::ENosys);
    }
}

PUBLIC
template< typename REAL_ICU >
L4_msg_tag
Icu_h<REAL_ICU>::kinvoke(L4_obj_ref ref, L4_fpage::Rights rights,
                         Syscall_frame *f,
                         Utcb const *in, Utcb *out)
{
  L4_msg_tag tag = f->tag();

  if (!Ko::check_basics(&tag, rights, L4_msg_tag::Label_irq))
    return tag;

  return icu_invoke(ref, rights, f, in, out);
}

