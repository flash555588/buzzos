from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_gui_hover import source_block


ROOT = Path(__file__).resolve().parents[1]

PAGING_FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "paging.h"
#include "user_bounds.h"
#define PAGE_SIZE 4096u
#define PT_ENTRIES 512u
#define ENTRY_ADDR_MASK UINT64_C(0x000FFFFFFFFFF000)
#define TWO_MIB UINT64_C(0x200000)
#define PAGE_PENDING UINT64_C(0x400)
#define MAX_TASKS 32
typedef uint64_t pte_t;
static uintptr_t current_cr3, process_heap_base[MAX_TASKS], process_heap_break[MAX_TASKS];
static int locked, active_pages, budget = -1;
static uintptr_t pmm_alloc_pages(size_t count) {
    assert(count == 1);
    if (budget == 0) return 0;
    if (budget > 0) budget--;
    void *page = _aligned_malloc(PAGE_SIZE, PAGE_SIZE);
    assert(page); memset(page, 0, PAGE_SIZE); active_pages++;
    return (uintptr_t)page;
}
static void pmm_free_pages(uintptr_t address, size_t count) {
    assert(count == 1 && address && active_pages > 0);
    active_pages--; _aligned_free((void *)address);
}
/* The allocator boundary is exercised separately using the real PMM. This
 * fixture injects failures into both data and page-table allocations. */
static uintptr_t pmm_alloc_user_pages(size_t count) { return pmm_alloc_pages(count); }
static void paging_lock(void) { assert(!locked); locked = 1; }
static void paging_unlock(void) { assert(locked); locked = 0; }
uintptr_t paging_current_cr3(void) { return current_cr3; }
static void flush_tlb(void) {}
static int task_get_pid(void) { return 1; }
PRODUCTION_FUNCTIONS
static pte_t *root, *kernel_pdpt;
static void setup(void) {
    budget = -1;
    root = (pte_t *)pmm_alloc_pages(1);
    kernel_pdpt = (pte_t *)pmm_alloc_pages(1);
    root[0] = (pte_t)(uintptr_t)kernel_pdpt | PAGE_PRESENT | PAGE_RW;
    kernel_pdpt[0] = PAGE_PRESENT | PAGE_LARGE;
    current_cr3 = (uintptr_t)root;
}
static void teardown(void) {
    prune_user_tables(current_cr3, USER_LOAD_START);
    assert(active_pages == 2);
    pmm_free_pages((uintptr_t)kernel_pdpt, 1);
    pmm_free_pages((uintptr_t)root, 1);
    assert(active_pages == 0 && !locked);
}
int main(void) {
    for (int failure = 0; failure < 6; failure++) {
        setup(); budget = failure;
        assert(paging_map_user_range(USER_LOAD_START, 4 * PAGE_SIZE) == -1);
        assert(active_pages == 2 && kernel_pdpt[0] == (PAGE_PRESENT | PAGE_LARGE));
        assert(!walk_pte(current_cr3, USER_LOAD_START, 0, 1));
        teardown();
    }
    puts("PASS allocation failures reclaim data and empty tables");
    for (int failure = 0; failure < 7; failure++) {
        setup(); root[0] = 0;
        pmm_free_pages((uintptr_t)kernel_pdpt, 1);
        budget = failure;
        assert(paging_map_user_range(USER_LOAD_START, 4 * PAGE_SIZE) == -1);
        assert(active_pages == 1 && root[0] == 0);
        pmm_free_pages((uintptr_t)root, 1);
        assert(active_pages == 0 && !locked);
    }
    puts("PASS failure at newly allocated PDPT level");
    for (int failure = 0; failure < 3; failure++) {
        setup();
        uintptr_t original = USER_LOAD_START + PAGE_SIZE;
        assert(paging_map_user_range(original, PAGE_SIZE) == 0);
        pte_t saved = *walk_pte(current_cr3, original, 0, 1);
        *(uint64_t *)entry_address(saved) = UINT64_C(0x123456789ABCDEF0);
        int baseline = active_pages;
        budget = failure;
        assert(paging_map_user_range(USER_LOAD_START, 4 * PAGE_SIZE) == -1);
        assert(active_pages == baseline);
        assert(*walk_pte(current_cr3, original, 0, 1) == saved);
        assert(*(uint64_t *)entry_address(saved) == UINT64_C(0x123456789ABCDEF0));
        budget = -1;
        assert(paging_release_user_range(current_cr3, original, PAGE_SIZE) == 0);
        teardown();
    }
    puts("PASS rollback preserves existing mappings and data");
    setup();
    uintptr_t boundary = USER_LOAD_START + TWO_MIB - PAGE_SIZE;
    budget = 4;
    assert(paging_map_user_range(boundary, 3 * PAGE_SIZE) == -1);
    assert(active_pages == 2);
    teardown();
    puts("PASS rollback across page-table boundary");
    uintptr_t boundaries[] = {USER_LOAD_START + UINT64_C(0x40000000), UINT64_C(0x8000000000)};
    for (int index = 0; index < 2; index++) {
        for (int failure = 0; failure < 8; failure++) {
            setup(); budget = failure;
            if (paging_map_user_range(boundaries[index] - PAGE_SIZE, 3 * PAGE_SIZE) == 0) {
                budget = -1;
                assert(paging_release_user_range(current_cr3, boundaries[index] - PAGE_SIZE, 3 * PAGE_SIZE) == 0);
                prune_user_tables(current_cr3, boundaries[index] - PAGE_SIZE);
                prune_user_tables(current_cr3, boundaries[index]);
            }
            assert(active_pages == 2);
            teardown();
        }
    }
    puts("PASS rollback across PD and PDPT boundaries");
    setup();
    uintptr_t shared = USER_LOAD_START + PAGE_SIZE;
    assert(paging_map_user_range(shared, PAGE_SIZE) == 0);
    pte_t *shared_pte = walk_pte(current_cr3, shared, 0, 1);
    *shared_pte |= PAGE_SHARED;
    pte_t shared_saved = *shared_pte;
    budget = 1;
    assert(paging_map_user_range(USER_LOAD_START, 3 * PAGE_SIZE) == -1);
    assert(*shared_pte == shared_saved);
    budget = -1;
    *shared_pte &= ~PAGE_SHARED;
    assert(paging_release_user_range(current_cr3, shared, PAGE_SIZE) == 0);
    teardown();
    puts("PASS rollback leaves shared neighbors untouched");
    setup();
    uintptr_t base = USER_LOAD_START + 80;
    process_heap_base[1] = process_heap_break[1] = base;
    assert(paging_map_user_range(USER_LOAD_START, PAGE_SIZE) == 0);
    int baseline = active_pages;
    assert(sys_sbrk(4 * PAGE_SIZE, 0, 0, 0, 0) == (intptr_t)base);
    pte_t *first = walk_pte(current_cr3, USER_LOAD_START, 0, 1);
    pte_t saved = *first;
    *(uint64_t *)entry_address(saved) = 55;
    assert(sys_sbrk((uintptr_t)0 - 4 * PAGE_SIZE, 0, 0, 0, 0) == (intptr_t)(base + 4 * PAGE_SIZE));
    assert(active_pages == baseline && *first == saved);
    assert(*(uint64_t *)entry_address(saved) == 55);
    assert(sys_sbrk((uintptr_t)INTPTR_MIN, 0, 0, 0, 0) == -1);
    assert(process_heap_break[1] == base);
    assert(paging_release_user_range(current_cr3, USER_LOAD_START, PAGE_SIZE) == 0);
    teardown();
    puts("PASS shrink preserves partial page and rejects signed minimum");
    setup(); process_heap_base[1] = process_heap_break[1] = USER_LOAD_START;
    budget = 3;
    assert(sys_sbrk(4 * PAGE_SIZE, 0, 0, 0, 0) == -1);
    assert(process_heap_break[1] == USER_LOAD_START && active_pages == 2);
    budget = -1;
    assert(sys_sbrk(2 * PAGE_SIZE, 0, 0, 0, 0) == (intptr_t)USER_LOAD_START);
    for (int page = 0; page < 2; page++)
        assert(!(*walk_pte(current_cr3, USER_LOAD_START + page * PAGE_SIZE, 0, 1) & PAGE_PENDING));
    assert(sys_sbrk((uintptr_t)0 - 2 * PAGE_SIZE, 0, 0, 0, 0) != -1);
    teardown();
    puts("PASS failed sbrk is recoverable and successful mappings commit");
    setup(); process_heap_base[1] = process_heap_break[1] = USER_LOAD_START;
    assert(sys_sbrk(3 * PAGE_SIZE, 0, 0, 0, 0) != -1);
    int mapped = active_pages;
    uintptr_t middle = USER_LOAD_START + PAGE_SIZE;
    assert(sys_heap_pages(USER_LOAD_START - PAGE_SIZE, PAGE_SIZE, 0, 0, 0) == -1);
    assert(sys_heap_pages(USER_LOAD_START + 3 * PAGE_SIZE, PAGE_SIZE, 1, 0, 0) == -1);
    assert(sys_heap_pages(middle + 1, PAGE_SIZE, 0, 0, 0) == -1);
    assert(sys_heap_pages(middle, PAGE_SIZE + 1, 0, 0, 0) == -1);
    assert(sys_heap_pages(middle, (uintptr_t)0 - PAGE_SIZE, 0, 0, 0) == -1);
    assert(sys_heap_pages(middle, PAGE_SIZE, 2, 0, 0) == -1);
    pte_t edge = *walk_pte(current_cr3, USER_LOAD_START, 0, 1);
    *(uint64_t *)entry_address(edge) = 99;
    assert(active_pages == mapped);
    assert(sys_heap_pages(middle, PAGE_SIZE, 0, 0, 0) == 0 && active_pages == mapped - 1);
    budget = 0;
    assert(sys_heap_pages(middle, PAGE_SIZE, 1, 0, 0) == -1 && active_pages == mapped - 1);
    budget = -1;
    assert(sys_heap_pages(middle, PAGE_SIZE, 1, 0, 0) == 0 && active_pages == mapped);
    assert(*walk_pte(current_cr3, USER_LOAD_START, 0, 1) == edge);
    assert(*(uint64_t *)entry_address(edge) == 99);
    assert(sys_sbrk((uintptr_t)0 - 3 * PAGE_SIZE, 0, 0, 0, 0) != -1);
    teardown();
    puts("PASS heap page operations reject invalid ranges and preserve neighbors through recommit failure");
    return 0;
}
'''

HEAP_FIXTURE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SYS_SBRK 1
#define SYS_HEAP_PAGES 2
static uintptr_t heap_base, heap_break;
static int trim_failure;
static int commit_failure;
static size_t released_bytes;
static intptr_t syscall3(int number, uintptr_t address, uintptr_t length, uintptr_t commit) {
    assert(number == SYS_HEAP_PAGES && length && !(address & 4095) && !(length & 4095));
    assert(address >= heap_base && address + length <= ((heap_break + 4095) & ~(uintptr_t)4095));
    if (commit) {
        if (commit_failure) return -1;
        return VirtualAlloc((void *)address, length, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
    }
    released_bytes += length;
    return VirtualFree((void *)address, length, MEM_DECOMMIT) ? 0 : -1;
}
static void yield(void) { assert(!"Unexpected heap lock contention"); }
static intptr_t syscall1(int number, uintptr_t argument) {
    assert(number == SYS_SBRK);
    intptr_t increment = (intptr_t)argument;
    uintptr_t old = heap_break;
    if (!increment) return (intptr_t)old;
    uintptr_t next;
    if (increment > 0) {
        if ((uintptr_t)increment > 16 * 1024 * 1024 - (old - heap_base)) return -1;
        next = old + (uintptr_t)increment;
        if (!VirtualAlloc((void *)old, next - old, MEM_COMMIT, PAGE_READWRITE)) return -1;
    } else {
        if (trim_failure) return -1;
        uintptr_t amount = (uintptr_t)0 - argument;
        if (amount > old - heap_base) return -1;
        next = old - amount;
        uintptr_t start = (next + 4095) & ~(uintptr_t)4095;
        uintptr_t end = (old + 4095) & ~(uintptr_t)4095;
        if (end > start) assert(VirtualFree((void *)start, end - start, MEM_DECOMMIT));
    }
    heap_break = next;
    return (intptr_t)old;
}
static void heap_guard_report(const char *message, uintptr_t value, uintptr_t caller) {
    (void)message; (void)value; (void)caller; assert(!"Unexpected heap corruption");
}
#define malloc test_malloc
#define free test_free
#define calloc test_calloc
#define realloc test_realloc
PRODUCTION_FUNCTIONS
int main(void) {
    heap_base = heap_break = (uintptr_t)VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE, PAGE_NOACCESS);
    assert(heap_base >= UINT64_C(0x100000000));
    unsigned char *live = malloc(128);
    assert(live); memset(live, 0x5a, 128);
    unsigned char *large = malloc(3 * 1024 * 1024);
    assert(large); memset(large, 0x77, 3 * 1024 * 1024);
    uintptr_t high = heap_break;
    free(large);
    assert(high - heap_break > 2 * 1024 * 1024);
    for (int index = 0; index < 128; index++) assert(live[index] == 0x5a);
    puts("PASS free trims large tail and preserves live allocation");
    large = malloc(2 * 1024 * 1024); assert(large);
    high = heap_break; trim_failure = 1; free(large);
    assert(heap_break == high);
    large = malloc(2 * 1024 * 1024); assert(large && heap_break == high);
    trim_failure = 0; free(large);
    assert(heap_break < high);
    puts("PASS failed trim remains reusable");
    large = malloc(3 * 1024 * 1024); assert(large);
    memset(large, 0x33, 512); high = heap_break;
    large = realloc(large, 512); assert(large && heap_break < high);
    for (int index = 0; index < 512; index++) assert(large[index] == 0x33);
    free(large); free(live);
    puts("PASS realloc shrink trims without corrupting data");
    uintptr_t baseline = heap_break;
    for (int round = 0; round < 100; round++) {
        unsigned char *first = malloc(700000), *second = calloc(800000, 1);
        assert(first && second);
        memset(first, 0x44, 700000);
        first = realloc(first, 1200000); assert(first);
        for (int index = 0; index < 700000; index++) assert(first[index] == 0x44);
        free(second); free(first);
        assert(heap_break == baseline && heap_used_bytes() == 0);
    }
    puts("PASS mixed allocation cycles recover the heap break");
    large = malloc(3 * 1024 * 1024); assert(large);
    unsigned char *blocker = malloc(128); assert(blocker); memset(blocker, 0x5c, 128);
    high = heap_break; size_t released_before = released_bytes; free(large);
    assert(heap_break == high && released_bytes - released_before > 2 * 1024 * 1024);
    commit_failure = 1; assert(!malloc(2 * 1024 * 1024));
    for (int index = 0; index < 128; index++) assert(blocker[index] == 0x5c);
    commit_failure = 0; large = malloc(2 * 1024 * 1024); assert(large);
    memset(large, 0x4a, 2 * 1024 * 1024);
    large = realloc(large, 512); assert(large);
    commit_failure = 1; assert(!realloc(large, 2 * 1024 * 1024));
    for (int index = 0; index < 512; index++) assert(large[index] == 0x4a);
    commit_failure = 0; large = realloc(large, 2 * 1024 * 1024); assert(large);
    for (int index = 0; index < 512; index++) assert(large[index] == 0x4a);
    memset(large, 0x4b, 2 * 1024 * 1024); free(large); free(blocker);
    assert(heap_used_bytes() == 0);
    puts("PASS interior page decommit, split recommit, realloc growth and commit failure preserve live data");
    unsigned char *vacant = malloc(2 * 1024 * 1024);
    unsigned char *separator = malloc(128);
    unsigned char *previous = malloc(200000);
    unsigned char *original = malloc(400000);
    assert(vacant && separator && previous && original);
    memset(original, 0x3d, 400000); memset(separator, 0x2e, 128);
    free(vacant); free(previous);
    unsigned char *replacement = realloc(original, 1000000); assert(replacement);
    for (int index = 0; index < 400000; index++) assert(replacement[index] == 0x3d);
    for (int index = 0; index < 128; index++) assert(separator[index] == 0x2e);
    free(separator); free(replacement);
    assert(heap_used_bytes() == 0);
    puts("PASS replacement realloc never reads merged-away headers after tail unmapping");
    high = heap_break; released_before = released_bytes;
    commit_failure = 1; assert(!malloc(2 * 1024 * 1024));
    assert(heap_break <= high + 4096);
    commit_failure = 0; large = malloc(2 * 1024 * 1024); assert(large);
    memset(large, 0x59, 2 * 1024 * 1024); free(large);
    puts("PASS growth followed by recommit failure reclaims the unused extension and remains reusable");
    assert(VirtualFree((void *)heap_base, 0, MEM_RELEASE));
    return 0;
}
'''


class HeapReclamationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('zig.exe') or shutil.which('zig') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('A host C compiler is required')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        paging = (ROOT / 'src/kernel/arch/x86_64/paging.c').read_text(encoding='utf-8')
        process = (ROOT / 'src/kernel/syscall/sys_proc.c').read_text(encoding='utf-8')
        libc = (ROOT / 'src/user/libc/libc.c').read_text(encoding='utf-8')
        paging_markers = ['static inline uintptr_t entry_address(', 'static void zero_table(',
                          'static int add_overflows(', 'static pte_t *alloc_table(',
                          'static pte_t *next_table(', 'static pte_t *walk_pte(',
                          'static int user_bounds(', 'static int table_empty(',
                          'static void prune_user_tables(', 'int paging_map_user_range_in_space(',
                          'int paging_map_user_range(', 'static int unmap_range(',
                          'int paging_release_user_range(']
        paging_functions = '\n'.join(source_block(paging, marker) for marker in paging_markers)
        paging_functions += '\n' + source_block(process, 'intptr_t sys_sbrk(')
        paging_functions += '\n' + source_block(process, 'intptr_t sys_heap_pages(')
        header_start = libc.index('#define HEAP_ALIGN')
        header_end = libc.index('static void heap_acquire(', header_start)
        heap_markers = ['static void heap_acquire(', 'static void heap_release(', 'size_t heap_used_bytes(',
                        'static size_t heap_align(', 'static void heap_split(', 'static void heap_merge_next(',
                        'static int heap_commit_unlocked(', 'static void heap_decommit_unlocked(',
                        'static void heap_trim_unlocked(', 'static struct heap_block *heap_grow(',
                        'static void *heap_malloc_unlocked(', 'void *malloc(', 'void free(',
                        'void *calloc(', 'void *realloc(']
        heap_functions = libc[header_start:header_end] + '\n'.join(
            source_block(libc, marker) for marker in heap_markers)
        cls.results = {}
        for name, fixture, functions in [('paging', PAGING_FIXTURE, paging_functions),
                                          ('heap', HEAP_FIXTURE, heap_functions)]:
            program = Path(cls.directory.name) / f'{name}.c'
            executable = program.with_suffix('.exe')
            program.write_text(fixture.replace('PRODUCTION_FUNCTIONS', functions), encoding='utf-8')
            command = [compiler]
            if Path(compiler).stem == 'zig':
                command.append('cc')
            compilation = subprocess.run(command + ['-std=c11', '-O1', '-UNDEBUG',
                                     '-I' + str(ROOT / 'src/kernel/arch/x86_64'),
                                     str(program), '-o', str(executable)],
                           capture_output=True, text=True, timeout=60)
            if compilation.returncode:
                raise AssertionError(compilation.stderr)
            execution = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
            if execution.returncode:
                raise AssertionError(execution.stdout + execution.stderr)
            cls.results[name] = execution.stdout

    def test_paging_failure_and_sbrk_reclamation(self):
        self.assertEqual(self.results['paging'].count('PASS '), 9)

    def test_heap_reclamation_and_data_preservation(self):
        self.assertEqual(self.results['heap'].count('PASS '), 7)


if __name__ == '__main__':
    unittest.main()
