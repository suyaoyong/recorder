#pragma once

#include <string>
#include <vector>
#include <wrl/client.h>
#include <mmdeviceapi.h>

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

class DeviceEnumerator {
public:
    DeviceEnumerator();
    std::vector<DeviceInfo> ListRenderDevices() const;
    Microsoft::WRL::ComPtr<IMMDevice> GetDeviceByIndex(size_t index) const;
    Microsoft::WRL::ComPtr<IMMDevice> GetDefaultRenderDevice() const;
    static std::wstring GetFriendlyName(IMMDevice* device);
private:
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
};
