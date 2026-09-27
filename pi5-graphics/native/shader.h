#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <array>

namespace pi5 {
constexpr unsigned ShaderRegisters = 32;
using ShaderSignature = std::array<uint32_t,ShaderRegisters>;
enum class UniformKind : uint32_t { Literal, ConstantBuffer, Viewport, VertexAttribute, FragmentVarying, TextureConfig, SamplerConfig, ConstantTextureConfig, ConstantSamplerConfig,
                                    BindingElements, BindingWidth, BindingHeight, BlendConstant, TargetColor, FloatConstantBuffer };
enum class ShaderStage : uint32_t { Pixel, Coordinate, Vertex };
struct Uniform {
    UniformKind kind;
    uint32_t value;
    uint32_t slot;
};
// A pixel-shader resource/sampler pair. Texel fetches have no sampler.
struct ShaderBinding {
    uint32_t resource, sampler;
    bool buffer;
    bool signedBuffer;
};
constexpr uint32_t NoSampler = UINT32_MAX;
struct Shader {
    std::vector<uint64_t> code;
    std::vector<Uniform> uniforms;
    uint32_t instructions = 0;
    ShaderSignature inputs = {};
    // D3D system-generated scalar inputs: 6 = VertexID, 8 = InstanceID.
    // These occupy the same packed attribute slots as ordinary inputs.
    std::array<std::array<uint32_t,4>,ShaderRegisters> systemInputs = {};
    uint32_t outputScalars = 0;
    uint32_t varyingScalars = 0,nonPerspectiveMask=0,flatMask=0;
    std::vector<ShaderBinding> bindings;
    uint32_t constantSlot=UINT32_MAX,constantWords=0,constantDeclaredWords=0;
    // Only these words influence the specialized instruction stream. Other
    // constants remain uniforms and can change without recompiling the shader.
    std::vector<Uniform> specialized;
};
struct ShaderBlend {
    // D3D blend enums: source/destination/operation for RGB, then alpha.
    // Zero operation selects the fixed-function blend path.
    std::array<uint32_t,6> functions={};
    bool opaqueTarget=false;
};
// An immutable snapshot for specializing uniform control flow at draw time.
// Empty or unbound buffers read as zero, as in the regular uniform upload path.
using ConstantBuffers = std::array<std::vector<uint32_t>,14>;
bool CompilePixelShader(const void *dxbc, size_t bytes, Shader &out, std::string &error);
bool CompileShader(const void *dxbc, size_t bytes, ShaderStage stage, Shader &out, std::string &error,
                   const ShaderSignature *linkedVaryings=nullptr);
bool CompileShaderTokens(const uint32_t *tokens, size_t words, ShaderStage stage, Shader &out, std::string &error,
                         const ShaderSignature *linkedVaryings=nullptr,const ConstantBuffers *constants=nullptr,const ShaderBlend *blend=nullptr);
}
