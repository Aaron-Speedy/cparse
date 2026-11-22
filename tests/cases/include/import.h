/* tests/cases/pp.c imports this twice: an error if read twice */
#ifdef PP_IMPORT_H
#error "#import read twice"
#endif
#define PP_IMPORT_H 1
imported_once
