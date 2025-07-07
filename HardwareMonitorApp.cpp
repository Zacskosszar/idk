#include <ntddk.h>
#include <wdf.h>
#include <intrin.h>
#include <pci.h>

#define DEVICE_NAME L"\\Device\\HardwareMonitor"
#define SYMBOLIC_NAME L"\\DosDevices\\HardwareMonitor"
#define DRIVER_TAG 'MHW'

#pragma pack(push, 1)
typedef struct _RAM_TIMINGS {
    UINT8 DDRVersion;
    UINT16 tCL;
    UINT16 tRCD;
    UINT16 tRP;
    UINT16 tRAS;
    UINT32 tRFC;
    UINT16 tFAW;
    UINT16 tRCDRD;
    UINT16 tRCDWR;
    float VDD;
    float VDDQ;
    float VPP;
} RAM_TIMINGS, *PRAM_TIMINGS;

typedef struct _SPD_DATA {
    UINT8 Data[512];       // SPD data buffer
    UINT8 Size;            // Actual SPD size
    UINT8 DIMMSlot;        // Slot number
    BOOLEAN Valid;         // Data validity
} SPD_DATA, *PSPD_DATA;
#pragma pack(pop)

// SMBus constants
#define SMBUS_IO_BASE 0x0400
#define SMBHSTSTAT 0
#define SMBHSTCTL  2
#define SMBHSTCMD  3
#define SMBHSTADD  4
#define SMBHSTDAT0 5
#define SMBHSTDAT1 6

// SMBus status bits
#define SMBHSTSTAT_BUSY   (1 << 0)
#define SMBHSTSTAT_INTR   (1 << 1)
#define SMBHSTSTAT_ERROR  (1 << 2)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DeviceControl;
NTSTATUS ReadPhysicalMemory(ULONG_PTR physAddr, PVOID buffer, SIZE_T size);
void ReadIntelTimings(PRAM_TIMINGS timings);
void ReadAMDTimings(PRAM_TIMINGS timings);
BOOLEAN SmbusReadSPD(UINT8 dimmAddr, UINT8 offset, UINT8* data);
void ReadAllSPD(PSPD_DATA spdData);

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    
    WDF_DRIVER_CONFIG_INIT(&config, DeviceAdd);
    config.DriverPoolTag = DRIVER_TAG;
    
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    return status;
}

NTSTATUS DeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    
    NTSTATUS status;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    DECLARE_CONST_UNICODE_STRING(deviceName, DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicName, SYMBOLIC_NAME);
    
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) return status;
    
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) return status;
    
    status = WdfDeviceCreateSymbolicLink(device, &symbolicName);
    if (!NT_SUCCESS(status)) return status;
    
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = DeviceControl;
    
    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

NTSTATUS ReadPhysicalMemory(ULONG_PTR physAddr, PVOID buffer, SIZE_T size)
{
    PHYSICAL_ADDRESS phys;
    phys.QuadPart = physAddr;
    
    PVOID va = MmMapIoSpace(phys, size, MmNonCached);
    if (!va) return STATUS_UNSUCCESSFUL;
    
    RtlCopyMemory(buffer, va, size);
    MmUnmapIoSpace(va, size);
    
    return STATUS_SUCCESS;
}

BOOLEAN SmbusReadSPD(UINT8 dimmAddr, UINT8 offset, UINT8* data)
{
    // Find SMBus controller base address
    ULONG smbusBase = 0;
    ReadPhysicalMemory(SMBUS_IO_BASE, &smbusBase, sizeof(smbusBase));
    smbusBase &= 0xFFFC;  // Mask to get base address
    
    if (smbusBase == 0) return FALSE;
    
    // Wait until SMBus is not busy
    UINT8 status;
    int timeout = 1000;
    do {
        READ_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTSTAT));
        KeStallExecutionProcessor(10);
    } while (status & SMBHSTSTAT_BUSY && --timeout > 0);
    
    if (timeout <= 0) return FALSE;
    
    // Clear status
    WRITE_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTSTAT), SMBHSTSTAT_INTR | SMBHSTSTAT_ERROR);
    
    // Set device address and read command
    WRITE_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTADD), (dimmAddr << 1) | 0x01);
    WRITE_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTCMD), offset);
    
    // Start block read
    WRITE_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTCTL), 0x0C);  // Block read
    
    // Wait for completion
    timeout = 1000;
    do {
        status = READ_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTSTAT));
        KeStallExecutionProcessor(10);
    } while (!(status & (SMBHSTSTAT_INTR | SMBHSTSTAT_ERROR)) && --timeout > 0);
    
    if (!(status & SMBHSTSTAT_INTR) || (status & SMBHSTSTAT_ERROR)) {
        return FALSE;
    }
    
    // Read data
    *data = READ_PORT_UCHAR((PUCHAR)(smbusBase + SMBHSTDAT0));
    return TRUE;
}

void ReadAllSPD(PSPD_DATA spdData)
{
    // Standard SPD addresses for DDR4/DDR5
    UINT8 spdAddresses[] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57};
    
    for (int i = 0; i < sizeof(spdAddresses); i++) {
        SPD_DATA dimmData = {0};
        dimmData.DIMMSlot = i;
        
        // Read SPD header to determine size
        UINT8 spdHeader[4] = {0};
        if (SmbusReadSPD(spdAddresses[i], 0, &spdHeader[0]) &&
            SmbusReadSPD(spdAddresses[i], 1, &spdHeader[1]) &&
            SmbusReadSPD(spdAddresses[i], 2, &spdHeader[2]) &&
            SmbusReadSPD(spdAddresses[i], 3, &spdHeader[3])) {
            
            // Determine SPD size based on DDR type
            dimmData.Size = (spdHeader[2] == 0x0C) ? 512 : 256;  // DDR4/5:512, DDR3:256
            
            // Read full SPD data
            BOOLEAN success = TRUE;
            for (int j = 0; j < dimmData.Size; j++) {
                if (!SmbusReadSPD(spdAddresses[i], j, &dimmData.Data[j])) {
                    success = FALSE;
                    break;
                }
            }
            
            dimmData.Valid = success;
        }
        
        spdData[i] = dimmData;
    }
}

void ReadIntelTimings(PRAM_TIMINGS timings) { /* Implementation same as before */ }
void ReadAMDTimings(PRAM_TIMINGS timings) { /* Implementation same as before */ }

VOID DeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(Queue);
    
    NTSTATUS status = STATUS_SUCCESS;
    size_t length = 0;
    PVOID outputBuffer;
    
    switch (IoControlCode) {
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS): {
            // RAM timings request
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(RAM_TIMINGS), &outputBuffer, &length);
            if (!NT_SUCCESS(status) || length < sizeof(RAM_TIMINGS)) {
                status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            
            PRAM_TIMINGS timings = static_cast<PRAM_TIMINGS>(outputBuffer);
            int cpuInfo[4];
            __cpuid(cpuInfo, 0);
            char vendor[13] = {0};
            *(int*)&vendor[0] = cpuInfo[1];
            *(int*)&vendor[4] = cpuInfo[3];
            *(int*)&vendor[8] = cpuInfo[2];
            
            if (strncmp(vendor, "GenuineIntel", 12) == 0) {
                ReadIntelTimings(timings);
            } else if (strncmp(vendor, "AuthenticAMD", 12) == 0) {
                ReadAMDTimings(timings);
            } else {
                status = STATUS_NOT_SUPPORTED;
            }
            
            if (NT_SUCCESS(status)) {
                WdfRequestSetInformation(Request, sizeof(RAM_TIMINGS));
            }
            break;
        }
        
        case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS): {
            // SPD data request
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SPD_DATA) * 8, &outputBuffer, &length);
            if (!NT_SUCCESS(status) || length < sizeof(SPD_DATA) * 8) {
                status = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            
            PSPD_DATA spdData = static_cast<PSPD_DATA>(outputBuffer);
            ReadAllSPD(spdData);
            WdfRequestSetInformation(Request, sizeof(SPD_DATA) * 8);
            break;
        }
        
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
    
    WdfRequestComplete(Request, status);
}