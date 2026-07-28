#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FormatTableResolution {
    uintptr_t var_address;
    uintptr_t table_address;
    bool is_indirect;
};

typedef struct {
    uintptr_t base_address;
    char path[1024];
} ModuleInfo;

struct FormatTableResolution resolve_gformat_table(void* is_depth_stencil_live_addr);
uintptr_t get_hidden_symbol_offset(const char* library_path, const char* symbol_name);
ModuleInfo get_mali_module_info(const char* library_keyword);
int hook_libGLES_mali();

extern char *(*format_to_string)(unsigned short format);

#ifdef __cplusplus
}
#endif
