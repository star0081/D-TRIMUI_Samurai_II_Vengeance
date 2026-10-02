/* puff.h — Mark Adler, zlib contrib. See puff.c for the license. */
#ifndef SG_PUFF_H
#define SG_PUFF_H

#ifndef NIL
#define NIL ((unsigned char *)0)
#endif

int puff(unsigned char *dest, unsigned long *destlen,
         const unsigned char *source, unsigned long *sourcelen);

#endif
