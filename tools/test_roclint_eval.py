"""Checks evaluator failure accounting and publication independence without model/SDK dependencies."""
import json
import hashlib
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

    def test_related_groups_cannot_cross_splits(self):
        rows = [{'suite': 'rewrite', 'group': 'same', 'split': 'development'},
                {'suite': 'rewrite', 'group': 'same', 'split': 'heldout'}]
        with self.assertRaises(ValueError):
            evaluation.validate_splits(rows)

    def test_freeze_rejects_changed_dataset_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'cases.json'
            path.write_text('{"version": 2}')
            freeze = Path(temporary) / 'freeze.json'
            freeze.write_text(json.dumps({'datasets': {'ast': {'path': str(path), 'sha256': evaluation.sha256(path)}}, 'thresholds': {}}))
            evaluation.verify_freeze({'ast': path}, freeze)
            path.write_text('{"version": 3}')
            with self.assertRaises(ValueError):
                evaluation.verify_freeze({'ast': path}, freeze)

    def test_stale_proposal_must_be_withheld_and_not_published(self):
        case = {'expected': {'post_generation_withheld': True}}
        self.assertTrue(evaluation.score_rewrite(case, {'contract': {'withheld': True, 'proposal_published': False}})['passed'])
        self.assertFalse(evaluation.score_rewrite(case, {'contract': {'withheld': False, 'proposal_published': True}})['passed'])

    def test_frozen_dataset_accepts_git_line_ending_conversion_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'data.json'
            original = b'{"version": 2}\n'
            path.write_bytes(original)
            freeze = Path(temporary) / 'freeze.json'
            freeze.write_text(json.dumps({'datasets': {'ast': {'path': str(path),
                'sha256': evaluation.sha256(path), 'lf_content_sha256': hashlib.sha256(original).hexdigest()}}, 'thresholds': {}}))
            path.write_bytes(original.replace(b'\n', b'\r\n'))
            evaluation.verify_freeze({'ast': path}, freeze)
            path.write_bytes(b'{"version": 3}\r\n')
            with self.assertRaises(ValueError):
                evaluation.verify_freeze({'ast': path}, freeze)

    def test_ambiguity_cannot_be_satisfied_by_not_found(self):
        case = {'expected': {'grounding': {'no_verified': True, 'required_statuses': ['AMBIGUOUS']}}}
        actual = {'grounding': {'evidence': [{'status': 'NOT_FOUND_IN_SCOPE'}]}}
        self.assertIn('grounding:missing_status:AMBIGUOUS', evaluation.grounding_errors(case, actual))

    def editorial_fixture(self):
        reply = {'status': 'rewritten', 'rewritten_prompt': 'Keep the request.'}
        report = {'run_id': 'reviewed-run', 'source': {'head': 'commit-a'},
                  'results': [{'suite': 'rewrite', 'id': 'case', 'actual': {'reply': reply}}]}
        review = {'reviewer_kind': 'AGENT', 'source_run': 'reviewed-run', 'source_commit': 'commit-a',
                  'reviews': [{'case_id': 'case', 'reply_sha256': hashlib.sha256(json.dumps(reply, sort_keys=True).encode()).hexdigest(),
                               'agent_label': 'concern', 'clarity_delta': 0, 'usefulness_delta': -1}]}
        return report, review

    def test_editorial_annotation_cannot_move_to_another_reply(self):
        report, review = self.editorial_fixture()
        evaluation.verify_editorial_review(report, review)
        report['results'][0]['actual']['reply']['rewritten_prompt'] = 'Different output'
        with self.assertRaises(ValueError):
            evaluation.verify_editorial_review(report, review)

    def test_agent_review_cannot_claim_human_authority_or_foreign_run(self):
        report, review = self.editorial_fixture()
        review['reviewer_kind'] = 'HUMAN'
        with self.assertRaises(ValueError):
            evaluation.verify_editorial_review(report, review)
        review['reviewer_kind'] = 'AGENT'
        review['source_run'] = 'another-run'
        with self.assertRaises(ValueError):
            evaluation.verify_editorial_review(report, review)

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
