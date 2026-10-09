from pathlib import Path
import sys
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_heap import heap_passes


class HeapEvidenceTests(unittest.TestCase):
    def test_interleaved_kernel_trace_preserves_success_message(self):
        log = ('heaptest: ok 64-bit allocation boundaries[exec] entry=0x100000000\n\n'
               '[exec] entry=0x100000000heaptest: ok alignment 16\n'
               'heaptest: failed allocation\n')
        self.assertEqual(heap_passes(log), ['heaptest: ok 64-bit allocation boundaries',
                                           'heaptest: ok alignment 16'])

    def test_physical_page_counts_are_preserved(self):
        message = 'heaptest: ok physical reclamation before=53169 during=51118 after=53167'
        self.assertEqual(heap_passes(message + '\r\nheaptest: ok physical OOM rollback and reuse\n'),
                         [message, 'heaptest: ok physical OOM rollback and reuse'])

    def test_kernel_start_trace_splitting_message_is_rejoined(self):
        log = ('heaptest: ok 64-bit[exec] entry=0x0000000100000000 task=0x00000002\r\n'
               ' allocation boundaries\nheaptest: failed allocation\n')
        self.assertEqual(heap_passes(log), ['heaptest: ok 64-bit allocation boundaries'])
