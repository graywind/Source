/*
 * Copyright 2026 RenkuOS contributors. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Helpers shared by the USB host controller drivers that a device tree can
 * describe, and the board bring-up the RK3399 needs before its controllers
 * see any device.
 */
#ifndef USB_FDT_H
#define USB_FDT_H


#include <bus/FDT.h>
#include <KernelExport.h>

#include <string.h>


static inline fdt_device_module_info*
usb_fdt_get(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (gDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


// "compatible" is a list of NUL-separated strings, e.g. the RK3399 DWC3
// node's "rockchip,rk3399-dwc3", "snps,dwc3".
static inline bool
usb_fdt_has_compatible(fdt_device_module_info* module, fdt_device* dev,
	const char* wanted)
{
	int length = 0;
	const char* compatible
		= (const char*)module->get_prop(dev, "compatible", &length);
	if (compatible == NULL)
		return false;

	for (int offset = 0; offset < length;
			offset += (int)strlen(compatible + offset) + 1) {
		if (strcmp(compatible + offset, wanted) == 0)
			return true;
	}
	return false;
}


// RK3399 board bring-up.
//
// The firmware configures the USB controllers and PHYs for its own use and
// then resets them when it hands over, so the controllers come up with their
// ports unpowered or suspended. Linux's dwc3 core, the Rockchip USB2 PHY
// driver and the Pinebook's always-on regulators do the following from the
// device tree; RenkuOS has no PHY or regulator driver for this board yet, so
// the drivers do it themselves. This belongs in a proper driver.
#define RK3399_USB_GRF_BASE		0xff770000
#define RK3399_USB2PHY_SUS_MASK		0x3
#define RK3399_USB2PHY_SUS_ON		0x2

#define RK3399_GPIO1_BASE		0xff730000
#define RK3399_GPIO4_BASE		0xff790000
#define RK3399_GPIO_SWPORT_DR		0x00
#define RK3399_GPIO_SWPORT_DDR		0x04
#define RK3399_GPIO1_VCC5V0_USB		(1u << 13)	// bank 1, B5
#define RK3399_GPIO1_VBUS_5VOUT		(1u << 3)	// bank 1, A3
#define RK3399_GPIO4_VCC5V0_OTG		(1u << 26)	// bank 4, D2

#define DWC3_GCTL			0xc110
#define DWC3_GCTL_PRTCAPDIR_HOST	(1 << 12)
#define DWC3_GCTL_PRTCAPDIR_MASK	(3 << 12)
#define DWC3_GUSB2PHYCFG(n)		(0xc200 + (n) * 4)
#define DWC3_GUSB2PHYCFG_SUSPHY		(1 << 6)
#define DWC3_GUSB3PIPECTL(n)		(0xc2c0 + (n) * 4)
#define DWC3_GUSB3PIPECTL_SUSPHY	(1 << 17)


static inline bool
usb_rk3399_usb_address(phys_addr_t address)
{
	// The six controllers the RK3399 describes: two EHCI/OHCI pairs for
	// the USB2 ports and two DWC3 blocks for USB3.
	return (address >= 0xfe380000 && address < 0xfe400000)
		|| address == 0xfe800000 || address == 0xfe900000;
}


static inline void
usb_rk3399_usb_power(void* gpio, uint32 pin)
{
	uint32* dr = (uint32*)((addr_t)gpio + RK3399_GPIO_SWPORT_DR);
	uint32* ddr = (uint32*)((addr_t)gpio + RK3399_GPIO_SWPORT_DDR);

	*dr |= pin;
	*ddr |= pin;
}


static inline void
usb_rk3399_usb_power_on(void)
{
	// vcc5v0_usb feeds the two port regulators; vcc5v0_otg powers the
	// USB-A port, vbus_5vout the Type-C port.
	static const struct {
		phys_addr_t	base;
		uint32		pin;
	} banks[] = {
		{ RK3399_GPIO1_BASE, RK3399_GPIO1_VCC5V0_USB },
		{ RK3399_GPIO1_BASE, RK3399_GPIO1_VBUS_5VOUT },
		{ RK3399_GPIO4_BASE, RK3399_GPIO4_VCC5V0_OTG }
	};

	for (uint32 i = 0; i < B_COUNT_OF(banks); i++) {
		void* gpio = NULL;
		if (map_physical_memory("rk3399 usb power", banks[i].base, 0x1000,
				B_ANY_KERNEL_ADDRESS,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &gpio) < B_OK) {
			TRACE_MODULE_ERROR("cannot map the USB power GPIOs\n");
			continue;
		}
		usb_rk3399_usb_power(gpio, banks[i].pin);
	}
}


static inline void
usb_rk3399_usb2phy_power_on(void)
{
	// PHY_SUS bits [1:0] = 2 means running, for the OTG port the DWC3
	// controllers use and the host port of the EHCI/OHCI ones. The upper
	// half of the write carries the GRF write mask.
	static const uint32 offsets[] = {
		0xe454,	// u2phy0 OTG
		0xe458,	// u2phy0 host
		0xe464,	// u2phy1 OTG
		0xe468	// u2phy1 host
	};

	void* grf = NULL;
	if (map_physical_memory("rk3399 usb phy", RK3399_USB_GRF_BASE, 0x10000,
			B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &grf) < B_OK) {
		TRACE_MODULE_ERROR("cannot map the GRF\n");
		return;
	}

	for (uint32 i = 0; i < B_COUNT_OF(offsets); i++) {
		uint32* reg = (uint32*)((addr_t)grf + offsets[i]);
		*reg = (*reg & ~RK3399_USB2PHY_SUS_MASK) | RK3399_USB2PHY_SUS_ON
			| (RK3399_USB2PHY_SUS_MASK << 16);
	}
}


// Put the DWC3 wrapper into host mode and keep both PHYs out of suspend,
// as dwc3_core_init() does before the xHCI block is started.
static inline void
usb_rk3399_dwc3_init(phys_addr_t address, uint64 length)
{
	const uint64 mapLength = length < 0x10000 ? length : 0x10000;

	void* wrapper = NULL;
	if (map_physical_memory("rk3399 dwc3 wrapper", address, mapLength,
			B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &wrapper) < B_OK) {
		TRACE_MODULE_ERROR("cannot map the DWC3 wrapper\n");
		return;
	}

	uint32* gctl = (uint32*)((addr_t)wrapper + DWC3_GCTL);
	uint32* usb2phycfg = (uint32*)((addr_t)wrapper + DWC3_GUSB2PHYCFG(0));
	uint32* usb3pipectl = (uint32*)((addr_t)wrapper + DWC3_GUSB3PIPECTL(0));

	*gctl = (*gctl & ~DWC3_GCTL_PRTCAPDIR_MASK) | DWC3_GCTL_PRTCAPDIR_HOST;
	*usb2phycfg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	*usb3pipectl &= ~DWC3_GUSB3PIPECTL_SUSPHY;
}


// Board bring-up for one controller, if it is one of the RK3399's.
static inline void
usb_rk3399_setup(phys_addr_t address, uint64 length)
{
	if (!usb_rk3399_usb_address(address))
		return;

	usb_rk3399_usb_power_on();
	usb_rk3399_usb2phy_power_on();

	// Only the DWC3 controllers have a wrapper at the register base.
	if (address == 0xfe800000 || address == 0xfe900000)
		usb_rk3399_dwc3_init(address, length);
}


#endif	// USB_FDT_H
