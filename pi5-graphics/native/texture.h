#pragma once
#include <stdint.h>
#include "abi.h"
namespace pi5 {
uint32_t TextureBytes(uint32_t width,uint32_t height);
bool EncodeTexture(uint32_t width,uint32_t height,uint32_t pitch,const void *source,
                   void *destination,uint32_t capacity,uint32_t &bytes);
bool TextureDescriptors(uint32_t width,uint32_t height,uint32_t address,bool bgra,
                        bool linear,void *texture,void *sampler,bool opaque=false,bool constants=false,
                        uint32_t borderMask=0,const uint32_t *border=nullptr);
// 32bpp V3D mip chains. Level 0 is UIF at 'base'; smaller levels are stored
// below it in memory using the tiling the TMU derives from their size.
enum class Tiling : uint32_t {Linear,UbLinear1,UbLinear2,Uif};
struct TextureLevel {uint32_t offset,width,height,paddedWidth,paddedHeight;Tiling tiling;bool xorEnabled;};
struct TextureChain {uint32_t levels=0,bytes=0,base=0;TextureLevel level[PI5_MAX_LEVELS]={};};
bool LayoutTextureChain(uint32_t width,uint32_t height,uint32_t levels,TextureChain &out);
uint32_t TexelOffset(const TextureLevel &level,uint32_t x,uint32_t y);
bool EncodeTextureLevel(const TextureChain &chain,uint32_t level,const void *source,uint32_t pitch,void *destination,uint32_t capacity);
bool ChainDescriptor(const TextureChain &chain,uint32_t address,bool bgra,bool opaque,void *texture);
bool SamplerDescriptor(uint32_t filter,uint32_t addressModes,const uint32_t *border,uint32_t minLod,uint32_t maxLod,bool bgra,void *sampler);
// Raster 1D integer view; elements includes the zero pad element.
bool BufferDescriptors(uint32_t elements,uint32_t kind,uint32_t address,void *texture,void *sampler);
}
