"""Checks evaluator failure accounting and publication independence without model/SDK dependencies."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import roclint_eval as evaluation


class EvaluationTests(unittest.TestCase):
    def fixture(self):
        case = {'expected': {'status': 'Verified', 'target': 'Engine::run',
                             'file': 'src/a.hpp', 'complete': True}}
        actual = {'evidence': [{'status': 'Verified', 'target': 'Engine::run',
                               'file': 'src/a.hpp', 'complete': True}],
                  'xml': '<canonical_intents><intent target="run" status="VERIFIED"/></canonical_intents>',
                  'rendered_count': 1}
        return case, actual

    def test_wrong_verified_binding_is_a_failure(self):
        case, actual = self.fixture()
        self.assertTrue(evaluation.score_ast(case, actual)['passed'])
        actual['evidence'][0]['file'] = 'src/foreign.hpp'
        score = evaluation.score_ast(case, actual)
        self.assertFalse(score['passed'])
        self.assertTrue(score['wrong_verified'])

    def test_renderer_identity_and_accounting(self):
        case, actual = self.fixture()
        actual['xml'] = '<canonical_intents><intent target=" ... " status="VERIFIED"/></canonical_intents>'
        self.assertIn('verified_identity', evaluation.score_ast(case, actual)['errors'])
        actual['rendered_count'] = 2
        self.assertIn('rendered_subset', evaluation.score_ast(case, actual)['errors'])

    def test_abstentions_and_errors_do_not_inflate_resolution(self):
        rows = [{'suite': 'ast', 'status': 'measured', 'expected': {'status': 'Ambiguous'},
                 'score': {'passed': True}, 'actual': {'evidence': [{'status': 'Ambiguous'}]}},
                {'suite': 'ast', 'status': 'error', 'expected': {'status': 'Verified'},
                 'score': {'passed': False}},
                {'suite': 'rewrite', 'status': 'error', 'expected': {},
                 'score': {'passed': False}}]
        summary = evaluation.summarize(rows, 4)
        self.assertEqual(summary['supported_resolvable_cases'], 1)
        self.assertEqual(summary['resolution_coverage'], 0)
        self.assertEqual(summary['missing_cases'], 1)
        self.assertEqual(summary['errors'], 2)
        self.assertIsNone(summary['fully_faithful_rate'])

    def test_literal_pass_is_not_semantic_approval(self):
        case = {'expected': {'must_preserve_literals': ['run', '250']}}
        actual = {'reply': {'status': 'rewritten', 'rewritten_prompt': 'Change run to 250.'},
                  'contract': {'parse_valid': True, 'structurally_valid': True}}
        score = evaluation.score_rewrite(case, actual)
        self.assertTrue(score['passed'])
        self.assertIsNone(score['fully_faithful'])
        self.assertEqual(score['semantic_verdict'], 'human_review_required')
        actual['reply']['rewritten_prompt'] = 'Change run to 500.'
        self.assertFalse(evaluation.score_rewrite(case, actual)['passed'])

    def test_remote_or_credentialed_publication_is_rejected(self):
        for url in ('https://example.com', 'http://127.0.0.1@evil.test',
                    'http://user:password@localhost:6006', 'http://localhost:6006/v1'):
            with self.assertRaises(ValueError):
                evaluation.local_endpoint(url)
        self.assertEqual(evaluation.local_endpoint('http://127.0.0.1:6006/'),
                         'http://127.0.0.1:6006')

    def test_annotation_readback_rejects_missing_or_false_semantic_approval(self):
        row = {'score': {'passed': True}}
        record = {'annotations': [{'name': 'deterministic_checks', 'score': 1.0}]}
        with self.assertRaises(RuntimeError):
            evaluation.check_annotation_readback(record, row, 'rewrite')
        semantic = {'name': 'semantic_fidelity', 'score': None, 'label': 'human_review_required'}
        record['annotations'].append(semantic)
        evaluation.check_annotation_readback(record, row, 'rewrite')
        semantic['score'] = 1.0
        with self.assertRaises(RuntimeError):
            evaluation.check_annotation_readback(record, row, 'rewrite')

    def test_non_synthetic_or_duplicate_fixtures_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.json'
            data = {'version': 1, 'synthetic': False, 'cases': []}
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                evaluation.load_cases(path, 'ast')
            data.update(synthetic=True, cases=[{'id': 'same'}, {'id': 'same'}])
            path.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                evaluation.load_cases(path, 'ast')

    def test_outage_keeps_existing_local_results_and_fails_strict_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'report.json'
            initial = {'run_id': 'outage-fixture', 'results': [{'id': 'kept'}],
                       'summary': {'checks_passed': 1}, 'publication': {'status': 'not_requested'}}
            path.write_text(json.dumps(initial))
            result = subprocess.run([sys.executable, str(Path(evaluation.__file__)),
                '--publish-existing', temporary, '--endpoint', 'http://127.0.0.1:1',
                '--strict-publication'], capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 1)
            after = json.loads(path.read_text())
            self.assertEqual(after['results'], initial['results'])
            self.assertEqual(after['summary'], initial['summary'])
            self.assertEqual(after['publication']['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
