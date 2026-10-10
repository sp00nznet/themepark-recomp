/*
 * Theme Park host: runs the lifted MAIN.EXE on pcrecomp's dos32 host and
 * shows it in a window, or headless with --record.
 *
 *   themepark.exe [--headless] [--record out.mp4] [--seconds N] [--log]
 *                 [--click T:x,y] [--key T:sc] [--game DIR] [--fps N]
 *                 [--scale sharp|smooth|crt|nearest|integer] [--fullscreen] [--gdi]
 *                 [-- game args]
 *
 * The game itself runs on a thread of its own with a large stack (each guest
 * call is a host call, and the game recurses); this thread owns the window.
 * docs/architecture.md has the picture.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos32.h"
#include "present.h"
#include "audio.h"

extern const uint32_t recomp_image_base, recomp_image_size, recomp_image_stack, recomp_entry_va;
extern const uint8_t recomp_image[];
extern double game_fps;   /* game.c */
extern int sound_mute, sound_headless;   /* sound.c */
void sound_tick(double dt);

static dos32_config cfg;
static char root[MAX_PATH];
static int headless, log_on;
static double seconds;
static const char* record_path;
static volatile int exit_code = -1;
static const char* present_dir;   /* --present-test */
static int start_hires = 1;       /* --lowres, [game] hires=0: the original's default */

/* Scripted input for headless runs: --click T:x,y and --key T:sc (seconds
 * of guest time, game pixels, a set-1 scancode in hex). Each fires once at
 * its frame of the 70 Hz clock; a click is a press then a release. */
typedef struct { long frame; int kind, a, b; } ev_t;
static char dump_path[8][MAX_PATH];
static int ndump;
static ev_t evs[256];
static int nev;
static long frame_no;

static void add_event(int kind, const char* spec) {
    double t; int a = 0, b = 0;
    if (nev >= 256) return;
    if (kind == 'c' && sscanf(spec, "%lf:%d,%d", &t, &a, &b) != 3) return;
    if (kind == 'k' && sscanf(spec, "%lf:%x", &t, &a) != 2) return;
    if (kind == 'f' && sscanf(spec, "%lf:%d", &t, &a) != 2) return;
    if (kind == 'p' && sscanf(spec, "%lf:%x=%x", &t, &a, &b) != 3) return;
    if (kind == 'd') { if (sscanf(spec, "%lf:", &t) != 1 || ndump >= 8) return;
                       snprintf(dump_path[ndump], MAX_PATH, "%s", strchr(spec, ':') + 1); a = ndump++; }
    evs[nev].frame = (long)(t * 70); evs[nev].kind = kind; evs[nev].a = a; evs[nev].b = b; nev++;
}

static void run_events(void) {
    for (int i = 0; i < nev; i++) {
        long f = evs[i].frame;
        if (evs[i].kind == 'c') {
            if (f == frame_no - 6) dos32_mouse(evs[i].a, evs[i].b, 0);   /* move there first */
            if (f == frame_no) dos32_mouse(evs[i].a, evs[i].b, 1);
            if (f == frame_no - 4) dos32_mouse(evs[i].a, evs[i].b, 0);
        } else if (evs[i].kind == 'd') {
            if (f == frame_no) {   /* --dump: the image and heap, for finding a variable */
                FILE* d = fopen(dump_path[evs[i].a], "wb");
                if (d) { fwrite(dos32_mem(0x400000), 1, 0x4000000, d); fclose(d); }
            }
        } else if (evs[i].kind == 'p') {          /* --poke: a guest byte */
            if (f == frame_no) *dos32_mem((uint32_t)evs[i].a) = (uint8_t)evs[i].b;
        } else if (evs[i].kind == 'f') {
            if (f == frame_no) game_fps = evs[i].a;
        } else if (evs[i].kind == 'k') {
            if (f == frame_no) dos32_key(evs[i].a, 1);
            if (f == frame_no - 4) dos32_key(evs[i].a, 0);
        }
    }
}

/* --record: frames piped to ffmpeg at 35 fps (every second retrace). */
static FILE* rec;
static FILE* wav;                /* --wav, or the sound of --record before it is muxed */
static char rec_video[MAX_PATH], wav_path[MAX_PATH];
static uint32_t rec_buf[1024 * 768];
static int rec_w, rec_h;
static uint32_t watch[8];
static uint16_t watch_v[8];
static int nwatch;

static void on_retrace(void) {
    static int tick;
    frame_no++;
    /* The park in the game's own 640x480 mode: the flag its resolution toggle
     * (R) keeps, set before the first park, so the game switches itself the
     * first time a park opens. Menus stay 320x200, as the game draws them. */
    if (frame_no == 1 && start_hires) *dos32_mem(0x4A1510) = 1;
    run_events();
    for (int i = 0; i < nwatch; i++) {     /* --watch: log a guest word when it changes */
        uint16_t v = *(uint16_t*)dos32_mem(watch[i]);
        if (v != watch_v[i] || frame_no == 1) {
            fprintf(stderr, "watch %.2fs %08X = %04X\n", frame_no / 70.0, watch[i], v);
            watch_v[i] = v;
        }
    }
    if (seconds > 0 && frame_no >= seconds * 70) dos32_quit = 1;   /* --seconds is guest time */
    if (headless) sound_tick(1.0 / 70);
    if (headless && !sound_mute) {           /* headless: the effects mixed on the frame clock */
        static int16_t pcm[44100 / 70 * 2];
        audio_render(pcm, 44100 / 70);
        if (wav) fwrite(pcm, 4, 44100 / 70, wav);
    }
    if (!rec || (tick++ & 1)) return;
    int w, h;
    if (!dos32_frame(rec_buf, 1024, 768, &w, &h)) { w = rec_w ? rec_w : 320; h = rec_h ? rec_h : 200; memset(rec_buf, 0, sizeof rec_buf); }
    if (!rec_w) { rec_w = w; rec_h = h; }
    if (w != rec_w || h != rec_h) {   /* one size per file: scale nearest into the first */
        static uint32_t tmp[1024 * 768];
        for (int y = 0; y < rec_h; y++)
            for (int x = 0; x < rec_w; x++) tmp[y * rec_w + x] = rec_buf[(y * h / rec_h) * w + x * w / rec_w];
        memcpy(rec_buf, tmp, (size_t)rec_w * rec_h * 4);
    }
    fwrite(rec_buf, 4, (size_t)rec_w * rec_h, rec);
}

static FILE* open_wav(const char* path) {
    FILE* f = fopen(path, "wb");
    if (f) { static const uint8_t blank[44]; fwrite(blank, 1, 44, f); }   /* header written at close */
    return f;
}

static void close_wav(void) {
    if (!wav) return;
    long n = ftell(wav) - 44;
    struct { char riff[4]; uint32_t size; char wave[4], fmt[4]; uint32_t fmtlen; uint16_t tag, ch;
             uint32_t rate, bps; uint16_t align, bits; char data[4]; uint32_t dlen; } h = {
        "RIFF", (uint32_t)n + 36, "WAVE", "fmt ", 16, 1, 2, 44100, 44100 * 4, 4, 16, "data", (uint32_t)n };
    fseek(wav, 0, SEEK_SET);
    fwrite(&h, sizeof h, 1, wav);
    fclose(wav);
    wav = NULL;
}

static void start_record(void) {
    /* The first frame decides the size, so ffmpeg starts on the first write;
     * 640x480 covers the menus and the park either way. */
    rec_w = 640; rec_h = 480;
    /* Video goes to a side file; the sound, if any, is muxed in at the end. */
    snprintf(rec_video, sizeof rec_video, "%s.video.mp4", record_path);
    if (!wav && !sound_mute) {
        snprintf(wav_path, sizeof wav_path, "%s.wav", record_path);
        wav = open_wav(wav_path);
    }
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 35 -i - "
             "-vf scale=1280:960:flags=neighbor -c:v libx264 -pix_fmt yuv420p \"%s\"", rec_w, rec_h, rec_video);
    rec = _popen(cmd, "wb");
    if (!rec) fprintf(stderr, "themepark: cannot start ffmpeg\n");
}

static DWORD WINAPI game_thread(LPVOID p) {
    (void)p;
    exit_code = dos32_run(&cfg);
    if (!headless) present_close();
    return 0;
}

static DWORD WINAPI timeout_thread(LPVOID p) {
    (void)p;
    Sleep((DWORD)(seconds * 1000));
    dos32_quit = 1;
    return 0;
}

int main(int argc, char** argv) {
    /* GOG's THEME.BAT passes -l3; the language number picks LANGn-0.DAT, and
     * 0 is English. --lang N picks another. */
    char args[256] = "-cC:/game/ -dC:/THEME.CD/ -l0";
    present_load_settings();                 /* themepark.ini; the options below override it */
    {
        char ini[MAX_PATH], v[32];
        GetModuleFileNameA(NULL, ini, MAX_PATH);
        strcpy(strrchr(ini, '\\') + 1, "themepark.ini");
        GetPrivateProfileStringA("game", "fps", "", v, sizeof v, ini);
        if (v[0]) game_fps = atof(v);
        start_hires = GetPrivateProfileIntA("game", "hires", 1, ini);
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--log")) log_on = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) dos32_speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) game_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            int m = present_scale_from_name(argv[++i]);
            if (m < 0) { fprintf(stderr, "themepark: --scale is sharp, smooth, crt, nearest or integer\n"); return 2; }
            g_present.scale = m; }
        else if (!strcmp(argv[i], "--fullscreen")) g_present.fullscreen = 1;
        else if (!strcmp(argv[i], "--gdi")) g_present.gdi = 1;
        else if (!strcmp(argv[i], "--mute")) sound_mute = 1;
        else if (!strcmp(argv[i], "--lowres")) start_hires = 0;
        else if (!strcmp(argv[i], "--hires")) start_hires = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wav = open_wav(argv[++i]); }
        else if (!strcmp(argv[i], "--present-test") && i + 1 < argc) { present_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--vsync") && i + 1 < argc) g_present.vsync = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) {
            char* l = strstr(args, "-l"); if (l) l[2] = argv[++i][0]; }
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) add_event('c', argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) add_event('k', argv[++i]);
        else if (!strcmp(argv[i], "--fps-at") && i + 1 < argc) add_event('f', argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) add_event('d', argv[++i]);
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) add_event('p', argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && nwatch < 8) watch[nwatch++] = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--")) {
            args[0] = 0;
            for (i++; i < argc; i++) { strncat(args, argv[i], sizeof args - strlen(args) - 2); strcat(args, " "); }
        } else { fprintf(stderr, "themepark: unknown option %s\n", argv[i]); return 2; }
    }
    if (!root[0]) {
        /* game\ beside the exe, or beside the repo when run from build\ */
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        *strrchr(exe, '\\') = 0;
        snprintf(root, sizeof root, "%s\\game", exe);
        if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) snprintf(root, sizeof root, "%s\\..\\game", exe);
    }
    if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "themepark: no game folder at %s (run Setup.cmd, or pass --game)\n", root);
        return 1;
    }

    cfg.root = root;
    cfg.cwd = "\\GAME";
    cfg.progname = "C:\\GAME\\MAIN.EXE";
    cfg.args = args;
    cfg.image = recomp_image;
    cfg.image_base = recomp_image_base;
    cfg.image_size = recomp_image_size;
    cfg.entry = recomp_entry_va;
    cfg.stack = recomp_image_stack;
    cfg.log = log_on;

    /* Headless runs make no sound on a machine someone uses: no audio device
     * and no MIDI; the effects are mixed on the frame clock for --record. */
    sound_headless = headless;
    audio_init(headless || sound_mute);
    /* Headless runs keep their own time: the same script gives the same run. */
    if (headless && !getenv("TP_REALTIME")) dos32_virtual_clock = 1;
    if (record_path) start_record();
    dos32_on_retrace = on_retrace;
    if (seconds > 0 && !dos32_virtual_clock)   /* a hung program still stops */
        CloseHandle(CreateThread(NULL, 0, timeout_thread, NULL, 0, NULL));
    HANDLE t = CreateThread(NULL, 256u << 20, game_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (headless) {
        WaitForSingleObject(t, INFINITE);
        if (present_dir) return present_test(present_dir);   /* the run's last screen, every mode */
    }
    else {
        present_run();          /* returns when the window closes */
        dos32_quit = 1;
        WaitForSingleObject(t, 5000);
    }
    if (rec) _pclose(rec);
    close_wav();
    if (record_path) {   /* video + sound -> --record's file */
        char cmd[2048];
        if (wav_path[0])
            snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -i \"%s\" -i \"%s\" -c:v copy -c:a aac -shortest \"%s\"",
                     rec_video, wav_path, record_path);
        else
            snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -i \"%s\" -c copy \"%s\"", rec_video, record_path);
        if (system(cmd) == 0) { remove(rec_video); if (wav_path[0]) remove(wav_path); }
    }
    return exit_code < 0 ? 0 : exit_code;
}
