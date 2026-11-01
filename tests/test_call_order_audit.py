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

    def test_quit_to_dos_body_is_parsed(self):
        # loc_10e35 has no sub_ label of its own: the body starts at the label (LOC_BODIES)
        # and carries the exit chain — the same four calls as the fatal exit sub_10dba.
        orig = audit.parse_orig()
        self.assertEqual(orig['sub_10e35'], ['sub_16546', 'sub_1754c', 'sub_1686f', 'sub_1292f'])
        self.assertEqual(orig['sub_10dba'], ['sub_16546', 'sub_1754c', 'sub_1686f', 'sub_1292f'])

    def test_quit_jumps_are_calls(self):
        # the jumps into loc_10e35 count as calls of sub_10e35 (LOC_MIRRORED)
        orig = audit.parse_orig()
        self.assertEqual(orig['sub_10350'].count('sub_10e35'), 2)   # 037C (game mode 8000h), 0386 (level flag 8)
        self.assertEqual(orig['sub_103ca'][-1], 'sub_10e35')        # 0418: word_28814 & 2, "End game"
        self.assertEqual(orig['sub_1434c'][-1], 'sub_10e35')        # loc_143eb: the menu's Quit

    def test_quit_to_dos_mirror_calls_the_chain(self):
        # the port's quit to DOS: the archive close, sub_1686f, sub_1292f (16546 and 1754c have no
        # mirror call — whitelisted by exact snapshot, as the fatal exit's)
        raw = audit.v2_raw_bodies()
        self.assertEqual(audit.flatten('v2_quit_to_dos_10e35', raw), ['sub_1686f', 'sub_1292f'])

    def test_whitelist_snapshots_load(self):
        wl = audit.load_whitelist()
        self.assertEqual(wl[('sub_10dba', 'v2_fatal_10dba')][2],
                         (['sub_16546', 'sub_1754c', 'sub_1686f', 'sub_1292f'], ['sub_1686f', 'sub_1292f']))

    def test_whitelist_snapshot_only_matches_reviewed_sequences(self):
        original = ['sub_16546', 'sub_1754c', 'sub_1686f', 'sub_1292f']
        port = ['sub_1686f', 'sub_1292f']
        entry = ('PORT', 'reviewed example', (original, port))
        self.assertTrue(audit.whitelist_matches(entry, original, port))
        self.assertFalse(audit.whitelist_matches(entry, original, port + ['sub_12345']))
        self.assertFalse(audit.whitelist_matches(entry, original[:-1], port))


if __name__ == '__main__':
    unittest.main()
