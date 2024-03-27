#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include "physheap.h"
#include "pvrsrv_device.h"
#include "rgxdevice.h"
#include "syscommon.h"

#define SYS_RGX_ACTIVE_POWER_LATENCY_MS	10
#define RGX_HW_CORE_CLOCK_SPEED		395000000
#define SG2380_SYSTEM_NAME		"sg2380"

static IMG_HANDLE ghSysData;

struct sg2380_sysdata
{
	struct device *dev;

	void __iomem *reg_base;
	resource_size_t rgx_start;
	resource_size_t rgx_size;

	int rgx_irq;

	bool has_dma;
	char *dma_tx_chan_name;
	char *dma_rx_chan_name;
};

/*
 * CPU to Device physical address translation
 */
static
void UMAPhysHeapCpuPAddrToDevPAddr(IMG_HANDLE hPrivData,
				   IMG_UINT32 ui32NumOfAddr,
				   IMG_DEV_PHYADDR *psDevPAddr,
				   IMG_CPU_PHYADDR *psCpuPAddr)
{
	PVR_UNREFERENCED_PARAMETER(hPrivData);

	/* Optimise common case */
	psDevPAddr[0].uiAddr = psCpuPAddr[0].uiAddr;
	if (ui32NumOfAddr > 1) {
		IMG_UINT32 ui32Idx;
		for (ui32Idx = 1; ui32Idx < ui32NumOfAddr; ++ui32Idx)
			psDevPAddr[ui32Idx].uiAddr = psCpuPAddr[ui32Idx].uiAddr;
	}
}

/*
 * Device to CPU physical address translation
 */
static
void UMAPhysHeapDevPAddrToCpuPAddr(IMG_HANDLE hPrivData,
				   IMG_UINT32 ui32NumOfAddr,
				   IMG_CPU_PHYADDR *psCpuPAddr,
				   IMG_DEV_PHYADDR *psDevPAddr)
{
	PVR_UNREFERENCED_PARAMETER(hPrivData);

	/* Optimise common case */
	psCpuPAddr[0].uiAddr = psDevPAddr[0].uiAddr;
	if (ui32NumOfAddr > 1) {
		IMG_UINT32 ui32Idx;
		for (ui32Idx = 1; ui32Idx < ui32NumOfAddr; ++ui32Idx)
			psCpuPAddr[ui32Idx].uiAddr = psDevPAddr[ui32Idx].uiAddr;
	}
}

static PHYS_HEAP_FUNCTIONS gsPhysHeapFuncs = {
	.pfnCpuPAddrToDevPAddr = UMAPhysHeapCpuPAddrToDevPAddr,
	.pfnDevPAddrToCpuPAddr = UMAPhysHeapDevPAddrToCpuPAddr,
};

static void *get_dma_chan(PVRSRV_DEVICE_CONFIG *psDevConfig, char *name)
{
	// get dev
	struct device *dev = psDevConfig->hSysData->dev;

	return dma_request_chan(dev, name);
}

static void free_dma_chan(PVRSRV_DEVICE_CONFIG *psDevConfig, void* channel)
{
	dma_release_channel(channel);
}

struct sg2380_sysdata *get_dev_resource(struct device *dev)
{
	struct sg2380_sysdata *sysdata;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	struct of_phandle_args args;

	sysdata = devm_kzalloc(dev, size_of(sysdata), GFP_KERNEL);
	if (!sysdata)
		return ERR_PTR(-ENOMEM);
	sysdata->dev = dev;

	// gpu address area
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	sysdata->rgx_start = res->start;
	sysdata->rgx_size = resource_size(res);

	// ioremapped register address area
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	sysdata->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(sysdata->reg_base))
		return PTR_ERR(sysdata->reg_base);

	sysdata->rgx_irq = platform_get_irq_byname(pdev, "RGX");
	if (sysdata->rgx_irq < 0)
		return sysdata->rgx_irq;

	// dma_ret == 1: no dma
	int dma_ret = of_parse_phandle_with_args(dev->of_node, "dmas", "#dma-cells", 0, &args);
	sysdata->has_dma = (dma_ret != 1);

	// TODO: clk

	return 0;
}

static PVRSRV_ERROR DeviceConfigCreate(void *pvOSDevice,
					struct sg2380_sysdata *sysdata,
					PVRSRV_DEVICE_CONFIG **ppsDevConfigOut)
{
	PVRSRV_DEVICE_CONFIG *psDevConfig;
	RGX_DATA *psRGXData;
	RGX_TIMING_INFORMATION *psRGXTimingInfo;
	PHYS_HEAP_CONFIG *psPhysHeapConfig;

	psDevConfig = OSAllocZMem(sizeof(*psDevConfig) +
				  sizeof(*psRGXData) +
				  sizeof(*psRGXTimingInfo) +
				  sizeof(*psPhysHeapConfig));
	if (!psDevConfig)
		return PVRSRV_ERROR_OUT_OF_MEMORY;

	psRGXData = (RGX_DATA *)((IMG_CHAR *)psDevConfig + sizeof(*psDevConfig));
	psRGXTimingInfo = (RGX_TIMING_INFORMATION *)((IMG_CHAR *)psRGXData + sizeof(*psRGXData));
	psPhysHeapConfig = (PHYS_HEAP_CONFIG *)((IMG_CHAR *)psRGXTimingInfo + sizeof(*psRGXTimingInfo));

	/* Set up the RGX timing information */
	psRGXTimingInfo->ui32CoreClockSpeed = RGX_HW_CORE_CLOCK_SPEED;
	psRGXTimingInfo->bEnableActivePM = IMG_TRUE;
	psRGXTimingInfo->bEnableRDPowIsland = IMG_TRUE;
	psRGXTimingInfo->ui32ActivePMLatencyms = SYS_RGX_ACTIVE_POWER_LATENCY_MS;

	/* Set up the RGX data */
	psRGXData->psRGXTimingInfo = psRGXTimingInfo;

	psPhysHeapConfig->eType = PHYS_HEAP_TYPE_UMA;
	psPhysHeapConfig->ui32UsageFlags = PHYS_HEAP_USAGE_GPU_LOCAL;
	psPhysHeapConfig->uConfig.sUMA.pszPDumpMemspaceName = "SYSMEM";
	psPhysHeapConfig->uConfig.sUMA.psMemFuncs = &gsPhysHeapFuncs;
	psPhysHeapConfig->uConfig.sUMA.pszHeapName = "uma_gpu_local";
	psPhysHeapConfig->uConfig.sUMA.hPrivData = NULL;

	psDevConfig->pasPhysHeaps = psPhysHeapConfig;
	psDevConfig->ui32PhysHeapCount = 1U;

	psDevConfig->pvOSDevice = pvOSDevice;
	psDevConfig->pszName = SG2380_SYSTEM_NAME;
	psDevConfig->pszVersion = NULL;

	psDevConfig->eDefaultHeap = PVRSRV_PHYS_HEAP_GPU_LOCAL;

	psDevConfig->bHasFBCDCVersion31 = IMG_FALSE;
	psDevConfig->bDevicePA0IsValid = IMG_FALSE;

	psDevConfig->hDevData = psRGXData;
	psDevConfig->hSysData = (IMG_HANDLE) sysdata;
	ghSysData = psDevConfig->hSysData;

	psDevConfig->pfnSysDevFeatureDepInit = NULL;

	psDevConfig->ui32IRQ = sysdata->rgx_irq;

	psDevConfig->sRegsCpuPBase.uiAddr = sysdata->rgx_start;
	psDevConfig->ui32RegsSize = sysdata->rgx_size;

	/* DMA channel config */
	if (sysdata->has_dma) {
		psDevConfig->pfnSlaveDMAGetChan = get_dma_chan;
		psDevConfig->pfnSlaveDMAFreeChan = free_dma_chan;
		psDevConfig->pfnDevPhysAddr2DmaAddr = NULL;
		psDevConfig->pszDmaTxChanName = "Tx";
		psDevConfig->pszDmaRxChanName = "Rx";
		psDevConfig->bHasDma = true;
	}


	/* power management on HW system */
	psDevConfig->pfnPrePowerState = NULL;
	psDevConfig->pfnPostPowerState = NULL;

	/* clock frequency */
	psDevConfig->pfnClockFreqGet = NULL;

	/* device error notify callback function */
	psDevConfig->pfnSysDevErrorNotify = NULL;

	*ppsDevConfigOut = psDevConfig;

	return PVRSRV_OK;
}

static void DeviceConfigDestroy(PVRSRV_DEVICE_CONFIG *psDevConfig)
{
	/*
	 * The device config, RGX data and RGX timing info are part of the same
	 * allocation so do only one free.
	 */
	OSFreeMem(psDevConfig);
}

PVRSRV_ERROR SysDevInit(void *pvOSDevice, PVRSRV_DEVICE_CONFIG **ppsDevConfig)
{
	PVRSRV_DEVICE_CONFIG *psDevConfig;
	struct device *dev = pvOSDevice;
	struct sg2380_sysdata *sysdata;
	PVRSRV_ERROR eError;

	sysdata = get_dev_resource(dev);
	if (IS_ERR(sysdata)) {
		if (PTR_ERR(sysdata) == -EPROBE_DEFER)
			return PVRSRV_ERROR_PROBE_DEFER;
		else
			return PVRSRV_ERROR_INIT_FAILURE;
	}

	// dma_set_mask


	eError = DeviceConfigCreate(pvOSDevice, sysdata, &psDevConfig);
	if (eError != PVRSRV_OK)
		return eError;

	*ppsDevConfig = psDevConfig;

	return PVRSRV_OK;
}

void SysDevDeInit(PVRSRV_DEVICE_CONFIG *psDevConfig)
{
	DeviceConfigDestroy(psDevConfig);
}

