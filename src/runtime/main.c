/*
 * Theme Park host: runs the lifted MAIN.EXE on pcrecomp's dos32 host and
 * shows it in a window, or headless with --record.
 *
 *   themepark.exe [--headless] [--record out.mp4] [--seconds N] [--log]
 *                 [--click T:x,y] [--key T:sc] [--save T:slot] [--load T:slot]
 *                 [--game DIR] [--fps N] [--res WxH] [--intro | --nointro] [--music PACK]
 *                 [--scale sharp|smooth|crt|nearest|integer] [--fullscreen] [--gdi]
 *                 [--rules KEY,KEY|none] [--call T:ADDR[:A1[:A2[:A3]]]]
 *                 [-- game args]
 *
 * The game itself runs on a thread of its own with a large stack (each guest
 * call is a host call, and the game recurses); this thread owns the window.
 * docs/architecture.md has the picture.
 */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos32.h"
#include "present.h"
#include "audio.h"
#include "music.h"
#include "rules.h"

extern const uint32_t recomp_image_base, recomp_image_size, recomp_image_stack, recomp_entry_va;
extern const uint8_t recomp_image[];
#ifdef HAVE_INTRO   /* INTRO.EXE, lifted beside the game (run_lift.py --intro) */
#include "recomp_types.h"
extern const uint32_t intro_image_base, intro_image_size, intro_image_stack, intro_entry_va, intro_dispatch_count;
extern const uint8_t intro_image[];
extern const recomp_dispatch_entry_t intro_dispatch_table[];
#endif
extern double game_fps;   /* game.c */
extern volatile int res_w;   /* game.c: the park's width (#11) */
int res_parse(const char* s);
extern volatile int state_request;   /* game.c: save states */
extern int sound_mute, sound_headless;   /* sound.c */
extern uint32_t sound_variant_va, sound_noop_fn;
extern int net_host_players, net_port, net_resume;   /* net.c: multiplayer */
extern char net_join[256];
extern int net_wait_on_drop;
void net_rules_give(int to, int amount), net_rules_grant(int to, int amount), net_rules_share(int to, int on);
void sound_tick(double dt);
void sound_reset(void);

static dos32_config cfg;
static char root[MAX_PATH];
static int headless, log_on;
static double seconds;
static const char* record_path;
static volatile int exit_code = -1;
static const char* present_dir;   /* --present-test */
static int start_hires = 1;       /* --lowres, [game] hires=0: the original's default */
static char mod_name[MAX_PATH];   /* --mod, [game] mod= */
static char music_name[64];       /* --music, [sound] music=: a music pack */
static char overlay[MAX_PATH];
static int play_intro = -1;       /* --intro / --nointro, [game] intro=; -1: windowed runs only */
static long game_frame0;          /* frame_no when MAIN.EXE started (after the intro) */

/* Scripted input for headless runs: --click T:x,y and --key T:sc (seconds
 * of guest time, game pixels, a set-1 scancode in hex). Each fires once at
 * its frame of the 70 Hz clock; a click is a press then a release. */
typedef struct { long frame; int kind, a, b; } ev_t;
static char dump_path[8][MAX_PATH];
static int ndump;
static ev_t evs[256];
static uint32_t call_args[16][5];   /* --call: address, count, arguments */
static int ncall;
static int nev;
long frame_no;               /* 70 Hz frames of guest time; sound.c logs with it */

static void add_event(int kind, const char* spec) {
    double t; int a = 0, b = 0;
    if (nev >= 256) return;
    if ((kind == 'c' || kind == 'm') && sscanf(spec, "%lf:%d,%d", &t, &a, &b) != 3) return;
    if (kind == 'k' && sscanf(spec, "%lf:%x", &t, &a) != 2) return;
    if ((kind == 'f' || kind == 's' || kind == 'l') && sscanf(spec, "%lf:%d", &t, &a) != 2) return;
    if (kind == 'p' && sscanf(spec, "%lf:%x=%x", &t, &a, &b) != 3) return;
    if ((kind == 'G' || kind == 'H') && sscanf(spec, "%lf:%d:%d", &t, &a, &b) != 3) return;
    if (kind == 'R' && sscanf(spec, "%lf:%d", &t, &a) != 2) return;
    if (kind == 'C') {                 /* --call T:ADDR[:A1[:A2[:A3]]], hex */
        uint32_t* c = call_args[ncall];
        int n = ncall < 16 ? sscanf(spec, "%lf:%x:%x:%x:%x", &t, &c[0], &c[2], &c[3], &c[4]) : 0;
        if (n < 2) return;
        c[1] = (uint32_t)(n - 2); a = ncall++;
    }
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
        } else if (evs[i].kind == 'm') {          /* --move T:dx,dy: relative, as a captured mouse */
            if (f == frame_no) dos32_mouse_move(evs[i].a, evs[i].b, 0);
        } else if (evs[i].kind == 'd') {
            if (f == frame_no) {   /* --dump: the image and heap, for finding a variable */
                FILE* d = fopen(dump_path[evs[i].a], "wb");
                if (d) { fwrite(dos32_mem(0x400000), 1, 0x4C00000, d); fclose(d); }   /* up to the heap top, 0x5000000 */
            }
        } else if (evs[i].kind == 'p') {          /* --poke: a guest byte */
            if (f == frame_no) *dos32_mem((uint32_t)evs[i].a) = (uint8_t)evs[i].b;
        } else if (evs[i].kind == 'G' || evs[i].kind == 'H' || evs[i].kind == 'R') {   /* house rules, players from 1 */
            if (f == frame_no) {
                if (evs[i].kind == 'G') net_rules_give(evs[i].a - 1, evs[i].b);
                else if (evs[i].kind == 'H') net_rules_grant(evs[i].a - 1, evs[i].b);
                else net_rules_share(evs[i].a - 1, 1);
            }
        } else if (evs[i].kind == 'C') {
            if (f == frame_no) rules_queue_call(call_args[evs[i].a][0], (int)call_args[evs[i].a][1], &call_args[evs[i].a][2]);
        } else if (evs[i].kind == 'f') {
            if (f == frame_no) game_fps = evs[i].a;
        } else if (evs[i].kind == 's' || evs[i].kind == 'l') {   /* --save / --load T:slot */
            if (f == frame_no) state_request = evs[i].kind == 's' ? evs[i].a : -evs[i].a;
        } else if (evs[i].kind == 'k') {
            if (f == frame_no) dos32_key(evs[i].a, 1);
            if (f == frame_no - 12) dos32_key(evs[i].a, 0);   /* held a sixth of a second: the park polls keys slowly */
        }
    }
}

/* --record: frames piped to ffmpeg at 35 fps (every second retrace). */
static FILE* rec;
static FILE* wav;                /* --wav, or the sound of --record before it is muxed */
static char rec_video[MAX_PATH], wav_path[MAX_PATH];
static uint32_t rec_buf[1920 * 1080];
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
    if (frame_no == game_frame0 + 1 && start_hires && game_frame0 >= 0) *dos32_mem(0x4A1510) = 1;
    run_events();
#ifdef RECOMP_TRACE
    {   /* the trace build: TP_TRACE=T0,T1 */
        extern int trace_on; void trace_report(void);
        static double t0 = -1, t1;
        if (t0 < 0) { const char* e = getenv("TP_TRACE"); t0 = e ? atof(e) : 0; t1 = e && strchr(e, ',') ? atof(strchr(e, ',') + 1) : 0; }
        if (t1 > 0 && frame_no == (long)(t0 * 70)) trace_on = 1;
        if (t1 > 0 && frame_no == (long)(t1 * 70)) { trace_on = 0; trace_report(); }
    }
#endif
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
    if (!dos32_frame(rec_buf, 1920, 1080, &w, &h)) { w = rec_w ? rec_w : 320; h = rec_h ? rec_h : 200; memset(rec_buf, 0, sizeof rec_buf); }
    else lights_apply(rec_buf, w, h);
    if (!rec_w) { rec_w = w; rec_h = h; }
    if (w != rec_w || h != rec_h) {   /* one size per file: scale nearest into the first */
        static uint32_t tmp[1920 * 1080];
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
#ifdef _WIN32
    rec = _popen(cmd, "wb");
#else
    rec = popen(cmd, "w");
#endif
    if (!rec) fprintf(stderr, "themepark: cannot start ffmpeg\n");
}

static DWORD WINAPI game_thread(LPVOID p) {
    (void)p;
#ifdef HAVE_INTRO
    /* The opening film, as GOG's THEME.BAT runs it before the game: INTRO.EXE
     * on the same machine, then MAIN.EXE on a fresh one. Esc or Space skips
     * it (the film's own test; it never reads the mouse). */
    if (play_intro) {
        dos32_config ic = cfg;
        ic.progname = "C:\\GAME\\INTRO.EXE";
        ic.args = "-C:/THEME.CD";
        ic.image = intro_image;
        ic.image_base = intro_image_base;
        ic.image_size = intro_image_size;
        ic.entry = intro_entry_va;
        ic.stack = intro_image_stack;
        ic.dispatch = intro_dispatch_table;
        ic.dispatch_count = intro_dispatch_count;
        uint32_t variant = sound_variant_va, noop = sound_noop_fn;
        sound_variant_va = 0x430AAC; sound_noop_fn = 0x004008C4u;   /* the intro's */
        game_frame0 = -1;
        dos32_run(&ic);
        sound_reset();   /* skipped mid-song: nothing of the film plays on */
        sound_variant_va = variant; sound_noop_fn = noop;
        if (dos32_quit) { if (!headless) present_close(); return 0; }
        game_frame0 = frame_no;
    }
#endif
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
        exe_dir(ini, sizeof ini);
        strncat(ini, "themepark.ini", sizeof ini - strlen(ini) - 1);
        GetPrivateProfileStringA("game", "fps", "", v, sizeof v, ini);
        if (v[0]) game_fps = atof(v);
        start_hires = GetPrivateProfileIntA("game", "hires", 1, ini);
        GetPrivateProfileStringA("game", "res", "", v, sizeof v, ini);
        res_w = res_parse(v);
        play_intro = GetPrivateProfileIntA("game", "intro", -1, ini);
        GetPrivateProfileStringA("game", "mod", "", mod_name, sizeof mod_name, ini);
        GetPrivateProfileStringA("sound", "music", "", music_name, sizeof music_name, ini);
        GetPrivateProfileStringA("game", "lang", "0", v, sizeof v, ini);   /* F10, Language */
        if (v[0] >= '0' && v[0] <= '4') { char* l = strstr(args, "-l"); if (l) l[2] = v[0]; }
        rules_load(ini);
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
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) {
            res_w = res_parse(argv[++i]);
            if (!res_w && strcmp(argv[i], "640x480")) {
                fprintf(stderr, "themepark: --res is WxH, 640 to 1920 wide and 480 high (e.g. 856x480)\n"); return 2; } }
        else if (!strcmp(argv[i], "--intro")) play_intro = 1;
        else if (!strcmp(argv[i], "--nointro")) play_intro = 0;
        else if (!strcmp(argv[i], "--mod") && i + 1 < argc) snprintf(mod_name, sizeof mod_name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) snprintf(music_name, sizeof music_name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--hires")) start_hires = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wav = open_wav(argv[++i]); }
        else if (!strcmp(argv[i], "--present-test") && i + 1 < argc) { present_dir = argv[++i]; headless = 1; }
        else if (!strcmp(argv[i], "--vsync") && i + 1 < argc) g_present.vsync = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) {
            char* l = strstr(args, "-l"); if (l) l[2] = argv[++i][0]; }
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) add_event('c', argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) add_event('k', argv[++i]);
        else if (!strcmp(argv[i], "--move") && i + 1 < argc) add_event('m', argv[++i]);
        else if (!strcmp(argv[i], "--fps-at") && i + 1 < argc) add_event('f', argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) add_event('s', argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) add_event('l', argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) add_event('d', argv[++i]);
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) add_event('C', argv[++i]);
        else if (!strcmp(argv[i], "--rules") && i + 1 < argc) {   /* exactly these modern rules, for this run */
            const char* v = argv[++i];
            tp_rules = 0;
            for (int k = 0; k < rule_count; k++) {
                const char* m = strstr(v, rule_list[k].key);
                size_t n = strlen(rule_list[k].key);
                if (m && (m == v || m[-1] == ',') && (m[n] == 0 || m[n] == ',')) tp_rules |= rule_list[k].bit;
            }
        }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) add_event('p', argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && nwatch < 8) watch[nwatch++] = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            net_host_players = atoi(argv[++i]);
            if (net_host_players < 2 || net_host_players > 4) { fprintf(stderr, "themepark: --host takes 2, 3 or 4 players\n"); return 2; } }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) snprintf(net_join, sizeof net_join, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) net_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resume")) net_resume = 1;
        else if (!strcmp(argv[i], "--give") && i + 1 < argc) add_event('G', argv[++i]);    /* T:PLAYER:AMOUNT */
        else if (!strcmp(argv[i], "--grant") && i + 1 < argc) add_event('H', argv[++i]);   /* T:PLAYER:AMOUNT, the host */
        else if (!strcmp(argv[i], "--share") && i + 1 < argc) add_event('R', argv[++i]);   /* T:PLAYER */
        else if (!strcmp(argv[i], "--")) {
            args[0] = 0;
            for (i++; i < argc; i++) { strncat(args, argv[i], sizeof args - strlen(args) - 2); strcat(args, " "); }
        } else { fprintf(stderr, "themepark: unknown option %s\n", argv[i]); return 2; }
    }
    if (headless && !getenv("TP_NET_REJOIN")) net_wait_on_drop = 0;   /* a test ends when a player does */
    /* The game's own switch for a network game; a joining machine is told
     * the real count by the host (net.c). */
    if ((net_host_players || net_join[0]) && !strstr(args, "-m")) {
        size_t l = strlen(args);
        snprintf(args + l, sizeof args - l, " -m%d", net_host_players ? net_host_players : 2);
    }
    if (!root[0]) {
        /* game/ beside the program, or beside the repo when run from build/ */
        char exe[MAX_PATH];
        exe_dir(exe, sizeof exe);
        snprintf(root, sizeof root, "%sgame", exe);
        if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) snprintf(root, sizeof root, "%s../game", exe);
    }
    if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "themepark: no game folder at %s (run the setup, or pass --game)\n", root);
        return 1;
    }

    cfg.root = root;
    /* A mod is a folder laid over the game folder, same layout: mods\NAME
     * beside game\, or any path. Files it has replace the game's for reading. */
    if (mod_name[0]) {
        if (strchr(mod_name, '\\') || strchr(mod_name, '/')) snprintf(overlay, sizeof overlay, "%s", mod_name);
        else snprintf(overlay, sizeof overlay, "%s/../mods/%s", root, mod_name);
        if (GetFileAttributesA(overlay) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "themepark: no mod folder %s\n", overlay);
            return 1;
        }
        cfg.overlay = overlay;
    }
    /* Music packs: music\NAME beside mods\, songs named from the game's banks. */
    {
        char packs[MAX_PATH], data[MAX_PATH];
        snprintf(packs, sizeof packs, "%s/../music", root);
        snprintf(data, sizeof data, "%s/GAME/DATA", root);
        if (GetFileAttributesA(data) == INVALID_FILE_ATTRIBUTES) snprintf(data, sizeof data, "%s/game/data", root);
        music_set_dirs(packs, data);
        music_set_pack(music_name);
    }
    cfg.cwd = "\\GAME";
    cfg.progname = "C:\\GAME\\MAIN.EXE";
    cfg.args = args;
    cfg.image = recomp_image;
    cfg.image_base = recomp_image_base;
    cfg.image_size = recomp_image_size;
    cfg.entry = recomp_entry_va;
    cfg.stack = recomp_image_stack;
    cfg.log = log_on;

    /* Scripted runs time their input from the start of the game: no film first. */
    if (play_intro < 0) play_intro = !headless;
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
