/* For --pgo-generate builds. libgcov's indirect call profiler reads thread-local storage from
   tpidr_el0, which is 0 on this console (libnx's TLS goes through -mtp=soft), so its first use
   faults at a small address. This is the same function and variable, built with -mtp=soft; being
   linked first, they keep libgcov's object out. As libgcov-profiler.c in GCC. The atomic variant is
   here too: -fprofile-update=atomic units call it (--pgo-scope all's host units), and from libgcov
   it would bring that object, and its tpidr_el0 copy, back in. */
struct indirect_call_tuple {
    void *callee;
    long long *counters;
};
__thread struct indirect_call_tuple __gcov_indirect_call;

void __gcov_topn_values_profiler(long long *counters, long long value);
void __gcov_topn_values_profiler_atomic(long long *counters, long long value);

void __gcov_indirect_call_profiler_v4(long long value, void *cur_func)
{
    if (cur_func == __gcov_indirect_call.callee)
        __gcov_topn_values_profiler(__gcov_indirect_call.counters, value);
    __gcov_indirect_call.callee = 0;
}

void __gcov_indirect_call_profiler_v4_atomic(long long value, void *cur_func)
{
    if (cur_func == __gcov_indirect_call.callee)
        __gcov_topn_values_profiler_atomic(__gcov_indirect_call.counters, value);
    __gcov_indirect_call.callee = 0;
}
