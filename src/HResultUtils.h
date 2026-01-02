#pragma once

#include <windows.h>
#include <string>

std::wstring DescribeHRESULTW(HRESULT hr);
std::string DescribeHRESULTA(HRESULT hr);