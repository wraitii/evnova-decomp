"""Exercise Apple queue transitions and preserved submission recovery offline."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import notarize_macos as notary


class NotarizationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / 'input.zip'
        self.archive.write_bytes(b'exact signed archive bytes')
        self.state = self.root / 'state'
        self.output = self.root / 'output.zip'
        self.auth = ['--keychain-profile', 'test-profile']

    def submit(self):
        with patch.object(notary, 'run', return_value='{"id": "submission-123"}') as run:
            receipt = notary.submit(self.state, self.archive, self.auth, '42', 'commit')
        self.assertEqual(run.call_count, 1)
        self.assertIn('--no-wait', run.call_args.args)
        return receipt

    def finish(self, **kwargs):
        return notary.finish(self.state, self.output, self.auth,
                             source_run='42', source_sha='commit', **kwargs)

    def test_submission_persists_exact_bytes_and_provenance(self):
        receipt = self.submit()
        self.assertEqual(receipt['source_run'], '42')
        self.assertEqual(receipt['source_sha'], 'commit')
        self.assertEqual((self.state / 'signed.zip').read_bytes(), self.archive.read_bytes())
        self.assertEqual(json.loads((self.state / 'submission.json').read_text()), receipt)

    def test_rerun_reuses_receipt_without_contacting_apple(self):
        first = self.submit()
        with patch.object(notary, 'run') as run:
            second = notary.submit(self.state, self.archive, self.auth, '42', 'commit')
        self.assertEqual(first, second)
        run.assert_not_called()

    def test_pending_is_resumable_and_does_not_publish(self):
        self.submit()
        with patch.object(notary, 'run', return_value='{"status": "In Progress"}') as run:
            self.assertFalse(self.finish(wait_seconds=0))
        self.assertEqual(run.call_count, 1)
        self.assertFalse(self.output.exists())
        self.assertEqual((self.state / 'signed.zip').read_bytes(), self.archive.read_bytes())
        self.assertTrue((self.state / 'submission.json').exists())

    def test_changed_archive_or_wrong_source_is_rejected_before_apple_call(self):
        self.submit()
        with patch.object(notary, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'different CI run'):
                notary.finish(self.state, self.output, self.auth, source_run='99', source_sha='commit')
            with self.assertRaisesRegex(RuntimeError, 'different CI run'):
                notary.finish(self.state, self.output, self.auth, source_run='42', source_sha='other')
            (self.state / 'signed.zip').write_bytes(b'changed bytes')
            with self.assertRaisesRegex(RuntimeError, 'differs'):
                self.finish()
            run.assert_not_called()

    def test_invalid_submission_logs_and_never_staples(self):
        self.submit()
        with patch.object(notary, 'run', return_value='{"status": "Invalid"}') as run:
            with self.assertRaisesRegex(RuntimeError, 'Invalid'):
                self.finish()
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args.args[:3], ('xcrun', 'notarytool', 'log'))
        self.assertFalse(self.output.exists())

    def test_poll_then_accept_staples_without_changing_saved_archive(self):
        self.submit()
        statuses = iter(['In Progress', 'Accepted'])
        commands = []

        def execute(*args, **kwargs):
            commands.append(args)
            if args[:3] == ('xcrun', 'notarytool', 'info'):
                return json.dumps({'status': next(statuses)})
            if args[:3] == ('ditto', '-c', '-k'):
                Path(args[-1]).write_bytes(b'final stapled archive')
            return ''

        with patch.object(notary, 'run', side_effect=execute), patch.object(notary.time, 'sleep'):
            self.assertTrue(self.finish())
        self.assertEqual(self.output.read_bytes(), b'final stapled archive')
        self.assertEqual((self.state / 'signed.zip').read_bytes(), self.archive.read_bytes())
        self.assertTrue(any(c[:3] == ('xcrun', 'stapler', 'validate') for c in commands))
        self.assertTrue(any(c[:2] == ('spctl', '--assess') for c in commands))
        self.assertFalse(any(c[:3] == ('xcrun', 'notarytool', 'submit') for c in commands))

    def test_stapling_failure_preserves_receipt_and_existing_output(self):
        self.submit()
        self.output.write_bytes(b'previous release')

        def execute(*args, **kwargs):
            if args[:3] == ('xcrun', 'notarytool', 'info'):
                return '{"status": "Accepted"}'
            if args[:3] == ('xcrun', 'stapler', 'staple'):
                raise RuntimeError('stapler failed')
            return ''

        with patch.object(notary, 'run', side_effect=execute):
            with self.assertRaisesRegex(RuntimeError, 'stapler failed'):
                self.finish()
        self.assertEqual(self.output.read_bytes(), b'previous release')
        self.assertEqual(notary.load_receipt(self.state, '42', 'commit')['id'], 'submission-123')


if __name__ == '__main__':
    unittest.main()
