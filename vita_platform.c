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

void load_image(SDL_Renderer* renderer, SDL_Texture** place_to_load, const char* file_path, b32* success)
{
    char full_path[512];
    vita_resolve_path(full_path, sizeof(full_path), file_path);

    SDL_Surface* loaded_surface = IMG_Load(full_path);
    if (loaded_surface)
    {
        /* vita's IMG_Load/png codec can hand back a surface with no alpha
           channel (e.g. 24bpp RGB) even when the source PNG has one, so
           SDL_SetTextureBlendMode(BLEND) below has nothing to blend against -
           every texel reads back fully opaque and glyphs/sprites keep their
           solid background box. Force a real RGBA pixel format first. */
        SDL_Surface* rgba_surface = SDL_ConvertSurfaceFormat(loaded_surface, SDL_PIXELFORMAT_RGBA32, 0);
        if (rgba_surface)
        {
            SDL_FreeSurface(loaded_surface);
            loaded_surface = rgba_surface;
        }

        *place_to_load = SDL_CreateTextureFromSurface(renderer, loaded_surface);
        SDL_FreeSurface(loaded_surface);

        /* vita's SDL2 renderer backend does not infer blend mode from the
           source surface's alpha channel like desktop SDL2 does - textures
           default to SDL_BLENDMODE_NONE, so alpha is ignored and glyphs/
           sprites render with an opaque background box. Force it. */
        SDL_SetTextureBlendMode(*place_to_load, SDL_BLENDMODE_BLEND);
    }
    else
    {
        print_sdl_image_error();
        *success = false;
    }
}

read_file_result read_file(const char* path)
{
    read_file_result result = {0};

    char full_path[512];
    vita_resolve_path(full_path, sizeof(full_path), path);

    SDL_RWops* file = SDL_RWFromFile(full_path, "r");
    if (file)
    {
        int64_t file_size = SDL_RWsize(file);
        if (file_size > 0 // was `!= -1`, only caught SDL's own sentinel, not any other negative return
            && file_size < 1024 * 1024 * 5) // safeguard
        {
            result.size = file_size;
            result.contents = calloc(file_size + 1, sizeof(byte));

            if (result.contents != 0)
            {
                for (int byte_index = 0;
                    byte_index < file_size;
                    ++byte_index)
                {
                    SDL_RWread(file, (void*)((char*)result.contents + byte_index), sizeof(char), 1);
                }
                *((char*)result.contents + file_size) = 0;
            }
        }
        else
        {
            print_sdl_error();
        }

        SDL_RWclose(file);
    }

    return result;
}

void save_file(const char* path, write_to_file contents)
{
    char full_path[512];
    vita_resolve_path(full_path, sizeof(full_path), path);

    SDL_RWops* file = SDL_RWFromFile(full_path, "w+b");
    if (file != NULL)
    {
        int bytes_written = SDL_RWwrite(file, contents.buffer, sizeof(char), contents.length);
        if (bytes_written < contents.length)
        {
            print_sdl_error();
        }

        SDL_RWclose(file);
    }
}

void store_preferences_file_path(sdl_data* sdl, memory_arena* permanent_arena)
{
    sceIoMkdir(VITA_SAVE_DIR, 0777);

    empty_string_builder(&sdl->path_buffer);
    push_c_string_to_builder(&sdl->path_buffer, VITA_SAVE_DIR "/completed_levels.txt");
    safe_push_null_terminator_to_builder(&sdl->path_buffer);
    string_ref path = get_string_from_string_builder(&sdl->path_buffer);
    sdl->preferences_file_path = copy_string(permanent_arena, path);
}

read_file_result load_prefs(void)
{
    read_file_result result = {0};
    if (GLOBAL_SDL_DATA.preferences_file_path.ptr != NULL)
    {
        result = read_file(GLOBAL_SDL_DATA.preferences_file_path.ptr);
    }
    return result;
}

void save_prefs(write_to_file contents)
{
    if (GLOBAL_SDL_DATA.preferences_file_path.ptr != NULL)
    {
        save_file(GLOBAL_SDL_DATA.preferences_file_path.ptr, contents);
    }
}

void start_playing_music(string_ref audio_file_name)
{
    sdl_data* sdl = &GLOBAL_SDL_DATA;
    if (sdl->audio_available && audio_file_name.string_size > 0)
    {
        if (sdl->music != NULL)
        {
            Mix_HaltMusic();
            Mix_FreeMusic(sdl->music);
            sdl->music = NULL;
        }

        empty_string_builder(&sdl->path_buffer);
        push_c_string_to_builder(&sdl->path_buffer, "audio/");
        push_string_to_builder(&sdl->path_buffer, audio_file_name);
        if (false == (ends_with(audio_file_name, ".ogg")
            || ends_with(audio_file_name, ".mp3")
            || ends_with(audio_file_name, ".wav")))
        {
            push_c_string_to_builder(&sdl->path_buffer, ".ogg");
        }
        safe_push_null_terminator_to_builder(&sdl->path_buffer);

        char full_path[512];
        vita_resolve_path(full_path, sizeof(full_path), sdl->path_buffer.ptr);

        sdl->music = Mix_LoadMUS(full_path);
        if (sdl->music == NULL)
        {
            print_sdl_mixer_error();
        }
        else
        {
            Mix_FadeInMusic(GLOBAL_SDL_DATA.music, -1, 4000); // -1 means loop infinitely
        }
    }
}

void stop_playing_music(int fade_out_ms)
{
    if (GLOBAL_SDL_DATA.audio_available && Mix_PlayingMusic() != 0)
    {
        Mix_FadeOutMusic(fade_out_ms);
    }
}

sdl_data init_sdl(void)
{
    sdl_data sdl_game = {0};
    b32 success = true;

    int init = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    if (init == 0) // success
    {
        SDL_ShowCursor(SDL_DISABLE);

        if (SDL_NumJoysticks() > 0)
        {
            sdl_game.pad = SDL_GameControllerOpen(0);
        }

        sdl_game.window = SDL_CreateWindow("Trifle Psychotic",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            VITA_SCREEN_WIDTH, VITA_SCREEN_HEIGHT, SDL_WINDOW_SHOWN);

        if (sdl_game.window)
        {
            sdl_game.renderer = SDL_CreateRenderer(sdl_game.window, -1, SDL_RENDERER_ACCELERATED);
            if (sdl_game.renderer)
            {
                /* keep the original 320x240 logical resolution, sdl letterboxes
                   it into the 960x544 vita screen automatically */
                SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
                SDL_RenderSetLogicalSize(sdl_game.renderer,
                    SCREEN_WIDTH / SCALING_FACTOR,
                    SCREEN_HEIGHT / SCALING_FACTOR);

                SDL_GetRendererOutputSize(sdl_game.renderer,
                    &sdl_game.screen_width, &sdl_game.screen_height);

                SDL_SetRenderDrawColor(sdl_game.renderer, 0xFF, 0xFF, 0xFF, 0xFF);

                int img_flags = IMG_INIT_PNG;
                if (IMG_Init(img_flags) & img_flags)
                {
                    load_image(sdl_game.renderer, &sdl_game.tileset_texture, "gfx/tileset.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.ui_font_texture, "gfx/ui_font.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.title_font_texture, "gfx/title_font.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.charset_texture, "gfx/charset.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.explosion_texture, "gfx/explosions.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_desert_texture, "gfx/background_desert.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_ice_desert_texture, "gfx/background_ice_desert.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_clouds_texture, "gfx/background_clouds.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_red_planet_sky_texture, "gfx/background_red_planet_sky.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_red_planet_desert_texture, "gfx/background_red_planet_desert.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_planet_orbit_texture, "gfx/background_planet_orbit.png", &success);
                    load_image(sdl_game.renderer, &sdl_game.background_title_screen_texture, "gfx/background_title_screen.png", &success);
                }
                else
                {
                    print_sdl_image_error();
                    success = false;
                }

                /* audio is optional: a device open failure or missing codec
                   must never take down the whole boot. flags matches the
                   codec libs linked in CMakeLists (vorbis/ogg, opus, mod).
                   Mix_Init failing here is also non-fatal, some of those
                   codecs may not be needed and vitasdk's static build
                   tolerates a partial mask. */
                int wanted_mix_flags = MIX_INIT_OGG | MIX_INIT_OPUS | MIX_INIT_MOD;
                int got_mix_flags = Mix_Init(wanted_mix_flags);
                if ((got_mix_flags & wanted_mix_flags) != wanted_mix_flags)
                {
                    print_sdl_mixer_error();
                }

                if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
                {
                    print_sdl_mixer_error();
                    sdl_game.audio_available = false;
                }
                else
                {
                    sdl_game.audio_available = true;
                }
            }
            else
            {
                print_sdl_error();
                success = false;
            }
        }
        else
        {
            print_sdl_error();
            success = false;
        }
    }
    else
    {
        print_sdl_error();
        success = false;
    }

    /* start the aim cursor centered, facing right, same as pc mouse default.
       mouse_x/mouse_y are consumed by player.c/ui.c as coordinates in the
       SCREEN_WIDTH x SCREEN_HEIGHT (640x480) space the pc build's window
       and SDL_GetMouseState use - NOT the vita's physical 960x544 panel -
       so aim_x/aim_y must live in that same 640x480 space, not VITA_SCREEN_*. */
    sdl_game.aim_x = (r32)(SCREEN_WIDTH / 2 + 60);
    sdl_game.aim_y = (r32)(SCREEN_HEIGHT / 2);

    if (success)
    {
        sdl_game.initialized = true;
        return sdl_game;
    }
    else
    {
        return (sdl_data){0};
    }
}

static r32 apply_deadzone(Sint16 axis_value)
{
    i32 magnitude = (axis_value < 0) ? -(i32)axis_value : (i32)axis_value;

    if (magnitude < STICK_DEADZONE)
    {
        return 0.0f;
    }

    /* rescale the post-deadzone range to 0..1 instead of jumping straight
       from 0.0 to (DEADZONE/32767) - the old hard cutoff produced a step
       discontinuity that felt like a digital switch instead of an analog
       ramp, and fed straight into the cursor's absolute positioning below. */
    r32 normalized = (r32)(magnitude - STICK_DEADZONE) / (r32)(32767 - STICK_DEADZONE);
    if (normalized > 1.0f)
    {
        normalized = 1.0f;
    }

    return (axis_value < 0) ? -normalized : normalized;
}

game_input get_input_from_sdl_events(void)
{
    game_input new_input = {0};
    sdl_data* sdl = &GLOBAL_SDL_DATA;

    SDL_Event e = {0};
    while (SDL_PollEvent(&e) != 0)
    {
        if (e.type == SDL_QUIT)
        {
            new_input.quit = true;
        }
    }

    const Uint8* state = SDL_GetKeyboardState(NULL);
    if (state[SDL_SCANCODE_UP])    new_input.up.number_of_presses++;
    if (state[SDL_SCANCODE_DOWN])  new_input.down.number_of_presses++;
    if (state[SDL_SCANCODE_LEFT])  new_input.left.number_of_presses++;
    if (state[SDL_SCANCODE_RIGHT]) new_input.right.number_of_presses++;

    if (sdl->pad)
    {
        r32 lx = apply_deadzone(SDL_GameControllerGetAxis(sdl->pad, SDL_CONTROLLER_AXIS_LEFTX));
        r32 ly = apply_deadzone(SDL_GameControllerGetAxis(sdl->pad, SDL_CONTROLLER_AXIS_LEFTY));

        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -0.4f)
            new_input.up.number_of_presses++;
        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 0.4f)
            new_input.down.number_of_presses++;
        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -0.4f)
            new_input.left.number_of_presses++;
        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 0.4f)
            new_input.right.number_of_presses++;

        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_START))
            new_input.escape.number_of_presses++;

        if (SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            || SDL_GameControllerGetButton(sdl->pad, SDL_CONTROLLER_BUTTON_A))
        {
            new_input.is_fire_button_held = true;
        }

        /* right stick moves a virtual cursor. previously this snapped aim_x/
           aim_y to an absolute point on a 200px circle around screen center
           every frame - any stick deflection change (even small ones, e.g.
           crossing the deadzone) caused the cursor to jump straight to the
           new point instead of gliding, which read as teleportation. drive
           it as velocity integrated onto the current position instead. */
        r32 rx = apply_deadzone(SDL_GameControllerGetAxis(sdl->pad, SDL_CONTROLLER_AXIS_RIGHTX));
        r32 ry = apply_deadzone(SDL_GameControllerGetAxis(sdl->pad, SDL_CONTROLLER_AXIS_RIGHTY));
        if (rx != 0.0f || ry != 0.0f)
        {
            r32 cursor_speed = 10.0f; /* pixels per frame at full deflection */
            sdl->aim_x += rx * cursor_speed;
            sdl->aim_y += ry * cursor_speed;

            /* clamp to the logical SCREEN_WIDTH x SCREEN_HEIGHT (640x480)
               space mouse_x/mouse_y are read in, not the vita's physical
               960x544 panel - clamping to the latter let the cursor drift
               well outside the actual game/UI coordinate range. */
            if (sdl->aim_x < 0.0f) sdl->aim_x = 0.0f;
            if (sdl->aim_x > (r32)SCREEN_WIDTH) sdl->aim_x = (r32)SCREEN_WIDTH;
            if (sdl->aim_y < 0.0f) sdl->aim_y = 0.0f;
            if (sdl->aim_y > (r32)SCREEN_HEIGHT) sdl->aim_y = (r32)SCREEN_HEIGHT;
        }
    }

    new_input.mouse_x = (i32)sdl->aim_x;
    new_input.mouse_y = (i32)sdl->aim_y;

    return new_input;
}

game_state* initialize_game_state(memory_arena* permanent_arena, memory_arena* transient_arena)
{
    game_state* game = push_struct(permanent_arena, game_state);
    if (game == NULL)
    {
        sceKernelExitProcess(0);
    }

    game->arena = permanent_arena;
    game->transient_arena = transient_arena;

    game->platform.read_file = &read_file;
    game->platform.save_file = &save_file;
    game->platform.load_prefs = &load_prefs;
    game->platform.save_prefs = &save_prefs;
    game->platform.start_playing_music = &start_playing_music;
    game->platform.stop_playing_music = &stop_playing_music;
    game->platform.render_list_to_output = &render_list_to_output;

    game->static_data = push_struct(permanent_arena, static_game_data);
    if (game->static_data == NULL)
    {
        sceKernelExitProcess(0);
    }
    load_static_game_data(&game->platform, game->static_data, permanent_arena, transient_arena);

    game->render.max_push_buffer_size = megabytes_to_bytes(1);
    game->render.push_buffer_base = (u8*)push_size(permanent_arena, game->render.max_push_buffer_size);

    game->level_state = push_struct(permanent_arena, level_state);
    game->level_name_buffer = (char*)push_size(permanent_arena, MAX_LEVEL_NAME_LENGTH);

    initialize_memory_for_checkpoint(game, permanent_arena);

    game->input_buffer = initialize_input_buffer(permanent_arena);

    game->current_scene = SCENE_MAIN_MENU;

    return game;
}

int main(int args_count, char* args[])
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* carve the arenas out first, before SDL/IMG/Mix ever run - a pristine
       heap has no fragmentation to fail a 48MB contiguous request against,
       and a dedicated memblock means SDL's own allocations afterward can
       never eat into or fragment this pool either way. */
    u32 memory_for_permanent_arena_size = megabytes_to_bytes(48);
    SceUID permanent_arena_memblock;
    void* memory_for_permanent_arena = allocate_vita_mem_block(
        "trifle_permanent_arena", memory_for_permanent_arena_size, &permanent_arena_memblock);
    if (memory_for_permanent_arena == NULL)
    {
        sceKernelExitProcess(0);
    }
    memory_arena* permanent_arena = initialize_memory_arena(memory_for_permanent_arena_size, (byte*)memory_for_permanent_arena);
    if (permanent_arena == NULL)
    {
        sceKernelFreeMemBlock(permanent_arena_memblock);
        sceKernelExitProcess(0);
    }

    u32 memory_for_transient_arena_size = megabytes_to_bytes(16);
    SceUID transient_arena_memblock;
    void* memory_for_transient_arena = allocate_vita_mem_block(
        "trifle_transient_arena", memory_for_transient_arena_size, &transient_arena_memblock);
    if (memory_for_transient_arena == NULL)
    {
        sceKernelFreeMemBlock(permanent_arena_memblock);
        sceKernelExitProcess(0);
    }
    memory_arena* transient_arena = initialize_memory_arena(memory_for_transient_arena_size, (byte*)memory_for_transient_arena);
    if (transient_arena == NULL)
    {
        sceKernelFreeMemBlock(permanent_arena_memblock);
        sceKernelFreeMemBlock(transient_arena_memblock);
        sceKernelExitProcess(0);
    }

    GLOBAL_SDL_DATA = init_sdl();
    sdl_data* sdl = &GLOBAL_SDL_DATA;
    sdl->permanent_arena_memblock = permanent_arena_memblock;
    sdl->transient_arena_memblock = transient_arena_memblock;
    if (sdl->initialized)
    {
        bool run = true;

        game_state* game = initialize_game_state(permanent_arena, transient_arena);

        int max_path_length = 512;
        sdl->path_buffer = get_string_builder(game->transient_arena, max_path_length);
        store_preferences_file_path(sdl, game->arena);

        game->show_exit_game_option = false;

        r32 target_elapsed_ms = 1000 / TARGET_HZ;
        r32 elapsed_work_ms = 0;
        r64 delta_time = 1 / TARGET_HZ;

        while (run)
        {
            u32 start_work_counter = SDL_GetPerformanceCounter();

            {
                game_input new_input = get_input_from_sdl_events();

                if (new_input.quit)
                {
                    run = false;
                }

                write_to_input_buffer(&game->input_buffer, &new_input);

                main_game_loop(game, delta_time);

                if (game->exit_game)
                {
                    run = false;
                }
            }

            u32 end_work_counter = SDL_GetPerformanceCounter();

            elapsed_work_ms = get_elapsed_miliseconds(start_work_counter, end_work_counter);
            if (elapsed_work_ms < target_elapsed_ms)
            {
                r32 how_long_to_sleep_ms = target_elapsed_ms - elapsed_work_ms;
                if (how_long_to_sleep_ms > 1)
                {
                    SDL_Delay((Uint32)how_long_to_sleep_ms);
                }

                r32 total_elapsed_ms = get_elapsed_miliseconds(start_work_counter, SDL_GetPerformanceCounter());
                while (target_elapsed_ms > total_elapsed_ms)
                {
                    total_elapsed_ms = get_elapsed_miliseconds(start_work_counter, SDL_GetPerformanceCounter());
                }
            }
        }

        if (game->game_level_memory.arena)
        {
            end_temporary_memory(game->game_level_memory, false);
        }

        check_arena(game->arena);
        check_arena(game->transient_arena);
    }
    else
    {
        invalid_code_path;
    }

    SDL_DestroyTexture(sdl->background_clouds_texture);
    SDL_DestroyTexture(sdl->background_desert_texture);
    SDL_DestroyTexture(sdl->background_ice_desert_texture);
    SDL_DestroyTexture(sdl->background_planet_orbit_texture);
    SDL_DestroyTexture(sdl->background_red_planet_desert_texture);
    SDL_DestroyTexture(sdl->background_red_planet_sky_texture);
    SDL_DestroyTexture(sdl->background_title_screen_texture);
    SDL_DestroyTexture(sdl->tileset_texture);
    SDL_DestroyTexture(sdl->charset_texture);
    SDL_DestroyTexture(sdl->ui_font_texture);
    SDL_DestroyTexture(sdl->title_font_texture);
    SDL_DestroyTexture(sdl->explosion_texture);

    if (sdl->audio_available)
    {
        Mix_HaltMusic();
        Mix_FreeMusic(sdl->music);
    }

    if (sdl->pad)
    {
        SDL_GameControllerClose(sdl->pad);
    }

    SDL_DestroyRenderer(sdl->renderer);
    SDL_DestroyWindow(sdl->window);

    Mix_Quit();
    IMG_Quit();
    SDL_Quit();

    if (sdl->transient_arena_memblock >= 0)
    {
        sceKernelFreeMemBlock(sdl->transient_arena_memblock);
    }
    if (sdl->permanent_arena_memblock >= 0)
    {
        sceKernelFreeMemBlock(sdl->permanent_arena_memblock);
    }

    sceKernelExitProcess(0);
    return 0;
}

void render_rect(sdl_data* sdl, rect rectangle)
{
    rectangle.min_corner.x = round(rectangle.min_corner.x);
    rectangle.min_corner.y = round(rectangle.min_corner.y);
    rectangle.max_corner.x = round(rectangle.max_corner.x);
    rectangle.max_corner.y = round(rectangle.max_corner.y);

    SDL_RenderDrawLine(sdl->renderer,
        rectangle.min_corner.x, rectangle.min_corner.y, rectangle.max_corner.x, rectangle.min_corner.y);
    SDL_RenderDrawLine(sdl->renderer,
        rectangle.min_corner.x, rectangle.min_corner.y, rectangle.min_corner.x, rectangle.max_corner.y);
    SDL_RenderDrawLine(sdl->renderer,
        rectangle.max_corner.x, rectangle.min_corner.y, rectangle.max_corner.x, rectangle.max_corner.y);
    SDL_RenderDrawLine(sdl->renderer,
        rectangle.min_corner.x, rectangle.max_corner.y, rectangle.max_corner.x, rectangle.max_corner.y);
}

internal SDL_Texture* get_texture(sdl_data sdl, textures type)
{
    SDL_Texture* result = NULL;
    switch (type)
    {
        case TEXTURE_NONE: { result = NULL; }; break;
        case TEXTURE_TILESET: { result = sdl.tileset_texture; }; break;
        case TEXTURE_FONT: { result = sdl.ui_font_texture; }; break;
        case TEXTURE_TITLE_FONT: { result = sdl.title_font_texture; }; break;
        case TEXTURE_CHARSET: { result = sdl.charset_texture; }; break;
        case TEXTURE_EXPLOSION: { result = sdl.explosion_texture; }; break;
        case TEXTURE_BACKGROUND_DESERT: { result = sdl.background_desert_texture; }; break;
        case TEXTURE_BACKGROUND_ICE_DESERT: { result = sdl.background_ice_desert_texture; }; break;
        case TEXTURE_BACKGROUND_CLOUDS: { result = sdl.background_clouds_texture; }; break;
        case TEXTURE_BACKGROUND_RED_PLANET_SKY: { result = sdl.background_red_planet_sky_texture; }; break;
        case TEXTURE_BACKGROUND_RED_PLANET_DESERT: { result = sdl.background_red_planet_desert_texture; }; break;
        case TEXTURE_BACKGROUND_PLANET_ORBIT: { result = sdl.background_planet_orbit_texture; }; break;
        case TEXTURE_BACKGROUND_TITLE_SCREEN: { result = sdl.background_title_screen_texture; }; break;
        invalid_default_case;
    }
    return result;
}

/* paints solid black over the physical screen area outside the 4:3 game
   content rect - the true fix for the vita gxm pillarbox bug (see bottom
   of render_list_to_output for why SDL_RenderClear can't be trusted for
   this). Must run with logical-size scaling OFF: while logical size is
   active, SDL maps/clips every draw call into the logical content rect,
   so the border area outside it is structurally unreachable through the
   normal draw path no matter what viewport is requested - this is why
   the previous "reset viewport then SDL_RenderClear" approach still left
   garbage on real hardware. Runs every frame (not just on resize) because
   the vita swaps between multiple physical framebuffers, and only the
   buffer actually presented this call gets repainted. */
internal void paint_pillarbox_borders(SDL_Renderer* renderer)
{
    SDL_RenderSetLogicalSize(renderer, 0, 0);
    SDL_RenderSetViewport(renderer, NULL);

    int output_w = 0;
    int output_h = 0;
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);

    r32 logical_w = (r32)(SCREEN_WIDTH / SCALING_FACTOR);
    r32 logical_h = (r32)(SCREEN_HEIGHT / SCALING_FACTOR);
    r32 scale_x = (r32)output_w / logical_w;
    r32 scale_y = (r32)output_h / logical_h;
    r32 scale = (scale_x < scale_y) ? scale_x : scale_y;

    int content_w = (int)round(logical_w * scale);
    int content_h = (int)round(logical_h * scale);
    int content_x = (output_w - content_w) / 2;
    int content_y = (output_h - content_h) / 2;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

    if (content_x > 0)
    {
        SDL_Rect left_bar = { 0, 0, content_x, output_h };
        SDL_Rect right_bar = { content_x + content_w, 0, output_w - (content_x + content_w), output_h };
        SDL_RenderFillRect(renderer, &left_bar);
        SDL_RenderFillRect(renderer, &right_bar);
    }

    if (content_y > 0)
    {
        SDL_Rect top_bar = { 0, 0, output_w, content_y };
        SDL_Rect bottom_bar = { 0, content_y + content_h, output_w, output_h - (content_y + content_h) };
        SDL_RenderFillRect(renderer, &top_bar);
        SDL_RenderFillRect(renderer, &bottom_bar);
    }

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);

    /* restore logical size so the next frame's game-space draws (and the
       RENDER_LIST_ENTRY_CLEAR case below, which clears the game content
       rect itself each frame and is unaffected by this bug) map correctly */
    SDL_RenderSetLogicalSize(renderer,
        SCREEN_WIDTH / SCALING_FACTOR, SCREEN_HEIGHT / SCALING_FACTOR);
}

void render_list_to_output(render_list* render)
{
    /* clear the logical 4:3 game-content area to black before any draws.
       scenes with a full-screen backdrop (levels) paint over every pixel
       anyway, but menu/UI scenes (main menu, level select) don't always
       push their own RENDER_LIST_ENTRY_CLEAR, and anything drawn without a
       fresh full backdrop underneath first - like the aim cursor - leaves
       a trail of its previous frame's position. This clear runs with
       logical size still ON (untouched), so it's scoped to just the
       logical viewport - that is the well-behaved case for SDL_RenderClear
       on vita's gxm backend; only clearing OUTSIDE the logical viewport is
       buggy there, which is why the physical border is handled separately
       by paint_pillarbox_borders() at the end of the frame instead. */
    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, 0, 0, 0, 255);
    SDL_RenderClear(GLOBAL_SDL_DATA.renderer);

    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, 255, 255, 255, 0);

    assert(GLOBAL_SDL_DATA.initialized);
    for (u32 base_address = 0;
        base_address < render->push_buffer_size;
        )
    {
        render_list_entry_header* header = (render_list_entry_header*)(render->push_buffer_base + base_address);

        // must match push_render_element()'s aligned header_size exactly, or
        // reader and writer strides desync
        u32 header_size = (sizeof(render_list_entry_header) + 3) & ~3u;
        void* data = (u8*)header + header_size;
        base_address += header_size;

        switch (header->type)
        {
            case RENDER_LIST_ENTRY_BITMAP:
            {
                render_list_entry_bitmap* entry = (render_list_entry_bitmap*)data;

                SDL_Texture* texture = get_texture(GLOBAL_SDL_DATA, entry->texture);
                SDL_Rect src = get_sdl_rect(entry->source_rect);
                SDL_Rect dst = get_sdl_rect(entry->destination_rect);

                /* vita's SDL2 renderer doesn't reliably keep a texture's
                   blend mode sticky across the frame - other entries in
                   this same push buffer (e.g. BITMAP_WITH_EFFECTS additive
                   glyphs) call SDL_SetTextureBlendMode on other textures,
                   and on vita's backend that appears to affect later draws
                   of THIS texture too, leaving glyph textures opaque
                   (SDL_BLENDMODE_NONE) by the time text is drawn. Force it
                   immediately before every plain-bitmap draw. */
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(GLOBAL_SDL_DATA.renderer, texture, &src, &dst);

                base_address += (sizeof(render_list_entry_bitmap) + 3) & ~3u;
            }
            break;
            case RENDER_LIST_ENTRY_BITMAP_WITH_EFFECTS:
            {
                render_list_entry_bitmap_with_effects* entry = (render_list_entry_bitmap_with_effects*)data;

                SDL_Texture* texture = get_texture(GLOBAL_SDL_DATA, entry->texture);
                SDL_Rect src = get_sdl_rect(entry->source_rect);
                SDL_Rect dst = get_sdl_rect(entry->destination_rect);
                v4 sdl_tint = multiply_v4(entry->tint_color, 255.0f);

                /* same vita blend-state stickiness issue as the plain
                   BITMAP case above - force BLEND as the default here too;
                   the additive branch below overrides it to ADD and
                   restores BLEND afterward as it already did. */
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

                if (entry->flip_horizontally)
                {
                    src = get_sdl_rect(move_rect(entry->source_rect, get_v2(0.0f, 240.0f)));
                }

                if (entry->render_in_additive_mode)
                {
                    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
                    SDL_SetTextureColorMod(texture, sdl_tint.r, sdl_tint.g, sdl_tint.b);

                    SDL_RenderCopyEx(GLOBAL_SDL_DATA.renderer, texture, &src, &dst, 0, NULL, SDL_FLIP_NONE);

                    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureColorMod(texture, 255, 255, 255);
                }
                else
                {
                    if (false == is_zero_v4(entry->tint_color))
                    {
                        SDL_SetTextureColorMod(texture, sdl_tint.r, sdl_tint.g, sdl_tint.b);

                        SDL_RenderCopyEx(GLOBAL_SDL_DATA.renderer, texture, &src, &dst, 0, NULL, SDL_FLIP_NONE);

                        SDL_SetTextureColorMod(texture, 255, 255, 255);
                    }
                    else
                    {
                        SDL_RenderCopyEx(GLOBAL_SDL_DATA.renderer, texture, &src, &dst, 0, NULL, SDL_FLIP_NONE);
                    }
                }

                base_address += (sizeof(render_list_entry_bitmap_with_effects) + 3) & ~3u;
            }
            break;
            case RENDER_LIST_ENTRY_RECTANGLE:
            {
                render_list_entry_rectangle* entry = (render_list_entry_rectangle*)data;

                if (false == is_zero_v4(entry->color))
                {
                    v4 sdl_tint = multiply_v4(entry->color, 255.0f);
                    if (entry->color.a != 1.0f)
                    {
                        SDL_SetRenderDrawBlendMode(GLOBAL_SDL_DATA.renderer, SDL_BLENDMODE_BLEND);
                    }
                    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, sdl_tint.r, sdl_tint.g, sdl_tint.b, sdl_tint.a);
                }

                if (entry->render_outline_only)
                {
                    render_rect(&GLOBAL_SDL_DATA, entry->destination_rect);
                }
                else
                {
                    SDL_Rect dst = get_sdl_rect(entry->destination_rect);
                    SDL_RenderFillRect(GLOBAL_SDL_DATA.renderer, &dst);
                }

                if (false == is_zero_v4(entry->color))
                {
                    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, 255, 255, 255, 0);
                    SDL_SetRenderDrawBlendMode(GLOBAL_SDL_DATA.renderer, SDL_BLENDMODE_NONE);
                }

                base_address += (sizeof(render_list_entry_rectangle) + 3) & ~3u;
            }
            break;
            case RENDER_LIST_ENTRY_CLEAR:
            {
                render_list_entry_clear* entry = (render_list_entry_clear*)data;

                if (false == is_zero_v4(entry->color))
                {
                    v4 sdl_tint = multiply_v4(entry->color, 255.0f);
                    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, sdl_tint.r, sdl_tint.g, sdl_tint.b, sdl_tint.a);
                }

                SDL_RenderClear(GLOBAL_SDL_DATA.renderer);

                if (false == is_zero_v4(entry->color))
                {
                    SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, 255, 255, 255, 0);
                }

                base_address += (sizeof(render_list_entry_clear) + 3) & ~3u;
            }
            break;
            case RENDER_LIST_ENTRY_FADE:
            {
                render_list_entry_fade* entry = (render_list_entry_fade*)data;

                v4 sdl_color = multiply_v4(entry->color, 255.0f);
                sdl_color.a = entry->percentage * 255;

                SDL_Rect fullscreen = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
                SDL_SetRenderDrawBlendMode(GLOBAL_SDL_DATA.renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
                SDL_RenderFillRect(GLOBAL_SDL_DATA.renderer, &fullscreen);
                SDL_SetRenderDrawColor(GLOBAL_SDL_DATA.renderer, 255, 255, 255, 0);
                SDL_SetRenderDrawBlendMode(GLOBAL_SDL_DATA.renderer, SDL_BLENDMODE_NONE);

                base_address += (sizeof(render_list_entry_fade) + 3) & ~3u;
            }
            break;

            invalid_default_case;
        }
    }

    /* game content for this frame is fully drawn - now cover the physical
       border area the logical-size game draws above can never reach.
       must be last, right before present: bypasses SDL_RenderClear
       entirely instead of trying to make it behave (see function above). */
    paint_pillarbox_borders(GLOBAL_SDL_DATA.renderer);

    SDL_RenderPresent(GLOBAL_SDL_DATA.renderer);

    render->push_buffer_size = 0;
}
