#include <windows.h>
#include <iostream>
#include <intrin.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>

#pragma comment(lib, "wbemuuid.lib")

// Matches driver structures
struct RAM_TIMINGS {
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
};

struct SPD_DATA {
    UINT8 Data[512];
    UINT8 Size;
    UINT8 DIMMSlot;
    BOOLEAN Valid;
};

// Function prototypes
void GetCPUInfo(std::string& cpuBrand, std::string& vendor);
void GetGPUInfo(std::wstring& gpuName, std::wstring& driverVersion);
bool GetRAMTimings(RAM_TIMINGS& timings);
bool GetSPDData(std::vector<SPD_DATA>& spdData);
void CalculateOptimizedTimings(RAM_TIMINGS& optimized);
void PrintTimings(const RAM_TIMINGS& timings, bool optimized = false);
void PrintSafetyWarnings(const RAM_TIMINGS& current, const RAM_TIMINGS& optimized);
void DecodeSPD(const SPD_DATA& spd);
void PrintSPDSummary(const SPD_DATA& spd);

int main() {
    std::cout << "=== Hardware Monitor & RAM Optimizer ===" << std::endl;
    
    // Get CPU information
    std::string cpuBrand, cpuVendor;
    GetCPUInfo(cpuBrand, cpuVendor);
    std::cout << "\nCPU: " << cpuBrand << " (" << cpuVendor << ")" << std::endl;
    
    // Get GPU information
    std::wstring gpuName, driverVersion;
    GetGPUInfo(gpuName, driverVersion);
    std::wcout << L"GPU: " << gpuName << std::endl;
    std::wcout << L"Driver: " << driverVersion << std::endl;
    
    // Get RAM timings
    RAM_TIMINGS currentTimings = {0};
    if (GetRAMTimings(currentTimings)) {
        std::cout << "\n=== Current RAM Timings ===" << std::endl;
        PrintTimings(currentTimings);
        
        RAM_TIMINGS optimized = currentTimings;
        CalculateOptimizedTimings(optimized);
        
        std::cout << "\n=== Optimized RAM Timings ===" << std::endl;
        PrintTimings(optimized, true);
        
        std::cout << "\n=== Safety Validation ===" << std::endl;
        PrintSafetyWarnings(currentTimings, optimized);
    } else {
        std::cerr << "\nError: Failed to retrieve RAM timings" << std::endl;
    }
    
    // Get SPD data
    std::vector<SPD_DATA> spdData(8);
    if (GetSPDData(spdData)) {
        std::cout << "\n=== SPD Information ===" << std::endl;
        for (const auto& spd : spdData) {
            if (spd.Valid) {
                PrintSPDSummary(spd);
            }
        }
    } else {
        std::cerr << "\nError: Failed to retrieve SPD data" << std::endl;
    }
    
    return 0;
}

// Existing functions (GetCPUInfo, GetGPUInfo, GetRAMTimings, CalculateOptimizedTimings, 
// PrintTimings, PrintSafetyWarnings) remain the same as previous implementation

bool GetSPDData(std::vector<SPD_DATA>& spdData) {
    HANDLE hDevice = CreateFileW(L"\\\\.\\HardwareMonitor", GENERIC_READ | GENERIC_WRITE, 
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) return false;

    DWORD bytesReturned = 0;
    BOOL success = DeviceIoControl(hDevice, CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, 
                                 FILE_ANY_ACCESS), nullptr, 0, spdData.data(), 
                                 sizeof(SPD_DATA) * 8, &bytesReturned, nullptr);
    CloseHandle(hDevice);

    return success && (bytesReturned == sizeof(SPD_DATA) * 8);
}

void PrintSPDSummary(const SPD_DATA& spd) {
    std::cout << "\nDIMM Slot: " << static_cast<int>(spd.DIMMSlot) << std::endl;
    
    // DDR type
    UINT8 ddrType = spd.Data[2];
    std::cout << "DDR Type: ";
    switch (ddrType) {
        case 0x0C: std::cout << "DDR4"; break;
        case 0x12: std::cout << "DDR5"; break;
        default: std::cout << "Unknown (0x" << std::hex << static_cast<int>(ddrType) << ")";
    }
    std::cout << std::dec << std::endl;
    
    // Module size
    UINT8 banks = spd.Data[4] & 0x07;
    UINT8 density = spd.Data[4] >> 3;
    UINT64 sizeMB = (1 << density) * (banks + 1) * 256;
    std::cout << "Size: " << sizeMB << " MB" << std::endl;
    
    // Manufacturer
    std::cout << "Manufacturer: ";
    if (spd.Data[320] != 0) {
        std::cout << "JEDEC ID: " << static_cast<int>(spd.Data[320]) << "-"
                  << static_cast<int>(spd.Data[321]);
    } else {
        std::cout << "Not specified";
    }
    std::cout << std::endl;
    
    // Timings
    UINT16 tCL = spd.Data[18] | (spd.Data[19] << 8);
    UINT16 tRCD = spd.Data[20] | (spd.Data[21] << 8);
    UINT16 tRP = spd.Data[22] | (spd.Data[23] << 8);
    UINT16 tRAS = spd.Data[24] | (spd.Data[25] << 8);
    
    std::cout << "SPD Timings: tCL=" << tCL << " tRCD=" << tRCD 
              << " tRP=" << tRP << " tRAS=" << tRAS << std::endl;
    
    // Save SPD to file
    std::string filename = "dimm" + std::to_string(spd.DIMMSlot) + ".spd";
    std::ofstream file(filename, std::ios::binary);
    if (file) {
        file.write(reinterpret_cast<const char*>(spd.Data), spd.Size);
        std::cout << "SPD saved to " << filename << std::endl;
    }
}

void DecodeSPD(const SPD_DATA& spd) {
    std::cout << "\nDetailed SPD Data for DIMM " << static_cast<int>(spd.DIMMSlot) << ":\n";
    
    // Print SPD in hex format
    for (int i = 0; i < spd.Size; i += 16) {
        printf("%03X: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < spd.Size) {
                printf("%02X ", spd.Data[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" ");
        for (int j = 0; j < 16; j++) {
            if (i + j < spd.Size) {
                char c = spd.Data[i + j];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
        }
        printf("\n");
    }
}