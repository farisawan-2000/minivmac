/*
	OSGLUNDS.c

	Copyright (C) 2012 Lazyone, Paul C. Pratt

	You can redistribute this file and/or modify it under the terms
	of version 2 of the GNU General Public License as published by
	the Free Software Foundation.  You should have received a copy
	of the license along with this file; see the file COPYING.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	license for more details.
*/

/*
	Operating System GLUe for Nintendo 64
	TODO: yeet all devkitpro and nds specific things
*/

#include "OSGCOMUI.h"
#include "OSGCOMUD.h"

#ifdef WantOSGLUN64

#include <libdragon.h>
#include <time.h>
#include "N64TYPES.h"

#define CONSOLE_TRACE() \
	fprintf(stderr, "%s() at line %d\n", __FUNCTION__, __LINE__)

/* --- some simple utilities --- */

GLOBALOSGLUPROC MyMoveBytes(anyp srcPtr, anyp destPtr, si5b byteCount)
{
	(void) memcpy((char *)destPtr, (char *)srcPtr, byteCount);
}

/*
	Nintendo DS port globals
*/
#define N64_ScreenWidth 640
#define N64_ScreenHeight 480
static surface_t zbuffer;
#define NOKEY 0

LOCALVAR volatile int VBlankCounter = 0;
LOCALVAR volatile int HBlankCounter = 0;
LOCALVAR volatile unsigned int TimerBaseMSec = 0;
LOCALVAR joypad_inputs_t __attribute__((aligned(8))) LastKeyboardKey = {0};
LOCALVAR joypad_inputs_t __attribute__((aligned(8))) KeyboardKey = {0};
LOCALVAR joypad_inputs_t __attribute__((aligned(8))) KeysHeld = {0};
LOCALVAR volatile int CursorX = 0;
LOCALVAR volatile int CursorY = 0;
LOCALVAR int Display_bg2_Main = 0;

// todo: adpcm AI buffer etc
LOCALVAR volatile tpSoundSamp TheSoundBuffer = 0;

/* --- control mode and internationalization --- */

#define NeedCell2PlainAsciiMap 1

#include "INTLCHAR.h"

/* --- sending debugging info to file --- */

#if dbglog_HAVE

#define dbglog_ToStdErr 1

#if ! dbglog_ToStdErr
LOCALVAR FILE *dbglog_File = NULL;
#endif

LOCALFUNC blnr dbglog_open0(void)
{
#if dbglog_ToStdErr
	return trueblnr;
#else
	dbglog_File = fopen("dbglog.txt", "w");
	return (NULL != dbglog_File);
#endif
}

LOCALPROC dbglog_write0(char *s, uimr L)
{
#if dbglog_ToStdErr
	(void) fwrite(s, 1, L, stderr);
#else
	if (dbglog_File != NULL) {
		(void) fwrite(s, 1, L, dbglog_File);
	}
#endif
}

LOCALPROC dbglog_close0(void)
{
#if ! dbglog_ToStdErr
	if (dbglog_File != NULL) {
		fclose(dbglog_File);
		dbglog_File = NULL;
	}
#endif
}

#endif

/* --- debug settings and utilities --- */

#if ! dbglog_HAVE
#define WriteExtraErr(s)
#else
LOCALPROC WriteExtraErr(char *s)
{
	dbglog_writeCStr("*** error: ");
	dbglog_writeCStr(s);
	dbglog_writeReturn();
}
#endif

/* --- information about the environment --- */

#define WantColorTransValid 0

#include "COMOSGLU.h"
#include "CONTROLM.h"

LOCALPROC NativeStrFromCStr(char *r, char *s)
{
	ui3b ps[ClStrMaxLength];
	int i;
	int L;

	ClStrFromSubstCStr(&L, ps, s);

	for (i = 0; i < L; ++i) {
		r[i] = Cell2PlainAsciiMap[ps[i]];
	}

	r[L] = 0;
}

/* --- drives --- */

#define NotAfileRef NULL

LOCALVAR FILE *Drives[NumDrives]; /* open disk image files */
#if IncludeSonyGetName || IncludeSonyNew
LOCALVAR char *DriveNames[NumDrives];
#endif

LOCALPROC InitDrives(void)
{
	/*
		This isn't really needed, Drives[i] and DriveNames[i]
		need not have valid values when not vSonyIsInserted[i].
	*/
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		Drives[i] = NotAfileRef;
#if IncludeSonyGetName || IncludeSonyNew
		DriveNames[i] = NULL;
#endif
	}
}

GLOBALOSGLUFUNC tMacErr vSonyTransfer(blnr IsWrite, ui3p Buffer,
	tDrive Drive_No, ui5r Sony_Start, ui5r Sony_Count,
	ui5r *Sony_ActCount)
{
	tMacErr err = mnvm_miscErr;
	FILE *refnum = Drives[Drive_No];
	ui5r NewSony_Count = 0;

	if (0 == fseek(refnum, Sony_Start, SEEK_SET)) {
		if (IsWrite) {
			NewSony_Count = fwrite(Buffer, 1, Sony_Count, refnum);
		} else {
			NewSony_Count = fread(Buffer, 1, Sony_Count, refnum);
		}

		if (NewSony_Count == Sony_Count) {
			err = mnvm_noErr;
		}
	}

	if (nullpr != Sony_ActCount) {
		*Sony_ActCount = NewSony_Count;
	}

	return err; /*& figure out what really to return &*/
}

GLOBALOSGLUFUNC tMacErr vSonyGetSize(tDrive Drive_No, ui5r *Sony_Count)
{
	tMacErr err = mnvm_miscErr;
	FILE *refnum = Drives[Drive_No];
	long v;

	if (0 == fseek(refnum, 0, SEEK_END)) {
		v = ftell(refnum);
		if (v >= 0) {
			*Sony_Count = v;
			err = mnvm_noErr;
		}
	}

	return err; /*& figure out what really to return &*/
}

LOCALFUNC tMacErr vSonyEject0(tDrive Drive_No, blnr deleteit)
{
	FILE *refnum = Drives[Drive_No];

	DiskEjectedNotify(Drive_No);

	fclose(refnum);
	Drives[Drive_No] = NotAfileRef; /* not really needed */

#if IncludeSonyGetName || IncludeSonyNew
	{
		char *s = DriveNames[Drive_No];
		if (NULL != s) {
			if (deleteit) {
				remove(s);
			}
			free(s);
			DriveNames[Drive_No] = NULL; /* not really needed */
		}
	}
#endif

	return mnvm_noErr;
}

GLOBALOSGLUFUNC tMacErr vSonyEject(tDrive Drive_No)
{
	return vSonyEject0(Drive_No, falseblnr);
}

#if IncludeSonyNew
GLOBALOSGLUFUNC tMacErr vSonyEjectDelete(tDrive Drive_No)
{
	return vSonyEject0(Drive_No, trueblnr);
}
#endif

LOCALPROC UnInitDrives(void)
{
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		if (vSonyIsInserted(i)) {
			(void) vSonyEject(i);
		}
	}
}

#if IncludeSonyGetName
GLOBALOSGLUFUNC tMacErr vSonyGetName(tDrive Drive_No, tPbuf *r)
{
	char *drivepath = DriveNames[Drive_No];
	if (NULL == drivepath) {
		return mnvm_miscErr;
	} else {
		char *s = strrchr(drivepath, '/');
		if (NULL == s) {
			s = drivepath;
		} else {
			++s;
		}
		return NativeTextToMacRomanPbuf(s, r);
	}
}
#endif

LOCALFUNC blnr Sony_Insert0(FILE *refnum, blnr locked,
	char *drivepath)
{
	tDrive Drive_No;
	blnr IsOk = falseblnr;

	if (! FirstFreeDisk(&Drive_No)) {
		MacMsg(kStrTooManyImagesTitle, kStrTooManyImagesMessage,
			falseblnr);
	} else {
		/* printf("Sony_Insert0 %d\n", (int)Drive_No); */

		{
			Drives[Drive_No] = refnum;
			DiskInsertNotify(Drive_No, locked);

#if IncludeSonyGetName || IncludeSonyNew
			{
				ui5b L = strlen(drivepath);
				char *p = malloc(L + 1);
				if (p != NULL) {
					(void) memcpy(p, drivepath, L + 1);
				}
				DriveNames[Drive_No] = p;
			}
#endif

			IsOk = trueblnr;
		}
	}

	if (! IsOk) {
		fclose(refnum);
	}

	return IsOk;
}

LOCALFUNC blnr Sony_Insert1(char *drivepath, blnr silentfail)
{
	blnr locked = falseblnr;
	/* printf("Sony_Insert1 %s\n", drivepath); */
	FILE *refnum = fopen(drivepath, "rb+");
	if (NULL == refnum) {
		locked = trueblnr;
		refnum = fopen(drivepath, "rb");
		CONSOLE_TRACE();
	}
	if (NULL == refnum) {
		if (! silentfail) {
			MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
			CONSOLE_TRACE();
		}
	} else {
		CONSOLE_TRACE();
		return Sony_Insert0(refnum, locked, drivepath);
	}

	return falseblnr;
}

#define Sony_Insert2(s) Sony_Insert1(s, trueblnr)

LOCALFUNC blnr Sony_InsertIth(int i)
{
	blnr v;

	if ((i > 9) || ! FirstFreeDisk(nullpr)) {
		v = falseblnr;
	} else {
		char s[] = "disk?.dsk";

		s[4] = '0' + i;

		v = Sony_Insert2(s);
	}

	return v;
}

LOCALFUNC blnr LoadInitialImages(void)
{
	int i;

	CONSOLE_TRACE();

	for (i = 1; Sony_InsertIth(i); ++i) {
		/* stop on first error (including file not found) */
	}

	return trueblnr;
}

#if IncludeSonyNew
LOCALFUNC blnr WriteZero(FILE *refnum, ui5b L)
{
#define ZeroBufferSize 2048
	ui5b i;
	ui3b buffer[ZeroBufferSize];

	memset(&buffer, 0, ZeroBufferSize);

	while (L > 0) {
		i = (L > ZeroBufferSize) ? ZeroBufferSize : L;
		if (fwrite(buffer, 1, i, refnum) != i) {
			return falseblnr;
		}
		L -= i;
	}
	return trueblnr;
}
#endif

#if IncludeSonyNew
LOCALPROC MakeNewDisk(ui5b L, char *drivepath)
{
	blnr IsOk = falseblnr;
	FILE *refnum = fopen(drivepath, "wb+");
	if (NULL == refnum) {
		MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
	} else {
		if (WriteZero(refnum, L)) {
			IsOk = Sony_Insert0(refnum, falseblnr, drivepath);
			refnum = NULL;
		}
		if (refnum != NULL) {
			fclose(refnum);
		}
		if (! IsOk) {
			(void) remove(drivepath);
		}
	}
}
#endif

#if IncludeSonyNew
LOCALPROC MakeNewDiskAtDefault(ui5b L)
{
	char s[ClStrMaxLength + 1];

	NativeStrFromCStr(s, "untitled.dsk");
	MakeNewDisk(L, s);
}
#endif

/* --- ROM --- */

LOCALFUNC tMacErr LoadMacRomFrom(char *path)
{
	tMacErr err;
	FILE *ROM_File;
	int File_Size;

	ROM_File = fopen(path, "rb");
	if (NULL == ROM_File) {
		err = mnvm_fnfErr;
	} else {
		File_Size = fread(ROM, 1, kROM_Size, ROM_File);
		if (kROM_Size != File_Size) {
			if (feof(ROM_File)) {
				MacMsgOverride(kStrShortROMTitle,
					kStrShortROMMessage);
				err = mnvm_eofErr;
			} else {
				MacMsgOverride(kStrNoReadROMTitle,
					kStrNoReadROMMessage);
				err = mnvm_miscErr;
			}
		} else {
			err = ROM_IsValid();
		}
		fclose(ROM_File);
	}

	return err;
}

LOCALFUNC blnr LoadMacRom(void)
{
	tMacErr err;

	if (mnvm_fnfErr == (err = LoadMacRomFrom(RomFileName)))
	{
	}

	return trueblnr; /* keep launching Mini vMac, regardless */
}

/* --- video out --- */

#if MayFullScreen
LOCALVAR short hOffset;
LOCALVAR short vOffset;
#endif

#if VarFullScreen
LOCALVAR blnr UseFullScreen = (WantInitFullScreen != 0);
#endif

#if EnableMagnify
LOCALVAR blnr UseMagnify = (WantInitMagnify != 0);
#endif

LOCALVAR blnr CurSpeedStopped = trueblnr;

#if EnableMagnify
#define MaxScale MyWindowScale
#else
#define MaxScale 1
#endif

LOCALPROC HaveChangedScreenBuff(ui4r top, ui4r left,
	ui4r bottom, ui4r right)
{
	/*
		Oh god, clean this up.
	*/
	u8 *octpix = NULL;
	u32 *vram = NULL;

	octpix = (u8 *)GetCurDrawBuff();
	vram = (u32 *)BG_BMP_RAM(0);

	octpix += ((top * vMacScreenWidth ) >> 3);
	vram += ((top * vMacScreenWidth ) >> 2);

	FB1BPPtoIndexed(vram, octpix,
		((bottom - top) * vMacScreenWidth) >> 3);
}

LOCALPROC MyDrawChangesAndClear(void)
{
	if (ScreenChangedBottom > ScreenChangedTop) {
		HaveChangedScreenBuff(ScreenChangedTop, ScreenChangedLeft,
			ScreenChangedBottom, ScreenChangedRight);
		ScreenClearChanges();
	}
}

GLOBALOSGLUPROC DoneWithDrawingForTick(void)
{
#if 0 && EnableFSMouseMotion
	if (HaveMouseMotion) {
		AutoScrollScreen();
	}
#endif
	MyDrawChangesAndClear();
}

/* --- mouse --- */

/* cursor state */

LOCALPROC CheckMouseState(void)
{
	si5b MotionX;
	si5b MotionY;

	/*
		TODO:

		- Don't hardcode motion values
		- Acceleration?
		- Allow key remapping
		- Handle touchscreen input (non-mouse motion)
		- Handle touchscreen input (trackpad style mouse motion)
	*/

	MotionX = KeysHeld.stick_x;
	MotionY = KeysHeld.stick_y;

	HaveMouseMotion = trueblnr;

	MyMousePositionSetDelta(MotionX, MotionY);
	MyMouseButtonSet(KeysHeld.btn.a);
}

/* --- keyboard input --- */

LOCALVAR ui3b KC2MKC[256];

/*
	AHA!
	GCC Was turning this into a macro of some sort which of course
	broke horribly with libnds's keyboard having some negative values.
*/
LOCALPROC AssignKeyToMKC(int UKey, int LKey, ui3r MKC)
{
	if (UKey != NOKEY) {
		KC2MKC[UKey] = MKC;
	}

	if (LKey != NOKEY) {
		KC2MKC[LKey] = MKC;
	}
}

LOCALFUNC blnr KC2MKCInit(void)
{
	int i;

	for (i = 0; i < 256; ++i) {
		KC2MKC[i] = MKC_None;
	}

	AssignKeyToMKC('A', 'a', MKC_A);
	AssignKeyToMKC('B', 'b', MKC_B);
	AssignKeyToMKC('C', 'c', MKC_C);
	AssignKeyToMKC('D', 'd', MKC_D);
	AssignKeyToMKC('E', 'e', MKC_E);
	AssignKeyToMKC('F', 'f', MKC_F);
	AssignKeyToMKC('G', 'g', MKC_G);
	AssignKeyToMKC('H', 'h', MKC_H);
	AssignKeyToMKC('I', 'i', MKC_I);
	AssignKeyToMKC('J', 'j', MKC_J);
	AssignKeyToMKC('K', 'k', MKC_K);
	AssignKeyToMKC('L', 'l', MKC_L);
	AssignKeyToMKC('M', 'm', MKC_M);
	AssignKeyToMKC('N', 'n', MKC_N);
	AssignKeyToMKC('O', 'o', MKC_O);
	AssignKeyToMKC('P', 'p', MKC_P);
	AssignKeyToMKC('Q', 'q', MKC_Q);
	AssignKeyToMKC('R', 'r', MKC_R);
	AssignKeyToMKC('S', 's', MKC_S);
	AssignKeyToMKC('T', 't', MKC_T);
	AssignKeyToMKC('U', 'u', MKC_U);
	AssignKeyToMKC('V', 'v', MKC_V);
	AssignKeyToMKC('W', 'w', MKC_W);
	AssignKeyToMKC('X', 'x', MKC_X);
	AssignKeyToMKC('Y', 'y', MKC_Y);
	AssignKeyToMKC('Z', 'z', MKC_Z);

	AssignKeyToMKC(')', '0', MKC_0);
	AssignKeyToMKC('!', '1', MKC_1);
	AssignKeyToMKC('@', '2', MKC_2);
	AssignKeyToMKC('#', '3', MKC_3);
	AssignKeyToMKC('$', '4', MKC_4);
	AssignKeyToMKC('%', '5', MKC_5);
	AssignKeyToMKC('^', '6', MKC_6);
	AssignKeyToMKC('&', '7', MKC_7);
	AssignKeyToMKC('*', '8', MKC_8);
	AssignKeyToMKC('(', '9', MKC_9);

	AssignKeyToMKC('~', '`', MKC_formac_Grave);
	AssignKeyToMKC('_', '-', MKC_Minus);
	AssignKeyToMKC('+', '=', MKC_Equal);
	AssignKeyToMKC(':', ';', MKC_SemiColon);
	AssignKeyToMKC('\"', '\'', MKC_SingleQuote);
	AssignKeyToMKC('{', '[', MKC_LeftBracket);
	AssignKeyToMKC('}', ']', MKC_RightBracket);
	AssignKeyToMKC('|', '\\', MKC_formac_BackSlash);
	AssignKeyToMKC('<', ',', MKC_Comma);
	AssignKeyToMKC('>', '.', MKC_Period);
	AssignKeyToMKC('?', '/', MKC_formac_Slash);

	// wtf is this...
	// AssignKeyToMKC(NOKEY, DVK_SPACE, MKC_Space);
	// AssignKeyToMKC(NOKEY, DVK_BACKSPACE, MKC_BackSpace);
	// AssignKeyToMKC(NOKEY, DVK_ENTER, MKC_formac_Enter);
	// AssignKeyToMKC(NOKEY, DVK_TAB, MKC_Tab);

	InitKeyCodes();

	return trueblnr;
}

LOCALPROC DoKeyCode0(int i, blnr down)
{
	ui3r key = KC2MKC[i];
	if (MKC_None != key) {
		fprintf(stderr, "%s() :: %c (%d) == %d\n",
			__FUNCTION__, (char) i, key, down);
		Keyboard_UpdateKeyMap2(key, down);
	}
}

LOCALPROC DoKeyCode(int i, blnr down)
{
	if ((i >= 0) && (i < 256)) {
		DoKeyCode0(i, down);
	}
}

/*
	TODO:

	Rethink keyboard input...
	Especially shift and capslock, the libnds keyboard
	is weird about those.
*/

LOCALVAR blnr N64_Keystate_Menu = falseblnr;
LOCALVAR blnr N64_Keystate_Shift = falseblnr;

LOCALPROC N64_HandleKey(joypad_inputs_t Key, blnr Down)
{
	if (Key.btn.raw == NOKEY) {
		return;
	}

	// Keyboard_UpdateKeyMap2(key, falseblnr); always
	// DoKeyCode(key, Down); also if normal key
}

LOCALPROC N64_HandleKeyboard(void)
{
	LastKeyboardKey = KeyboardKey;
	KeyboardKey = joypad_get_inputs(JOYPAD_PORT_1);

	if ((KeyboardKey.btn.raw == NOKEY) && (LastKeyboardKey.btn.raw != NOKEY)) {
		N64_HandleKey(LastKeyboardKey, falseblnr);
		LastKeyboardKey.btn.raw = NOKEY;
	} else {
		N64_HandleKey(KeyboardKey, trueblnr);
		LastKeyboardKey = KeyboardKey;
	}
}

/* --- time, date, location --- */

LOCALVAR ui5b TrueEmulatedTime = 0;

#include "DATE2SEC.h"

#define TicksPerSecond 1000000
/* #define TicksPerSecond  1000 */

LOCALVAR blnr HaveTimeDelta = falseblnr;
LOCALVAR ui5b TimeDelta;

LOCALVAR ui5b NewMacDateInSeconds;

LOCALVAR ui5b LastTimeSec;
LOCALVAR ui5b LastTimeUsec;

LOCALPROC GetCurrentTicks(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);

	/*
		HACKHACKHACK
	*/
	t.tv_usec = TimerBaseMSec + get_ticks();
	t.tv_usec = t.tv_usec * 1000;

	if (! HaveTimeDelta) {
		time_t Current_Time;
		struct tm *s;

		(void) time(&Current_Time);
		s = localtime(&Current_Time);
		TimeDelta = Date2MacSeconds(s->tm_sec, s->tm_min, s->tm_hour,
			s->tm_mday, 1 + s->tm_mon, 1900 + s->tm_year) - t.tv_sec;
#if 0 && AutoTimeZone /* how portable is this ? */
		CurMacDelta = ((ui5b)(s->tm_gmtoff) & 0x00FFFFFF)
			| ((s->tm_isdst ? 0x80 : 0) << 24);
#endif
		HaveTimeDelta = trueblnr;
	}

	NewMacDateInSeconds = t.tv_sec + TimeDelta;
	LastTimeSec = (ui5b)t.tv_sec;
	LastTimeUsec = (ui5b)t.tv_usec;
}

/* #define MyInvTimeStep 16626 */ /* TicksPerSecond / 60.14742 */
#define MyInvTimeStep 17

LOCALVAR ui5b NextTimeSec;
LOCALVAR ui5b NextTimeUsec;

LOCALPROC IncrNextTime(void)
{
	NextTimeUsec += MyInvTimeStep;
	if (NextTimeUsec >= TicksPerSecond) {
		NextTimeUsec -= TicksPerSecond;
		NextTimeSec += 1;
	}
}

LOCALPROC InitNextTime(void)
{
	NextTimeSec = LastTimeSec;
	NextTimeUsec = LastTimeUsec;
	IncrNextTime();
}

LOCALPROC StartUpTimeAdjust(void)
{
	GetCurrentTicks();
	InitNextTime();
}

LOCALFUNC si5b GetTimeDiff(void)
{
	return ((si5b)(LastTimeSec - NextTimeSec)) * TicksPerSecond
		+ ((si5b)(LastTimeUsec - NextTimeUsec));
}

LOCALPROC UpdateTrueEmulatedTime(void)
{
	si5b TimeDiff;

	GetCurrentTicks();

	TimeDiff = GetTimeDiff();
	if (TimeDiff >= 0) {
		if (TimeDiff > 4 * MyInvTimeStep) {
			/* emulation interrupted, forget it */
			++TrueEmulatedTime;
			InitNextTime();
		} else {
			do {
				++TrueEmulatedTime;
				IncrNextTime();
				TimeDiff -= TicksPerSecond;
			} while (TimeDiff >= 0);
		}
	} else if (TimeDiff < - 2 * MyInvTimeStep) {
		/* clock goofed if ever get here, reset */
		InitNextTime();
	}
}

LOCALFUNC blnr CheckDateTime(void)
{
	if (CurMacDateInSeconds != NewMacDateInSeconds) {
		CurMacDateInSeconds = NewMacDateInSeconds;
		return trueblnr;
	} else {
		return falseblnr;
	}
}

LOCALFUNC blnr InitLocationDat(void)
{
	GetCurrentTicks();
	CurMacDateInSeconds = NewMacDateInSeconds;

	return trueblnr;
}

/* --- basic dialogs --- */

LOCALPROC CheckSavedMacMsg(void)
{
	if (nullpr != SavedBriefMsg) {
		char briefMsg0[ClStrMaxLength + 1];
		char longMsg0[ClStrMaxLength + 1];

		NativeStrFromCStr(briefMsg0, SavedBriefMsg);
		NativeStrFromCStr(longMsg0, SavedLongMsg);

		fprintf(stderr, "%s\n", briefMsg0);
		fprintf(stderr, "%s\n", longMsg0);

		SavedBriefMsg = nullpr;
	}
}

/* --- main window creation and disposal --- */

/*
	Screen_Init
*/
LOCALFUNC blnr Screen_Init(void)
{
	if (N64_ScreenWidth == 320) {
		display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE_FETCH_ALWAYS);
	} else if (N64_ScreenWidth == 640) {
		display_init(RESOLUTION_640x480, DEPTH_16_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE_FETCH_ALWAYS);
	}
    zbuffer = surface_alloc(FMT_RGBA16, display_get_width(), display_get_height());

	return trueblnr;
}

#if VarFullScreen
LOCALPROC ToggleWantFullScreen(void)
{
	WantFullScreen = ! WantFullScreen;
}
#endif

/* --- SavedTasks --- */

LOCALPROC LeaveSpeedStopped(void)
{
#if MySoundEnabled
	MySound_Start();
#endif

	StartUpTimeAdjust();
}

LOCALPROC EnterSpeedStopped(void)
{
#if MySoundEnabled
	MySound_Stop();
#endif
}

LOCALPROC CheckForSavedTasks(void)
{
	if (MyEvtQNeedRecover) {
		MyEvtQNeedRecover = falseblnr;

		/* attempt cleanup, MyEvtQNeedRecover may get set again */
		MyEvtQTryRecoverFromFull();
	}

	if (RequestMacOff) {
		RequestMacOff = falseblnr;
		if (AnyDiskInserted()) {
			MacMsgOverride(kStrQuitWarningTitle,
				kStrQuitWarningMessage);
		} else {
			ForceMacOff = trueblnr;
		}
	}

	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped != SpeedStopped) {
		CurSpeedStopped = ! CurSpeedStopped;
		if (CurSpeedStopped) {
			EnterSpeedStopped();
		} else {
			LeaveSpeedStopped();
		}
	}

#if IncludeSonyNew
	if (vSonyNewDiskWanted) {
#if IncludeSonyNameNew
		if (vSonyNewDiskName != NotAPbuf) {
			ui3p NewDiskNameDat;
			if (MacRomanTextToNativePtr(vSonyNewDiskName, trueblnr,
				&NewDiskNameDat))
			{
				MakeNewDisk(vSonyNewDiskSize, (char *)NewDiskNameDat);
				free(NewDiskNameDat);
			}
			PbufDispose(vSonyNewDiskName);
			vSonyNewDiskName = NotAPbuf;
		} else
#endif
		{
			MakeNewDiskAtDefault(vSonyNewDiskSize);
		}
		vSonyNewDiskWanted = falseblnr;
			/* must be done after may have gotten disk */
	}
#endif

	if ((nullpr != SavedBriefMsg) & ! MacMsgDisplayed) {
		MacMsgDisplayOn();
	}

	if (NeedWholeScreenDraw) {
		NeedWholeScreenDraw = falseblnr;
		ScreenChangedAll();
	}

#if NeedRequestIthDisk
	if (0 != RequestIthDisk) {
		Sony_InsertIth(RequestIthDisk);
		RequestIthDisk = 0;
	}
#endif
}

/* --- main program flow --- */

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void)
{
	UpdateTrueEmulatedTime();
	return TrueEmulatedTime == OnTrueTime;
}

LOCALPROC WaitForTheNextEvent(void)
{
}

LOCALPROC CheckForSystemEvents(void)
{
	// todo: handle n64 keyboard-like interface
}

GLOBALOSGLUPROC WaitForNextTick(void)
{
label_retry:
	CheckForSystemEvents();
	CheckForSavedTasks();
	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped) {
		MyDrawChangesAndClear();
		WaitForTheNextEvent();
		goto label_retry;
	}

	if (ExtraTimeNotOver()) {
		si5b TimeDiff = GetTimeDiff();
		if (TimeDiff < 0) {
			/*
				FIXME:

				Implement this?

				struct timespec rqt;
				struct timespec rmt;

				rqt.tv_sec = 0;
				rqt.tv_nsec = (- TimeDiff) * 1000;

				(void) nanosleep(&rqt, &rmt);
			*/
		}
		goto label_retry;
	}

	if (CheckDateTime()) {
#if MySoundEnabled
		MySound_SecondNotify();
#endif
#if EnableDemoMsg
		DemoModeSecondNotify();
#endif
	}

	CheckMouseState();

	OnTrueTime = TrueEmulatedTime;
}

/*
	N64_ScrollBackground:

	Positions the screen as to center it over the emulated cursor.
	TODO: do we need this?
*/
LOCALPROC N64_ScrollBackground(void)
{
	int ScrollX = 0;
	int ScrollY = 0;
	int Scale = 0;

	/*
		TODO:
		Lots of magic numbers here.
	*/
#if EnableMagnify
	if (WantMagnify) {
		ScrollX = ((int) CurMouseH) - (N64_ScreenWidth / 4);
		ScrollY = ((int) CurMouseV) - (N64_ScreenHeight / 4);
		Scale = 128;

		ScrollX = ScrollX > vMacScreenWidth - (N64_ScreenWidth / 2)
			? vMacScreenWidth - (N64_ScreenWidth / 2)
			: ScrollX;
		ScrollY = ScrollY > vMacScreenHeight - (N64_ScreenHeight / 2)
			? vMacScreenHeight - (N64_ScreenHeight / 2)
			: ScrollY;
	} else
#endif
	{
		ScrollX = ((int) CurMouseH) - (N64_ScreenWidth / 2);
		ScrollY = ((int) CurMouseV) - (N64_ScreenHeight / 2);
		Scale = 256;

		ScrollX = ScrollX > vMacScreenWidth - N64_ScreenWidth
			? vMacScreenWidth - N64_ScreenWidth
			: ScrollX;
		ScrollY = ScrollY > vMacScreenHeight - N64_ScreenHeight
			? vMacScreenHeight - N64_ScreenHeight
			: ScrollY;
	}

	ScrollX = ScrollX < 0 ? 0 : ScrollX;
	ScrollY = ScrollY < 0 ? 0 : ScrollY;

	if (Display_bg2_Main) {
		bgSetScale(Display_bg2_Main, Scale, Scale);
		bgSetScroll(Display_bg2_Main, ScrollX, ScrollY);
	}
}

/*
	N64_Timer1_IRQ

	Called when TIMER0_DATA overflows.
*/
LOCALPROC N64_Timer1_IRQ(void)
{
	TimerBaseMSec += 65536;
}

/*
	N64_VBlank_IRQ

	Vertical blank interrupt callback.
*/
LOCALPROC N64_VBlank_IRQ(void)
{
	scanKeys();

	KeysHeld.btn = joypad_get_buttons_held(JOYPAD_PORT_1);

	if (++VBlankCounter == 60) {
		VBlankCounter = 0;
	}

	/*
		TODO:
		n64 stick to cursor
	// */
	// if (0 != (KeysHeld & KEY_LEFT)) {
	// 	--CursorX;
	// } else if (0 != (KeysHeld & KEY_RIGHT)) {
	// 	++CursorX;
	// }

	// if (0 != (KeysHeld & KEY_UP)) {
	// 	--CursorY;
	// } else if (0 != (KeysHeld & KEY_DOWN)) {
	// 	++CursorY;
	// }

	CursorX = CursorX < 0 ? 0 : CursorX;
	CursorX = CursorX > vMacScreenWidth ? vMacScreenWidth : CursorX;

	CursorY = CursorY < 0 ? 0 : CursorY;
	CursorY = CursorY > vMacScreenHeight ? vMacScreenHeight : CursorY;

	N64_ScrollBackground();
	bgUpdate();
}

/*
	N64_HBlank_IRQ

	Called at the start of the horizontal blanking period.
	This is here mainly as a simple performance test.
*/
LOCALPROC N64_HBlank_IRQ(void)
{
	++HBlankCounter;
}

/*
	N64_SysInit

	Initializes DS specific system hardware and interrupts.
*/
LOCALPROC N64_SysInit(void)
{
	/*
		Drop back to a read only filesystem embedded in the
		Mini vMac binary if we cannot open a media device.

		TODO: replace fatInitDefault with a libcart fatfs init
	*/
	if (! fatInitDefault()) {
		dfs_init(DFS_DEFAULT_LOCATION);
	}
}

/*
	N64_ClearVRAM: probably useless
*/
LOCALPROC N64_ClearVRAM(void)
{

}

/* --- platform independent code can be thought of as going here --- */

#include "PROGMAIN.h"

LOCALPROC ReserveAllocAll(void)
{
#if dbglog_HAVE
	dbglog_ReserveAlloc();
#endif
	ReserveAllocOneBlock(&ROM, kROM_Size, 5, falseblnr);

	ReserveAllocOneBlock(&screencomparebuff,
		vMacScreenNumBytes, 5, trueblnr);
#if UseControlKeys
	ReserveAllocOneBlock(&CntrlDisplayBuff,
		vMacScreenNumBytes, 5, falseblnr);
#endif

#if MySoundEnabled
	ReserveAllocOneBlock((ui3p *)&TheSoundBuffer,
		dbhBufferSize, 5, falseblnr);
#endif

	EmulationReserveAlloc();
}

LOCALFUNC blnr AllocMyMemory(void)
{
	uimr n;
	blnr IsOk = falseblnr;

	ReserveAllocOffset = 0;
	ReserveAllocBigBlock = nullpr;
	ReserveAllocAll();
	n = ReserveAllocOffset;
	ReserveAllocBigBlock = (ui3p)calloc(1, n);
	if (NULL == ReserveAllocBigBlock) {
		MacMsg(kStrOutOfMemTitle, kStrOutOfMemMessage, trueblnr);
	} else {
		ReserveAllocOffset = 0;
		ReserveAllocAll();
		if (n != ReserveAllocOffset) {
			/* oops, program error */
		} else {
			IsOk = trueblnr;
		}
	}

	return IsOk;
}

LOCALPROC UnallocMyMemory(void)
{
	if (nullpr != ReserveAllocBigBlock) {
		free((char *)ReserveAllocBigBlock);
	}
}

LOCALPROC ZapOSGLUVars(void)
{
	InitDrives();
	N64_ClearVRAM();
}

LOCALFUNC blnr InitOSGLU(void)
{
	N64_SysInit();

	if (AllocMyMemory())
#if dbglog_HAVE
	if (dbglog_open())
#endif
	if (LoadMacRom())
	if (LoadInitialImages())
	if (InitLocationDat())
#if MySoundEnabled
	if (MySound_Init())
#endif
	if (Screen_Init())
	if (KC2MKCInit())
	if (WaitForRom())
	{
		return trueblnr;
	}

	return falseblnr;
}

LOCALPROC UnInitOSGLU(void)
{
	if (MacMsgDisplayed) {
		MacMsgDisplayOff();
	}

#if MySoundEnabled
	MySound_Stop();
#endif
#if MySoundEnabled
	MySound_UnInit();
#endif

	UnInitDrives();

#if dbglog_HAVE
	dbglog_close();
#endif

	UnallocMyMemory();
	CheckSavedMacMsg();
}

int main(int argc, char **argv)
{
	ZapOSGLUVars();

	if (InitOSGLU()) {
		iprintf("Entering ProgramMain...\n");

		ProgramMain();

		iprintf("Leaving ProgramMain...\n");
	}

	UnInitOSGLU();

	/*
		On some homebrew launchers this could return to
		the menu by default.
	*/
	exit(1);

	while (1) {
		// swiWaitForVBlank();
	}

	return 0;
}

#endif /* WantOSGLUN64 */
