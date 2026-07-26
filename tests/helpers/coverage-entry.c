/*
 * Purpose: Gives every LLVM-instrumented executable the same external main
 * control flow while keeping each program's real entry point uniquely named.
 */

#ifdef main
#undef main
#endif

#ifndef CUP_COVERAGE_ENTRY
#error "CUP_COVERAGE_ENTRY must name the instrumented program entry point"
#endif

#ifdef CUP_COVERAGE_VOID_ENTRY
int CUP_COVERAGE_VOID_ENTRY(void);

int CUP_COVERAGE_ENTRY(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return CUP_COVERAGE_VOID_ENTRY();
}
#else
int CUP_COVERAGE_ENTRY(int argc, char **argv);
#endif

int main(int argc, char **argv) {
    return CUP_COVERAGE_ENTRY(argc, argv);
}
