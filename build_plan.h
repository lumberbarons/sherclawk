/* Validated protocol-2 representation shared by shell and native executors. */
#ifndef SHERCLAWK_BUILD_PLAN_H
#define SHERCLAWK_BUILD_PLAN_H
#include <stddef.h>
#define BUILD_INPUTS 5
typedef struct {
    char paths[BUILD_INPUTS][96], staged[BUILD_INPUTS][32], includes[3][96];
    int count, sources, resources, include_count, stdclib;
    char output[32], creator[5];
} BuildPlan;
int build_project_plan(const char *,BuildPlan *);
int build_project_command(const BuildPlan *,int,char *,size_t,char *,size_t);
#endif
