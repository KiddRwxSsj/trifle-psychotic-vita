#include <string.h>

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#include "main.h"
#include "game_data.h"
#include "rendering.h"
#include "input.h"
#include "progress.h"
#include "level_initialization.h"

#define TARGET_HZ 30.0f

#define VITA_SCREEN_WIDTH 960
#define VITA_SCREEN_HEIGHT 544
#define VITA_SAVE_DIR "ux0:data/trifle_psychotic"

/* deadzone for the sdl gamecontroller axes, out of 32767 */
#define STICK_DEADZONE 8000

typedef struct sdl_data
{
    b32 initialized;
    i32 screen_width;
    i32 screen_height;

    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_GameController* pad;

    SDL_Texture* tileset_texture;
    SDL_Texture* ui_font_texture;
    SDL_Texture* title_font_texture;
    SDL_Texture* charset_texture;
    SDL_Texture* explosion_texture;
    SDL_Texture* background_desert_texture;
    SDL_Texture* background_ice_desert_texture;
    SDL_Texture* background_clouds_texture;
    SDL_Texture* background_red_planet_sky_texture;
    SDL_Texture* background_red_planet_desert_texture;
    SDL_Texture* background_planet_orbit_texture;
    SDL_Texture* background_title_screen_texture;

    string_builder path_buffer;

    string_ref preferences_file_path;
    Mix_Music* music;
    b32 audio_available;

    SceUID permanent_arena_memblock;
    SceUID transient_arena_memblock;

    /* virtual cursor driven by the right stick, aiming works like the pc mouse did */
    r32 aim_x;
    r32 aim_y;
} sdl_data;

sdl_data GLOBAL_SDL_DATA;

/* vitasdk crt0 reads these globals before main() runs. the permanent+transient
   arenas no longer come out of this heap (see allocate_vita_mem_block below) -
   they're carved out of their own sceKernelAllocMemBlock pool before SDL/IMG/Mix
   touch anything, so they can't be fragmented by texture/audio-buffer churn.
   this heap now only has to cover SDL/SDL_image/SDL_mixer's own allocations
   (decoded PNGs, mixer buffers, etc) with headroom to spare.
   stack bumped too since tmx parsing recurses. */
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
unsigned int sceLibcHeapSize = 32 * 1024 * 1024;
int sceLibcHeapExtendedAlloc = 1;

void render_list_to_output(render_list* render_list);

/* allocates size bytes as their own dedicated SCE_KERNEL_MEMBLOCK_TYPE_USER_RW
   block, independent of the newlib/SDL_malloc heap. size must be a multiple of
   4KiB - the arena sizes below are already 1MB-aligned so this holds trivially.
   out_uid receives the block's SceUID so it can be released with
   sceKernelFreeMemBlock later; the uid is set to -1 and NULL is returned on
   failure so callers only need to check the returned pointer. */
internal void* allocate_vita_mem_block(const char* name, u32 size, SceUID* out_uid)
{
    *out_uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (*out_uid < 0)
    {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "allocate_vita_mem_block: sceKernelAllocMemBlock('%s', %u bytes) failed: 0x%08X",
            name, (unsigned int)size, (unsigned int)*out_uid);
        *out_uid = -1;
        return NULL;
    }

    void* base = NULL;
    int rc = sceKernelGetMemBlockBase(*out_uid, &base);
    if (rc < 0 || base == NULL)
    {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "allocate_vita_mem_block: sceKernelGetMemBlockBase('%s') failed: 0x%08X",
            name, (unsigned int)rc);
        sceKernelFreeMemBlock(*out_uid);
        *out_uid = -1;
        return NULL;
    }

    return base;
}

r32 get_elapsed_miliseconds(u32 start_counter, u32 end_counter)
{
    r32 result = ((end_counter - start_counter) * 1000) / (r64)SDL_GetPerformanceFrequency();
    return result;
}

SDL_Color get_sdl_color(v4 color)
{
    SDL_Color result = { (Uint8)color.r, (Uint8)color.g, (Uint8)color.b, (Uint8)color.a };
    return result;
}

SDL_Rect get_sdl_rect(rect rect)
{
    SDL_Rect result = {0};
    result.x = (int)round(rect.min_corner.x);
    result.y = (int)round(rect.min_corner.y);
    result.w = (int)round(rect.max_corner.x - rect.min_corner.x);
    result.h = (int)round(rect.max_corner.y - rect.min_corner.y);
    return result;
}

void print_sdl_error(void)
{
#ifdef TRIFLE_DEBUG
    const char* error = SDL_GetError();
    printf("SDL error: %s\n", error);
    invalid_code_path;
#endif
}

void print_sdl_image_error(void)
{
#ifdef TRIFLE_DEBUG
    const char* error = IMG_GetError();
    printf("SDL_image error: %s\n", error);
    invalid_code_path;
#endif
}

/* unlike print_sdl_error/print_sdl_image_error, this one must NOT trap.
   both of its call sites (the Mix_Init flag check and start_playing_music's
   Mix_LoadMUS check) are explicitly documented as non-fatal - a missing or
   unsupported audio file is a data/packaging problem, not a code bug, and
   the game is fully playable without music. invalid_code_path here used to
   turn "audio/foo.ogg didn't ship in the vpk" into an unrelated-looking
   data abort at NULL indistinguishable from real memory corruption. */
void print_sdl_mixer_error(void)
{
#ifdef TRIFLE_DEBUG
    const char* error = Mix_GetError();
    printf("SDL_mixer error (non-fatal, continuing without audio): %s\n", error);
#endif
}

/* assets are bundled under the vpk root (app0:), everything else that
   comes in already prefixed with ux0:/app0: is left alone */
static void vita_resolve_path(char* out, size_t out_size, const char* path)
{
    if (strncmp(path, "ux0:", 4) == 0 || strncmp(path, "app0:", 5) == 0)
    {
        snprintf(out, out_size, "%s", path);
    }
    else
    {
        snprintf(out, out_size, "app0:/%s", path);
    }
}
