#include "HResultUtils.h"

#include <iomanip>
#include <sstream>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace {
std::string NarrowFromWide(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) {
        throw std::runtime_error("WideCharToMultiByte 失败");
    }
    std::string result(sizeNeeded, '\0');
    int converted = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                        static_cast<int>(input.size()),
                                        result.data(), sizeNeeded, nullptr, nullptr);
    if (converted != sizeNeeded) {
        throw std::runtime_error("WideCharToMultiByte 写入长度异常");
    }
    return result;
}
}

std::wstring DescribeHRESULTW(HRESULT hr) {
    wchar_t* buffer = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message;
    if (size && buffer) {
        message.assign(buffer, size);
        LocalFree(buffer);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    std::wstringstream ss;
    ss << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
       << static_cast<uint32_t>(hr);
    if (!message.empty()) {
        ss << L" (" << message << L")";
    }
    return ss.str();
}

std::string DescribeHRESULTA(HRESULT hr) {
    return NarrowFromWide(DescribeHRESULTW(hr));
}
