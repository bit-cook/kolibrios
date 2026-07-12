//========================================================//
//                                                        //
//   BirdBench - a modular system benchmark               //
//   The window is a launcher: tick the tests you want,   //
//   press Run - results open as an HTML report in        //
//   WebView (clearer there than in the tiny window).     //
//                                                        //
//   Add a test  : edit tests_cpu.h / tests_gpu.h / ...   //
//   Add a rival : edit cpudb.h                           //
//                                                        //
//========================================================//

#define MEMSIZE 4096*1536       // ~6 MB heap (2+2 MB work buffers + canvas)

#include "../lib/kolibri.h"     // must be first (sets entry point)
#include "../lib/mem.h"
#include "../lib/strings.h"
#include "../lib/fs.h"
#include "../lib/io.h"
#include "../lib/gui.h"
#include "../lib/system.h"
#include "../lib/gui/checkbox.h"
#include "../lib/gui/menu.h"

#include "bbench.h"
#include "tests_cpu.h"
#include "tests_gpu.h"
#include "tests_disk.h"
#include "cpudb.h"

//---------------- layout ----------------//
#define PAD    16
#define COL2_X 170              // second column (Graphics)
#define COL3_X 328              // third column (Disk)
#define HEADER_H  42
#define ROW_H  22
#define WIN_W  GCV_W
#define WIN_H  GCV_H + HEADER_H

#define BUTTON_Y WIN_H - 62
#define BUTTON_W 70
#define BUTTON_H 24

#define STATUSBAR_Y WIN_H - 25

#define BAR_FULL 3000           // score that fills a full HTML bar

#define BTN_RUN        30
#define BTN_ALL        33
#define BTN_NONE       34
#define BTN_DISK_DROP  39       // disk dropdown toggle
#define MAX_DISKS      10
#define BTN_SECT_BASE  50       // 50..52 section masters
#define BTN_CHECK_BASE 60       // 60.. per-test checkboxes

checkbox cb;                     // reused as a renderer for every checkbox

byte  t_enabled[MAX_TESTS];
dword sums[SECT_NUM];
dword cnts[SECT_NUM];

char sysinfo[] = "CPU: %d MHz   RAM: %d MB   Screen: %dx%d@%db";

//---------------- disk list (LMENU dropdown) ----------------//
char  disk_store[MAX_DISKS*24];
dword disk_dirp[MAX_DISKS];
int   disk_count = 0;
int   disk_sel = 0;
byte  disk_menu_open = 0;         // an LMENU popup is currently open
char  disk_menu[MAX_DISKS*26];    // "\n"-separated item list for /sys/develop/menu
dword disk_drop_x, disk_drop_y;
char  probe_buf[16];

int DiskWritable(dword dir)
{
	char pf[80];
	dword r;
	strcpy(#pf, dir);  strcat(#pf, "/bbprobe.tmp");
	r = FileWrite(#pf, #probe_buf, 16);
	if (r==0) dk_delete(#pf);
	if (r==0) return 1;
	return 0;
}

void AddDisk(dword dir)
{
	dword slot;
	if (disk_count >= MAX_DISKS) return;
	slot = disk_count * 24;  slot = slot + #disk_store;
	strcpy(slot, dir);
	disk_dirp[disk_count] = slot;
	disk_count++;
}

void TryDisk(dword dir) { if (DiskWritable(dir)) AddDisk(dir); }

void ScanDisks()                  // enumerate real mounts under "/", keep writable
{
	int i;
	dword nm, l;
	char base[48];
	byte c0, c1, skip;
	disk_count = 0;
	io.dir.load("/", DIR_ONLYREAL);
	for (i=0; i<io.dir.count; i++) {
		nm = io.dir.position(i);
		c0 = DSBYTE[nm];  c1 = DSBYTE[nm+1];
		skip = 0;
		l = strlen(nm);
		if (l > 12) skip = 1;                 // path slots are fixed 24 bytes
		if (c0=='c') && (c1=='d') skip = 1;   // CD (read-only)
		if (c0=='f') && (c1=='d') skip = 1;   // floppy
		if (c0=='r') && (c1=='d') skip = 1;   // ramdisk /rd (system)
		if (!skip) {
			strcpy(#base, "/");  strcat(#base, nm);
			TryDisk(#base);
			strcat(#base, "/1");
			TryDisk(#base);
		}
	}
	if (disk_count < 1) AddDisk("/tmp0/1");
	disk_sel = 0;
	disk_dir = disk_dirp[0];
	disk_menu[0] = 0;                          // "\n"-joined paths for the LMENU app
	for (i=0; i<disk_count; i++) {
		strcat(#disk_menu, disk_dirp[i]);
		if (i < disk_count-1) strcat(#disk_menu, "\n");
	}
}

//---------------- helpers ----------------//
void bench_exit()      // single exit point: never leave bbtst.tmp behind
{
	Disk_Cleanup();
	ExitProcess();
}

void FlatButton(dword x, id, bg, tc, label)
{
	DefineButton(x, BUTTON_Y, BUTTON_W, BUTTON_H - 1, id, bg);
	WriteText(-strlen(label)*8 + BUTTON_W / 2 + x, 
		BUTTON_H - 16 / 2 + BUTTON_Y, 0x90, tc, label);
}

//---------------- checkbox model ----------------//
byte SectionAllOn(dword sect)
{
	int i;
	for (i=0; i<t_count; i++) if (t_sect[i]==sect) && (!t_enabled[i]) return 0;
	return 1;
}

void ToggleSection(dword sect)
{
	int i;
	byte v;
	v = SectionAllOn(sect);  v = v ^ 1;
	for (i=0; i<t_count; i++) if (t_sect[i]==sect) t_enabled[i] = v;
}

void SetAll(byte v)
{
	int i;
	for (i=0; i<t_count; i++) t_enabled[i] = v;
}

//========================================================//
//                    HTML export                         //
//========================================================//
#define ICON_CPU    48
#define ICON_GPU    52
#define ICON_DISK   50

dword ebuf = 0;
dword epos = 0;
char  etmp[176];
char  epath[64];

void AppendBcd(dword dst, byte b)        // BCD byte -> 2 ASCII digits
{
	dword hi, lo;
	hi = b >> 4;  hi = hi & 0xF;
	lo = b & 0xF;
	DSBYTE[dst]   = hi + '0';
	DSBYTE[dst+1] = lo + '0';
}

void BuildExportPath()                   // unique name from date + time
{
	dword dt, tm, adr, v;
	EAX = 29;  $int 0x40;  dt = EAX;     // fn 29: 0x00DDMMYY (BCD)
	EAX = 3;   $int 0x40;  tm = EAX;     // fn  3: 0x00SSMMHH (BCD)
	strcpy(#epath, "/tmp0/1/bb_");
	adr = #epath;  adr = adr + strlen(#epath);
	v = dt;        v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // YY
	v = dt >> 8;   v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // MM
	v = dt >> 16;  v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // DD
	DSBYTE[adr] = '_';  adr = adr + 1;
	v = tm;        v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // HH
	v = tm >> 8;   v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // MM
	v = tm >> 16;  v = v & 0xFF;   AppendBcd(adr, v);  adr = adr + 2;  // SS
	DSBYTE[adr] = 0;
	strcat(#epath, ".htm");
}

void Hput(dword s)
{
	strcpy(ebuf+epos, s);
	epos = epos + strlen(s);
}

void Hfield(dword s, dword width)
{
	dword l;
	Hput(s);
	l = strlen(s);
	while (l < width) { Hput(" ");  l++; }
}

void Hbar(dword score, fillcol, trackcol)
{
	dword filled, track, i;
	filled = score;  if (filled > BAR_FULL) filled = BAR_FULL;
	filled = muldiv(filled, 26, BAR_FULL);
	track = 26 - filled;
	Hput("<font bg=");  Hput(fillcol);  Hput(">");
	for (i=0; i<filled; i++) Hput(" ");
	Hput("</font><font bg=");  Hput(trackcol);  Hput(">");
	for (i=0; i<track; i++) Hput(" ");
	Hput("</font>");
}

void ExportSection(dword sect, title, icon)
{
	int i, r;
	dword whole, frac, barcol, trkcol;
	if (!SectionRan(sect)) return;   // no completed test here -> no header at all
	if (sect==SECT_DISK)             // show which disk was actually tested
		sprintf(#etmp, "<h3><kosicon n=%d><b>%s</b> (%s): %d</h3><blockquote>\n", icon, title, disk_dir, sect_score[sect]);
	else
		sprintf(#etmp, "<h3><kosicon n=%d><b>%s</b>: %d</h3><blockquote>\n", icon, title, sect_score[sect]);
	Hput(#etmp);
	r = 0;
	for (i=0; i<t_count; i++) {
		if (t_sect[i]==sect) && (t_done[i]) {
			whole = t_raw[i] / 100;  frac = t_raw[i] % 100;
			if (r & 1) { barcol = "#4a90e2";  trkcol = "#e4e7ec"; }
			else       { barcol = "#2c7be5";  trkcol = "#cdd2da"; }
			Hfield(t_name[i], 17);
			sprintf(#etmp, "%d", t_score[i]);  Hfield(#etmp, 8);
			if (frac < 10) sprintf(#etmp, "%d.0%d %s", whole, frac, t_unit[i]);
			else           sprintf(#etmp, "%d.%d %s",  whole, frac, t_unit[i]);
			Hfield(#etmp, 16);
			Hbar(t_score[i], barcol, trkcol);
			Hput("\n");
			r++;
		}
	}
	Hput("</blockquote>\n");
}

// name(22) + score(8) + mhz(11) = 41 chars before the bar - exactly like the
// test rows (17+8+16), so all bars on the page line up; bar width 26 too.
void CompareRow(dword name, mhz, score, maxscore, fillcol, trkcol)
{
	dword filled, track, i;
	char sb[20];
	Hfield(name, 22);
	sprintf(#sb, "%d", score);  Hfield(#sb, 8);
	sprintf(#sb, "%d MHz", mhz);  Hfield(#sb, 11);
	filled = muldiv(score, 26, maxscore);  if (filled > 26) filled = 26;
	track = 26 - filled;
	Hput("<font bg=");  Hput(fillcol);  Hput(">");
	for (i=0; i<filled; i++) Hput(" ");
	Hput("</font><font bg=");  Hput(trkcol);  Hput(">");
	for (i=0; i<track; i++) Hput(" ");
	Hput("</font>\n");
}

void ExportCompare()
{
	int i;
	dword mx, thisc;
	thisc = sect_score[SECT_CPU];
	Hput("<h3><kosicon n=47><b>CPU compare</b></h3><blockquote>\n");
	mx = thisc;
	for (i=0; i<cpudb_count; i++) if (cpudb_score[i] > mx) mx = cpudb_score[i];
	if (mx < 1) mx = 1;
	CompareRow("This PC", sys_cpu_mhz, thisc, mx, "#2c7be5", "#cdd2da");
	for (i=0; i<cpudb_count; i++) {
		if (i < 8) CompareRow(cpudb_name[i], cpudb_mhz[i], cpudb_score[i], mx, "#8fb8e8", "#e4e7ec");
	}
	Hput("</blockquote>\n");
}

// styles for Chrome/Firefox (WebView ignores <style>, uses bg= directly)
char html_head[] = "</title><style>h3{margin:0}
font{margin-bottom:1px;display:inline-block;}
font[bg='#2c7be5']{background:#2c7be5}font[bg='#4a90e2']{background:#4a90e2}
font[bg='#8fb8e8']{background:#8fb8e8}font[bg='#cdd2da']{background:#cdd2da}
font[bg='#e4e7ec']{background:#e4e7ec}
</style><body bgcolor=#ffffff><pre><h2>BirdBench results</h2>";

void ExportHTML()
{
	dword sp, fname;
	if (!ebuf) ebuf = malloc(16384);
	BuildExportPath();                       // need the name for <title>
	sp = strrchr(#epath, '/');
	fname = #epath + sp;                     // -> "bb_YYMMDD_HHMMSS.htm"
	epos = 0;
	DSBYTE[ebuf] = 0;
	Hput("<html><title>");  Hput(fname);
	Hput(#html_head);
	Hput("<font color=#86868b>");
	sprintf(#etmp, #sysinfo, sys_cpu_mhz, sys_ram_mb, screen.w, screen.h, sys_bpp);
	Hput(#etmp);
	Hput("</font>\n\n");
	ExportSection(SECT_CPU,  "CPU",      ICON_CPU);    // each self-guards on t_done
	ExportSection(SECT_GPU,  "Graphics", ICON_GPU);
	ExportSection(SECT_DISK, "Disk",     ICON_DISK);
	if (SectionRan(SECT_CPU)) ExportCompare();
	Hput("</pre></body></html>");
	FileWrite(#epath, ebuf, epos);
	RunProgram("/sys/network/webview", #epath);
}

//========================================================//
//                     the panel                          //
//========================================================//
void DrawDiskButton(dword x, y)   // combobox trigger (Eolite style) -> opens LMENU
{
	#define DISK_BW 100
	#define DISK_BH 18
	dword ax;
	disk_drop_x = x;  disk_drop_y = y;
	ax = x + DISK_BW - DISK_BH;                                 // arrow-cell left
	// sunken field with a light background
	DrawRectangle3D(x, y, DISK_BW-1, DISK_BH, sc.dark, sc.light);
	DrawBar(x+1, y+1, DISK_BW-2, DISK_BH-1, sc.light);
	WriteText(x+5, y+3, 0x90, sc.work_text, disk_dirp[disk_sel]);
	// raised arrow button on the right
	DrawRectangle3D(x-1, y-1, DISK_BW+1, DISK_BH+2, sc.line, sc.line);
	DrawRectangle3D(ax-1, y-1, DISK_BH+1, DISK_BH+2, sc.line, sc.line);
	DrawRectangle3D(ax, y, DISK_BH-1, DISK_BH, sc.light, sc.dark);
	DrawBar(ax+1, y+1, DISK_BH-2, DISK_BH-1, sc.work);
	WriteText(ax+6, y+6, 0x80, sc.work_text, "\x19");         // CP866 down arrow
	DefineHiddenButton(x, y, DISK_BW-1, DISK_BH, BTN_DISK_DROP);
}

dword DrawSectionChecks(dword sect, title, x, y)
{
	int i;
	cb.disabled = 0;
	cb.text = title;
	cb.checked = SectionAllOn(sect);
	cb.id = BTN_SECT_BASE + sect;
	cb.draw(x, y);
	y = y + 30;                  // bigger gap after the master (the first)
	for (i=0; i<t_count; i++) {
		if (t_sect[i]==sect) {
			cb.disabled = 0;
			cb.text = t_name[i];
			cb.checked = t_enabled[i];
			cb.id = BTN_CHECK_BASE + i;
			cb.draw(x, y);       // aligned with the master (no extra indent)
			y = y + ROW_H;
		}
	}
	DrawBar(x-1, HEADER_H+32, math.min(140, WIN_W-x-10), 1, sc.light);   // line under the top (master) checkboxes
	DrawBar(x-1, HEADER_H+33, math.min(140, WIN_W-x-10), 1, sc.dark);   // line under the top (master) checkboxes
	if (sect==SECT_DISK) { // dropdown under bottom checkbox
		DrawDiskButton(x, y+2);  y = y + 24; 
	}   
	return y;
}

void DrawPanel()
{
	char line[96];
	// Icons
	draw_icon_32(67,  6, sc.work, 37);       // CPU chip, centered over column 1
	draw_icon_32(225, 6, sc.work, 27);       // Graphics card, over column 2
	draw_icon_32(362, 6, sc.work, 50);       // Disk, over column 3
	// Main Checkboxes
	DrawSectionChecks(SECT_CPU,  "CPU",      PAD,    HEADER_H + 10);
	DrawSectionChecks(SECT_GPU,  "Graphics", COL2_X, HEADER_H + 10);
	DrawSectionChecks(SECT_DISK, "Disk",     COL3_X, HEADER_H + 10);
	// Buttons
	FlatButton(PAD,                    BTN_ALL,  sc.work,  sc.work_text, "All");
	FlatButton(PAD + BUTTON_W + 9,     BTN_NONE, sc.work,  sc.work_text, "None");
	FlatButton(WIN_W - BUTTON_W - PAD, BTN_RUN,  0x4A90E2, 0xFFFFFF,     "Run");
	// Foter
	DrawBar(0, STATUSBAR_Y, WIN_W, 1, sc.light);
	DrawBar(0, STATUSBAR_Y+1, WIN_W, 1, sc.dark);
	sprintf(#line, #sysinfo, sys_cpu_mhz, sys_ram_mb, screen.w, screen.h, sys_bpp);
	WriteText(PAD, STATUSBAR_Y+7, 0x90, sc.work_text, #line);
	// Disk open
}

proc_info Form;                   // global: menu.h reads Form.left/top for LMENU

void draw_window()
{
	dword wx, wy;
	sc.get();
	RefreshScreen();
	wx = 0;  if (screen.w > WIN_W) { wx = screen.w - WIN_W;  wx = wx/2; }
	wy = 0;  if (screen.h > WIN_H) { wy = screen.h - WIN_H;  wy = wy/2; }
	DefineAndDrawWindow(wx, wy, WIN_W+9, WIN_H+skin_h+4, 0x34, sc.work, "BirdBench", 0);
	GetProcessInfo(#Form, SelfInfo);
	DrawPanel();
}

//---------------- run flow ----------------//
char bench_caption[] = "This Bird is Benching";

void DrawRunScreen()      // once per run: clear the body + draw the mascot
{
	dword tx;
	DrawBar(0, 0, WIN_W, STATUSBAR_Y, sc.work);
	draw_icon_32(WIN_W-32/2, 96, sc.work, 121);
	tx = strlen(#bench_caption)*8;                       // 0x90 = 8x16 font
	tx = WIN_W - tx;  tx = tx/2;
	WriteText(tx, 140, 0x90, sc.work_text, #bench_caption);
}

void DrawRunStatus(dword name, idx, total, sect)
{
	char msg[80];
	dword icon;
	icon = ICON_CPU;
	if (sect==SECT_GPU)  icon = ICON_GPU;
	if (sect==SECT_DISK) icon = ICON_DISK;
	DrawBar(0, 0, WIN_W, HEADER_H, sc.work);             // header only
	draw_icon_16w(PAD, 11, icon);                        // section icon, left of text
	sprintf(#msg, "Running %s (%d/%d)", name, idx, total);
	WriteText(PAD+24, 13, 0x90, sc.work_text, #msg);
}

// drain events queued while a test was running; returns 1 if ESC = abort
byte AbortRequested()
{
	dword ev, id;
	byte stop = 0;
	loop() {
		ev = CheckEvent();
		if (!ev) break;
		if (ev==evButton) {
			id = GetButtonID();
			if (id==1) bench_exit();           // window X pressed mid-run
		}
		if (ev==evKey) {
			GetKeys();
			if (key_scancode==SCAN_CODE_ESC) stop = 1;
		}
		if (ev==evReDraw) { draw_window();  DrawRunScreen(); }   // keep the run look
	}
	return stop;
}

void RunSelected()
{
	int i, idx, total;
	dword s, v;
	byte aborted;
	total = 0;
	for (i=0; i<t_count; i++) if (t_enabled[i]) total++;
	if (total < 1) return;
	for (s=0; s<SECT_NUM; s++) { sums[s] = 0;  cnts[s] = 0; }
	for (i=0; i<t_count; i++) t_done[i] = 0;
	// occlusion skews fn13/fn7/fn38/fn4 (the kernel only draws visible
	// parts) - keep the window on top for the duration of the run
	SetWindowLayerBehaviour(-1, ZPOS_ALWAYS_TOP);
	DrawRunScreen();                              // the mascot, once per run
	aborted = 0;
	idx = 0;
	for (i=0; i<t_count; i++) {
		if (t_enabled[i]) {
			if (AbortRequested()) { aborted = 1;  break; }
			idx++;
			DrawRunStatus(t_name[i], idx, total, t_sect[i]);
			RunOne(i);
			s = t_sect[i];
			v = t_score[i];  if (v < 1) v = 1;
			sums[s] = sums[s] + ilog2_16(v);     // geometric mean accumulator
			cnts[s] = cnts[s] + 1;
		}
	}
	Disk_Cleanup();                              // remove the 8 MB temp file
	SetWindowLayerBehaviour(-1, ZPOS_NORMAL);
	// section score = geomean of its tests (robust to single-test outliers)
	for (s=0; s<SECT_NUM; s++) {
		if (cnts[s] > 0) { v = sums[s] / cnts[s];  sect_score[s] = iexp2_16(v); }
		else sect_score[s] = 0;
	}
	draw_window();
	if (!aborted) ExportHTML();  // auto-open the report (ESC = silent abort)
}

//---------------- events ----------------//
void HandleButton(dword id)
{
	if (id==1) bench_exit();
	if (id==BTN_DISK_DROP) {           // open the LMENU popup below the combobox
		open_lmenu(disk_drop_x, disk_drop_y+DISK_BH+1, MENU_TOP_LEFT, disk_sel+1, #disk_menu);
		disk_menu_open = 1;
		return;
	}
	if (id==BTN_RUN)  RunSelected();
	if (id==BTN_ALL)  { SetAll(1);  draw_window(); }
	if (id==BTN_NONE) { SetAll(0);  draw_window(); }
	if (id>=BTN_SECT_BASE) && (id<BTN_SECT_BASE+SECT_NUM) {
		ToggleSection(id - BTN_SECT_BASE);
		draw_window();
	}
	if (id>=BTN_CHECK_BASE) && (id<BTN_CHECK_BASE+MAX_TESTS) {
		t_enabled[id-BTN_CHECK_BASE] ^= 1;
		draw_window();
	}
}

void main()
{
	int i;
	dword click;
	GetSysInfo();
	BenchAllocBuffers();
	cpudb_init();
	Register_CPU();
	Register_GPU();
	Register_DISK();
	ScanDisks();
	for (i=0; i<t_count; i++) t_enabled[i] = 1;

	loop() switch(WaitEvent())
	{
		case evButton:
			HandleButton(GetButtonID());
			break;
		case evKey:
			GetKeys();
			if (key_scancode==SCAN_CODE_ESC) bench_exit();
			if (key_scancode==SCAN_CODE_ENTER) RunSelected();
			break;
		case evReDraw:
			if (disk_menu_open) {                 // an LMENU popup was opened
				click = get_menu_click();
				if (click) {                      // user picked a disk (1-based)
					disk_sel = click - 1;
					disk_dir = disk_dirp[disk_sel];
				}
				if (!menu_process_id) disk_menu_open = 0;   // popup closed
			}
			draw_window();
			break;
	}
}
