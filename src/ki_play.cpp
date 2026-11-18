// Plays the recompiled Killer Instinct program in a window (SDL2).
//
// usage: ki_play [--data <dir>] [--scale N] [--fullscreen] [--mute] [--opponent NAMES] [--stage N,...]
//        ki_play --boot <snapshot dir|rom> --disk <kinst.img> --rom <ki-l15d.u98>
//                [--sound-roms <dir>] [--scale N] [--mute]      (development)
//
// Normally the program starts like the arcade board, at the reset vector,
// from the player's MAME `kinst` set: a folder with kinst.zip (or its files)
// and kinst.chd, given with --data, dropped onto the window, or remembered
// from the last run. Every file is checked against MAME's SHA1. Sectors the
// game writes (settings, high scores) are kept in the user's preferences
// folder and put back on the next start.
//
// The guest runs on the main thread; at every VBLANK the board callback
// presents the framebuffer, reads input, queues the DCS samples produced
// during the frame and paces emulation to the arcade's refresh rate
// (6.25 MHz / (406 x 261) = 58.98 Hz).
//
// Keys (player 1): arrows move, A S D = high quick/medium/fierce,
// Z X C = low quick/medium/fierce, 1 = start, 5 = coin.
// Player 2: I J K L move, Q W E / T Y U attacks... see kKeys. F2 = service
// (test switch), Esc quits, Tab toggles unthrottled speed (no sound while on).
// Gamepads work too (Xbox layout by default, see ki_pad.cpp); G opens the
// controller setup screen. F5 saves the state, F9 loads it.
// P pauses, R resets. --opponent NAME[,NAME...] and --stage N[,N...] choose the coming
// fights (in turn, one per fight); F6 / F7 cycle the opponent / stage of the
// next fight (shown in the title bar).
#include <SDL.h>
#ifdef _WIN32
#include <process.h>
#define execv _execv
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ki/kinst_board.h"
#include "ki/runtime.h"
#include "ki_boot.h"
#include "ki_fight.h"
#include "ki_pad.h"
#include "ki_pad_ui.h"
#ifdef KI_HAVE_FILES
#include "ki_files.h"
#endif
#ifdef KI_HAVE_HD
#include "ki_hd.h"
#endif

namespace {

struct KeyBit {
    SDL_Scancode key;
    int player;  // 1 or 2
    uint32_t bit;
};

// Bits of the P1/P2 ports (active low), MAME kinst.cpp INPUT_PORTS_START(kinst).
const KeyBit kKeys[] = {
    {SDL_SCANCODE_A, 1, 0x001},     {SDL_SCANCODE_S, 1, 0x002},     {SDL_SCANCODE_D, 1, 0x004},
    {SDL_SCANCODE_Z, 1, 0x008},     {SDL_SCANCODE_X, 1, 0x010},     {SDL_SCANCODE_C, 1, 0x020},
    {SDL_SCANCODE_UP, 1, 0x040},    {SDL_SCANCODE_DOWN, 1, 0x080},  {SDL_SCANCODE_LEFT, 1, 0x100},
    {SDL_SCANCODE_RIGHT, 1, 0x200}, {SDL_SCANCODE_1, 1, 0x400},     {SDL_SCANCODE_5, 1, 0x800},
    {SDL_SCANCODE_F2, 1, 0x1000},   // service mode (test switch)
    {SDL_SCANCODE_Q, 2, 0x001},     {SDL_SCANCODE_W, 2, 0x002},     {SDL_SCANCODE_E, 2, 0x004},
    {SDL_SCANCODE_T, 2, 0x008},     {SDL_SCANCODE_Y, 2, 0x010},     {SDL_SCANCODE_U, 2, 0x020},
    {SDL_SCANCODE_I, 2, 0x040},     {SDL_SCANCODE_K, 2, 0x080},     {SDL_SCANCODE_J, 2, 0x100},
    {SDL_SCANCODE_L, 2, 0x200},     {SDL_SCANCODE_2, 2, 0x400},     {SDL_SCANCODE_6, 2, 0x800},
};

constexpr double kFrameSeconds = 1.0 / (6.25e6 / (406.0 * 261.0));
constexpr const char* kTitle = "Killer Instinct (recompiled)";
constexpr int kWideW = 427;  // 240 * 16 / 9

void write_ppm_file(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
}

std::string pref_path() {
    char* p = SDL_GetPrefPath("kilnan", "KIRecompiled");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text << "\n";
}

// Disk sectors written by the game: [u32 lba][512 bytes] records.
void apply_saved_sectors(const std::string& path, std::vector<uint8_t>& disk) {
    std::ifstream f(path, std::ios::binary);
    uint8_t rec[4 + 512];
    while (f.read(reinterpret_cast<char*>(rec), sizeof rec)) {
        uint32_t lba = rec[0] | rec[1] << 8 | rec[2] << 16 | static_cast<uint32_t>(rec[3]) << 24;
        if ((static_cast<uint64_t>(lba) + 1) * 512 <= disk.size()) std::memcpy(&disk[lba * 512ull], rec + 4, 512);
    }
}

void save_sectors(const std::string& path, const ki::KinstBoard& board, const std::vector<uint32_t>& earlier) {
    std::vector<uint32_t> all = earlier;
    all.insert(all.end(), board.written_sectors().begin(), board.written_sectors().end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        for (uint32_t lba : all) {
            uint8_t head[4] = {static_cast<uint8_t>(lba), static_cast<uint8_t>(lba >> 8),
                               static_cast<uint8_t>(lba >> 16), static_cast<uint8_t>(lba >> 24)};
            f.write(reinterpret_cast<const char*>(head), 4);
            f.write(reinterpret_cast<const char*>(&board.disk()[lba * 512ull]), 512);
        }
    }
    std::rename(tmp.c_str(), path.c_str());
}

std::vector<uint32_t> saved_lbas(const std::string& path) {
    std::vector<uint32_t> out;
    std::ifstream f(path, std::ios::binary);
    uint8_t rec[4 + 512];
    while (f.read(reinterpret_cast<char*>(rec), sizeof rec))
        out.push_back(rec[0] | rec[1] << 8 | rec[2] << 16 | static_cast<uint32_t>(rec[3]) << 24);
    return out;
}

}  // namespace

// Runs the drawing jobs on a thread of their own, one at a time: the game goes
// on with the next frame while the GPU work and the buffer swap of this one are
// done. Until start() (or with KI_RENDER_THREAD=0) a job runs in place. Only
// this thread uses the renderer once it is started.
class RenderThread {
public:
    void start(SDL_Window* window) {
        SDL_GL_MakeCurrent(window, nullptr);  // the renderer's context moves to the thread
        thread_ = std::thread([this] { loop(); });
    }
    bool started() const { return thread_.joinable(); }
    // Waits for the previous job, hands this one over; with wait, until it is done.
    void run(std::function<void()> job, bool wait) {
        if (!started()) {
            job();
            return;
        }
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return !busy_; });
        job_ = std::move(job);
        busy_ = true;
        cv_.notify_all();
        if (wait) cv_.wait(lock, [&] { return !busy_; });
    }
    void stop() {
        if (!started()) return;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return !busy_; });
            stop_ = true;
            cv_.notify_all();
        }
        thread_.join();
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            cv_.wait(lock, [&] { return busy_ || stop_; });
            if (!busy_) return;
            std::function<void()> job = std::move(job_);
            lock.unlock();
            job();
            lock.lock();
            busy_ = false;
            cv_.notify_all();
        }
    }
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool busy_ = false, stop_ = false;
};

char** g_argv = nullptr;  // for the reset key

int main(int argc, char** argv) {
    g_argv = argv;
    std::string boot, disk, rom, sound_roms, data, hd_pack;
    static ki::FightChoice fight;
    std::string aspect_arg;
    int scale = 3;
    bool mute = false;
    bool fullscreen = false;  // --fullscreen, and always on KMS/DRM (no desktop: the window is the screen)
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--mute") { mute = true; continue; }
        if (k == "--fullscreen") { fullscreen = true; continue; }
        if (i + 1 >= argc) break;
        std::string v = argv[++i];
        if (k == "--boot") boot = v;
        else if (k == "--disk") disk = v;
        else if (k == "--rom") rom = v;
        else if (k == "--scale") scale = std::stoi(v);
        else if (k == "--sound-roms") sound_roms = v;
        else if (k == "--data") data = v;
        else if (k == "--hd-pack") hd_pack = v;
        else if (k == "--aspect") aspect_arg = v;  // 16:9 or 4:3 (remembered; V switches while playing)
        else if (k == "--opponent" && !ki::parse_characters(v, fight.opponents)) {
            std::fprintf(stderr, "--opponent: names or numbers 0-10 (jago combo spinal thunder glacius fulgore cinder sabrewulf orchid riptor eyedol)\n");
            return 1;
        } else if (k == "--stage" && !ki::parse_numbers(v, ki::kStageCount, fight.stages)) {
            std::fprintf(stderr, "--stage: numbers 0-15\n");
            return 1;
        }
    }
    ki::install_fight_choice(&fight);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // Fullscreen (and always on KMS/DRM, where the window is the screen): created at
    // the screen's size from the start. A window made at another size and switched
    // to fullscreen afterwards kept the old height in GL on KMS/DRM, which put the
    // picture at the top of the screen instead of in the middle.
    const char* video_driver = SDL_GetCurrentVideoDriver();
    if (video_driver && std::string(video_driver) == "KMSDRM") fullscreen = true;
    int win_w = 320 * scale, win_h = 240 * scale;
    Uint32 win_flags = SDL_WINDOW_RESIZABLE;
    SDL_DisplayMode desktop;
    if (fullscreen && SDL_GetDesktopDisplayMode(0, &desktop) == 0) {
        win_w = desktop.w;
        win_h = desktop.h;
        win_flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    SDL_Window* window =
        SDL_CreateWindow(kTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, win_flags);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    // 16:9: the 4:3 picture in the middle of a 427x240 view; the sides show a
    // blurred, darkened copy of the picture ("ambient"). V switches, remembered.
    bool wide = read_text(pref_path() + "aspect.txt") == "16:9";
    if (!aspect_arg.empty()) wide = aspect_arg == "16:9";
    RenderThread render;
    auto set_aspect = [&](bool w) {
        wide = w;
        render.run([&] {
            SDL_RenderSetLogicalSize(renderer, wide ? kWideW : 320, 240);
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            if (!(SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)))
                SDL_SetWindowSize(window, wh * (wide ? kWideW : 320) / 240, wh);
        }, true);
        write_text(pref_path() + "aspect.txt", wide ? "16:9" : "4:3");
    };
    set_aspect(wide);
    if (std::getenv("KI_STATS")) {
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(renderer, &ww, &wh);
        SDL_Rect vp;
        SDL_RenderGetViewport(renderer, &vp);
        float sx = 1, sy = 1;
        SDL_RenderGetScale(renderer, &sx, &sy);
        std::fprintf(stderr, "video: %s, output %dx%d, picture at %d,%d size %dx%d\n", video_driver ? video_driver : "?", ww,
                     wh, static_cast<int>(vp.x * sx), static_cast<int>(vp.y * sy), static_cast<int>(vp.w * sx),
                     static_cast<int>(vp.h * sy));
    }
    SDL_Texture* ambient = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 40, 30);
    SDL_SetTextureScaleMode(ambient, SDL_ScaleModeLinear);
    // ARGB8888: a format the renderers take as is (RGB24 made SDL convert every frame).
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    SDL_Texture* hd_texture = nullptr;  // 4x picture when an HD pack is loaded (H toggles)

    // ---- the game's files ----
    std::vector<uint8_t> disk_image, boot_rom;
    std::vector<std::vector<uint8_t>> sound;
    const bool dev = !boot.empty();
    std::string saves;  // file with the sectors the game wrote (player mode only)
    std::vector<uint32_t> earlier_saves;
    if (dev) {
        if (disk.empty() || rom.empty()) {
            std::fprintf(stderr, "usage: %s --boot <dir|rom> --disk <img> --rom <u98> [--sound-roms <dir>]\n", argv[0]);
            return 2;
        }
        disk_image = ki::read_file_or_die(disk);
        boot_rom = ki::read_file_or_die(rom);
        if (sound_roms.empty()) {
            size_t slash = rom.find_last_of('/');
            sound_roms = slash == std::string::npos ? "." : rom.substr(0, slash);
        }
        if (FILE* f = std::fopen((sound_roms + "/u10-l1").c_str(), "rb")) {
            std::fclose(f);
            sound = ki::read_sound_roms_or_die(sound_roms);
        }
    } else {
#ifdef KI_HAVE_FILES
        const std::string prefs = pref_path();
        if (data.empty()) data = read_text(prefs + "data_dir.txt");
        ki::GameFiles files;
        std::string error;
        bool ok = !data.empty() && ki::load_game_files(data, files, error, prefs);
        if (!ok) {
            std::string msg =
                (data.empty() ? std::string("Drag the folder with your MAME 'kinst' set onto this window.")
                              : "The game files in " + data + " cannot be used:\n" + error +
                                    "\n\nDrag another folder onto this window.") +
                "\n\nThe folder must contain kinst.zip (BIOS v1.5d) and kinst.chd (or a .zip with it).";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, kTitle, msg.c_str(), window);
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): drop the kinst folder here");
            SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
            while (!ok) {
                SDL_RenderClear(renderer);
                SDL_RenderPresent(renderer);
                SDL_Event ev;
                if (!SDL_WaitEvent(&ev)) continue;
                if (ev.type == SDL_QUIT) return 0;
                if (ev.type != SDL_DROPFILE) continue;
                std::string dropped = ev.drop.file;
                SDL_free(ev.drop.file);
                // A dropped file (kinst.zip, kinst.chd) stands for its folder.
                if (!ki::load_game_files(dropped, files, error, prefs)) {
                    size_t slash = dropped.find_last_of('/');
                    std::string parent = slash == std::string::npos ? dropped : dropped.substr(0, slash);
                    std::string e2;
                    if (ki::load_game_files(parent, files, e2, prefs)) dropped = parent;
                    else {
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kTitle, error.c_str(), window);
                        continue;
                    }
                }
                data = dropped;
                ok = true;
            }
            SDL_SetWindowTitle(window, kTitle);
        }
        std::error_code ec;
        std::string absolute = std::filesystem::absolute(data, ec).lexically_normal().string();
        write_text(prefs + "data_dir.txt", ec ? data : absolute);
        disk_image = std::move(files.disk);
        boot_rom = std::move(files.boot_rom);
        sound = std::move(files.sound_roms);
        saves = prefs + "disk_writes.bin";
        earlier_saves = saved_lbas(saves);
        apply_saved_sectors(saves, disk_image);
#else
        std::fprintf(stderr, "built without KI_WITH_CHD: use --boot/--disk/--rom\n");
        return 2;
#endif
    }

    static ki::Context c;
    // --boot rom (and player mode): start at the reset vector, like the real board.
    const bool cold = !dev || boot == "rom";
    uint32_t entry = cold ? ki::cold_reset(c) : ki::load_boot_state(c, boot);
    static ki::KinstBoard board(std::move(disk_image), std::move(boot_rom));
    c.io = &board;
    if (cold) board.set_cycles_to_vblank(0);
    static std::unique_ptr<ki::Dcs> dcs;
    SDL_AudioDeviceID audio = 0;
    if (!sound.empty()) {
        dcs = std::make_unique<ki::Dcs>(sound);
        board.dcs = dcs.get();
        // The sound DSP on its own core (KI_DCS_THREAD=0: in line, as before).
        const char* t = std::getenv("KI_DCS_THREAD");
        if (!t || std::string(t) != "0") dcs->start_thread();
        if (!mute) {
            // KI's DCS program drives the DAC at 31250 Hz; SDL converts to the device rate.
            SDL_AudioSpec want{};
            want.freq = 31250;
            want.format = AUDIO_S16SYS;
            want.channels = 1;
            want.samples = 1024;
            audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
            if (!audio) std::fprintf(stderr, "no audio: %s\n", SDL_GetError());
            else SDL_PauseAudioDevice(audio, 0);
        }
    }

#ifdef KI_HAVE_HD
    static ki::HdRenderer hd;
    bool hd_on = false;
    if (!hd_pack.empty()) {
        if (hd.load_pack(hd_pack)) {
            hd.attach();
            hd_on = true;
            hd_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                           ki::HdRenderer::kW, ki::HdRenderer::kH);
            std::fprintf(stderr, "HD pack: %zu sprites, %zu shapes (H switches HD on and off)\n", hd.pack_size(), hd.shape_count());
        } else {
            std::fprintf(stderr, "no HD pack in %s\n", hd_pack.c_str());
        }
    }
#endif
    static ki::SaveState state;  // F5 / F9
    state.bind(board, dcs.get(), sound);

    bool turbo = false;
    std::vector<SDL_GameController*> pads;  // opened as SDL reports them (SDL_CONTROLLERDEVICEADDED)
    uint64_t saved_writes = 0;
    uint64_t title_reset_frame = 0;
    auto save = [&]() {
        if (!saves.empty() && board.disk_writes() != saved_writes) {
            save_sectors(saves, board, earlier_saves);
            saved_writes = board.disk_writes();
        }
    };
    auto next_frame = std::chrono::steady_clock::now();
    bool paused = false;
    // Controller bindings: G opens the setup screen (game paused meanwhile).
    static std::vector<ki::pad::Action> pad_actions = ki::pad::load_bindings(pref_path() + "pad_map.txt");
    static ki::pad::SetupScreen pad_setup(pad_actions, pref_path() + "pad_map.txt");
    std::atomic<double> swap_ms{0};  // KI_STATS: time inside SDL_RenderPresent (render thread)
    // Draws the frame, from the live game state (ctx) or from a copy of the
    // displayed framebuffer (fb, on the render thread while the game runs on).
    auto draw = [&](const ki::Context* ctx, const uint8_t* fb, bool wide) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        std::vector<uint8_t> rgb;
        if (wide) rgb = fb ? ki::KinstBoard::rgb_of(fb) : board.framebuffer_rgb(*ctx);  // for the ambient sides
        const SDL_FRect game{wide ? (kWideW - 320) / 2.0f : 0.0f, 0.0f, 320.0f, 240.0f};
        if (wide) {
            // Ambient sides: the picture averaged down to 40x30, stretched over the
            // whole view (the linear filter blurs it), then darkened.
            std::vector<uint8_t> small(40 * 30 * 3);
            for (int y = 0; y < 30; ++y)
                for (int x = 0; x < 40; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        int sum = 0;
                        for (int yy = 0; yy < 8; ++yy)
                            for (int xx = 0; xx < 8; ++xx) sum += rgb[((y * 8 + yy) * 320 + x * 8 + xx) * 3 + ch];
                        small[(y * 40 + x) * 3 + ch] = static_cast<uint8_t>(sum / 64);
                    }
            SDL_UpdateTexture(ambient, nullptr, small.data(), 40 * 3);
            // Each side continues the picture's outer columns (blurred by the
            // linear filter), fading to dark towards the window's edge.
            const SDL_Rect left_src{0, 0, 2, 30}, right_src{38, 0, 2, 30};
            const SDL_FRect left_dst{0.0f, 0.0f, game.x, 240.0f}, right_dst{game.x + 320.0f, 0.0f, game.x, 240.0f};
            SDL_RenderCopyF(renderer, ambient, &left_src, &left_dst);
            SDL_RenderCopyF(renderer, ambient, &right_src, &right_dst);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            const int side = static_cast<int>(game.x);
            for (int x = 0; x < side; ++x) {
                Uint8 a = static_cast<Uint8>(60 + 170 * (side - x) / side);  // darker away from the picture
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, a);
                SDL_RenderDrawLine(renderer, x, 0, x, 239);
                SDL_RenderDrawLine(renderer, kWideW - 1 - x, 0, kWideW - 1 - x, 239);
            }
        }
#ifdef KI_HAVE_HD
        // The HD copy is kept up to date even when not shown, so H switches instantly.
        if (hd_texture && ctx) {
            const std::vector<uint32_t>& px = hd.frame(*ctx, board.displayed_bank());
            if (hd_on) {
                // 16:9 on stages drawn from a wide panorama: the real sky and
                // buildings beyond the picture's edges, over the ambient fill.
                static std::vector<uint32_t> left, right;
                static SDL_Texture* side_tex[2] = {};
                constexpr int kSide = 54;
                if (wide && hd.sides(*ctx, board.displayed_bank(), kSide, left, right)) {
                    for (int i = 0; i < 2; ++i) {
                        if (!side_tex[i]) {
                            side_tex[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                            kSide * ki::HdRenderer::kScale, ki::HdRenderer::kH);
                            SDL_SetTextureBlendMode(side_tex[i], SDL_BLENDMODE_BLEND);
                        }
                        SDL_UpdateTexture(side_tex[i], nullptr, (i ? right : left).data(), kSide * ki::HdRenderer::kScale * 4);
                    }
                    const SDL_FRect l{game.x - kSide, 0.0f, static_cast<float>(kSide), 240.0f}, r{game.x + 320.0f, 0.0f, static_cast<float>(kSide), 240.0f};
                    SDL_RenderCopyF(renderer, side_tex[0], nullptr, &l);
                    SDL_RenderCopyF(renderer, side_tex[1], nullptr, &r);
                }
                SDL_UpdateTexture(hd_texture, nullptr, px.data(), ki::HdRenderer::kW * 4);
                SDL_RenderCopyF(renderer, hd_texture, nullptr, &game);
            }
        }
        if (!hd_on || !ctx)
#endif
        {
            void* pixels;
            int pitch;
            if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
                if (fb) ki::KinstBoard::argb_of(fb, static_cast<uint32_t*>(pixels), pitch);
                else board.framebuffer_argb(*ctx, static_cast<uint32_t*>(pixels), pitch);
                SDL_UnlockTexture(texture);
            }
            SDL_RenderCopyF(renderer, texture, nullptr, &game);
        }
        // The setup screen is laid out for the 4:3 area.
        // Restore the viewport afterwards rather than reset it: with a logical size,
        // SDL_RenderSetViewport(nullptr) makes it the whole window, which drops the
        // letterbox offset and drew every later frame at the top of a non-4:3 screen.
        SDL_Rect saved;
        SDL_RenderGetViewport(renderer, &saved);
        SDL_Rect area{saved.x + static_cast<int>(game.x), saved.y, 320, 240};  // inside the letterbox
        SDL_RenderSetViewport(renderer, &area);
        if (ctx) pad_setup.draw(renderer, pads);  // the setup screen is drawn in place (see present)
        SDL_RenderSetViewport(renderer, &saved);
        // KI_SHOT=N: the whole drawn screen of every N-th presented frame to screen_<frame>.ppm (diagnostics).
        static const long shot_at = std::getenv("KI_SHOT") ? std::atol(std::getenv("KI_SHOT")) : -1;
        static long presented = 0;
        if (shot_at > 0 && ++presented % shot_at == 0) {
            int lw = 0, lh = 0, ow = 0, oh = 0;
            SDL_RenderGetLogicalSize(renderer, &lw, &lh);
            SDL_RenderSetLogicalSize(renderer, 0, 0);
            SDL_GetRendererOutputSize(renderer, &ow, &oh);
            std::vector<uint8_t> px(static_cast<size_t>(ow) * oh * 3);
            if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGB24, px.data(), ow * 3) == 0)
                write_ppm_file(pref_path() + "screen_" + std::to_string(presented) + ".ppm", px, ow, oh);
            SDL_RenderSetLogicalSize(renderer, lw, lh);
        }
        auto ts = std::chrono::steady_clock::now();
        SDL_RenderPresent(renderer);
        swap_ms = swap_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
    };
    // In the common case the render thread draws a copy of the frame (two copies
    // in turn: the one being drawn stays untouched). Paused, with the setup screen
    // or with HD the drawing reads live state, so the game waits for it.
    static std::vector<uint8_t> fb_copy[2] = {std::vector<uint8_t>(ki::KinstBoard::kFramebufferBytes),
                                              std::vector<uint8_t>(ki::KinstBoard::kFramebufferBytes)};
    static int fb_next = 0;
    auto present = [&](ki::Context& ctx) {
        bool live = paused || pad_setup.open() || hd_texture != nullptr;
        if (live || !render.started()) {
            render.run([&] { draw(&ctx, nullptr, wide); }, true);
            return;
        }
        uint8_t* fb = fb_copy[fb_next].data();
        fb_next ^= 1;
        std::memcpy(fb, &ctx.sram[board.displayed_bank()], ki::KinstBoard::kFramebufferBytes);
        render.run([&draw, fb, w = wide] { draw(nullptr, fb, w); }, false);
    };
    board.on_vblank = [&](ki::Context& ctx, uint64_t frame) {
        SDL_Event ev;
        for (bool first = true; first || paused || pad_setup.open(); first = false) {
            if (!first) {
                // Paused: the frame stays on screen (H still switches HD, to compare), the game does not run.
                present(ctx);
                SDL_Delay(16);
                next_frame = std::chrono::steady_clock::now();
            }
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED || ev.type == SDL_QUIT) {
                    // handled below even while the setup screen is open
                } else if (pad_setup.handle(ev)) {
                    continue;
                } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_G) {
                    pad_setup.toggle();
                    continue;
                }
                if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                    save();
                    render.stop();
                    SDL_Quit();
                    std::exit(0);
                }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_TAB && !ev.key.repeat) turbo = !turbo;
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                    (ev.key.keysym.scancode == SDL_SCANCODE_F6 || ev.key.keysym.scancode == SDL_SCANCODE_F7)) {
                    // Next fight: off -> 0 -> 1 ... -> off. Replaces any --opponent / --stage list.
                    bool opp = ev.key.keysym.scancode == SDL_SCANCODE_F6;
                    std::vector<int>& list = opp ? fight.opponents : fight.stages;
                    int limit = opp ? 11 : ki::kStageCount;
                    int next = list.size() == 1 ? list[0] + 1 : 0;
                    list.clear();
                    if (next < limit) list.push_back(next);
                    fight.fights = 0;
                    std::string t = std::string(kTitle) + ": next fight: opponent " +
                                    (fight.opponents.empty() ? "(game)" : ki::kCharacterNames[fight.opponents[0]]) +
                                    ", stage " + (fight.stages.empty() ? "(game)" : std::to_string(fight.stages[0]));
                    SDL_SetWindowTitle(window, t.c_str());
                }
#ifdef KI_HAVE_HD
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_M && hd_texture) {
                    // Diagnostics: the sprites recently drawn without HD, and the two pictures.
                    std::string base = pref_path() + "hd_report";
                    if (FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
                        hd.dump_recent_misses(f);
                        std::fclose(f);
                    }
                    write_ppm_file(base + "_original.ppm", board.framebuffer_rgb(ctx), 320, 240);
                    const std::vector<uint32_t>& px = hd.frame(ctx, board.displayed_bank());
                    std::vector<uint8_t> rgb(px.size() * 3);
                    for (size_t i = 0; i < px.size(); ++i)
                        rgb[3 * i] = px[i] >> 16, rgb[3 * i + 1] = px[i] >> 8, rgb[3 * i + 2] = px[i];
                    write_ppm_file(base + "_hd.ppm", rgb, ki::HdRenderer::kW, ki::HdRenderer::kH);
                    SDL_SetWindowTitle(window, ("Killer Instinct (recompiled): report in " + base + ".txt").c_str());
                }
#endif
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_R) {
                    // Reset: like switching the board off and on (same arguments).
                    save();
                    if (audio) SDL_CloseAudioDevice(audio);
                    render.stop();
                    SDL_Quit();
                    execv(g_argv[0], g_argv);
                    std::perror("reset");
                    std::exit(1);
                }
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_V) set_aspect(!wide);
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_P) {
                    paused = !paused;
                    if (audio) SDL_PauseAudioDevice(audio, paused ? 1 : 0);
                    SDL_SetWindowTitle(window, paused ? "Killer Instinct (recompiled): paused (P resumes)" : kTitle);
                }
#ifdef KI_HAVE_HD
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_H && hd_texture)
                    hd_on = !hd_on;
#endif
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F5)
                    state.request(ctx);  // taken at the next dispatcher entry
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F9 && state.valid())
                    throw ki::LoadStateRequest{};
                if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                    if (SDL_GameController* pad = SDL_GameControllerOpen(ev.cdevice.which)) pads.push_back(pad);
                }
                if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    for (SDL_GameController*& pad : pads)
                        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == ev.cdevice.which) {
                            SDL_GameControllerClose(pad);
                            pad = nullptr;
                        }
                    pads.erase(std::remove(pads.begin(), pads.end(), nullptr), pads.end());
                }
            }
        }
        if (frame % 600 == 0) save();  // every ~10 s, if the game wrote anything
        if (state.just_saved()) {
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): state saved (F9 loads it)");
            title_reset_frame = frame + 120;
        }
        if (title_reset_frame && frame >= title_reset_frame) {
            SDL_SetWindowTitle(window, kTitle);
            title_reset_frame = 0;
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        uint32_t p1 = 0xffffffff, p2 = 0xffffffff;
        for (const KeyBit& k : kKeys)
            if (keys[k.key]) (k.player == 1 ? p1 : p2) &= ~k.bit;
        int player = 0;
        for (SDL_GameController* pad : pads)
            if (pad && SDL_GameControllerGetAttached(pad) && player < 2) (player++ == 0 ? p1 : p2) &= ~ki::pad::pressed(pad, pad_actions);
        board.p1 = p1;
        board.p2 = p2;
        // Quit on handhelds without a keyboard: Select (Back) + Start held for half a
        // second on any pad, handled like the window's close button (high scores saved).
        static int quit_hold = 0;
        bool combo = false;
        for (SDL_GameController* pad : pads)
            if (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))
                combo = true;
        quit_hold = combo ? quit_hold + 1 : 0;
        if (quit_hold == 30) {
            SDL_Event quit{};
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
        }

        if (dcs) {
            static std::vector<int16_t> pcm;
            dcs->take_samples(pcm);
            // Drop audio in turbo mode, or if the queue backs up past 200 ms.
            if (audio && !turbo && SDL_GetQueuedAudioSize(audio) < 31250 * 2 / 5)
                SDL_QueueAudio(audio, pcm.data(), static_cast<Uint32>(pcm.size() * sizeof(int16_t)));
            pcm.clear();
        }
        // KI_STATS=1: once a second, where the time goes (game code, drawing, waiting).
        static const bool stats = std::getenv("KI_STATS") != nullptr;
        static auto last = std::chrono::steady_clock::now(), sec_start = last;
        static double t_game = 0, t_present = 0, t_wait = 0;
        static int n_frames = 0;
        auto t0 = std::chrono::steady_clock::now();
        if (stats) t_game += std::chrono::duration<double, std::milli>(t0 - last).count();
        if (!turbo || frame % 8 == 0) present(ctx);
        auto t1 = std::chrono::steady_clock::now();
        if (stats) {
            t_present += std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (++n_frames, t1 - sec_start >= std::chrono::seconds(1)) {
                double secs = std::chrono::duration<double>(t1 - sec_start).count();
                std::fprintf(stderr, "stats: %.1f fps  game %.1f  present %.1f (swap %.1f)  wait %.1f ms/frame\n",
                             n_frames / secs, t_game / n_frames, t_present / n_frames, swap_ms.load() / n_frames, t_wait / n_frames);
                swap_ms = 0;
                t_game = t_present = t_wait = 0;
                n_frames = 0;
                sec_start = t1;
            }
        }
        if (!turbo) {
            next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(kFrameSeconds));
            // Follow the audio clock: when more than ~60 ms are queued, slow down slightly.
            if (audio && SDL_GetQueuedAudioSize(audio) > 31250 * 2 * 6 / 100)
                next_frame += std::chrono::microseconds(500);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) std::this_thread::sleep_until(next_frame);
            else if (now - next_frame > std::chrono::milliseconds(100)) next_frame = now;  // fell behind: resync
        }
        last = std::chrono::steady_clock::now();
        if (stats) t_wait += std::chrono::duration<double, std::milli>(last - t1).count();
    };
    if (const char* t = std::getenv("KI_RENDER_THREAD"); !t || std::string(t) != "0") render.start(window);
    ki::schedule(c);
    bool ok = false;
    for (;;) {
        try {
            ok = ki::run_entry(c, entry);
            break;
        } catch (const ki::LoadStateRequest&) {
            entry = state.restore(c);
            if (audio) SDL_ClearQueuedAudio(audio);
            next_frame = std::chrono::steady_clock::now();
            SDL_SetWindowTitle(window, "Killer Instinct (recompiled): state loaded");
        }
    }
    render.stop();
    save();
    if (!ok)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle,
                                 "The recompiled program stopped on an unexpected guest fault "
                                 "(details on the terminal).",
                                 window);
    SDL_Quit();
    return ok ? 0 : 1;
}
