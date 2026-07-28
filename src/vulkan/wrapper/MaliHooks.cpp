#include "And64InlineHook.hpp"
#include "MaliHooks.hpp"
#include "wrapper_log.h"
#include <cstdint>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <libelf.h>
#include <gelf.h>
#include <lzma.h>

typedef struct {
    uint8_t *data;
    size_t size;
} Buffer;

ModuleInfo mali;

static Buffer decompress_xz(const uint8_t* compressed_data, size_t size) {
    Buffer decompressed = {NULL, 0};
    lzma_stream strm = LZMA_STREAM_INIT;

    if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) {
        return decompressed;
    }

    strm.next_in = compressed_data;
    strm.avail_in = size;

    uint8_t chunk_buffer[64 * 1024];
    lzma_action action = LZMA_RUN;

    while (strm.avail_in > 0 || strm.avail_out == 0) {
        strm.next_out = chunk_buffer;
        strm.avail_out = sizeof(chunk_buffer);

        lzma_ret ret = lzma_code(&strm, action);
        size_t produced = sizeof(chunk_buffer) - strm.avail_out;

        if (produced > 0) {
            uint8_t *new_ptr = (uint8_t*)realloc(decompressed.data, decompressed.size + produced);
            if (!new_ptr) {
                free(decompressed.data);
                decompressed.data = NULL;
                decompressed.size = 0;
                lzma_end(&strm);
                return decompressed;
            }
            decompressed.data = new_ptr;
            memcpy(decompressed.data + decompressed.size, chunk_buffer, produced);
            decompressed.size += produced;
        }

        if (ret == LZMA_STREAM_END) {
            break;
        }
        if (ret != LZMA_OK) {
            free(decompressed.data);
            decompressed.data = NULL;
            decompressed.size = 0;
            lzma_end(&strm);
            return decompressed;
        }
    }

    lzma_end(&strm);
    return decompressed;
}

static uintptr_t find_symbol_in_elf_buffer(const void* buffer, size_t size, const char* target_symbol) {
    if (elf_version(EV_CURRENT) == EV_NONE) return 0;

    Elf* elf = elf_memory((char*)buffer, size);
    if (!elf) return 0;

    Elf_Scn* scn = NULL;
    GElf_Shdr shdr;
    uintptr_t symbol_offset = 0;

    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);

        // Scan both SHT_SYMTAB and SHT_DYNSYM
        if (shdr.sh_type == SHT_SYMTAB || shdr.sh_type == SHT_DYNSYM) {
            Elf_Data* data = elf_getdata(scn, NULL);
            if (!data) continue;

            int symbol_count = shdr.sh_size / shdr.sh_entsize;

            for (int i = 0; i < symbol_count; ++i) {
                GElf_Sym sym;
                gelf_getsym(data, i, &sym);

                const char* name = elf_strptr(elf, shdr.sh_link, sym.st_name);
                if (name && strstr(name, target_symbol)) { // Substring or exact match
                    symbol_offset = (uintptr_t)sym.st_value;
                    break;
                }
            }
        }
        if (symbol_offset != 0) break;
    }

    elf_end(elf);
    return symbol_offset;
}

// Extract symbol offset directly from .gnu_debugdata in shared library file
extern "C" uintptr_t get_hidden_symbol_offset(const char* library_path, const char* symbol_name) {
    int fd = open(library_path, O_RDONLY, 0);
    if (fd < 0) return 0;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        close(fd);
        return 0;
    }

    Elf* elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) {
        close(fd);
        return 0;
    }

    size_t shstrndx;
    if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
        elf_end(elf);
        close(fd);
        return 0;
    }

    Elf_Scn* scn = NULL;
    GElf_Shdr shdr;
    uintptr_t found_offset = 0;

    while ((scn = elf_nextscn(elf, scn)) != NULL) {
        gelf_getshdr(scn, &shdr);
        const char* name = elf_strptr(elf, shstrndx, shdr.sh_name);

        if (name && strcmp(name, ".gnu_debugdata") == 0) {
            Elf_Data* data = elf_getdata(scn, NULL);
            if (data && data->d_buf) {
                Buffer decompressed_elf = decompress_xz((const uint8_t*)data->d_buf, data->d_size);
                if (decompressed_elf.data && decompressed_elf.size > 0) {
                    found_offset = find_symbol_in_elf_buffer(
                        decompressed_elf.data, 
                        decompressed_elf.size, 
                        symbol_name
                    );
                    free(decompressed_elf.data);
                }
            }
            break;
        }
    }

    elf_end(elf);
    close(fd);
    return found_offset;
}

extern "C" ModuleInfo get_mali_module_info(const char* library_keyword) {
    ModuleInfo info = {0, ""};
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return info;

    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, library_keyword) != NULL) {
            info.base_address = (uintptr_t)strtoull(line, NULL, 16);
            char *path_start = strchr(line, '/');
            if (path_start) {
                char *newline = strchr(path_start, '\n');
                if (newline) *newline = '\0';

                strncpy(info.path, path_start, sizeof(info.path) - 1);
                info.path[sizeof(info.path) - 1] = '\0';
                break;
            }
        }
    }

    fclose(maps);
    return info;
}

static bool decode_adrp(uint32_t insn, uint64_t pc, uint64_t* out_page) {
    if ((insn & 0x9F000000) != 0x90000000) return false;

    int64_t immlo = (insn >> 29) & 0x3;
    int64_t immhi = (insn >> 5) & 0x7FFFF;
    int64_t imm = (immhi << 2) | immlo;

    if (imm & (1ULL << 20)) {
        imm |= ~((1ULL << 21) - 1);
    }

    *out_page = (pc & ~0xFFFULL) + (imm << 12);
    return true;
}

static bool decode_add_imm64(uint32_t insn, uint32_t* out_imm12) {
    if ((insn & 0xFFC00000) != 0x91000000) return false;
    *out_imm12 = (insn >> 10) & 0xFFF;
    return true;
}

static bool decode_ldr_imm64(uint32_t insn, uint32_t* out_offset) {
    if ((insn & 0xFFC00000) != 0xF9400000) return false;
    *out_offset = ((insn >> 10) & 0xFFF) << 3; // Shifted by 8 for 64-bit LDR
    return true;
}

typedef union cobj_pfs {
    uint32_t raw;
    struct {
        uint32_t swizzle_order_code : 12; // Bits [11:0] : Component permutation / channel swizzle order
        uint32_t format_type_id     : 8;  // Bits [19:12]: Mali Hardware Surface Format ID
        uint32_t srgb_enable        : 1;  // Bit  [20]   : sRGB color space conversion enable (1 = sRGB)
        uint32_t reserved           : 11; // Bits [31:21]: Extended flags / reserved
    } bits;
} cobj_pfs_t;

typedef struct format_table_entry {
    /* 0x00 */ cobj_pfs_t pfs;                   // Pixel Format Specification / HW format flags
    /* 0x04 */ uint32_t pad_04;
    /* 0x08 */ uint64_t linear_features;         // VkFormatFeatureFlags for VK_IMAGE_TILING_LINEAR
    /* 0x10 */ uint64_t optimal_features;        // VkFormatFeatureFlags for VK_IMAGE_TILING_OPTIMAL
    /* 0x18 */ uint64_t buffer_features;         // VkFormatFeatureFlags for VkBuffer usage
    /* 0x20 */ uint8_t  sample_counts;           // VkSampleCountFlags bitmask (dynamic GPU caps)
    /* 0x21 */ uint8_t  component_count;         // Number of HAL components (1..4)
    /* 0x22 */ uint16_t reserved_22;             // Alignment / plane descriptor flags
    /* 0x24 */ uint32_t flags;                   // Format capability bitfield (see Bitmask Breakdown)
    /* 0x28 */ uint32_t pad_28;                  // Reserved / alignment padding
    /* 0x2C */ uint32_t component_descr1;        // Mali HW Component Packing Descriptor 1
    /* 0x30 */ uint32_t component_descr2;        // Mali HW Component Packing Descriptor 2
    /* 0x34 */ uint32_t pad_34;                  // Padding to 0x38-byte alignment
} format_table_entry_t;

static_assert(sizeof(format_table_entry_t) == 0x38, "format_table_entry_t must be exactly 56 bytes");

typedef enum format_table_flag_bits {
    FORMAT_FLAG_IS_YUV                   = (1u << 0),  // 0x0001: YUV pixel format
    FORMAT_FLAG_IS_SRGB                  = (1u << 1),  // 0x0002: sRGB color space format
    FORMAT_FLAG_NO_CRC                   = (1u << 2),  // 0x0004: Transaction Elimination / CRC disabled
    FORMAT_FLAG_COMPRESSION_ALLOWED      = (1u << 5),  // 0x0020: Lossless/AFRC/AFBC compression permitted
    FORMAT_FLAG_IS_ASTC_SPECIAL_SAMPLING = (1u << 6),  // 0x0040: ASTC or special compressed sampling
    FORMAT_FLAG_IS_DEPTH                 = (1u << 7),  // 0x0080: Depth aspect format
    FORMAT_FLAG_IS_STENCIL               = (1u << 8),  // 0x0100: Stencil aspect format
    FORMAT_FLAG_HORIZ_SUBSAMPLING_SHIFT  = (1u << 9),  // 0x0200: Horizontal chroma subsampling shift (1 = 2x)
    FORMAT_FLAG_VERT_SUBSAMPLING_SHIFT   = (1u << 10), // 0x0400: Vertical chroma subsampling shift (1 = 2x)
} format_table_flag_bits_t;

static FormatTableResolution cached_format_table;

extern "C" struct FormatTableResolution resolve_gformat_table(void* is_depth_stencil_live_addr) {
    FormatTableResolution res{0, 0, false};
    if (!is_depth_stencil_live_addr) return res;

    const uint32_t* instructions = reinterpret_cast<const uint32_t*>(is_depth_stencil_live_addr);
    const uint64_t pc_base = reinterpret_cast<uint64_t>(is_depth_stencil_live_addr);
    uint64_t adrp_page = 0;
    bool found_adrp = false;

    // Scan up to 12 instructions (handles optional BTI/NOP headers)
    for (int i = 0; i < 12; ++i) {
        uint64_t pc = pc_base + (i * 4);
        uint32_t insn = instructions[i];

        if (!found_adrp) {
            if (decode_adrp(insn, pc, &adrp_page)) {
                found_adrp = true;
            }
        } else {
            uint32_t offset = 0;

            // Direct array reference via ADD
            if (decode_add_imm64(insn, &offset)) {
                res.var_address = static_cast<uintptr_t>(adrp_page + offset);
                res.table_address = res.var_address;
                res.is_indirect = false;
                cached_format_table = res;
                return res;
            }

            // Indirect reference via LDR
            if (decode_ldr_imm64(insn, &offset)) {
                res.var_address = static_cast<uintptr_t>(adrp_page + offset);
                res.is_indirect = true;
                uintptr_t* ptr = reinterpret_cast<uintptr_t*>(res.var_address);
                if (ptr) {
                    res.table_address = *ptr;
                }
                cached_format_table = res;
                return res;
            }
        }
    }

    cached_format_table = res;
    return cached_format_table;
}

struct Rect2D {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

// Target size: 0x330 bytes
struct copy_image_info_t {
    uint8_t  pad_000[0x18];
    Rect2D   src_rect;            // 0x018
    uint8_t  pad_028[0x8];
    uint8_t  copy_aspect_flags;   // 0x030 (0x1 = Color, 0x2 = Depth, 0x4 = Stencil)
    uint8_t  readback_flags;      // 0x031
    uint8_t  pad_032[0x0A];
    uint32_t readback_param_1;    // 0x03C
    uint32_t readback_param_2;    // 0x040
    uint8_t  pad_044[0x4];
    uint16_t src_format;          // 0x048
    uint8_t  src_flags;           // 0x04A
    uint8_t  pad_04B[0x16D];
    uint16_t dst_format;          // 0x1B8
    uint8_t  dst_mode_flags;      // 0x1BA
    uint8_t  pad_1BB[0x16D];
    uint8_t  template_mode;       // 0x328
};

// Target size: ~0x19B0 bytes
struct copy_image_template_internal_t {
    uintptr_t p_program_cache;     // 0x0000
    uint16_t  cached_src_format;   // 0x0008
    uint16_t  cached_dst_format;   // 0x000A
    uint16_t  cached_readback_fmt; // 0x000C
    uint8_t   active_copy_aspect;  // 0x000E
    uint8_t   readback_shader_type;// 0x000F
    uint8_t   needs_software_copy; // 0x0010 (1 if area > 65536 px)
    uint8_t   copy_shader_variant; // 0x0011
    uint8_t   pad_0012[0x6];
    uintptr_t readback_program;    // 0x0018
    uintptr_t copy_program;        // 0x0020
    uint8_t   pad_0028[0x70];
    uint16_t  readback_render_flags; // 0x0098
    uint8_t   pad_009A[0x2];
    int16_t   readback_state_mode;  // 0x009C
    uint8_t   pad_009E[0x0E];
    uint16_t  readback_state_mask;  // 0x00AC
    uint8_t   pad_00AE[0x3A];
    int32_t   readback_hsr_mode;    // 0x00E8
    uint8_t   pad_00EC[0x0C];
    uint16_t  copy_render_flags;    // 0x00F8
    uint8_t   pad_00FA[0x2];
    int16_t   copy_state_mode;      // 0x00FC
    uint8_t   pad_00FE[0x0E];
    uint16_t  copy_state_mask;      // 0x010C
    uint8_t   pad_010E[0x3A];
    int32_t   copy_hsr_mode;        // 0x0148
    uint8_t   pad_014C[0x37D];
    uint8_t   enable_readback;      // 0x04C9
    uint8_t   template_mode;        // 0x04CA
};

typedef uint64_t (*copy_image_update_t)(copy_image_template_internal_t *this_ptr, const copy_image_info_t *info_ptr);
static copy_image_update_t orig_copy_image_update = NULL;

static void dump_raw_hex(const char *tag, const void *data, size_t size) {
    if (!data) return;
    const uint8_t *bytes = static_cast<const uint8_t*>(data);
    char line[128];
    
    WRAPPER_LOG(info, "=== HEX DUMP: %s (%p, %zu bytes) ===", tag, data, size);
    for (size_t i = 0; i < size; i += 16) {
        int off = snprintf(line, sizeof(line), "+0x%04zX: ", i);
        for (size_t j = 0; j < 16 && (i + j) < size; ++j) {
            off += snprintf(line + off, sizeof(line) - off, "%02X ", bytes[i + j]);
        }
        WRAPPER_LOG(info, "%s", line);
    }
}

static void dump_copy_image_info(const copy_image_info_t *info) {
    if (!info) return;

    int w = (info->src_rect.x2 - info->src_rect.x1) + 1;
    int h = (info->src_rect.y2 - info->src_rect.y1) + 1;
    int area = w * h;

    WRAPPER_LOG(info, "--- [STRUCT] hal::copy_image_info (%p) ---", info);
    WRAPPER_LOG(info, "  src_rect:         (%d, %d) -> (%d, %d) [%dx%d, area: %d px]", 
                info->src_rect.x1, info->src_rect.y1, 
                info->src_rect.x2, info->src_rect.y2, 
                w, h, area);
    WRAPPER_LOG(info, "  copy_aspect_flags:0x%02X (Color:%d, Depth:%d, Stencil:%d)", 
                info->copy_aspect_flags, 
                (info->copy_aspect_flags & 1) != 0, 
                (info->copy_aspect_flags & 2) != 0, 
                (info->copy_aspect_flags & 4) != 0);
    WRAPPER_LOG(info, "  readback_flags:   0x%02X", info->readback_flags);
    WRAPPER_LOG(info, "  readback_params:  P1=0x%08X, P2=0x%08X", info->readback_param_1, info->readback_param_2);
    WRAPPER_LOG(info, "  src_format:       0x%04X (flags: 0x%02X)", info->src_format, info->src_flags);
    WRAPPER_LOG(info, "  dst_format:       0x%04X (mode_flags: 0x%02X)", info->dst_format, info->dst_mode_flags);
    WRAPPER_LOG(info, "  template_mode:    0x%02X", info->template_mode);
}

static void dump_copy_image_template_internal(const copy_image_template_internal_t *tmpl) {
    if (!tmpl) return;

    WRAPPER_LOG(info, "--- [STRUCT] hal::halp::copy_image_template_internal (%p) ---", tmpl);
    WRAPPER_LOG(info, "  p_program_cache:    %p", (void*)tmpl->p_program_cache);
    WRAPPER_LOG(info, "  cached_src_format:  0x%04X", tmpl->cached_src_format);
    WRAPPER_LOG(info, "  cached_dst_format:  0x%04X", tmpl->cached_dst_format);
    WRAPPER_LOG(info, "  cached_readback_fmt:0x%04X", tmpl->cached_readback_fmt);
    WRAPPER_LOG(info, "  active_copy_aspect: 0x%02X", tmpl->active_copy_aspect);
    WRAPPER_LOG(info, "  readback_shader_typ:0x%02X", tmpl->readback_shader_type);
    WRAPPER_LOG(info, "  needs_software_copy:%u (area > 65536)", tmpl->needs_software_copy);
    WRAPPER_LOG(info, "  copy_shader_variant:%u", tmpl->copy_shader_variant);
    WRAPPER_LOG(info, "  readback_program:   %p", (void*)tmpl->readback_program);
    WRAPPER_LOG(info, "  copy_program:       %p", (void*)tmpl->copy_program);
    WRAPPER_LOG(info, "  readback_flags:     0x%04X (mode: %d, mask: 0x%04X, hsr: %d)",
                tmpl->readback_render_flags, tmpl->readback_state_mode, 
                tmpl->readback_state_mask, tmpl->readback_hsr_mode);
    WRAPPER_LOG(info, "  copy_render_flags:  0x%04X (mode: %d, mask: 0x%04X, hsr: %d)",
                tmpl->copy_render_flags, tmpl->copy_state_mode, 
                tmpl->copy_state_mask, tmpl->copy_hsr_mode);
    WRAPPER_LOG(info, "  enable_readback:    %u", tmpl->enable_readback);
    WRAPPER_LOG(info, "  template_mode:      0x%02X", tmpl->template_mode);
}

void log_format_table_entry(format_table_entry_t* table, int index) {
   char flags_str[256];
   snprintf(flags_str, sizeof(flags_str), "%s%s%s%s%s%s%s%s%s",
            (table[index].flags & FORMAT_FLAG_IS_YUV) ? "YUV " : "",
            (table[index].flags & FORMAT_FLAG_IS_SRGB) ? "sRGB " : "",
            (table[index].flags & FORMAT_FLAG_NO_CRC) ? "no-CRC " : "",
            (table[index].flags & FORMAT_FLAG_COMPRESSION_ALLOWED) ? "compression-allowed " : "",
            (table[index].flags & FORMAT_FLAG_IS_ASTC_SPECIAL_SAMPLING) ? "ASTC-special-sampling " : "",
            (table[index].flags & FORMAT_FLAG_IS_DEPTH) ? "depth " : "",
            (table[index].flags & FORMAT_FLAG_IS_STENCIL) ? "stencil " : "",
            (table[index].flags & FORMAT_FLAG_HORIZ_SUBSAMPLING_SHIFT) ? "horiz-subsampling " : "",
            (table[index].flags & FORMAT_FLAG_VERT_SUBSAMPLING_SHIFT) ? "vert-subsampling " : "");

   auto format_feature_flags_to_str = [](uint64_t flags) -> std::string {
      std::string result;
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) result += "SAMPLED_IMAGE ";
      if (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) result += "STORAGE_IMAGE ";
      if (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) result += "STORAGE_IMAGE_ATOMIC ";
      if (flags & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) result += "UNIFORM_TEXEL_BUFFER ";
      if (flags & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) result += "STORAGE_TEXEL_BUFFER ";
      if (flags & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT) result += "STORAGE_TEXEL_BUFFER_ATOMIC ";
      if (flags & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) result += "VERTEX_BUFFER ";
      if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) result += "COLOR_ATTACHMENT ";
      if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) result += "COLOR_ATTACHMENT_BLEND ";
      if (flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) result += "DEPTH_STENCIL_ATTACHMENT ";
      if (flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT) result += "BLIT_SRC ";
      if (flags & VK_FORMAT_FEATURE_BLIT_DST_BIT) result += "BLIT_DST ";
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) result += "SAMPLED_IMAGE_FILTER_LINEAR ";
      if (flags & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) result += "TRANSFER_SRC ";
      if (flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) result += "TRANSFER_DST ";
      if (flags & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) result += "MIDPOINT_CHROMA_SAMPLES ";
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) result += "YCBCR_CONVERSION_LINEAR_FILTER ";
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT) result += "YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER ";
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_BIT) result += "YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT ";
      if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_FORCEABLE_BIT) result += "YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_FORCEABLE ";
      if (flags & VK_FORMAT_FEATURE_DISJOINT_BIT) result += "DISJOINT ";
      return result;
   };

   WRAPPER_LOG(info, "  entry[%d] @ %p | swizzle_order_code=%x, format_type_id=%x, srgb_enable=%u, optimal_features=%lx (%s), buffer_features=%lx (%s), sample_counts=%u, component_count=%u, flags=0x%04X (%s), component_descr1=0x%08X, component_descr2=0x%08X",
               index,
               &table[index],
               table[index].pfs.bits.swizzle_order_code,
               table[index].pfs.bits.format_type_id,
               table[index].pfs.bits.srgb_enable,
               table[index].optimal_features,
               format_feature_flags_to_str(table[index].optimal_features).c_str(),
               table[index].buffer_features,
               format_feature_flags_to_str(table[index].buffer_features).c_str(),
               table[index].sample_counts,
               table[index].component_count,
               table[index].flags,
               flags_str,
               table[index].component_descr1,
               table[index].component_descr2);
   
}

static uint64_t my_copy_image_template_update_detour(copy_image_template_internal_t *this_ptr, 
                                                     const copy_image_info_t *info_ptr) {
    WRAPPER_LOG(info, "calling ::update called");
    auto table_ptr_location = reinterpret_cast<format_table_entry_t**>(cached_format_table.var_address);
    auto active_table_base = *table_ptr_location;
    for (int i = 0; i < 271; i++) {
       WRAPPER_LOG(info, "hal::format_table_entry[%d] (%s)", i, format_to_string(i));
       log_format_table_entry(active_table_base, i);
    }

    dump_copy_image_info(info_ptr);
    dump_copy_image_template_internal(this_ptr);

    // dump_raw_hex("hal::copy_image_info", info_ptr, sizeof(copy_image_info_t));    
    // dump_raw_hex("hal::halp::copy_image_template_internal (head)", this_ptr, 0x500);

    uint64_t result = 0;
    if (orig_copy_image_update) {
        result = orig_copy_image_update(this_ptr, info_ptr);
    }

    WRAPPER_LOG(info, "after ::update called");
    dump_copy_image_template_internal(this_ptr);

    return result;
}

extern "C" int install_mali_hook(void* copy_image_template_update) {
    A64HookFunction(
        (void*)copy_image_template_update,
        (void*)my_copy_image_template_update_detour,
        (void**)&orig_copy_image_update
    );

    WRAPPER_LOG(info, "[+] Hook for copy_image_template_internal::update placed successfully at %p", copy_image_template_update);
    return 0;
}

void* get_symbol(std::string symbol) {
   auto func_offset = get_hidden_symbol_offset(mali.path, symbol.c_str());

   if (func_offset == 0) {
       WRAPPER_LOG(info, "Failed to resolve symbol %s in .gnu_debugdata", symbol.c_str());
       return nullptr;
   }

   auto live_target_address = mali.base_address + func_offset;
   return reinterpret_cast<void *>(live_target_address);
}

char *(*format_to_string)(unsigned short format) = nullptr;

extern "C" int hook_libGLES_mali() {
   mali = get_mali_module_info("libGLES_mali");

   if (mali.base_address == 0 || mali.path[0] == '\0') {
      WRAPPER_LOG(info, "Failed to find Mali driver in process memory map!\n");
      return -1;
   }

   WRAPPER_LOG(info, "[+] Found Mali binary on disk: %s\n", mali.path);
   WRAPPER_LOG(info, "[+] Mali RAM Base Address: 0x%" PRIxPTR "\n", mali.base_address);

   auto copy_image_update_addr = get_symbol("copy_image_template_internal6update");
   WRAPPER_LOG(info, "[+] copy_image_template_internal::update @ 0x%p", copy_image_update_addr);
   if (copy_image_update_addr == nullptr) {
      return -1;
   }
   
   auto is_depth_stencil_addr = get_symbol("format_query_internal16is_depth_stencil");
   WRAPPER_LOG(info, "[+] is_depth_stencil @ 0x%p", is_depth_stencil_addr);
   struct FormatTableResolution gfmt = resolve_gformat_table(is_depth_stencil_addr);
   if (gfmt.var_address == 0) {
      WRAPPER_LOG(info, "[-] Failed to resolve gFORMAT_TABLE variable location!\n");
      return -1;
   }
   
   auto format_to_string_addr = get_symbol("_ZN3hal12format_query16format_to_stringENS_");
   WRAPPER_LOG(info, "[+] format_to_string @ 0x%p", format_to_string_addr);
   if (format_to_string_addr == nullptr) {
      return -1;
   }
   format_to_string = reinterpret_cast<decltype(format_to_string)>(format_to_string_addr);
   
   install_mali_hook(copy_image_update_addr);
   return 0;
}
