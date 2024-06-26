/*
 *	The PCI Library -- Direct Configuration access via i386 Ports
 *
 *	Copyright (c) 1997--2006 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL v2+.
 *
 *	SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "internal.h"

#include <string.h>

#if defined(PCI_OS_LINUX)
#include "i386-io-linux.h"
#elif defined(PCI_OS_GNU)
#include "i386-io-hurd.h"
#elif defined(PCI_OS_SUNOS)
#include "i386-io-sunos.h"
#elif defined(PCI_OS_WINDOWS)
#include "i386-io-windows.h"
#elif defined(PCI_OS_CYGWIN)
#include "i386-io-cygwin.h"
#elif defined(PCI_OS_HAIKU)
#include "i386-io-haiku.h"
#elif defined(PCI_OS_BEOS)
#include "i386-io-beos.h"
#elif defined(PCI_OS_DJGPP)
#include "i386-io-djgpp.h"
#elif defined(PCI_OS_OPENBSD)
#include "i386-io-openbsd.h"
#else
#error Do not know how to access I/O ports on this OS.
#endif

static int conf12_io_enabled = -1;		/* -1=haven't tried, 0=failed, 1=succeeded */

static int
conf12_setup_io(struct pci_access *a)
{
  if (conf12_io_enabled < 0)
    conf12_io_enabled = intel_setup_io(a);
  return conf12_io_enabled;
}

static void
conf12_init(struct pci_access *a)
{
  if (!conf12_setup_io(a))
    a->error("No permission to access I/O ports (you probably have to be root).");
}

static void
conf12_cleanup(struct pci_access *a)
{
  if (conf12_io_enabled > 0)
    {
      intel_cleanup_io(a);
      conf12_io_enabled = -1;
    }
}

/*
 * Before we decide to use direct hardware access mechanisms, we try to do some
 * trivial checks to ensure it at least _seems_ to be working -- we just test
 * whether bus 00 contains a host bridge (this is similar to checking
 * techniques used in XFree86, but ours should be more reliable since we
 * attempt to make use of direct access hints provided by the PCI BIOS).
 *
 * This should be close to trivial, but it isn't, because there are buggy
 * chipsets (yes, you guessed it, by Intel and Compaq) that have no class ID.
 */

static int
intel_sanity_check(struct pci_access *a, struct pci_methods *m)
{
  struct pci_dev d;

  memset(&d, 0, sizeof(d));
  a->debug("...sanity check");
  d.bus = 0;
  d.func = 0;
  for (d.dev = 0; d.dev < 32; d.dev++)
    {
      u16 class, vendor;
      if (m->read(&d, PCI_CLASS_DEVICE, (byte *) &class, sizeof(class)) &&
	  (class == cpu_to_le16(PCI_CLASS_BRIDGE_HOST) || class == cpu_to_le16(PCI_CLASS_DISPLAY_VGA)) ||
	  m->read(&d, PCI_VENDOR_ID, (byte *) &vendor, sizeof(vendor)) &&
	  (vendor == cpu_to_le16(PCI_VENDOR_ID_INTEL) || vendor == cpu_to_le16(PCI_VENDOR_ID_COMPAQ)))
	{
	  a->debug("...outside the Asylum at 0/%02x/0", d.dev);
	  return 1;
	}
    }
  a->debug("...insane");
  return 0;
}

/*
 *	Configuration type 1
 */

#define CONFIG_CMD(bus, device_fn, where)   (0x80000000 | (bus << 16) | (device_fn << 8) | (where & ~3))

static int
conf1_detect(struct pci_access *a)
{
  unsigned int tmp;
  int res = 0;

  if (!conf12_setup_io(a))
    {
      a->debug("...no I/O permission");
      return 0;
    }

  intel_io_lock();
  intel_outb (0x01, 0xCFB);
  tmp = intel_inl (0xCF8);
  intel_outl (0x80000000, 0xCF8);
  if (intel_inl (0xCF8) == 0x80000000)
    res = 1;
  intel_outl (tmp, 0xCF8);
  intel_io_unlock();

  if (res)
    res = intel_sanity_check(a, &pm_intel_conf1);
  return res;
}

static int
conf1_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  int addr = 0xcfc + (pos&3);
  int res = 1;

  if (d->domain || pos >= 256)
    return 0;

  if (len != 1 && len != 2 && len != 4)
    return pci_generic_block_read(d, pos, buf, len);

  intel_io_lock();
  intel_outl(0x80000000 | ((d->bus & 0xff) << 16) | (PCI_DEVFN(d->dev, d->func) << 8) | (pos&~3), 0xcf8);

  switch (len)
    {
    case 1:
      buf[0] = intel_inb(addr);
      break;
    case 2:
      ((u16 *) buf)[0] = cpu_to_le16(intel_inw(addr));
      break;
    case 4:
      ((u32 *) buf)[0] = cpu_to_le32(intel_inl(addr));
      break;
    }

  intel_io_unlock();
  return res;
}

static int
conf1_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  int addr = 0xcfc + (pos&3);
  int res = 1;

  if (d->domain || pos >= 256)
    return 0;

  if (len != 1 && len != 2 && len != 4)
    return pci_generic_block_write(d, pos, buf, len);

  intel_io_lock();
  intel_outl(0x80000000 | ((d->bus & 0xff) << 16) | (PCI_DEVFN(d->dev, d->func) << 8) | (pos&~3), 0xcf8);

  switch (len)
    {
    case 1:
      intel_outb(buf[0], addr);
      break;
    case 2:
      intel_outw(le16_to_cpu(((u16 *) buf)[0]), addr);
      break;
    case 4:
      intel_outl(le32_to_cpu(((u32 *) buf)[0]), addr);
      break;
    }
  intel_io_unlock();
  return res;
}

/*
 *	Configuration type 2. Obsolete and brain-damaged, but existing.
 */

static int
conf2_detect(struct pci_access *a)
{
  int res = 0;

  if (!conf12_setup_io(a))
    {
      a->debug("...no I/O permission");
      return 0;
    }

  /* This is ugly and tends to produce false positives. Beware. */

  intel_io_lock();
  intel_outb(0x00, 0xCFB);
  intel_outb(0x00, 0xCF8);
  intel_outb(0x00, 0xCFA);
  if (intel_inb(0xCF8) == 0x00 && intel_inb(0xCFA) == 0x00)
    res = intel_sanity_check(a, &pm_intel_conf2);
  intel_io_unlock();
  return res;
}

static int
conf2_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  int res = 1;
  int addr = 0xc000 | (d->dev << 8) | pos;

  if (d->domain || pos >= 256)
    return 0;

  if (d->dev >= 16)
    /* conf2 supports only 16 devices per bus */
    return 0;

  if (len != 1 && len != 2 && len != 4)
    return pci_generic_block_read(d, pos, buf, len);

  intel_io_lock();
  intel_outb((d->func << 1) | 0xf0, 0xcf8);
  intel_outb(d->bus, 0xcfa);
  switch (len)
    {
    case 1:
      buf[0] = intel_inb(addr);
      break;
    case 2:
      ((u16 *) buf)[0] = cpu_to_le16(intel_inw(addr));
      break;
    case 4:
      ((u32 *) buf)[0] = cpu_to_le32(intel_inl(addr));
      break;
    }
  intel_outb(0, 0xcf8);
  intel_io_unlock();
  return res;
}

static int
conf2_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  int res = 1;
  int addr = 0xc000 | (d->dev << 8) | pos;

  if (d->domain || pos >= 256)
    return 0;

  if (d->dev >= 16)
    /* conf2 supports only 16 devices per bus */
    return 0;

  if (len != 1 && len != 2 && len != 4)
    return pci_generic_block_write(d, pos, buf, len);

  intel_io_lock();
  intel_outb((d->func << 1) | 0xf0, 0xcf8);
  intel_outb(d->bus, 0xcfa);
  switch (len)
    {
    case 1:
      intel_outb(buf[0], addr);
      break;
    case 2:
      intel_outw(le16_to_cpu(* (u16 *) buf), addr);
      break;
    case 4:
      intel_outl(le32_to_cpu(* (u32 *) buf), addr);
      break;
    }

  intel_outb(0, 0xcf8);
  intel_io_unlock();
  return res;
}

struct pci_methods pm_intel_conf1 = {
  .name = "intel-conf1",
  .help = "Raw I/O port access using Intel conf1 interface",
  .detect = conf1_detect,
  .init = conf12_init,
  .cleanup = conf12_cleanup,
  .scan = pci_generic_scan,
  .fill_info = pci_generic_fill_info,
  .read = conf1_read,
  .write = conf1_write,
};

struct pci_methods pm_intel_conf2 = {
  .name = "intel-conf2",
  .help = "Raw I/O port access using Intel conf2 interface",
  .detect = conf2_detect,
  .init = conf12_init,
  .cleanup = conf12_cleanup,
  .scan = pci_generic_scan,
  .fill_info = pci_generic_fill_info,
  .read = conf2_read,
  .write = conf2_write,
};
