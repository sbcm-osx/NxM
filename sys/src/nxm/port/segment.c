#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

uintmem
segppn(Segment *s, uintmem pa)
{
	uintmem pgsz;

	pgsz = m->pgsz[s->pgszi];
	pa &= ~(pgsz-1);
	return pa;
}

/*
 * Sizes are given in multiples of BIGPGSZ.
 * The actual page size used is either BIGPGSZ or 1*GiB
 * if base is aligned to 1G and size is >= 1G and we support 1G pages.
 */
Segment *
newseg(int type, uintptr base, u64int size)
{
	Segment *s;
	int mapsize;
	uint pgsz;

	if(size > SEGMAPSIZE*(PTEMAPMEM/BIGPGSZ)){
		print("newseg bails out because %ld is too big\n", size);
		error("newseg: segment size too large (max SEGMAPSIZE bigpages");
	}

	pgsz = BIGPGSZ;
	if(size*BIGPGSZ >= 1*GiB && m->pgsz[getpgszi(1*GiB)] == 1*GiB &&
	   (base&(1ULL*GiB-1)) == 0 && ((size*BIGPGSZ)&(1ULL*GiB-1)) == 0){
		DBG("newseg: using 1G pages\n");
		pgsz = 1*GiB;
	}
	s = smalloc(sizeof(Segment));
	s->ref = 1;
	s->type = type;
	s->base = base;
	s->ptepertab = PTEMAPMEM/pgsz;
	s->top = base+(size*BIGPGSZ);
	s->size = size;
	s->pgszi = getpgszi(pgsz);
	if(s->pgszi < 0)
		panic("newseg: getpgszi %d", pgsz);
	s->sema.prev = &s->sema;
	s->sema.next = &s->sema;
	s->color = NOCOLOR;

	mapsize = HOWMANY(size*BIGPGSZ/pgsz, s->ptepertab);
	if(mapsize > nelem(s->ssegmap)){
		mapsize *= 2;
		if(mapsize > (SEGMAPSIZE*s->ptepertab))
			mapsize = (SEGMAPSIZE*s->ptepertab);
		s->map = smalloc(mapsize*sizeof(Pte*));
		s->mapsize = mapsize;
	}
	else{
		s->map = s->ssegmap;
		s->mapsize = nelem(s->ssegmap);
	}

	return s;
}

#define	NHASH 101
#define SHASH(np)	(PTR2UINT(np)%NHASH)

Sem*
segmksem(Segment *sg, int *np)
{
	Sem *s, **l;

	qlock(&sg->lk);
	if(sg->sems.s == nil)
		sg->sems.s = mallocz(NHASH * sizeof(Sem*), 1);
	for(l = &sg->sems.s[SHASH(np)]; (s = *l) != nil; l = &s->next)
		if(s->np == np){
			qunlock(&sg->lk);
			return s;
		}
	s = mallocz(sizeof *s, 1);
	s->np = np;
	*l = s;
	qunlock(&sg->lk);
	return s;
}

void
putseg(Segment *s)
{
	Pte **pp, **emap;
	Image *i;
	extern void freezseg(Segment*);

	if(s == 0)
		return;

	i = s->image;
	if(i != 0) {
		lock(i);
		lock(s);
		if(i->s == s && s->ref == 1)
			i->s = 0;
		unlock(i);
	}
	else
		lock(s);

	s->ref--;
	if(s->ref != 0) {
		unlock(s);
		return;
	}
	unlock(s);

	qlock(&s->lk);
	if(i)
		putimage(i);

	emap = &s->map[s->mapsize];
	for(pp = s->map; pp < emap; pp++)
		if(*pp)
			freepte(s, *pp);

	qunlock(&s->lk);
	if(s->map != s->ssegmap)
		free(s->map);
	if(s->profile != 0)
		free(s->profile);
	if(s->sems.s != nil)
		free(s->sems.s);
	if(s->type&SG_ZIO)
		freezseg(s);
	free(s);
}

void
relocateseg(Segment *s, uintptr offset)
{
	Page **pg, *x;
	Pte *pte, **p, **endpte;

	endpte = &s->map[s->mapsize];
	for(p = s->map; p < endpte; p++) {
		if(*p == 0)
			continue;
		pte = *p;
		for(pg = pte->first; pg <= pte->last; pg++) {
			if(x = *pg)
				x->va += offset;
		}
	}
}

Segment*
dupseg(Segment **seg, int segno, int share, int sharestack)
{
	int i, size;
	Pte *pte;
	Segment *n, *s;

	SET(n);
	s = seg[segno];

	qlock(&s->lk);
	if(waserror()){
		qunlock(&s->lk);
		nexterror();
	}
	switch(s->type&SG_TYPE) {
	case SG_TEXT:		/* New segment shares pte set */
	case SG_SHARED:
	case SG_PHYSICAL:
		goto sameseg;

	case SG_STACK:
		if (sharestack)
			goto sameseg;
		n = newseg(s->type, s->base, s->size);
		break;

	case SG_BSS:		/* Just copy on write */
		if(share)
			goto sameseg;
		n = newseg(s->type, s->base, s->size);
		break;

	case SG_DATA:		/* Copy on write plus demand load info */
		if(segno == TSEG){
			poperror();
			qunlock(&s->lk);
			return data2txt(s);
		}

		if(share)
			goto sameseg;
		n = newseg(s->type, s->base, s->size);

		incref(s->image);
		n->image = s->image;
		n->fstart = s->fstart;
		n->flen = s->flen;
		n->pgszi = s->pgszi;
		n->color = s->color;
		n->ptepertab = s->ptepertab;
		break;
	}
	size = s->mapsize;
	for(i = 0; i < size; i++)
		if(pte = s->map[i])
			n->map[i] = ptecpy(n, pte);

	n->flushme = s->flushme;
	if(s->ref > 1)
		procflushseg(s);
	poperror();
	qunlock(&s->lk);
	return n;

sameseg:
	incref(s);
	poperror();
	qunlock(&s->lk);
	return s;
}

void
segpage(Segment *s, Page *p)
{
	Pte **pte;
	uintptr soff;
	uintmem pgsz;
	Page **pg;

	if(s->pgszi < 0)
		s->pgszi = p->pgszi;
	if(s->color == NOCOLOR)
		s->color = p->color;
	if(s->pgszi != p->pgszi)
		panic("segpage: s->pgszi != p->pgszi");

	if(p->va < s->base || p->va >= s->top)
		panic("segpage: p->va < s->base || p->va >= s->top");

	soff = p->va - s->base;
	pte = &s->map[soff/PTEMAPMEM];
	if(*pte == 0)
		*pte = ptealloc(s);
	pgsz = m->pgsz[s->pgszi];
	pg = &(*pte)->pages[(soff&(PTEMAPMEM-1))/pgsz];
	*pg = p;
	if(pg < (*pte)->first)
		(*pte)->first = pg;
	if(pg > (*pte)->last)
		(*pte)->last = pg;
}

/*
 *  called with s->lk locked
 */
void
mfreeseg(Segment *s, uintptr start, int pages)
{
	int i, j, size;
	uintptr soff;
	uintmem pgsz;
	Page *pg;
	Page *list;

	pgsz = m->pgsz[s->pgszi];
	soff = start-s->base;
	j = (soff&(PTEMAPMEM-1))/pgsz;

	size = s->mapsize;
	list = nil;
	for(i = soff/PTEMAPMEM; i < size; i++) {
		if(pages <= 0)
			break;
		if(s->map[i] == 0) {
			pages -= s->ptepertab-j;
			j = 0;
			continue;
		}
		while(j < s->ptepertab) {
			pg = s->map[i]->pages[j];
			/*
			 * We want to zero s->map[i]->page[j] and putpage(pg),
			 * but we have to make sure other processors flush the
			 * entry from their TLBs before the page is freed.
			 * We construct a list of the pages to be freed, zero
			 * the entries, then (below) call procflushseg, and call
			 * putpage on the whole list.
			 *
			 * Swapped-out pages don't appear in TLBs, so it's okay
			 * to putswap those pages before procflushseg.
			 */
			if(pg){
				if(onswap(pg))
					putswap(pg);
				else{
					pg->next = list;
					list = pg;
				}
				s->map[i]->pages[j] = 0;
			}
			if(--pages == 0)
				goto out;
			j++;
		}
		j = 0;
	}
out:
	/* flush this seg in all other processes */
	if(s->ref > 1)
		procflushseg(s);

	/* free the pages */
	for(pg = list; pg != nil; pg = list){
		list = list->next;
		putpage(pg);
	}
}

Segment*
isoverlap(Proc* p, uintptr va, usize len)
{
	int i;
	Segment *ns;
	uintptr newtop;

	newtop = va+len;
	for(i = 0; i < NSEG; i++) {
		ns = p->seg[i];
		if(ns == 0)
			continue;
		if((newtop > ns->base && newtop <= ns->top) ||
		   (va >= ns->base && va < ns->top))
			return ns;
	}
	return nil;
}

void
segclock(uintptr pc)
{
	Segment *s;

	s = up->seg[TSEG];
	if(s == 0 || s->profile == 0)
		return;

	s->profile[0] += TK2MS(1);
	if(pc >= s->base && pc < s->top) {
		pc -= s->base;
		s->profile[pc>>LRESPROF] += TK2MS(1);
	}
}

static void
prepageseg(int i)
{
	Segment *s;
	uintptr addr, pgsz;

	s = up->seg[i];
	if(s == nil)
		return;
	DBG("prepage: base %#p top %#p\n", s->base, s->top);
	pgsz = m->pgsz[s->pgszi];
	for(addr = s->base; addr < s->top; addr += pgsz)
		fault(addr, i == TSEG);
}

/*
 * BUG: should depend only in segment attributes, not in
 * the slot used in up->seg.
 */
void
nixprepage(int i)
{
	if(i >= 0)
		prepageseg(i);
	else
		for(i = 0; i < NSEG; i++)
			prepageseg(i);
}

