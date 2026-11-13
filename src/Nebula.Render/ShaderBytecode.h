#pragma once
#include "CompiledShaders.h"

inline HRESULT CompileShaderBytes(const void* source, SIZE_T length, LPCSTR, const D3D_SHADER_MACRO*,
    ID3DInclude*, LPCSTR entry, LPCSTR profile, UINT, UINT, ID3DBlob** output, ID3DBlob** errors) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (errors) *errors = nullptr;
    uint64_t hash = 1469598103934665603ull;
    auto* bytes = static_cast<const unsigned char*>(source);
    for (SIZE_T i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    for (const auto& program : nebula_compiled::programs) {
        if (program.hash != hash || strcmp(program.entry, entry) || strcmp(program.profile, profile)) continue;
        HRESULT hr = D3DCreateBlob(program.size, output);
        if (SUCCEEDED(hr)) memcpy((*output)->GetBufferPointer(), program.data, program.size);
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}
