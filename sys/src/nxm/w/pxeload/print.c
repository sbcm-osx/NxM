#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

static int spinning;
static char* wheel[4] = { "\b|", "\b/", "\b-", "\b\\", };
static int spoke;

void
startwheel(void)
{
	spoke = 1;
	consputs("|", 1);
	spinning = 1;
}

void
turnwheel(int dir)
{
	if(!spinning)
		return;
	spoke += dir;
	consputs(wheel[spoke & 3], 2);
}

void
stopwheel(void)
{
	consputs("\b", 1);
	spinning = 0;
}

int
print(char* fmt, ...)
{
	va_list arg;
	char buf[PRINTSIZE], *e, *p;

	p = buf;
	if(spinning)
		*p++ = '\b';

	va_start(arg, fmt);
	e = vseprint(p, buf+sizeof(buf), fmt, arg);
	va_end(arg);

	if(spinning && e < &buf[PRINTSIZE-2]){
		*e++ = ' ';
		*e = 0;
	}
	consputs(buf, e-buf);

	return e-buf;
}

static Lock fmtl;

void
_fmtlock(void)
{
	lock(&fmtl);
}

void
_fmtunlock(void)
{
	unlock(&fmtl);
}

int
_efgfmt(Fmt*)
{
	return -1;
}
