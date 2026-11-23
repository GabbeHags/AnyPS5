#include <cstdio>
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <chrono>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <array>
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

namespace {

void requireValidDstSel(std::uint32_t value) {
    Require(value == 0 || value == 1 || (value >= 4 && value <= 7), "guest texture descriptor has an invalid destination channel selector");
}

TextureTileMode resolveTileMode(std::uint32_t raw) {
    switch (raw) {
        case 0x00: return TextureTileMode::kLinear;
        case 0x01: return TextureTileMode::kStandard256B;
        case 0x02: return TextureTileMode::kD256B;
        case 0x05: return TextureTileMode::kStandard4KB;
        case 0x06: return TextureTileMode::kD4KB;
        case 0x09: return TextureTileMode::kStandard64KB;
        case 0x0a: return TextureTileMode::kD64KB;
        case 0x11: return TextureTileMode::kS64KBT;
        case 0x12: return TextureTileMode::kD64KBT;
        case 0x15: return TextureTileMode::kS4KBX;
        case 0x16: return TextureTileMode::kD4KBX;
        case 0x18: return TextureTileMode::kZ64KBX;
        case 0x19: return TextureTileMode::kS64KBX;
        case 0x1a: return TextureTileMode::kD64KBX;
        case 0x1b: return TextureTileMode::kR64KBX;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor uses an unsupported tile mode " + std::to_string(raw));
    }
}

TextureTileMode resolveGcnTileMode(std::uint32_t index) {
    switch (index) {
        case 0u: return TextureTileMode::kGcnDepth2D64;
        case 1u: return TextureTileMode::kGcnDepth2D128;
        case 2u: return TextureTileMode::kGcnDepth2D256;
        case 3u: return TextureTileMode::kGcnDepth2D512;
        case 4u: return TextureTileMode::kGcnDepth2D1K;
        case 6u: return TextureTileMode::kGcnDepthPrt256;
        case 7u: return TextureTileMode::kGcnDepthPrt1K;
        case 8u: case 31u: return TextureTileMode::kLinear;
        case 11u: return TextureTileMode::kGcnDisplayPrt;
        case 12u: return TextureTileMode::kGcnDisplay2DPrt;
        case 16u: return TextureTileMode::kGcnThinPrt;
        case 17u: return TextureTileMode::kGcnThin2DPrt;
        case 5u: case 13u: return TextureTileMode::kGcnThin1D;
        case 9u: return TextureTileMode::kGcnDisplay1D;
        case 10u: return TextureTileMode::kGcnDisplay2D;
        case 14u: return TextureTileMode::kGcnThin2D;
        case 19u: return TextureTileMode::kGcnThick1D;
        default: throw std::runtime_error("AGC graphics: PS4 texture tile mode index " + std::to_string(index) + " (3D or thick) is not implemented");
    }
}

TextureTileMode resolveDescriptorTileMode(std::span<const std::uint32_t> words, bool gcn) {
    return gcn ? resolveGcnTileMode(ShaderRecompiler::GcnImageTilingIndex(words[7])) : resolveTileMode((words[3] >> 20u) & 0x1fu);
}

std::uint64_t descriptorBaseAddress(std::span<const std::uint32_t> words) {
    Require(words.size() == 8, "guest texture descriptor must contain 8 dwords");
    const auto base = (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32u)) & (ShaderRecompiler::GcnGuestShaders() ? 0x3fffffffffull : 0xffffffffffull);
    return base << 8u;
}

std::uint64_t normalizedDescriptorBaseAddress(std::uint64_t baseAddress, TextureTileMode tileMode, std::uint32_t* pipeBankXor) {
    auto xorBytes = 0u;
    if (XorSwizzleMode(tileMode) != 0) {
        xorBytes = static_cast<std::uint32_t>(baseAddress & (tileMode == TextureTileMode::kS4KBX || tileMode == TextureTileMode::kD4KBX ? 0xfffu : 0xffffu));
        baseAddress -= xorBytes;
    }
    if (pipeBankXor != nullptr) *pipeBankXor = xorBytes;
    return baseAddress;
}

TextureDimension resolveDimension(std::uint32_t raw) {
    switch (raw) {
        case 8: return TextureDimension::k1D;
        case 9: return TextureDimension::k2D;
        case 10: return TextureDimension::k3D;
        case 11: return TextureDimension::kCube;
        case 12: return TextureDimension::k1DArray;
        case 13: return TextureDimension::k2DArray;
        case 14: return TextureDimension::k2DMsaa;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor uses an unsupported image type " + std::to_string(raw));
    }
}

bool ZeroTail(std::span<const std::uint32_t> words) {
    return words[4] == 0u && words[5] == 0u && words[6] == 0u && words[7] == 0u;
}

bool SameLevels(const TileMipLayout& a, const TileMipLayout& b) {
    return a.tiledOffset == b.tiledOffset && a.tiledSize == b.tiledSize && a.width == b.width && a.height == b.height && a.blocksPerRow == b.blocksPerRow && a.pitchBytes == b.pitchBytes && a.tail == b.tail && a.tailX == b.tailX && a.tailY == b.tailY;
}

std::optional<std::array<std::uint32_t, 8>> R128ResidentLevelWords(std::span<const std::uint32_t> words) {
    const auto baseLevel = (words[3] >> 12u) & 0xfu;
    const auto lastLevel = (words[3] >> 16u) & 0xfu;
    const auto typeRaw = (words[3] >> 28u) & 0xfu;
    if (baseLevel == 0u || baseLevel > lastLevel || (typeRaw != 8u && typeRaw != 9u)) return std::nullopt;
    const std::array<std::uint32_t, 8> chainWords{words[0], words[1], words[2], words[3], 0u, lastLevel << 4u, 0u, 0u};
    const auto width = (((words[1] >> 30u) & 0x3u) | ((words[2] & 0xfffu) << 2u)) + 1u;
    const auto height = ((words[2] >> 14u) & 0x3fffu) + 1u;
    const auto minLod = (words[1] >> 8u) & 0xfffu;
    const auto widthMinus1 = std::max(width >> baseLevel, 1u) - 1u;
    const auto heightMinus1 = std::max(height >> baseLevel, 1u) - 1u;
    const auto rebasedMinLod = minLod > baseLevel * 256u ? minLod - baseLevel * 256u : 0u;
    const auto levels = lastLevel - baseLevel;
    auto rebased = chainWords;
    rebased[1] = (words[1] & ~((0xfffu << 8u) | (0x3u << 30u))) | (rebasedMinLod << 8u) | ((widthMinus1 & 0x3u) << 30u);
    rebased[2] = (words[2] & ~(0xfffu | (0x3fffu << 14u))) | ((widthMinus1 >> 2u) & 0xfffu) | ((heightMinus1 & 0x3fffu) << 14u);
    rebased[3] = (words[3] & ~(0xffu << 12u)) | (levels << 16u);
    rebased[5] = levels << 4u;
    try {
        const auto chain = DescribeSurface(DecodeTextureResource(chainWords));
        const auto view = DescribeSurface(DecodeTextureResource(rebased));
        if (chain.mips.size() != lastLevel + 1u || view.mips.size() != levels + 1u || chain.layers != view.layers) return std::nullopt;
        for (std::uint32_t level = baseLevel; level <= lastLevel; ++level) {
            if (!SameLevels(chain.mips[level], view.mips[level - baseLevel])) return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return rebased;
}

std::uint64_t descriptorBase(std::span<const std::uint32_t> words) {
    return (static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32u)) & (ShaderRecompiler::GcnGuestShaders() ? 0x3fffffffffull : 0xffffffffffull);
}

}

std::optional<std::uint64_t> TextureResourceAllocationAddress(std::span<const std::uint32_t> words) {
    const auto baseAddress = descriptorBaseAddress(words);
    if (baseAddress == 0) return std::nullopt;
    const bool gcn = ShaderRecompiler::GcnGuestShaders();
    TextureTileMode tileMode;
    try {
        tileMode = resolveDescriptorTileMode(words, gcn);
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    const auto normalized = normalizedDescriptorBaseAddress(baseAddress, tileMode, nullptr);
    if (normalized == 0) return std::nullopt;
    return normalized;
}

bool IsNullTextureResource(std::span<const std::uint32_t> words) {
    Require(words.size() == 8, "guest texture descriptor must contain 8 dwords");
    const auto type = (words[3] >> 28u) & 0xfu;
    return descriptorBase(words) == 0 || type <= 1;
}

bool IsNullSampledTextureResource(std::span<const std::uint32_t> words) {
    if (IsNullTextureResource(words)) return true;
    const auto type = static_cast<ShaderRecompiler::ImageType>((words[3] >> 28u) & 0xfu);
    if (type < ShaderRecompiler::ImageType::Color1D || type > ShaderRecompiler::ImageType::Color2DMsaa) return false;
    const auto format = (words[1] >> 20u) & 0x1ffu;
    return format == 0u || format == 0x1ffu;
}

GuestTextureResource DecodeTextureResource(std::span<const std::uint32_t> descriptor) {
    Require(descriptor.size() == 8, "guest texture descriptor must contain 8 dwords");
    const bool gcn = ShaderRecompiler::GcnGuestShaders();
    std::array<std::uint32_t, 8> local{};
    std::copy(descriptor.begin(), descriptor.end(), local.begin());
    const auto gcnTiling = gcn ? local[7] : 0u;
    if (gcn) local[6] = local[7] = 0u;
    const std::span<const std::uint32_t> words(local);

    auto baseAddress = descriptorBase(words) << 8u;
    Require(baseAddress != 0, "guest texture descriptor has a null base address");

    const auto minLod = (words[1] >> 8u) & 0xfffu;
    const auto format = (words[1] >> 20u) & 0x1ffu;
    const auto width = (((words[1] >> 30u) & 0x3u) | (((words[2] >> 0u) & 0xfffu) << 2u)) + 1u;
    const auto height = ((words[2] >> 14u) & 0x3fffu) + 1u;

    const auto dstSelX = (words[3] >> 0u) & 0x7u;
    const auto dstSelY = (words[3] >> 3u) & 0x7u;
    const auto dstSelZ = (words[3] >> 6u) & 0x7u;
    const auto dstSelW = (words[3] >> 9u) & 0x7u;
    const auto baseLevel = (words[3] >> 12u) & 0xfu;
    auto lastLevel = (words[3] >> 16u) & 0xfu;
    const auto tileModeRaw = (words[3] >> 20u) & 0x1fu;
    const auto bcSwizzle = (words[3] >> 25u) & 0x7u;
    const auto typeRaw = (words[3] >> 28u) & 0xfu;

    auto depth = (words[4] >> 0u) & 0x1fffu;
    const auto baseArray = (words[4] >> 16u) & 0x1fffu;

    const auto arrayPitch = (words[5] >> 0u) & 0xfu;
    auto maxMip = (words[5] >> 4u) & 0xfu;
    const auto minLodWarn = (words[5] >> 8u) & 0xfffu;
    const auto cornerSample = ((words[5] >> 23u) & 0x1u) != 0;
    const auto mipStatsCntEn = ((words[5] >> 25u) & 0x1u) != 0;
    const auto prtDefColor = ((words[5] >> 26u) & 0x1u) != 0;

    const auto mipStatsCntId = words[6] & 0xffu;
    const auto iterate256 = ((words[6] >> 10u) & 0x1u) != 0;
    const auto maxUncompBlkSize = (words[6] >> 15u) & 0x3u;
    const auto maxCompBlkSize = (words[6] >> 17u) & 0x3u;
    const auto metaPipeAligned = ((words[6] >> 19u) & 0x1u) != 0;
    const auto writeCompress = ((words[6] >> 20u) & 0x1u) != 0;
    const auto metaCompress = ((words[6] >> 21u) & 0x1u) != 0;
    const auto dccAlphaPos = ((words[6] >> 22u) & 0x1u) != 0;
    const auto dccColorTransf = ((words[6] >> 23u) & 0x1u) != 0;
    const auto metaAddr = ((static_cast<std::uint64_t>(words[6]) >> 24u) & 0xffu) | (static_cast<std::uint64_t>(words[7]) << 8u);

    requireValidDstSel(dstSelX);
    requireValidDstSel(dstSelY);
    requireValidDstSel(dstSelZ);
    requireValidDstSel(dstSelW);

    // The LOD warning threshold and mip statistics counters are texture-streaming feedback: the GPU
    // reports which mips were wanted. Nothing is reported back here, which only affects what the title
    // chooses to stream, not what this access returns.
    if (minLodWarn != 0 || mipStatsCntId != 0 || mipStatsCntEn) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[gpu] texture streaming feedback (LOD warning / mip statistics) is not reported\n");
        }
    }
    Require(!cornerSample, "guest texture descriptor uses corner sampling which is not implemented");
    Require(!prtDefColor, "guest texture descriptor uses a partially resident default color which is not implemented");
    Require(arrayPitch == 0, "guest texture descriptor uses a nonzero array pitch which is not implemented");
    Require(!iterate256 || (typeRaw == 14u && tileModeRaw == 24u && (format == 7u || format == 22u)), "guest texture descriptor ITERATE_256 is implemented only for multisampled Z depth");
    static_cast<void>(maxUncompBlkSize);
    static_cast<void>(maxCompBlkSize);
    // Surfaces are always written uncompressed here (render targets and storage images bypass DCC), so
    // only the fast-clear keys in DCC metadata change what a read returns (see DccMetadata.hpp).
    static_cast<void>(writeCompress);
    if (dccColorTransf) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[gpu] texture DCC color transform is ignored (word6=0x%08x word7=0x%08x)\n", words[6], words[7]);
        }
    }
    Require(bcSwizzle <= 5u, "guest texture descriptor uses a reserved BC swizzle");

    Require(baseLevel <= lastLevel, "guest texture descriptor has a base mip level past its last mip level");

    const bool fmask = ShaderRecompiler::IsFmaskTextureFormat(static_cast<ShaderRecompiler::IrBufferFormat>(format));
    const auto fmaskLayout = ShaderRecompiler::FmaskLayoutOf(static_cast<ShaderRecompiler::IrBufferFormat>(format));
    if (fmask) {
        Require(fmaskLayout.has_value(), "guest FMASK format is not implemented");
        Require(!writeCompress && (!metaCompress || metaAddr != 0u), "FMASK CMASK compression is not implemented without readable metadata");
        Require(!metaCompress || metaPipeAligned, "FMASK CMASK metadata must be pipe aligned");
        Require(!metaCompress || format == static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Fmask32_S8_F8) || format == static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Fmask8_S2_F2), "FMASK CMASK compression is implemented only for 2-sample, 2-fragment and 8-sample, 8-fragment FMASK");
        Require((typeRaw == 9u || typeRaw == 13u) && tileModeRaw == 24u && baseLevel == 0u && lastLevel == 0u && maxMip == 0u, "FMASK reads require a single-mip 2D or 2D-array Z_64KB_X surface");
        const bool identity = format != static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Fmask32_S8_F8) && dstSelX == 4u && dstSelY == 5u && dstSelZ == 6u && dstSelW == 7u;
        for (const auto selector : {dstSelX, dstSelY, dstSelZ, dstSelW}) Require(identity || selector == 0u || selector == 1u || selector == 4u, "FMASK destination channel selector is not implemented");
    }

    const auto describe = [&] { return " (format " + std::to_string(format) + ", " + std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(depth + 1) + ", type " + std::to_string(typeRaw) + (gcn ? ", PS4 tile index " + std::to_string(ShaderRecompiler::GcnImageTilingIndex(gcnTiling)) : ", tile " + std::to_string(tileModeRaw)) + ", mips " + std::to_string(baseLevel) + ".." + std::to_string(lastLevel) + " of " + std::to_string(maxMip + 1) + ")"; };
    TextureTileMode tileMode;
    TextureDimension dimension;
    try {
        tileMode = resolveDescriptorTileMode(descriptor, gcn);
        dimension = resolveDimension(typeRaw);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + describe());
    }
    std::uint32_t samples = 1;
    if (dimension == TextureDimension::k2DMsaa && baseLevel == 0 && lastLevel == 0 && maxMip == 0) dimension = TextureDimension::k2D;
    if (dimension == TextureDimension::k2DMsaa) {
        Require(baseLevel == 0 && lastLevel >= 1u && lastLevel <= 4u && maxMip == lastLevel, "guest multisampled texture descriptor does not hold the same sample count of 2 to 16 in its last level and its last mip, which is not implemented" + describe());
        samples = 1u << lastLevel;
        lastLevel = 0;
        maxMip = 0;
    }
    // Views may name levels past MAX_MIP (a 512x512 view through 1x1 over a 9-level surface); the
    // hardware never addresses them, so the view ends at the surface's last level.
    if (baseLevel <= maxMip) lastLevel = std::min(lastLevel, maxMip);
    std::uint32_t pipeBankXor = 0;
    baseAddress = normalizedDescriptorBaseAddress(baseAddress, tileMode, &pipeBankXor);
    Require(baseAddress != 0, "guest texture descriptor has a null base address");

    std::uint32_t linearPitch = 0;
    switch (dimension) {
        case TextureDimension::k1D:
            Require(height == 1 && depth == 0 && baseArray == 0, "guest 1D texture descriptor has a nonzero height, depth or base array");
            break;
        case TextureDimension::k2D:
            if (depth != 0 && tileMode == TextureTileMode::kLinear) {
                Require(!IsBlockCompressed(format) && maxMip == 0 && baseArray == 0, "guest 2D linear texture descriptor sets a pitch on a block-compressed, mipmapped or base-array surface, which is not implemented" + describe());
                const auto elementsPerPitchUnit = 256u / BytesPerElement(format);
                const auto defaultPitch = (width + elementsPerPitchUnit - 1u) / elementsPerPitchUnit * elementsPerPitchUnit;
                Require(!ShaderRecompiler::GcnGuestShaders() || depth + 1u == defaultPitch, "guest 2D linear texture descriptor of a PS4 guest sets a custom pitch other than its default pitch, which is not implemented" + describe());
                const auto pitch = (std::max(depth + 1u, width) + elementsPerPitchUnit - 1u) / elementsPerPitchUnit * elementsPerPitchUnit;
                linearPitch = pitch == defaultPitch ? 0u : pitch;
                depth = 0;
            }
            Require(depth == 0 && baseArray == 0, "guest 2D texture descriptor has a nonzero depth or base array");
            break;
        case TextureDimension::k2DMsaa:
            Require(depth == 0 && baseArray == 0, "guest multisampled 2D texture descriptor has a nonzero depth or base array");
            break;
        case TextureDimension::k3D:
            // word4 DEPTH holds the depth minus one; BASE_ARRAY has no meaning for volumes.
            Require(baseArray == 0, "guest 3D texture descriptor has a nonzero base array");
            break;
        case TextureDimension::k2DArray:
            Require(baseArray <= depth, "guest 2D array texture descriptor has a base array past its last array slice");
            break;
        case TextureDimension::k1DArray:
            Require(height == 1, "guest 1D array texture descriptor has a nonzero height");
            Require(baseArray <= depth, "guest 1D array texture descriptor has a base array past its last array slice");
            Require(gcn || tileMode == TextureTileMode::kLinear || tileMode == TextureTileMode::kZ64KBX || tileMode == TextureTileMode::kR64KBX, "guest 1D array texture descriptor uses a tile mode other than linear, Z or R, which 1D resources cannot use" + describe());
            break;
        case TextureDimension::kCube:
            Require(width == height, "guest cube texture descriptor is not square");
            Require(baseArray <= depth, "guest cube texture descriptor has a base array past its last array slice");
            if (depth - baseArray < 5u) depth = baseArray + 5u;
            break;
    }

    GuestTextureResource result{};
    result.baseAddress = baseAddress;
    result.pipeBankXor = pipeBankXor;
    result.width = width;
    result.height = height;
    result.depthOrLastArray = depth;
    result.baseArray = baseArray;
    result.mipCount = maxMip + 1u;
    result.allocatedMipCount = maxMip + 1u;
    result.baseLevel = baseLevel;
    result.lastLevel = lastLevel;
    result.tileMode = tileMode;
    result.dimension = dimension;
    result.format = !fmask ? format : static_cast<std::uint32_t>(fmaskLayout->storageBits == 8u ? ShaderRecompiler::IrBufferFormat::Format8UInt : fmaskLayout->storageBits == 16u ? ShaderRecompiler::IrBufferFormat::Format16UInt : ShaderRecompiler::IrBufferFormat::Format32UInt);
    result.dstSelX = static_cast<std::uint8_t>(dstSelX);
    result.dstSelY = static_cast<std::uint8_t>(dstSelY);
    result.dstSelZ = static_cast<std::uint8_t>(dstSelZ);
    result.dstSelW = static_cast<std::uint8_t>(dstSelW);
    result.bcSwizzle = static_cast<std::uint8_t>(bcSwizzle);
    result.dccAddress = metaCompress && !fmask ? metaAddr << 8u : 0u;
    result.fmaskCmaskAddress = metaCompress && fmask ? metaAddr << 8u : 0u;
    result.dccAlphaOnMsb = dccAlphaPos;
    result.dccPipeAligned = metaPipeAligned;
    result.iterate256 = iterate256;
    result.minLod = minLod;
    result.samples = samples;
    if (gcn) {
        result.gcnPitch = ShaderRecompiler::GcnImagePitch(gcnTiling);
        result.gcnPow2Pad = ShaderRecompiler::GcnImagePow2Pad(gcnTiling);
        result.gcnLinearGeneral = ShaderRecompiler::GcnImageTilingIndex(gcnTiling) == 31u;
        Require(result.gcnPitch >= width, "PS4 texture descriptor has a pitch below its width" + describe());
    }
    result.linearPitch = linearPitch;
    if (baseLevel > maxMip) {
        const bool fits = LevelsFitAllocation(result, lastLevel + 1u);
        if (!fits && ZeroTail(words)) {
            if (const auto resident = R128ResidentLevelWords(words)) return DecodeTextureResource(*resident);
        }
        Require(fits, "guest texture descriptor starts past the surface's last mip level at levels that would move the surface's own" + describe());
        result.mipCount = lastLevel + 1u;
    }
    if (result.dccAddress != 0 || result.fmaskCmaskAddress != 0) {
        char text[160];
        std::snprintf(text, sizeof(text), "texture %ux%u format %u at 0x%llx", width, height, format, static_cast<unsigned long long>(result.baseAddress));
        const std::uint64_t texels = static_cast<std::uint64_t>(width) * height * (dimension == TextureDimension::k3D || dimension == TextureDimension::k2DArray ? depth + 1u : 1u);
        if (result.dccAddress != 0) DebugLutWatchRange("texture dcc", result.dccAddress, std::max<std::uint64_t>(texels * 16u / 256u, 1u), text);
        if (result.fmaskCmaskAddress != 0) DebugLutWatchRange("texture cmask", result.fmaskCmaskAddress, std::max<std::uint64_t>(texels / 64u, 1u), text);
    }
    DebugLutWatch("texture", result.baseAddress, width, height, dimension == TextureDimension::k3D || dimension == TextureDimension::k2DArray || dimension == TextureDimension::k1DArray ? depth + 1u : 1u, format, dimension == TextureDimension::k3D, descriptor);
    return result;
}

namespace {

struct LutWatchView {
    std::uint64_t address;
    std::uint64_t end;
    double seconds;
    std::string text;
};

struct LutWatchState {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::mutex mutex;
    std::vector<LutWatchView> history;
    std::vector<std::uint64_t> luts;
};

LutWatchState& lutWatch() {
    static LutWatchState state;
    return state;
}

bool lutWatchEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_LUT_WATCH") != nullptr;
    return enabled;
}

}

void DebugLutWatchRange(const char* kind, std::uint64_t address, std::uint64_t bytes, const char* text) {
    if (!lutWatchEnabled() || address == 0 || bytes == 0) return;
    static std::unordered_map<std::uint64_t, bool> seen;
    std::uint64_t key = 1469598103934665603ull;
    for (const std::uint64_t part : {reinterpret_cast<std::uint64_t>(kind), address, bytes}) key = (key ^ part) * 1099511628211ull;
    auto& state = lutWatch();
    std::lock_guard lock(state.mutex);
    const bool fresh = seen.try_emplace(key, true).second;
    bool coversLut = false;
    for (const auto lut : state.luts) coversLut = coversLut || (address < lut + 0x10000u && lut < address + bytes);
    const bool packetCopy = kind[0] == 'd' || kind[0] == 'c' || kind[0] == 'p';
    static std::uint32_t copiesPrinted = 0;
    if (packetCopy && bytes >= 0x1000u && bytes <= 0x10000u && copiesPrinted < 600u) {
        ++copiesPrinted;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
        std::fprintf(stderr, "[lut-watch] at %.3f s %s 0x%llx+0x%llx: %s\n", now, kind, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), text);
    }
    if (!fresh && !coversLut) return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
    char line[320];
    std::snprintf(line, sizeof(line), "%s 0x%llx..0x%llx of %s", kind, static_cast<unsigned long long>(address), static_cast<unsigned long long>(address + bytes), text);
    if (fresh) state.history.push_back({address, address + bytes, seconds, line});
    static std::uint32_t printed = 0;
    if (coversLut && printed < 40u) {
        ++printed;
        std::fprintf(stderr, "[lut-watch] at %.3f s %s covers a LUT\n", seconds, line);
    }
}

void DebugLutWatchWriteBack(std::uint64_t from, std::uint64_t to, std::uint64_t base, std::uint32_t width, std::uint32_t height, std::uint32_t format, bool keptGuest) {
    if (!lutWatchEnabled() || from >= to) return;
    auto& state = lutWatch();
    std::lock_guard lock(state.mutex);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
    char line[256];
    std::snprintf(line, sizeof(line), "write-back 0x%llx..0x%llx of storage image 0x%llx %ux%u format %u%s", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to), static_cast<unsigned long long>(base), width, height, format, keptGuest ? " (keeping guest bytes)" : "");
    state.history.push_back({from, to, seconds, line});
    for (const auto lut : state.luts) {
        if (from < lut + 0x10000u && lut < to) std::fprintf(stderr, "[lut-watch] at %.3f s %s covers LUT 0x%llx\n", seconds, line, static_cast<unsigned long long>(lut));
    }
}

void DebugLutWatch(const char* source, std::uint64_t address, std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t format, bool volume, std::span<const std::uint32_t> words) {
    if (!lutWatchEnabled()) return;
    const bool lutShape = volume && width == 16u && height == 16u && depth == 16u;
    const bool interesting = lutShape || (volume && width <= 64u && height <= 64u) || (width == 16u && height == 16u) || (width * height == 4096u && (width == 256u || height == 256u)) || format == 130u;
    struct Seen {
        std::chrono::steady_clock::time_point lastScan{};
        std::uint64_t lastHash = 0;
        std::uint32_t dumps = 0;
    };
    auto& state = lutWatch();
    const auto start = state.start;
    auto& mutex = state.mutex;
    auto& history = state.history;
    static std::unordered_map<std::uint64_t, Seen> seen;
    std::uint64_t key = 1469598103934665603ull;
    for (const std::uint64_t part : {static_cast<std::uint64_t>(source[0]), address, static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height), static_cast<std::uint64_t>(depth), static_cast<std::uint64_t>(format), static_cast<std::uint64_t>(volume)}) key = (key ^ part) * 1099511628211ull;
    std::lock_guard lock(mutex);
    auto [entry, inserted] = seen.try_emplace(key);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (inserted) {
        std::string text;
        for (const auto word : words) {
            char hex[12];
            std::snprintf(hex, sizeof(hex), " %08x", word);
            text += hex;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "%s 0x%llx %ux%ux%u format %u%s words%s", source, static_cast<unsigned long long>(address), width, height, depth, format, volume ? " 3D" : "", text.c_str());
        const std::uint64_t bytes = std::max<std::uint64_t>(0x10000u, static_cast<std::uint64_t>(width) * height * std::max(depth, 1u) * 16u);
        history.push_back({address, address + bytes, seconds, line});
        if (lutShape) state.luts.push_back(address);
        static std::vector<std::pair<std::uint64_t, std::string>> unwrapped;
        if (width == 256u && height == 16u && !volume && format == 56u) unwrapped.emplace_back(address, line);
        if (interesting) std::fprintf(stderr, "[lut-watch] first %s\n", line);
        if (lutShape) {
            const char* dir = std::getenv("APS5_TRACE_LUT_WATCH_DIR");
            const std::string directory = dir != nullptr && *dir != '\0' ? std::string(dir) + "/" : std::string();
            for (const auto& [source, description] : unwrapped) {
                constexpr std::size_t SourceBytes = 0x4000;
                const auto* bytes32 = reinterpret_cast<const std::uint32_t*>(source);
                if (!GuestMemory::Accessible(bytes32, SourceBytes)) {
                    std::fprintf(stderr, "[lut-watch] source for 0x%llx: %s not accessible\n", static_cast<unsigned long long>(address), description.c_str());
                    continue;
                }
                std::size_t negInf = 0, zero = 0, opaque = 0;
                for (std::size_t i = 0; i < SourceBytes / 4u; ++i) {
                    negInf += bytes32[i] == 0xff800000u;
                    zero += bytes32[i] == 0u;
                    opaque += (bytes32[i] >> 24u) == 0xffu;
                }
                char name[128];
                std::snprintf(name, sizeof(name), "lut-watch-src-%llx-for-%llx.bin", static_cast<unsigned long long>(source), static_cast<unsigned long long>(address));
                if (std::FILE* file = std::fopen((directory + name).c_str(), "wb")) {
                    std::fwrite(bytes32, 1, SourceBytes, file);
                    std::fclose(file);
                }
                std::fprintf(stderr, "[lut-watch] source for 0x%llx: first 16 KiB of %s: 0xff800000 %zu, zero %zu, alpha 0xff %zu (of 4096 dwords) -> %s\n", static_cast<unsigned long long>(address), description.c_str(), negInf, zero, opaque, name);
            }
            std::size_t shown = 0;
            for (const auto& view : history) {
                if (view.address >= address + 0x10000u || view.end <= address) continue;
                if (++shown > 200) continue;
                std::fprintf(stderr, "[lut-watch] overlap of 0x%llx: at %.3f s %s\n", static_cast<unsigned long long>(address), view.seconds, view.text.c_str());
            }
            std::fprintf(stderr, "[lut-watch] overlap of 0x%llx: %zu views of %zu recorded (at most 200 shown)\n", static_cast<unsigned long long>(address), shown, history.size());
        }
    }
    if (!lutShape) return;
    if (!inserted && now - entry->second.lastScan < std::chrono::milliseconds(250)) return;
    entry->second.lastScan = now;
    constexpr std::size_t Bytes = 0x10000;
    const auto* memory = reinterpret_cast<const std::uint32_t*>(address);
    if (!GuestMemory::Accessible(memory, Bytes)) {
        if (inserted) std::fprintf(stderr, "[lut-watch] %s 0x%llx: 64 KiB not accessible\n", source, static_cast<unsigned long long>(address));
        return;
    }
    std::uint64_t hash = 1469598103934665603ull;
    std::size_t negInf = 0, zero = 0, black = 0, other = 0;
    for (std::size_t i = 0; i < Bytes / 4u; ++i) {
        const auto value = memory[i];
        hash = (hash ^ value) * 1099511628211ull;
        if (value == 0xff800000u) ++negInf;
        else if (value == 0u) ++zero;
        else if (value == 0xff000000u) ++black;
        else ++other;
    }
    if (!inserted && hash == entry->second.lastHash) return;
    entry->second.lastHash = hash;
    std::fprintf(stderr, "[lut-watch] contents %s 0x%llx at %.3f s: 0xff800000 %zu, zero %zu, 0xff000000 %zu, other %zu (of %zu dwords), hash %016llx\n", source, static_cast<unsigned long long>(address), seconds, negInf, zero, black, other, Bytes / 4u, static_cast<unsigned long long>(hash));
    if (entry->second.dumps < 16u) {
        static const std::string directory = [] {
            const char* text = std::getenv("APS5_TRACE_LUT_WATCH_DIR");
            return text != nullptr && *text != '\0' ? std::string(text) + "/" : std::string();
        }();
        char name[96];
        std::snprintf(name, sizeof(name), "lut-watch-%llx-%02u.bin", static_cast<unsigned long long>(address), entry->second.dumps++);
        if (std::FILE* file = std::fopen((directory + name).c_str(), "wb")) {
            std::fwrite(memory, 1, Bytes, file);
            std::fclose(file);
        }
    }
}

namespace {

using RejectionKey = std::array<std::uint32_t, 9>;

struct RememberedRejections {
    std::shared_mutex mutex;
    std::map<RejectionKey, std::shared_ptr<const std::string>> entries;
};

RememberedRejections& rememberedRejections() {
    static RememberedRejections remembered;
    return remembered;
}

}

TextureResourceDecode TryDecodeTextureResource(std::span<const std::uint32_t> words) {
    TextureResourceDecode decoded;
    if (words.size() != 8) {
        try {
            decoded.resource = DecodeTextureResource(words);
        } catch (const std::runtime_error& error) {
            decoded.rejection = std::make_shared<const std::string>(error.what());
        }
        return decoded;
    }
    RejectionKey key{};
    std::copy(words.begin(), words.end(), key.begin());
    key[8] = ShaderRecompiler::GcnGuestShaders() ? 1u : 0u;
    auto& remembered = rememberedRejections();
    {
        std::shared_lock lock(remembered.mutex);
        if (const auto found = remembered.entries.find(key); found != remembered.entries.end()) {
            decoded.rejection = found->second;
            return decoded;
        }
    }
    try {
        decoded.resource = DecodeTextureResource(words);
    } catch (const std::runtime_error& error) {
        decoded.rejection = std::make_shared<const std::string>(error.what());
        std::unique_lock lock(remembered.mutex);
        if (remembered.entries.size() >= RememberedTextureRejections) remembered.entries.clear();
        remembered.entries.emplace(key, decoded.rejection);
    }
    return decoded;
}

std::size_t RememberedTextureRejectionCount() {
    auto& remembered = rememberedRejections();
    std::shared_lock lock(remembered.mutex);
    return remembered.entries.size();
}

std::array<std::uint32_t, 8> EncodeTextureResource(const GuestTextureResource& resource) {
    const bool multisampled = resource.dimension == TextureDimension::k2DMsaa;
    const bool array = resource.dimension == TextureDimension::k2DArray;
    Require(((resource.dimension == TextureDimension::k2D || array) && resource.samples == 1u) || (multisampled && resource.samples >= 2u && resource.samples <= 16u && std::has_single_bit(resource.samples)), "only 2D, 2D array and multisampled 2D surfaces have a texture descriptor encoding");
    Require(resource.mipCount >= 1u && resource.mipCount <= 16u && (resource.allocatedMipCount == 0u || resource.allocatedMipCount == resource.mipCount) && resource.baseLevel <= resource.lastLevel && resource.lastLevel < resource.mipCount && (array ? resource.baseArray <= resource.depthOrLastArray && resource.depthOrLastArray <= 0x1fffu : resource.depthOrLastArray == 0u && resource.baseArray == 0u), "the surface's mip levels or array slices have no texture descriptor encoding");
    Require((!multisampled || resource.mipCount == 1u) && resource.dccAddress == 0u && resource.minLod == 0u && !resource.iterate256 && resource.bcSwizzle == 0u && resource.linearPitch == 0u, "only single-level multisampled surfaces and surfaces without metadata have a texture descriptor encoding");
    Require(resource.width >= 1u && resource.width <= 0x4000u && resource.height >= 1u && resource.height <= 0x4000u && resource.format < 0x200u, "the surface's extent or format has no texture descriptor encoding");
    const auto xorMask = resource.tileMode == TextureTileMode::kS4KBX || resource.tileMode == TextureTileMode::kD4KBX ? 0xfffull : 0xffffull;
    Require(XorSwizzleMode(resource.tileMode) != 0 ? (resource.baseAddress & xorMask) == 0 && (resource.pipeBankXor & ~xorMask) == 0 : resource.pipeBankXor == 0, "the surface's pipe and bank XOR has no texture descriptor encoding");
    const auto base = (resource.baseAddress | resource.pipeBankXor) >> 8u;
    Require(base != 0 && (resource.baseAddress & 0xffu) == 0 && base <= 0xffffffffffull, "the surface's address has no texture descriptor encoding");
    std::uint32_t tileMode = 0;
    switch (resource.tileMode) {
        case TextureTileMode::kLinear: tileMode = 0x00; break;
        case TextureTileMode::kStandard256B: tileMode = 0x01; break;
        case TextureTileMode::kD256B: tileMode = 0x02; break;
        case TextureTileMode::kStandard4KB: tileMode = 0x05; break;
        case TextureTileMode::kD4KB: tileMode = 0x06; break;
        case TextureTileMode::kStandard64KB: tileMode = 0x09; break;
        case TextureTileMode::kD64KB: tileMode = 0x0a; break;
        case TextureTileMode::kS64KBT: tileMode = 0x11; break;
        case TextureTileMode::kD64KBT: tileMode = 0x12; break;
        case TextureTileMode::kS4KBX: tileMode = 0x15; break;
        case TextureTileMode::kD4KBX: tileMode = 0x16; break;
        case TextureTileMode::kZ64KBX: tileMode = 0x18; break;
        case TextureTileMode::kS64KBX: tileMode = 0x19; break;
        case TextureTileMode::kD64KBX: tileMode = 0x1a; break;
        case TextureTileMode::kR64KBX: tileMode = 0x1b; break;
    }
    const auto sampleLevel = static_cast<std::uint32_t>(std::countr_zero(resource.samples));
    std::array<std::uint32_t, 8> words{};
    words[0] = static_cast<std::uint32_t>(base);
    words[1] = static_cast<std::uint32_t>(base >> 32u) | (resource.format << 20u) | (((resource.width - 1u) & 3u) << 30u);
    words[2] = ((resource.width - 1u) >> 2u) | ((resource.height - 1u) << 14u);
    const auto lastLevel = multisampled ? sampleLevel : resource.lastLevel;
    words[3] = resource.dstSelX | (resource.dstSelY << 3u) | (resource.dstSelZ << 6u) | (resource.dstSelW << 9u) | (resource.baseLevel << 12u) | (lastLevel << 16u) | (tileMode << 20u) | ((multisampled ? 14u : array ? 13u : 9u) << 28u);
    words[4] = resource.depthOrLastArray | (resource.baseArray << 16u);
    words[5] = (multisampled ? sampleLevel : resource.mipCount - 1u) << 4u;
    return words;
}

float EffectiveMinLod(const GuestTextureResource& resource) {
    if (resource.minLod <= resource.baseLevel * 256u) return 0.0f;
    return std::min(static_cast<float>(resource.minLod) / 256.0f, static_cast<float>(resource.lastLevel));
}

bool DescriptorSingleLevel(std::span<const std::uint32_t> words) {
    return ((words[3] >> 12u) & 0xfu) == ((words[3] >> 16u) & 0xfu);
}

bool FirstLayerDescriptor(std::span<const std::uint32_t> words, std::array<std::uint32_t, 8>& narrowed) {
    if (words.size() != narrowed.size()) return false;
    const auto resource = DecodeTextureResource(words);
    if (resource.dimension != TextureDimension::k2DArray || resource.depthOrLastArray <= resource.baseArray) return false;
    std::copy(words.begin(), words.end(), narrowed.begin());
    narrowed[4] = (narrowed[4] & ~0x1fffu) | resource.baseArray;
    return true;
}

bool MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension) {
    switch (shape) {
        case ShaderRecompiler::DescriptorImageShape::Image1D: return dimension == TextureDimension::k1D;
        case ShaderRecompiler::DescriptorImageShape::Image2D: return dimension == TextureDimension::k2D;
        case ShaderRecompiler::DescriptorImageShape::Image2DArray: return dimension == TextureDimension::k2DArray || dimension == TextureDimension::kCube;
        case ShaderRecompiler::DescriptorImageShape::ImageCube: return dimension == TextureDimension::kCube;
        case ShaderRecompiler::DescriptorImageShape::Image3D: return dimension == TextureDimension::k3D;
        case ShaderRecompiler::DescriptorImageShape::Image1DArray: return dimension == TextureDimension::k1DArray;
        case ShaderRecompiler::DescriptorImageShape::Image2DMultisample: return dimension == TextureDimension::k2DMsaa;
        case ShaderRecompiler::DescriptorImageShape::Image2DMultisampleArray: return dimension == TextureDimension::k2DMsaa;
    }
    throw std::runtime_error("AGC graphics: MatchesGuestDimension encountered an unknown descriptor image shape");
}

bool ArrayLayersServeView(const GuestTextureResource& surface, const GuestTextureResource& view) {
    if (view.dimension == surface.dimension && view.depthOrLastArray == surface.depthOrLastArray) return true;
    if (surface.dimension != TextureDimension::k2DArray) return false;
    if (view.dimension == TextureDimension::k2D) return view.baseArray == 0;
    return view.dimension == TextureDimension::k2DArray && view.depthOrLastArray <= surface.depthOrLastArray;
}

}
