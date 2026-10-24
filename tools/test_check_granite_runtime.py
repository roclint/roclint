"""GPU-free checks for the explicit native development probe."""
import unittest

from check_granite_runtime import compact_json, select_cases


class CompactJsonTests(unittest.TestCase):
    def test_objects(self):
        for value in ('{}', '{"items":[1,true,null,{"x":"y"}]}'):
            self.assertTrue(compact_json(value))

    def test_preserves_string_content_and_escapes(self):
        for value in ('{"text":"retain spaces here"}',
                      r'{"text":"a\" b\\ c\n\t\r"}', '{"text":"日本語 🙂"}'):
            self.assertTrue(compact_json(value))

    def test_rejects_outside_string_whitespace(self):
        for value in (' {}', '{}\n', '{"a": 1}', '{\n"a":1}', '{"a":\t1}', '{"a":\r1}'):
            self.assertFalse(compact_json(value))

    def test_rejects_invalid_or_non_object_output(self):
        for value in (None, 1, '', '[]', 'null', '{', '{"x":"unterminated}', '{}{}', '{"x":"a\nb"}'):
            self.assertFalse(compact_json(value))


class FixtureSelectionTests(unittest.TestCase):
    def test_default_keeps_entire_corpus(self):
        cases = [{"id": "a"}, {"id": "b"}]
        self.assertEqual(select_cases(cases), cases)

    def test_filter_preserves_frozen_corpus_order(self):
        self.assertEqual(select_cases([{"id": "a"}, {"id": "b"}, {"id": "c"}], ["c", "a", "c"]),
                         [{"id": "a"}, {"id": "c"}])

    def test_unknown_fixture_fails_before_model_start(self):
        with self.assertRaisesRegex(ValueError, "Unknown fixture IDs: missing"):
            select_cases([{"id": "a"}], ["missing"])


if __name__ == "__main__":
    unittest.main()
