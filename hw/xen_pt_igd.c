/*
 * graphics passthrough
 */
#include "xen_pt.h"
#include "xen-host-pci-device.h"
#include "xen_backend.h"
#include "pci_bridge.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <assert.h>

#define D(fmt, args...) \
	fprintf(stderr, "D [ %lu ] %s() - %d: "fmt"\n", time(NULL), __func__, __LINE__, ##args)

#define E(fmt, args...) \
	fprintf(stderr, "E [ %lu ] %s() - %d: "fmt"\n", time(NULL), __func__, __LINE__, ##args)

int gfx_passthru;

static uint32_t igd_guest_opregion = 0;

typedef struct {
	PCIDevice dev;
} ISABridgeState;

static TypeInfo isa_bridge_info = {
	.name          = "inte-pch-isa-bridge",
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(ISABridgeState),
};

static void xen_pt_graphics_register_types(void)
{
	type_register_static(&isa_bridge_info);
}

type_init(xen_pt_graphics_register_types)

static int create_pch_isa_bridge(PCIBus *bus, XenHostPCIDevice *hdev)
{
	struct PCIDevice *dev;

	char rid;

	dev = pci_create(bus, PCI_DEVFN(0x1f, 0), "inte-pch-isa-bridge");
	if (!dev) {
		E("fail to create PCH ISA bridge.");
		return -1;
	}

	qdev_init_nofail(&dev->qdev);

	pci_config_set_vendor_id(dev->config, hdev->vendor_id);
	pci_config_set_device_id(dev->config, hdev->device_id);

	xen_host_pci_get_block(hdev, PCI_REVISION_ID, (uint8_t *)&rid, 1);

	pci_config_set_revision(dev->config, rid);
	pci_config_set_class(dev->config, PCI_CLASS_BRIDGE_ISA);

	D("vid: 0x%x, did: 0x%x rid: 0x%x.", (unsigned int)hdev->vendor_id,
			(unsigned int)hdev->device_id,
			(unsigned int)rid);

	return 0;
}

void intel_pch_init(PCIBus *bus)
{
	XenHostPCIDevice hdev;
	int r;

	D("Called.");

	r = xen_host_pci_device_get(&hdev, 0, 0, 0x1f, 0);
	if (r) {
		E("fail to find intel PCH.");
		goto err;
	}

	if (hdev.vendor_id == PCI_VENDOR_ID_INTEL) {
		r = create_pch_isa_bridge(bus, &hdev);
		if (r) {
			E("fail to create PCH ISA bridge.");
			goto err;
		}
	}

	xen_host_pci_device_put(&hdev);

	return;

err:
	E("fail to detect intel PCH.");
	abort();

	return;
}

uint32_t igd_read_opregion(struct XenHostPCIDevice *dev)
{
	uint32_t val = -1;

	if ( igd_guest_opregion == 0 )
		return -1;

	val = igd_guest_opregion;

	D("val: 0x%x.", val);

	return val;
}

void igd_write_opregion(struct XenHostPCIDevice *dev, uint32_t val)
{
	uint32_t host_opregion = 0;
	int ret;

	D("Called.");

	if (igd_guest_opregion) {
		E("opregion register already been set, ignoring %x\n", val);
		return;
	}

	xen_host_pci_get_block(dev, PCI_INTEL_OPREGION, (uint8_t *)&host_opregion, 4);
	igd_guest_opregion = (val & ~0xfff) | (host_opregion & 0xfff);

	D("Map OpRegion: %x -> %x", host_opregion, igd_guest_opregion);

	ret = xc_domain_memory_mapping(xen_xc, xen_domid,
			igd_guest_opregion >> XC_PAGE_SHIFT,
			host_opregion >> XC_PAGE_SHIFT,
			2,
			DPCI_ADD_MAPPING);

	if ( ret != 0 )
	{
		E("Can't map opregion");
		igd_guest_opregion = 0;
	}

	return;
}

void igd_pci_write(PCIDevice *pci_dev, uint32_t config_addr, uint32_t val, int len)
{
	XenHostPCIDevice dev;
	int r;

	assert(pci_dev->devfn == 0x00);

	D("B W %x %x %x", config_addr, val, len);

	switch (config_addr)
	{
		case 0x58:        // PAVPC Offset
			break;
		default:
			goto write_default;
	}

	/* Host write */
	r = xen_host_pci_device_get(&dev, 0, 0, 0, 0);
	if (r) {
		E("Can't get pci_dev_host_bridge");
		abort();
	}

	r = xen_host_pci_set_block(&dev, config_addr, (uint8_t *)&val, len);
	if (r) {
		E("Can't get pci_dev_host_bridge");
		abort();
	}

	xen_host_pci_device_put(&dev);

	D("addr=%x len=%x val=%x", config_addr, len, val);

	return;

write_default:
	pci_default_write_config(pci_dev, config_addr, val, len);

	return;
}

uint32_t igd_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len)
{
	XenHostPCIDevice dev;
	uint32_t val;
	int r;

	D("B R %x %x", config_addr, len);

	assert(pci_dev->devfn == 0x00);

	switch (config_addr)
	{
		case 0x00:        /* vendor id */
		case 0x02:        /* device id */
		case 0x08:        /* revision id */
		case 0x2c:        /* sybsystem vendor id */
		case 0x2e:        /* sybsystem id */
		case 0x50:        /* SNB: processor graphics control register */
		case 0x52:        /* processor graphics control register */
		case 0xa0:        /* top of memory */
		case 0xb0:        /* ILK: BSM: should read from dev 2 offset 0x5c */
		case 0x58:        /* SNB: PAVPC Offset */
		case 0xa4:        /* SNB: graphics base of stolen memory */
		case 0xa8:        /* SNB: base of GTT stolen memory */
			break;
		default:
			goto read_default;
	}

	/* Host read */
	r = xen_host_pci_device_get(&dev, 0, 0, 0, 0);
	if (r) {
		E("Can't get pci_dev_host_bridge");
		abort();
	}

	r = xen_host_pci_get_block(&dev, config_addr, (uint8_t *)&val, len);
	if (r) {
		E("Can't get pci_dev_host_bridge");
		abort();
	}

	xen_host_pci_device_put(&dev);

	D("B TR %x %x %x", config_addr, val, len);

	return val;

read_default:

	return pci_default_read_config(pci_dev, config_addr, len);
}

/*
 * register VGA resources for the domain with assigned gfx
 */
int register_vga_regions(struct PCIDevice *dev)
{
	int ret = 0;

	ret |= xc_domain_ioport_mapping(xen_xc, xen_domid, 0x3B0,
			0x3B0, 0xA, DPCI_ADD_MAPPING);

	ret |= xc_domain_ioport_mapping(xen_xc, xen_domid, 0x3C0,
			0x3C0, 0x20, DPCI_ADD_MAPPING);

	ret |= xc_domain_memory_mapping(xen_xc, xen_domid,
			0xa0000 >> XC_PAGE_SHIFT,
			0xa0000 >> XC_PAGE_SHIFT,
			0x20,
			DPCI_ADD_MAPPING);

	if (ret != 0)
		E("VGA region mapping failed");

	return ret;
}

/*
 * unregister VGA resources for the domain with assigned gfx
 */
int unregister_vga_regions(struct PCIDevice *dev)
{
	int ret = 0;

	if ( !gfx_passthru || PCI_DEVICE_GET_CLASS(dev)->class_id != 0x0300 )
		return ret;

	ret |= xc_domain_ioport_mapping(xen_xc, xen_domid, 0x3B0,
			0x3B0, 0xC, DPCI_REMOVE_MAPPING);

	ret |= xc_domain_ioport_mapping(xen_xc, xen_domid, 0x3C0,
			0x3C0, 0x20, DPCI_REMOVE_MAPPING);

	ret |= xc_domain_memory_mapping(xen_xc, xen_domid,
			0xa0000 >> XC_PAGE_SHIFT,
			0xa0000 >> XC_PAGE_SHIFT,
			20,
			DPCI_REMOVE_MAPPING);

	ret |= xc_domain_memory_mapping(xen_xc, xen_domid,
			igd_guest_opregion >> XC_PAGE_SHIFT,
			igd_guest_opregion >> XC_PAGE_SHIFT,
			2,
			DPCI_REMOVE_MAPPING);

    if (ret != 0)
        E("VGA region unmapping failed");

	return ret;
}

static int get_vgabios(unsigned char *buf)
{
    int fd;
    uint32_t bios_size = 0;
    uint32_t start = 0xC0000;
    uint16_t magic = 0;

    if ((fd = open("/dev/mem", O_RDONLY)) < 0) {
        E("Error: Can't open /dev/mem: %s", strerror(errno));
        return 0;
    }

    /*
     * Check if it a real bios extension.
     * The magic number is 0xAA55.
     */
    if (start != lseek(fd, start, SEEK_SET))
        goto out;
    if (read(fd, &magic, 2) != 2)
        goto out;
    if (magic != 0xAA55)
        goto out;

    /* Find the size of the rom extension */
    if (start != lseek(fd, start, SEEK_SET))
        goto out;
    if (lseek(fd, 2, SEEK_CUR) != (start + 2))
        goto out;
    if (read(fd, &bios_size, 1) != 1)
        goto out;

    /* This size is in 512 bytes */
    bios_size *= 512;

    /*
     * Set the file to the begining of the rombios,
     * to start the copy.
     */
    if (start != lseek(fd, start, SEEK_SET))
        goto out;

    if (bios_size != read(fd, buf, bios_size))
        bios_size = 0;

out:
    close(fd);
    return bios_size;
}

int setup_vga_pt(struct PCIDevice *dev)
{
	unsigned char *bios = NULL;
	int bios_size = 0;
	char *c = NULL;
	char checksum = 0;
	int rc = 0;

	/* Allocated 64K for the vga bios */
	if (!(bios = malloc(64 * 1024)))
		return -1;

	bios_size = get_vgabios(bios);
	if (bios_size == 0 || bios_size > 64 * 1024) {
		E("vga bios size (0x%x) is invalid!", bios_size);
		rc = -1;
		goto out;
	}

	/* Adjust the bios checksum */
	for (c = (char*)bios; c < ((char*)bios + bios_size); c++)
		checksum += *c;

	if (checksum) {
		bios[bios_size - 1] -= checksum;
		D("vga bios checksum is adjusted!");
	}

	cpu_physical_memory_rw(0xc0000, bios, bios_size, 1);
out:
	free(bios);
	return rc;
}
