#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include "Shaders.h"
#include "DepthEffects.h"

uint64_t Hash(const char* source) {
    uint64_t value = 1469598103934665603ull;
    while (*source) value = (value ^ static_cast<unsigned char>(*source++)) * 1099511628211ull;
    return value;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    struct Program { const char* source; const char* entry; const char* profile; };
    Program programs[]{
        {PhotoShader,"VS","vs_5_0"}, {PhotoShader,"PS","ps_5_0"}, {PhotoShader,"OpaqueDepthPS","ps_5_0"},
        {LineShader,"LineVS","vs_5_0"}, {LineShader,"LinePS","ps_5_0"},
        {GlassShader,"FullVS","vs_5_0"}, {GlassShader,"DownsamplePS","ps_5_0"},
        {GlassShader,"BlurPS","ps_5_0"}, {GlassShader,"CompositePS","ps_5_0"},
        {DepthShader,"DownPS","ps_5_0"}, {DepthShader,"BlurPS","ps_5_0"}, {DepthShader,"ResolvePS","ps_5_0"}
    };
    std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary);
    if (!output) return 1;
    output << "#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace nebula_compiled {\n";
    for (size_t i = 0; i < std::size(programs); ++i) {
        const auto& program = programs[i];
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG;
#endif
        HRESULT hr = D3DCompile(program.source, strlen(program.source), program.entry, nullptr, nullptr,
            program.entry, program.profile, flags, 0, &code, &errors);
        if (FAILED(hr)) {
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return 1;
        }
        output << "inline constexpr unsigned char shader" << i << "[]={";
        auto* bytes = static_cast<const unsigned char*>(code->GetBufferPointer());
        for (size_t j = 0; j < code->GetBufferSize(); ++j) {
            if (j % 32 == 0) output << '\n';
            output << static_cast<unsigned>(bytes[j]) << ',';
        }
        output << "};\n";
    }
    output << "struct Program { uint64_t hash; const char* entry; const char* profile; const unsigned char* data; size_t size; };\n";
    output << "inline constexpr Program programs[]={\n";
    for (size_t i = 0; i < std::size(programs); ++i)
        output << '{' << Hash(programs[i].source) << "ull,\"" << programs[i].entry << "\",\"" << programs[i].profile
               << "\",shader" << i << ",sizeof(shader" << i << ")},\n";
    output << "};\n}\n";
    return output.good() ? 0 : 1;
}
