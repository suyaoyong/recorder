#include "DeviceEnumerator.h"
#include "HResultUtils.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <vector>
#include <stdexcept>
#include <memory>
#include <utility>

using Microsoft::WRL::ComPtr;

DeviceEnumerator::DeviceEnumerator() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create MMDeviceEnumerator: " + DescribeHRESULTA(hr));
    }
}

std::vector<DeviceInfo> DeviceEnumerator::ListRenderDevices() const {
    std::vector<DeviceInfo> devices;
    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        throw std::runtime_error("EnumAudioEndpoints failed: " + DescribeHRESULTA(hr));
    }
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        throw std::runtime_error("IMMDeviceCollection::GetCount failed: " + DescribeHRESULTA(hr));
    }

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    HRESULT defaultHr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (SUCCEEDED(defaultHr) && defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) {
            throw std::runtime_error("IMMDeviceCollection::Item failed: " + DescribeHRESULTA(hr));
        }
        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.name = GetFriendlyName(device.Get());
        info.isDefault = (!defaultId.empty() && info.id == defaultId);
        devices.push_back(std::move(info));
    }
    return devices;
}

ComPtr<IMMDevice> DeviceEnumerator::GetDeviceByIndex(size_t index) const {
    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        throw std::runtime_error("EnumAudioEndpoints failed: " + DescribeHRESULTA(hr));
    }
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        throw std::runtime_error("IMMDeviceCollection::GetCount failed: " + DescribeHRESULTA(hr));
    }
    if (index >= count) {
        throw std::out_of_range("Device index out of range");
    }
    ComPtr<IMMDevice> device;
    hr = collection->Item(static_cast<UINT>(index), &device);
    if (FAILED(hr)) {
        throw std::runtime_error("IMMDeviceCollection::Item failed: " + DescribeHRESULTA(hr));
    }
    return device;
}

ComPtr<IMMDevice> DeviceEnumerator::GetDefaultRenderDevice() const {
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        throw std::runtime_error("GetDefaultAudioEndpoint failed: " + DescribeHRESULTA(hr));
    }
    return device;
}

std::wstring DeviceEnumerator::GetFriendlyName(IMMDevice* device) {
    if (!device) {
        return L"<Unknown>";
    }
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return L"<Unknown>";
    }
    PROPVARIANT varName;
    PropVariantInit(&varName);
    std::wstring name = L"<Unknown>";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR && varName.pwszVal) {
        name = varName.pwszVal;
    }
    PropVariantClear(&varName);
    return name;
}
