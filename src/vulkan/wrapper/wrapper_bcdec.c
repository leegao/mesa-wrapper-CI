#include "wrapper_bcdec.h"
#include "wrapper_log.h"
#include "wrapper_util.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "util/xxhash.h"

#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION

#include "bcdec.h"
#include "wrapper_astc.h"

/* Transcode BCn to ASTC 4x4 (Mali-native, stays compressed) instead of
 * decoding to RGBA8 (4-8x larger). Default on; WRAPPER_BCN_ASTC=0 falls back
 * to the RGBA decode path. */
static int
astc_enabled(void)
{
   static int e = -1;
   if (e == -1)
      e = getenv("WRAPPER_BCN_ASTC") ? atoi(getenv("WRAPPER_BCN_ASTC")) : 1;
   return e;
}

/* ASTC block footprint: 0 = 4x4 (8bpp, crisp), 1 = 8x8 (2bpp, 4x smaller/softer).
 * Selected by WRAPPER_ASTC_BLOCK ("4x4" default, "8x8"). */
static int
astc_block8(void)
{
   static int b = -1;
   if (b == -1) {
      const char *e = getenv("WRAPPER_ASTC_BLOCK");
      b = (e && strstr(e, "8x8")) ? 1 : 0;
   }
   return b;
}

int
is_astc_4x4(VkFormat format)
{
   return format == VK_FORMAT_ASTC_4x4_UNORM_BLOCK ||
          format == VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
}

int
is_astc_8x8(VkFormat format)
{
   return format == VK_FORMAT_ASTC_8x8_UNORM_BLOCK ||
          format == VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
}

int
is_astc(VkFormat format)
{
   return is_astc_4x4(format) || is_astc_8x8(format);
}

static int
bcn_has_alpha(VkFormat bcn_format)
{
   switch (bcn_format) {
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      return 1;
   default:
      /* BC1 is treated as opaque (bcdec emits alpha=255). */
      return 0;
   }
}

#define WRAPPER_CACHE_DIR "/data/data/app.gamenative/files/imagefs/usr/cache"

struct decompression_params {
   int block_x;
   int block_x_src;
   int block_y_count;
   int block_y_start;
   int stride;
   int texel_size;
   int astc;
   int astc8;
   int bc_bx;      /* BC grid width in blocks (astc8 bounds) */
   int bc_by;      /* BC grid height in blocks (astc8 bounds) */
   int has_alpha;
   VkFormat format;
   char *src;
   char *dst;
};

static int
get_block_size(VkFormat format) 
{
    switch(format) {
       case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
       case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
       case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
       case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
       case VK_FORMAT_BC4_UNORM_BLOCK:
       case VK_FORMAT_BC4_SNORM_BLOCK:
          return 8;
       default:
          return 16;
    }
}

static void *
decompression_routine(void *args)
{
   struct decompression_params *params = args;

   char *dst_base = params->dst;
   int block_size = get_block_size(params->format);

   /* 8x8 ASTC: one block per 2x2 group of BC blocks. Decode the 4 BC blocks into
    * an 8x8 RGBA scratch (row stride 32 bytes), then encode one ASTC 8x8 block. */
   if (params->astc8) {
      for (int by = 0; by < params->block_y_count; by++) {
         int BY = params->block_y_start + by;
         for (int BX = 0; BX < params->block_x; BX++) {
            unsigned char scratch[256];
            memset(scratch, 0, sizeof(scratch));
            for (int dy = 0; dy < 2; dy++) {
               for (int dx = 0; dx < 2; dx++) {
                  int bcx = 2 * BX + dx, bcy = 2 * BY + dy;
                  if (bcx >= params->bc_bx || bcy >= params->bc_by)
                     continue;
                  char *sblk = params->src +
                     ((size_t)bcy * params->block_x_src + bcx) * block_size;
                  unsigned char *dsub = scratch + (dy * 4) * 32 + (dx * 4) * 4;
                  switch (params->format) {
                  case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
                  case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
                  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                  case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
                     bcdec_bc1(sblk, dsub, 32); break;
                  case VK_FORMAT_BC2_SRGB_BLOCK:
                  case VK_FORMAT_BC2_UNORM_BLOCK:
                     bcdec_bc2(sblk, dsub, 32); break;
                  case VK_FORMAT_BC3_UNORM_BLOCK:
                  case VK_FORMAT_BC3_SRGB_BLOCK:
                     bcdec_bc3(sblk, dsub, 32); break;
                  case VK_FORMAT_BC7_SRGB_BLOCK:
                  case VK_FORMAT_BC7_UNORM_BLOCK:
                     bcdec_bc7(sblk, dsub, 32); break;
                  default: break;
                  }
               }
            }
            astc_encode_block_8x8(scratch, params->has_alpha,
               (unsigned char *)dst_base + ((size_t)BY * params->block_x + BX) * 16);
         }
      }
      return NULL;
   }

   for (int by = 0; by < params->block_y_count; by++) {
      for (int bx = 0; bx < params->block_x; bx++) {
         int pixel_x = (bx * 4);
         int pixel_y = (by + params->block_y_start) * 4;
         /* Explicit source addressing using the source row block stride, so a
          * padded bufferRowLength does not misalign subsequent rows. */
         char *src = params->src +
            ((size_t)by * params->block_x_src + bx) * block_size;

         /* ASTC target: decode the BCn block into a 4x4 RGBA scratch, then
          * re-encode it as one ASTC 4x4 block written to the block grid. */
         if (params->astc) {
            uint8_t scratch[64];
            switch (params->format) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
               bcdec_bc1(src, scratch, 16);
               break;
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK:
               bcdec_bc2(src, scratch, 16);
               break;
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
               bcdec_bc3(src, scratch, 16);
               break;
            case VK_FORMAT_BC7_SRGB_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
               bcdec_bc7(src, scratch, 16);
               break;
            default:
               break;
            }
            int block_index = (by + params->block_y_start) * params->block_x + bx;
            astc_encode_block_4x4(scratch, params->has_alpha,
               (uint8_t *)dst_base + (size_t)block_index * 16);
            continue;
         }

         char *dst = dst_base + (pixel_y * params->stride) + (pixel_x * params->texel_size);
         if (!dst || !src)
            return NULL;

         switch (params->format) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
               bcdec_bc1(src, dst, params->stride);
               break;
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK:
               bcdec_bc2(src, dst, params->stride);
               break;
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
               bcdec_bc3(src, dst, params->stride);
               break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
            case VK_FORMAT_BC4_SNORM_BLOCK:
               bcdec_bc4(src, dst, params->stride, params->format == VK_FORMAT_BC4_SNORM_BLOCK);
               break;
            case VK_FORMAT_BC5_SNORM_BLOCK:
            case VK_FORMAT_BC5_UNORM_BLOCK:
               bcdec_bc5(src, dst, params->stride, params->format == VK_FORMAT_BC5_SNORM_BLOCK);
               break;
            case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
               bcdec_bc6h_half(src, dst, (params->stride / params->texel_size) * 3, params->format == VK_FORMAT_BC6H_SFLOAT_BLOCK);
               break;
            case VK_FORMAT_BC7_SRGB_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
               bcdec_bc7(src, dst, params->stride);
               break;
            default:
               break;
         }
      }
   }
   
   return NULL;
}

VkFormat 
get_decode_format_for_bcn(VkFormat bcn_format)
{
   switch(bcn_format) {
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
         return VK_FORMAT_R8G8B8A8_SRGB;
      case VK_FORMAT_BC4_UNORM_BLOCK:
         return VK_FORMAT_R8_UNORM;
      case VK_FORMAT_BC4_SNORM_BLOCK:
         return VK_FORMAT_R8_SNORM;
      case VK_FORMAT_BC5_UNORM_BLOCK:
          return VK_FORMAT_R8G8_UNORM;
      case VK_FORMAT_BC5_SNORM_BLOCK:
         return VK_FORMAT_R8G8_SNORM;
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
         return VK_FORMAT_R16G16B16_SFLOAT;
      default:
         return VK_FORMAT_R8G8B8A8_UNORM;
   }
}

/* Storage (image) format. For BC1/2/3/7 this is ASTC 4x4 (kept compressed);
 * BC4/5/6H stay uncompressed (decoded). */
VkFormat
get_format_for_bcn(VkFormat bcn_format)
{
   if (astc_enabled()) {
      int b8 = astc_block8();
      switch (bcn_format) {
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
         return b8 ? VK_FORMAT_ASTC_8x8_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
         return b8 ? VK_FORMAT_ASTC_8x8_UNORM_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      default:
         break;
      }
   }
   return get_decode_format_for_bcn(bcn_format);
}

/* Bytes needed to hold the transcoded upload for a w*h mip of this BCn format:
 * ASTC block bytes for ASTC targets, else linear decoded size. */
size_t
bcn_upload_size(VkFormat bcn_format, int w, int h)
{
   VkFormat img = get_format_for_bcn(bcn_format);
   if (is_astc_8x8(img))
      return (size_t)((w + 7) / 8) * ((h + 7) / 8) * 16;
   if (is_astc_4x4(img))
      return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
   return (size_t)w * h *
      get_texel_size_for_format(get_decode_format_for_bcn(bcn_format));
}

int 
get_texel_size_for_format(VkFormat format) 
{
   switch (format) {
      case VK_FORMAT_R16G16B16_SFLOAT:
         return 6;
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_SNORM:
         return 2;
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_SNORM:
         return 1;
      default:
         return 4;
   }
}

int
is_emulated_bcn(struct wrapper_physical_device *pdev, VkFormat format)
{
   switch(format) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
         if (pdev->emulate_bcn == 3 && 
             pdev->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY)
         {
            return 0;
         }
         else if (pdev->emulate_bcn > 1) {
            return 1;
         } else {
            return 0;
         }
         break;
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK: 
      case VK_FORMAT_BC7_SRGB_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
         if (pdev->emulate_bcn > 1)
            return 1;
         else
            return 0;
         break;
      default:
         return 0;
   }
}

void
decompress_bcn_format(void *srcBuffer,
					  void *dstBuffer,
					  int w,
					  int h,
					  int src_w,
					  VkFormat format,
					  int offset)
{
   static int wrapper_mark_bcn =  -1;
   static int wrapper_no_bcn_thread = -1;
   static int wrapper_use_bcn_cache = -1;
   static char *wrapper_cache_path = NULL;

   if (wrapper_mark_bcn == -1)
      wrapper_mark_bcn = getenv("WRAPPER_MARK_BCN") && atoi(getenv("WRAPPER_MARK_BCN"));

   if (wrapper_no_bcn_thread == -1)
      wrapper_no_bcn_thread = getenv("WRAPPER_NO_BCN_THREAD") && atoi(getenv("WRAPPER_NO_BCN_THREAD"));

   if (wrapper_use_bcn_cache == -1)
      wrapper_use_bcn_cache = getenv("WRAPPER_USE_BCN_CACHE") ? atoi(getenv("WRAPPER_USE_BCN_CACHE")) : 0;

   if (wrapper_cache_path == NULL)
      wrapper_cache_path = getenv("WRAPPER_CACHE_PATH") ? getenv("WRAPPER_CACHE_PATH") : WRAPPER_CACHE_DIR;

   int astc = is_astc_4x4(get_format_for_bcn(format));
   int astc8 = is_astc_8x8(get_format_for_bcn(format));
   int has_alpha = bcn_has_alpha(format);
   int texel_size = get_texel_size_for_format(get_decode_format_for_bcn(format));
   int block_size = get_block_size(format);
   int block_x = (w + 3) / 4;
   /* source row stride in blocks (bufferRowLength may pad rows wider than w) */
   int block_x_src = ((src_w > 0 ? src_w : w) + 3) / 4;
   int block_y = (h + 3) / 4;
   int block_x8 = (w + 7) / 8;
   int block_y8 = (h + 7) / 8;
   int stride = w * texel_size;
   int compressed_size = (block_x * block_y * block_size);
   int uncompressed_size = astc8 ? (block_x8 * block_y8 * 16)
                                 : (astc ? (block_x * block_y * 16) : (w * h * texel_size));
   char *src = srcBuffer + offset;
   char *dst = dstBuffer;

   if (wrapper_mark_bcn && !astc) {
      WRAPPER_LOG(bcn, "Filling %dx%d BCn %d texture with custom color", w, h, format);

      for (int i = 0; i < h; i++) {
         for (int j = 0; j < w; j++) {
            dst = dstBuffer + (i * stride) + (j * texel_size);
            
            switch(format) {
               case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
               case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
               case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
               case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
                  /* Yellow */
                  dst[0] = 0xFF;
                  dst[1] = 0xFF;
                  dst[2] = 0;
                  dst[3] = 255;
                  break;
               case VK_FORMAT_BC2_SRGB_BLOCK:
               case VK_FORMAT_BC2_UNORM_BLOCK:
                  /* Blue */
                  dst[0] = 0;
                  dst[1] = 0;
                  dst[2] = 0xFF;
                  dst[3] = 255;
                  break;
                case VK_FORMAT_BC3_UNORM_BLOCK:
                case VK_FORMAT_BC3_SRGB_BLOCK:
                  /* Light Blue */
                  dst[0] = 0;
                  dst[1] = 0xFF;
                  dst[2] = 0xFF;
                  dst[3] = 255;
                  break;
               case VK_FORMAT_BC4_UNORM_BLOCK:
               case VK_FORMAT_BC4_SNORM_BLOCK:
                  /* Red */
                  dst[0] = 0xFF;
                  break;
               case VK_FORMAT_BC5_UNORM_BLOCK:
               case VK_FORMAT_BC5_SNORM_BLOCK:
                  /* Green */
                  dst[0] = 0;
                  dst[1] = 0xFF;
                  break;
               case VK_FORMAT_BC6H_SFLOAT_BLOCK:
               case VK_FORMAT_BC6H_UFLOAT_BLOCK:
                  /* Purple */
                  dst[0] = 0x90;
                  dst[1] = 0x40;
                  dst[2] = 0xA0;
                  break;
               case VK_FORMAT_BC7_UNORM_BLOCK:
               case VK_FORMAT_BC7_SRGB_BLOCK:
                  /* Black */
                  dst[0] = 0xFF;
                  dst[1] = 0;
                  dst[2] = 0xFF;
                  dst[3] = 255;
                  break;
               default:
                  break;
            }
         }
      }

      return;
   }

   /* Optional disk cache of the transcoded output, keyed by a hash of the
    * compressed source. Skips decode+encode on subsequent loads. Only touched
    * when explicitly enabled, so there is zero overhead by default. */
   char *cache_filename = NULL;
   if (wrapper_use_bcn_cache) {
      CREATE_FOLDER(wrapper_cache_path, 0700);
      XXH64_hash_t hash = XXH64(src, compressed_size, 0);
      asprintf(&cache_filename, "%s/%s_%llu.cache", wrapper_cache_path,
         get_executable_name(), (unsigned long long)hash);

      if (access(cache_filename, F_OK) == 0) {
         FILE *fp = fopen(cache_filename, "rb");
         if (fp) {
            size_t length = fread(dst, 1, uncompressed_size, fp);
            fclose(fp);
            if (length == uncompressed_size) {
               WRAPPER_LOG(bcn, "Restored texture %s from cache", cache_filename);
               free(cache_filename);
               return;
            }
            unlink(cache_filename);
         }
      }
   }


   if (astc8) {
      /* 8x8 ASTC: each block covers a 2x2 group of BC blocks (8 = 2*4, aligned).
       * Decode the 4 BC blocks into an 8x8 RGBA scratch, then encode one ASTC
       * 8x8 block. Threaded over 8x8-block rows. */
      int core_count = sysconf(_SC_NPROCESSORS_CONF);
      int num_threads = (block_y8 >= core_count) ? core_count : (block_y8 >= 4 ? 4 : 1);
      int rows_per = block_y8 / num_threads, rem = block_y8 % num_threads;
      pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
      struct decompression_params *args = malloc(sizeof(struct decompression_params) * num_threads);
      int cur = 0;
      for (int i = 0; i < num_threads; i++) {
         int rows = rows_per + ((i < rem) ? 1 : 0);
         args[i].src = src;                 /* full src; absolute BC addressing */
         args[i].dst = dst;
         args[i].block_x = block_x8;         /* ASTC 8x8 grid width */
         args[i].block_x_src = block_x_src;  /* BC source row stride (blocks) */
         args[i].format = format;
         args[i].block_y_count = rows;
         args[i].block_y_start = cur;
         args[i].texel_size = block_size;    /* BC block bytes */
         args[i].bc_bx = block_x;
         args[i].bc_by = block_y;
         args[i].astc = 0;
         args[i].astc8 = 1;
         args[i].has_alpha = has_alpha;
         pthread_create(&threads[i], NULL, decompression_routine, &args[i]);
         cur += rows;
      }
      for (int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);
      free(threads);
      free(args);
   } else if (wrapper_no_bcn_thread) {
      WRAPPER_LOG(bcn, "Decompressing %dx%d BCN %d texture from main thread",
         w, h, format);
         
      struct decompression_params args[1];
      args[0].src = src;
      args[0].dst = dst;
      args[0].block_x = block_x;
      args[0].block_x_src = block_x_src;
      args[0].format = format;
      args[0].block_y_count = block_y;
      args[0].block_y_start = 0;
      args[0].stride = stride;
      args[0].texel_size = texel_size;
      args[0].astc = astc;
      args[0].astc8 = 0;
      args[0].has_alpha = has_alpha;
      decompression_routine(&args[0]);
   } else {
      int core_count = sysconf(_SC_NPROCESSORS_CONF);
      int num_threads;
      if (block_y >= core_count)
         num_threads = core_count;
      else if (block_y >= 4)
         num_threads = 4;
      else
         num_threads = 1;
      
      int rows_per_thread = block_y / num_threads;
      int rem = block_y % num_threads;

      pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
      struct decompression_params *args = malloc(sizeof(struct decompression_params) * num_threads);
      int current_row = 0;

      WRAPPER_LOG(bcn, "Decompressing %dx%d BCN %d texture using %d threads",
         w, h, format, num_threads);

      for (int i = 0; i < num_threads; i++) {
         int rows = rows_per_thread + ((i < rem) ? 1 : 0);
         args[i].src = src + ((size_t)current_row * block_x_src * block_size);
         args[i].dst = dst;
         args[i].block_x = block_x;
         args[i].block_x_src = block_x_src;
         args[i].format = format;
         args[i].block_y_count = rows;
         args[i].block_y_start = current_row;
         args[i].stride = stride;
         args[i].texel_size = texel_size;
         args[i].astc = astc;
         args[i].astc8 = 0;
         args[i].has_alpha = has_alpha;
         pthread_create(&threads[i], NULL, decompression_routine, &args[i]);
         current_row += rows;
      }
   
      for (int i = 0; i < num_threads; i++) {
         pthread_join(threads[i], NULL);
      }

      free(threads);
      free(args);
   }

   if (wrapper_use_bcn_cache && cache_filename) {
      FILE *fp = fopen(cache_filename, "wb");
      if (fp) {
         size_t length = fwrite(dst, 1, uncompressed_size, fp);
         fclose(fp);
         if (length == uncompressed_size)
            WRAPPER_LOG(bcn, "Saved texture %s to cache", cache_filename);
         else {
            WRAPPER_LOG(bcn, "Failed to save texture %s to cache", cache_filename);
            unlink(cache_filename);
         }
      }
   }

   free(cache_filename);
   
}
