#ifndef INCLUDE_CPUDB_H
#define INCLUDE_CPUDB_H
//========================================================//
//  CPU comparison database  --  EDIT ME                  //
//                                                        //
//  Add one line per reference CPU in cpudb_init():       //
//      cpudb_add("CPU name", mhz, cpu_score);            //
//  cpu_score = the CPU section score you measured on      //
//  that machine (the big number on the CPU tab).          //
//  The values below are placeholders - replace them.      //
//========================================================//

#define CPUDB_MAX 24
dword cpudb_name [CPUDB_MAX];
dword cpudb_mhz  [CPUDB_MAX];
dword cpudb_score[CPUDB_MAX];
int   cpudb_count = 0;

void cpudb_add(dword name, mhz, score)
{
	if (cpudb_count >= CPUDB_MAX) return;
	cpudb_name [cpudb_count] = name;
	cpudb_mhz  [cpudb_count] = mhz;
	cpudb_score[cpudb_count] = score;
	cpudb_count++;
}

void cpudb_init()
{
	cpudb_add("Intel Core i5-2400",  3100, 1600);
	cpudb_add("AMD Athlon XP 2500+", 1833,  360);
	cpudb_add("Intel Pentium 4",     2400,  300);
	cpudb_add("Intel Pentium III",   1000,  180);
	cpudb_add("Intel Atom N270",     1600,  420);
	cpudb_add("VIA C7",              1200,  150);
}

#endif
