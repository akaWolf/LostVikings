import unittest
from unittest.mock import patch

import call_order_audit as audit


class CallOrderAuditTest(unittest.TestCase):
    def test_forward_declaration_is_not_a_call(self):
        source = [
            'bool v2_first_12345() {',
            '    v2_real_12346();',
            '    return v2_helper();',
            '}',
            'bool v2_next_helper(int value); // forward declaration',
            'bool v2_next_helper(int value) {',
            '    v2_other_12347();',
            '}',
        ]
        with patch.object(audit, 'V2_FILES', ['fixture']), patch.object(audit, 'load', return_value=source):
            raw = audit.v2_raw_bodies()
        self.assertEqual(raw['v2_first_12345'], [
            ('hex', 'sub_12346'), ('helper', 'v2_helper')])
        self.assertEqual(raw['v2_next_helper'], [('hex', 'sub_12347')])

    def test_down_collision_does_not_include_up_collision(self):
        raw = audit.v2_raw_bodies()
        self.assertEqual(audit.flatten('v2_vm_collision_check_1584e', raw), [
            'sub_15afd', 'sub_15972', 'sub_1614e', 'sub_15da8'])

    def test_whitelist_snapshot_only_matches_reviewed_sequences(self):
        original = ['sub_16546', 'sub_1754c', 'sub_1686f', 'sub_1292f']
        port = ['sub_1686f', 'sub_1292f']
        entry = ('PORT', 'reviewed example', (original, port))
        self.assertTrue(audit.whitelist_matches(entry, original, port))
        self.assertFalse(audit.whitelist_matches(entry, original, port + ['sub_12345']))
        self.assertFalse(audit.whitelist_matches(entry, original[:-1], port))


if __name__ == '__main__':
    unittest.main()
