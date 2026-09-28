import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from idle_work_progress import IdleWorkProgress, parse_activity, process_tree_cpu


def state(epoch=2, generation=1, healthy=1):
    return f'version=1 generation_ns={generation} epoch={epoch} pending={epoch%2} healthy={healthy}\n'


class IdleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.path = self.root/'activity'
        self.path.write_text(state()); self.cpu = {(10, 100): 10}
        self.reader = IdleWorkProgress(self.root/'log', self.path, 1, 10,
                                      lambda group: dict(self.cpu))

    def test_cpu_alone_or_idle_alone_is_not_progress(self):
        self.assertFalse(self.reader.poll())
        self.path.write_text(state(3)); self.cpu[(10, 100)] += 5
        self.assertFalse(self.reader.poll())
        self.path.write_text(state(4)); self.assertFalse(self.reader.poll())
        self.cpu[(10, 100)] += 5; self.assertTrue(self.reader.poll())
        self.assertFalse(self.reader.poll())

    def test_transition_during_sampling_cannot_authorize(self):
        def sample(group):
            self.path.write_text(state(4)); return {(10, 100): 20}
        self.reader.cpu_reader = sample
        self.assertFalse(self.reader.poll())

    def test_fault_generation_regression_and_missing_fail_closed(self):
        self.reader.poll()
        for value in [state(healthy=0), state(generation=2), state(1)]:
            self.path.write_text(value)
            with self.assertRaises(RuntimeError): self.reader.poll()
        self.path.unlink()
        with self.assertRaises(FileNotFoundError): self.reader.poll()
        rows=[json.loads(x) for x in (self.root/'log').read_text().splitlines()]
        self.assertTrue(any(r.get('healthy') == 0 for r in rows))

    def test_cpu_reuse_and_counter_regression(self):
        self.cpu = {(10, 100): 10, (11, 101): 0}; self.assertFalse(self.reader.poll())
        self.cpu[(11, 101)] = 5; self.assertTrue(self.reader.poll())
        self.cpu[(11, 101)] = 4
        with self.assertRaises(RuntimeError): self.reader.poll()

    def test_launcher_pid_reuse_is_rejected(self):
        self.cpu = {(10, 101): 20}
        with self.assertRaisesRegex(RuntimeError, 'identity changed'): self.reader.poll()

    def test_stale_observation(self):
        with patch('idle_work_progress.time.monotonic',side_effect=[0,0,3,3]):
            with self.assertRaisesRegex(RuntimeError, 'Stale'): self.reader.poll()

    def test_abi_validation(self):
        for text in ['', state().replace('version=1','version=2'),
                     state().replace('pending=0','pending=1'), state(0),
                     state(-1), state(2**64), state(healthy=2),
                     state().replace('healthy=1','epoch=2')]:
            with self.subTest(text=text), self.assertRaises((RuntimeError,ValueError)):
                parse_activity(text)

    def test_descendants_across_sessions_and_birth_identity(self):
        for pid, parent, group in [(10,1,10),(11,10,11),(12,1,12),(13,11,13)]:
            p=self.root/str(pid);p.mkdir()
            # state ppid pgrp session tty tpgid flags faults utime stime ... starttime
            values=['S',str(parent),str(group),'10','0','-1','0','0','0','0','0','7','9',
                    '0','0','20','0','2','0','123']
            (p/'stat').write_text(f'{pid} (name with ) parens) '+ ' '.join(values))
        self.assertEqual(process_tree_cpu(10,self.root),{(10,123):16,(11,123):16,(13,123):16})
        (self.root/'10/stat').unlink()
        self.assertEqual(process_tree_cpu(10,self.root),{})


if __name__ == '__main__': unittest.main()
