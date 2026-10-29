/*
 * ocerz -report (src/report.c): runs a program as ocerz would and writes a field report a tester can send.
 */
#ifndef OCERZ_REPORT_H
#define OCERZ_REPORT_H

#include <stddef.h>

/* argv: what followed -report, ocerz's own flags then the program and its arguments; answers the run's exit code */
int ocerz_report_main(const char *self, int argc, char **argv);

#endif
