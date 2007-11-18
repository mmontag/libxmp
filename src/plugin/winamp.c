/*
 * XMP plugin for WinAmp
 *
 * $Id: winamp.c,v 1.13 2007-11-18 16:16:50 cmatsuoka Exp $
 */

#include <windows.h>
#include <mmreg.h>
#include <msacm.h>
#include <math.h>

#include "xmpi.h"
#include "in2.h"


// avoid CRT. Evil. Big. Bloated.
BOOL WINAPI
_DllMainCRTStartup(HANDLE hInst, ULONG ul_reason_for_call, LPVOID lpReserved)
{
	return TRUE;
}

DWORD WINAPI __stdcall play_loop(void *);

static int paused;
HANDLE decode_thread = INVALID_HANDLE_VALUE;
HANDLE load_mutex;

#define FREQ_SAMPLE_44 0
#define FREQ_SAMPLE_22 1
#define FREQ_SAMPLE_11 2

typedef struct {
	int mixing_freq;
	int force8bit;
	int force_mono;
	int interpolation;
	int filter;
	int convert8bit;
	int fixloops;
	int loop;
	int modrange;
	int pan_amplitude;
	int time;
	struct xmp_module_info mod_info;
} XMPConfig;

XMPConfig xmp_cfg;
static int xmp_plugin_audio_error = FALSE;
static short audio_open = FALSE;
static xmp_context ctx;
static int playing;

static void	config		(HWND);
static void	about		(HWND);
static void	init		(void);
static void	quit		(void);
static void	get_file_info	(char *, char *, int *);
static int	infoDlg		(char *, HWND);
static int	is_our_file	(char *);
static int	play_file	(char *);
static void	pause		(void);
static void	unpause		(void);
static int	is_paused	(void);
static void	stop		(void);
static int	getlength	(void);
static int	getoutputtime	(void);
static void	setoutputtime	(int);
static void	setvolume	(int);
static void	setpan		(int);
static void	eq_set		(int, char[10], int);

In_Module mod = {
	IN_VER,
	"XMP Plugin " VERSION,
	0,			/* hMainWindow */
	0,			/* hDllInstance */
	"\0",			/* DLL instance handle */
	1,			/* is_seekable */
	1,			/* uses output */
	config,
	about,
	init,
	quit,
	get_file_info,
	infoDlg,
	is_our_file,
	play_file,
	pause,
	unpause,
	is_paused,
	stop,
	getlength,
	getoutputtime,
	setoutputtime,
	setvolume,
	setpan,
	0, 0, 0, 0, 0, 0, 0, 0, 0,	// vis stuff
	0, 0,			// dsp
	eq_set, NULL,		// setinfo
	0			// out_mod
};

__declspec(dllexport) In_Module *winampGetInModule2()
{
	return &mod;
}


static void stop()
{
	if (!playing)
		return;

	xmp_stop_module(ctx);

	if (decode_thread != INVALID_HANDLE_VALUE) {
		if (WaitForSingleObject(decode_thread, INFINITE) ==
		    WAIT_TIMEOUT) {
			MessageBox(mod.hMainWindow,
				   "error asking thread to die!\n",
				   "error killing decode thread", 0);
			TerminateThread(decode_thread, 0);
		}
		CloseHandle(decode_thread);
		decode_thread = INVALID_HANDLE_VALUE;
	}
	mod.outMod->Close();
	mod.SAVSADeInit();
}

static void driver_callback(void *b, int i)
{
	int numch = xmp_cfg.force_mono ? 1 : 2;
	int n = (i / 2) << (mod.dsp_isactive()? 1 : 0);
	int t;

	while (mod.outMod->CanWrite() < n)
		Sleep(50);

	t = mod.outMod->GetWrittenTime();
	mod.SAAddPCMData(b, numch, 16, t);
	mod.VSAAddPCMData(b, numch, 16, t);

	mod.outMod->Write(b, i);
}

static void config(HWND hwndParent)
{
	MessageBox(hwndParent, "Later.", "Configuration", MB_OK);
}

static void about(HWND hwndParent)
{
	MessageBox(hwndParent, "Later.", "About XMP", MB_OK);
}

static void init()
{
	//ConfigFile *cfg;
	//char *filename;

	ctx = xmp_create_context();

	xmp_cfg.mixing_freq = 0;
	xmp_cfg.convert8bit = 0;
	xmp_cfg.fixloops = 0;
	xmp_cfg.modrange = 0;
	xmp_cfg.force8bit = 0;
	xmp_cfg.force_mono = 0;
	xmp_cfg.interpolation = TRUE;
	xmp_cfg.filter = TRUE;
	xmp_cfg.pan_amplitude = 80;

#if 0
#define CFGREADINT(x) xmms_cfg_read_int (cfg, "XMP", #x, &xmp_cfg.x)

	filename = g_strconcat(g_get_home_dir(), CONFIG_FILE, NULL);

	if ((cfg = xmms_cfg_open_file(filename))) {
		CFGREADINT(mixing_freq);
		CFGREADINT(force8bit);
		CFGREADINT(convert8bit);
		CFGREADINT(modrange);
		CFGREADINT(fixloops);
		CFGREADINT(force_mono);
		CFGREADINT(interpolation);
		CFGREADINT(filter);
		CFGREADINT(pan_amplitude);

		xmms_cfg_free(cfg);
	}
#endif

	xmp_init_callback(ctx, driver_callback);
	//xmp_register_event_callback(x11_event_callback);
}

static void quit()
{
}

static int is_our_file(char *fn)
{
	_D(_D_WARN "fn = %s", fn);
	if (xmp_test_module(ctx, fn, NULL) == 0)
		return 1;

	return 0;
}

static int play_file(char *fn)
{
	int maxlatency;
	DWORD tmp;
	int numch = 1;
	FILE *f;
	struct xmp_options *opt;
	int lret;
	int /*fmt,*/ nch;

	_D("fn = %s", fn);

	opt = xmp_get_options(ctx);

	stop();				/* sanity check */

	if ((f = fopen(fn, "rb")) == NULL) {
		playing = 0;
		return -1;
	}
	fclose(f);

	xmp_plugin_audio_error = FALSE;
	playing = 1;

	opt->resol = 8;
	opt->verbosity = 0;
	opt->drv_id = "callback";

	switch (xmp_cfg.mixing_freq) {
	case 1:
		opt->freq = 22050;	/* 1:2 mixing freq */
		break;
	case 2:
		opt->freq = 11025;	/* 1:4 mixing freq */
		break;
	default:
		opt->freq = 44100;	/* standard mixing freq */
		break;
	}

	if (xmp_cfg.force8bit == 0)
		opt->resol = 16;

	if (xmp_cfg.force_mono == 0) {
		numch = 2;
	} else {
		opt->outfmt |= XMP_FMT_MONO;
	}

	if (xmp_cfg.interpolation == 1)
		opt->flags |= XMP_CTL_ITPT;

	if (xmp_cfg.filter == 1)
		opt->flags |= XMP_CTL_FILTER;

	opt->mix = xmp_cfg.pan_amplitude;

	//fmt = opt->resol == 16 ? FMT_S16_NE : FMT_U8;
	nch = opt->outfmt & XMP_FMT_MONO ? 1 : 2;

	if (audio_open)
		mod.outMod->Close();
	audio_open = TRUE;

	paused = 0;

	maxlatency = mod.outMod->Open(opt->freq, numch, opt->resol, -1, -1);
	if (maxlatency < 0)
		return 1;
	mod.SetInfo(0, 44, 1, 1);
	mod.SAVSAInit(maxlatency, 44100);
	mod.VSASetInfo(44100, 1);
	mod.outMod->SetVolume(-666);

	xmp_open_audio(ctx);

	load_mutex = CreateMutex(NULL, TRUE, "load_mutex");
	lret = xmp_load_module(ctx, fn);
	ReleaseMutex(load_mutex);

	xmp_cfg.time = lret;
	xmp_get_module_info(ctx, &xmp_cfg.mod_info);

	decode_thread = (HANDLE)CreateThread(NULL, 0,
			(LPTHREAD_START_ROUTINE)play_loop, NULL, 0, &tmp);
	return 0;
}

DWORD WINAPI __stdcall play_loop(void *b)
{
	_D(_D_WARN "play");
	xmp_play_module(ctx);
	xmp_release_module(ctx);
	xmp_close_audio(ctx);
	playing = 0;

	_D(_D_WARN "exit thread");
	ExitThread(0);

	return 0;
}

static void pause()
{
	paused = 1;
	mod.outMod->Pause(1);
}

static void unpause()
{
	paused = 0;
	mod.outMod->Pause(0);
}

static int is_paused()
{
	return paused;
}

static int getlength()
{
	return xmp_cfg.time;
}

static int getoutputtime()
{
	if (!playing)
		return -1;

	return mod.outMod->GetOutputTime();
}

static void setoutputtime(int time)
{
	int i, t;
	struct xmp_player_context *p = &((struct xmp_context *)ctx)->p;

	_D("seek to %d, total %d", time, xmp_cfg.time);

	for (i = 0; i < xmp_cfg.mod_info.len; i++) {
		t = p->m.xxo_info[i].time;

		_D("%2d: %d %d", i, time, t);

		if (t > time) {
			int a;
			if (i > 0)
				i--;
			a = xmp_ord_set(ctx, i);
			mod.outMod->Flush(p->m.xxo_info[i].time);
			break;
		}
	}
}

static void setvolume(int volume)
{
	mod.outMod->SetVolume(volume);
}

static void setpan(int pan)
{
	mod.outMod->SetPan(pan);
}

static int infoDlg(char *fn, HWND hwnd)
{
	return 0;
}

static void get_file_info(char *filename, char *title, int *length_in_ms)
{
	xmp_context ctx2;
	int lret;
	struct xmp_module_info mi;
	struct xmp_options *opt;

	_D(_D_WARN "filename = %s", filename);

	/* Winamp docs say "if filename == NULL, current playing is used"
	 * but it seems to pass an empty filename. I'll test for both
	 */
	if (filename == NULL || strlen(filename) == 0) {
		xmp_get_module_info(ctx, &mi);
		wsprintf(title, "%s", mi.name);
		return;
	}

	/* Create new context to load a file and get the length */
	
	_D("create context");
	ctx2 = xmp_create_context();
	_D("get options");
	opt = xmp_get_options(ctx2);
	opt->skipsmp = 1;	/* don't load samples */

	_D("create mutex");
	load_mutex = CreateMutex(NULL, TRUE, "load_mutex");
	_D("load module");
	lret = xmp_load_module(ctx2, filename);
	_D("release mutex");
	ReleaseMutex(load_mutex);

	if (lret < 0) {
		_D("free context");
		xmp_free_context(ctx2);
		return;
        }

	*length_in_ms = lret;
	_D("length_in_ms = %d", *length_in_ms);
	xmp_get_module_info(ctx2, &mi);
	wsprintf(title, "%s", mi.name);
	_D("title = %s", mi.name);


        xmp_release_module(ctx2);
        xmp_free_context(ctx2);
}

static void eq_set(int on, char data[10], int preamp)
{
}

