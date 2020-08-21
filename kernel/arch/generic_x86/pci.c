/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>

#include <tilck/kernel/hal.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/mods/acpi.h>

#include <3rd_party/acpi/acpi.h>
#include <3rd_party/acpi/accommon.h>

#include "pci_classes.c.h"

#if KRN_PCI_VENDORS_LIST

   #include "pci_vendors.c.h"

#else

   const struct pci_vendor pci_vendors_list[] = {
      { 0xffff, "Illegal Vendor ID" }
   };

#endif

#define PCI_CONFIG_ADDRESS              0xcf8
#define PCI_CONFIG_DATA                 0xcfc

#define PCI_CLASS_BRIDGE                 0x06
#define PCI_SUBCLASS_PCI_BRIDGE          0x04
#define PCI_DEV_BASE_INFO                0x00
#define PCI_CLASS_INFO_OFF               0x08
#define PCI_HDR_TYPE_OFF                 0x0e

#define PCI_HDR1_SECOND_BUS              0x19
#define PCI_HDR1_SUBORD_BUS              0x1a

#define BUS_NOT_VISITED                     0
#define BUS_TO_VISIT                        1
#define BUS_VISITED                         2

struct pci_segment {
   u64 base_paddr;
   u16 segment;
   u8 start_bus;
   u8 end_bus;
};

static u8 pci_buses[256];
static u32 pcie_segments_cnt;
static struct pci_segment *pcie_segments;

int (*__pci_config_read_func)(struct pci_device_loc, u32, u32, u32 *);
int (*__pci_config_write_func)(struct pci_device_loc, u32, u32, u32);

static void
pci_mark_bus_to_visit(u8 bus)
{
   if (pci_buses[bus] == BUS_NOT_VISITED) {
      pci_buses[bus] = BUS_TO_VISIT;
   }
}

static void
pci_mark_bus_as_visited(u8 bus)
{
   pci_buses[bus] = BUS_VISITED;
}

const char *
pci_find_vendor_name(u16 id)
{
   for (int i = 0; i < ARRAY_SIZE(pci_vendors_list); i++)
      if (pci_vendors_list[i].vendor_id == id)
         return pci_vendors_list[i].name;

   return NULL;
}

void
pci_find_device_class_name(struct pci_device_class *dev_class)
{
   int i;

   dev_class->class_name = NULL;
   dev_class->subclass_name = NULL;
   dev_class->progif_name = NULL;

   for (i = 0; i < ARRAY_SIZE(pci_device_classes_list); i++) {
      if (pci_device_classes_list[i].class_id == dev_class->class_id) {
         dev_class->class_name = pci_device_classes_list[i].class_name;
         break;
      }
   }

   if (!dev_class->class_name)
      return; /* PCI device class not found */

   /* Ok, we've found the device class, now look for the subclass */
   for (; i < ARRAY_SIZE(pci_device_classes_list); i++) {

      if (pci_device_classes_list[i].class_id != dev_class->class_id)
         break; /* it's pointless to search further */

      if (pci_device_classes_list[i].subclass_id == dev_class->subclass_id) {
         dev_class->subclass_name = pci_device_classes_list[i].subclass_name;
         break;
      }
   }

   if (!dev_class->subclass_name)
      return; /* PCI device sub-class not found */

   /* Ok, we've found both the class and the subclass. Look for a progif */
   for (; i < ARRAY_SIZE(pci_device_classes_list); i++) {

      if (pci_device_classes_list[i].subclass_id != dev_class->subclass_id)
         break; /* it's pointless to search further */

      if (pci_device_classes_list[i].progif_id == dev_class->progif_id) {
         dev_class->progif_name = pci_device_classes_list[i].progif_name;
         break;
      }
   }
}

static int
pci_ioport_config_read(struct pci_device_loc loc, u32 off, u32 width, u32 *val)
{
   const u32 bus = loc.bus;
   const u32 dev = loc.dev;
   const u32 func = loc.func;
   const u32 aoff = off & ~3u;    /* off aligned at 4-byte boundary */
   const u32 addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8) | aoff;
   const u16 data_port = PCI_CONFIG_DATA + (off & 3);

   if (UNLIKELY(loc.seg != 0))
      return -EINVAL; /* Conventional PCI has no segment support */

   if (UNLIKELY(off >= 256 || off & ((width >> 3) - 1)))
      return -EINVAL;

   /* Write the address to the PCI config. space addr I/O port */
   outl(PCI_CONFIG_ADDRESS, addr);

   /* Read the data from the PCI config. space data I/O port */
   switch (width) {
      case 8:
         *val = inb(data_port);
         break;
      case 16:
         *val = inw(data_port);
         break;
      case 32:
         *val = inl(data_port);
         break;
      default:
         return -EINVAL;
   }

   return 0;
}

static int
pci_ioport_config_write(struct pci_device_loc loc, u32 off, u32 width, u32 val)
{
   const u32 bus = loc.bus;
   const u32 dev = loc.dev;
   const u32 func = loc.func;
   const u32 aoff = off & ~3u;    /* off aligned at 4-byte boundary */
   const u32 addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8) | aoff;
   const u16 data_port = PCI_CONFIG_DATA + (off & 3);

   if (UNLIKELY(loc.seg != 0))
      return -EINVAL; /* Conventional PCI has no segment support */

   if (UNLIKELY(off >= 256 || off & ((width >> 3) - 1)))
      return -EINVAL;

   /* Write the address to the PCI config. space addr I/O port */
   outl(PCI_CONFIG_ADDRESS, addr);

   /* Write the data to the PCI config. space data I/O port */
   switch (width) {
      case 8:
         outb(data_port, (u8)val);
         break;
      case 16:
         outw(data_port, (u16)val);
         break;
      case 32:
         outl(data_port, (u32)val);
         break;
      default:
         return -EINVAL;
   }

   return 0;
}

static int
pci_mmio_config_read(struct pci_device_loc loc, u32 off, u32 width, u32 *val)
{
   return 0;
}

static int
pci_mmio_config_write(struct pci_device_loc loc, u32 off, u32 width, u32 val)
{
   return 0;
}

int pci_device_get_info(struct pci_device_loc loc,
                        struct pci_device_basic_info *nfo)
{
   int rc;
   u32 tmp;

   if ((rc = pci_config_read(loc, PCI_DEV_BASE_INFO, 32, &nfo->__dev_vendr)))
      return rc;

   if (nfo->vendor_id == 0xffff || nfo->vendor_id == 0)
      return -ENOENT;

   if ((rc = pci_config_read(loc, PCI_CLASS_INFO_OFF, 32, &nfo->__class_info)))
      return rc;

   if ((rc = pci_config_read(loc, PCI_HDR_TYPE_OFF, 8, &tmp)))
      return rc;

   nfo->__header_type = tmp & 0xff;
   return 0;
}

/*
 * Initialize the support for the Enhanced Configuration Access Mechanism,
 * used by PCI Express.
 */
static void
init_pci_ecam(void)
{
   ACPI_STATUS rc;
   ACPI_TABLE_HEADER *hdr;
   const ACPI_EXCEPTION_INFO *ex;
   struct acpi_mcfg_allocation *it;

   if (get_acpi_init_status() < ais_tables_initialized) {
      printk("PCI: no ACPI. Don't check for MCFG\n");
      return;
   }

   if (!MOD_acpi)
      return;

   rc = AcpiGetTable("MCFG", 1, &hdr);

   if (rc == AE_NOT_FOUND) {
      printk("PCI: ACPI table MCFG not found.\n");
      return;
   }

   if (rc != AE_OK) {

      ex = AcpiUtValidateException(rc);

      if (ex)
         printk("PCI: AcpiGetTable() failed with: %s\n", ex->Name);
      else
         printk("PCI: AcpiGetTable() failed with: %d\n", rc);

      return;
   }

   pcie_segments_cnt =
      (hdr->Length - sizeof(struct acpi_table_mcfg)) / sizeof(*it);
   it = (void *)((char *)hdr + sizeof(struct acpi_table_mcfg));

   printk("PCI: ACPI table MCFG found.\n");
   printk("PCI: MCFG has %u elements\n", pcie_segments_cnt);

   pcie_segments = kalloc_array_obj(struct pci_segment, pcie_segments_cnt);

   if (UNLIKELY(!pcie_segments)) {
      printk("PCI: ERROR: no memory for PCIe segments list\n");
      pcie_segments_cnt = 0;
      return;
   }

   for (u32 i = 0; i < pcie_segments_cnt; i++, it++) {

      printk("PCI: MCFG elem[%u]\n", i);
      printk("    Base paddr: %#llx\n", it->Address);
      printk("    Segment:    %u\n", it->PciSegment);
      printk("    Start bus:  %u\n", it->StartBusNumber);
      printk("    End bus:    %u\n", it->EndBusNumber);

      pcie_segments[i] = (struct pci_segment) {
         .base_paddr = it->Address,
         .segment = it->PciSegment,
         .start_bus = it->StartBusNumber,
         .end_bus = it->EndBusNumber,
      };
   }

   AcpiPutTable(hdr);
}


static void
pci_dump_device_info(struct pci_device_loc loc,
                     struct pci_device_basic_info *nfo)
{
   struct pci_device_class dc = {0};
   const char *vendor;

   dc.class_id = nfo->class_id;
   dc.subclass_id = nfo->subclass_id;
   dc.progif_id = nfo->progif_id;

   pci_find_device_class_name(&dc);
   vendor = pci_find_vendor_name(nfo->vendor_id);

   printk("PCI: %04x:%02x:%02x.%x: ", loc.seg, loc.bus, loc.dev, loc.func);

   if (dc.subclass_name && dc.progif_name) {

      if (vendor)
         printk("%s: %s %s\n", dc.subclass_name, vendor, dc.progif_name);
      else
         printk("%s (%s)\n", dc.subclass_name, dc.progif_name);

   } else if (dc.subclass_name) {

      if (vendor)
         printk("%s: %s\n", dc.subclass_name, vendor);
      else
         printk("%s\n", dc.subclass_name);

   } else if (dc.class_name) {

      if (vendor)
         printk("%s: %s (subclass: %#x)\n",
                dc.class_name, vendor, dc.subclass_id);
      else
         printk("%s (subclass: %#x)\n", dc.class_name, dc.subclass_id);

   } else {

      if (vendor)
         printk("vendor: %s, class: %#x, subclass: %#x\n",
                vendor, dc.class_id, dc.subclass_id);
      else
         printk("class: %#x, subclass: %#x\n", dc.class_id, dc.subclass_id);
   }
}

static bool
pci_discover_device_func(struct pci_device_loc loc,
                         struct pci_device_basic_info *dev_nfo)
{
   struct pci_device_basic_info __nfo;
   struct pci_device_basic_info *nfo = &__nfo;
   u32 secondary_bus;
   u32 subordinate_bus;

   if (loc.func != 0) {

      if (pci_device_get_info(loc, nfo))
         return false; /* no such device function */

   } else {

      ASSERT(dev_nfo != NULL);
      nfo = dev_nfo;
   }

   pci_dump_device_info(loc, nfo);

   if (nfo->class_id == PCI_CLASS_BRIDGE &&
       nfo->subclass_id == PCI_SUBCLASS_PCI_BRIDGE)
   {
      if (pci_config_read(loc, PCI_HDR1_SECOND_BUS, 8, &secondary_bus)) {
         printk("PCI: error while reading from config space\n");
         return false;
      }

      if (pci_config_read(loc, PCI_HDR1_SUBORD_BUS, 8, &subordinate_bus)) {
         printk("PCI: error while reading from config space\n");
         return false;
      }

      for (u32 i = secondary_bus; i <= subordinate_bus; i++)
         pci_mark_bus_to_visit((u8)i);
   }

   return true;
}

static bool
pci_discover_device(struct pci_device_loc loc)
{
   struct pci_device_basic_info nfo;
   ASSERT(loc.func == 0);

   if (pci_device_get_info(loc, &nfo))
      return false; /* no such device */

   if (!pci_discover_device_func(loc, &nfo)) {
      printk("PCI: ERROR discover func 0 failed on existing device!");
      return false;
   }

   if (nfo.multi_func) {
      /* Multi-function device */
      for (u8 func = 1; func < 8; func++) {
         loc.func = func;
         pci_discover_device_func(loc, NULL);
      }
   }

   return true;
}

static void
pci_before_discover_bus(struct pci_segment *seg, u8 bus)
{
   if (!seg)
      return; /* nothing to do */

   // TODO: map the (seg, bus) config space
}

static void
pci_after_discover_bus(struct pci_segment *seg, u8 bus)
{
   if (!seg)
      return; /* nothing to do */

   // TODO: un-map the (seg, bus) config space
}

static void
pci_discover_bus(struct pci_segment *seg, u8 bus)
{
   u16 seg_num = seg ? seg->segment : 0;
   struct pci_device_loc loc = pci_make_loc(seg_num, bus, 0, 0);

   pci_mark_bus_as_visited(bus);
   pci_before_discover_bus(seg, bus);

   for (u8 dev = 0; dev < 32; dev++) {
      loc.dev = dev;
      pci_discover_device(loc);
   }

   pci_after_discover_bus(seg, bus);
}

static void
pci_discover_segment(struct pci_segment *seg)
{
   struct pci_device_basic_info nfo;
   int visit_count;

   if (pci_device_get_info(pci_make_loc(0, 0, 0, 0), &nfo)) {
      printk("PCI: FATAL ERROR: cannot get root PCI device info\n");
      return;
   }

   if (!nfo.multi_func) {

      /* Single PCI controller */
      pci_discover_bus(seg, 0);

   } else {

      /* Multiple PCI controllers */
      for (u8 func = 0; func < 8; func++) {

         if (pci_device_get_info(pci_make_loc(0, 0, 0, func), &nfo))
            break;

         pci_discover_bus(seg, func);
      }
   }

   /* Discover devices in the additional buses marked for visit */
   do {

      visit_count = 0;

      for (u32 bus = 1; bus < 256; bus++) {
         if (pci_buses[bus] == BUS_TO_VISIT) {
            pci_discover_bus(seg, (u8)bus);
            visit_count++;
         }
      }

   } while (visit_count > 0);
}

void
init_pci(void)
{
   init_pci_ecam();

   if (pcie_segments_cnt) {

      /* PCI Express is supported */
      __pci_config_read_func = &pci_mmio_config_read;
      __pci_config_write_func = &pci_mmio_config_write;

      /* Iterate over all the PCI Express segment groups */
      for (u32 i = 0; i < pcie_segments_cnt; i++)
         pci_discover_segment(&pcie_segments[i]);

   } else {

      /* No PCI Express support */
      __pci_config_read_func = &pci_ioport_config_read;
      __pci_config_write_func = &pci_ioport_config_write;
      pci_discover_segment(NULL);
   }
}
