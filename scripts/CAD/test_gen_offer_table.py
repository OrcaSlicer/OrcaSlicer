#!/usr/bin/env python3
"""The Design table generator's checks: every fault of an atlas is found and names its entry.

    python3 scripts/CAD/test_gen_offer_table.py

Each case builds a small valid atlas, breaks one rule and looks for the fault that names the entry
breaking it. The checked-in atlas must have none.
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_offer_table as gen  # noqa: E402


def block(params=None, **extra):
    """A tool block with every key the generator requires."""
    b = {"access": "change", "ok_false": False, "summary": "Paint a body.", "details": "",
         "params": params if params is not None else [
             {"name": "shade", "type": "string", "default": "red", "enum": ["red", "blue"]},
             {"name": "body", "type": "integer", "default": -1}],
         "examples": [], "ids": False}
    b.update(extra)
    return b


def atlas():
    """One slot, two selections, a command with a block and a menu item (paint), and a second item (tint), a command with
    a handler only; each under its heading."""
    return {
        "slots": [{"id": "make", "label": "Make"}],
        "selections": [{"id": "none"}, {"id": "body_solid"}],
        "verbs": [
            {"id": "paint", "name": "Paint", "section": "Solid", "slot": "make", "key": None,
             "accepts": ["none", "body_solid"], "action": "btn:paint", "icon": "paint", "hint": "Paint the body",
             "tool": block()},
            {"id": "tint", "name": "Tint", "section": "Menu items", "slot": "make", "key": None, "accepts": ["body_solid"],
             "action": "btn:tint", "icon": "tint", "hint": "Tint the body"},
        ],
    }


def entry(A, vid):
    return next(v for v in A["verbs"] if v["id"] == vid)


def item(vid, handler, **extra):
    v = {"id": vid, "name": vid.title(), "slot": "make", "key": None, "accepts": ["body_solid"],
         "action": handler, "icon": vid, "hint": "Do " + vid}
    v.update(extra)
    return v


class Faults(unittest.TestCase):
    def assertFault(self, A, vid, words):
        """A fault of A names entry `vid` and says `words`."""
        found = gen.faults(A)
        mine = [f for f in found if f.startswith(vid + ":")]
        self.assertTrue(any(words in f for f in mine), f"no fault of {vid} says {words!r}: {found}")

    # --- the atlases that pass -------------------------------------------------------------------

    def test_the_small_atlas_has_no_fault(self):
        self.assertEqual(gen.faults(atlas()), [])

    def test_the_checked_in_atlas_has_no_fault(self):
        with open(gen.ATLAS, encoding="utf-8") as f:
            self.assertEqual(gen.faults(json.load(f)), [])

    def test_a_folded_item_with_args_its_command_takes_passes(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        del entry(A, "tint")["section"]
        entry(A, "paint")["args"] = {"shade": "red"}
        self.assertEqual(gen.faults(A), [])

    def test_a_greyed_item_passes_whatever_its_id(self):
        A = atlas()
        del entry(A, "tint")["action"]
        del entry(A, "tint")["section"]
        entry(A, "tint")["id"] = "anything"
        self.assertEqual(gen.faults(A), [])

    def test_items_of_one_command_with_equal_args_and_one_handler_pass(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        del entry(A, "tint")["section"]
        A["verbs"].append(item("tint_again", "btn:tint", fold={"into": "paint", "args": {"shade": "blue"}}))
        self.assertEqual(gen.faults(A), [])

    def test_a_block_with_access_ui_passes(self):
        # A command that acts on what the tab has open, as design_confirm_tool and design_cancel_tool do.
        A = atlas()
        entry(A, "paint")["tool"]["access"] = "ui"
        self.assertEqual(gen.faults(A), [])

    def test_every_heading_passes(self):
        for heading in gen.SECTIONS:
            A = atlas()
            entry(A, "paint")["section"] = heading
            self.assertEqual(gen.faults(A), [], heading)

    # --- a fold ----------------------------------------------------------------------------------

    def test_a_fold_on_an_entry_with_no_handler(self):
        A = atlas()
        del entry(A, "tint")["action"]
        entry(A, "tint")["fold"] = {"into": "paint", "args": {}}
        self.assertFault(A, "tint", "fold needs a handler")

    def test_a_fold_on_an_entry_with_a_block(self):
        A = atlas()
        entry(A, "tint")["tool"] = block()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {}}
        self.assertFault(A, "tint", "a folded item has no tool block")

    def test_a_fold_that_is_no_object(self):
        A = atlas()
        entry(A, "tint")["fold"] = "paint"
        self.assertFault(A, "tint", "fold must be an object")

    def test_an_into_that_names_no_entry(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "nothing", "args": {}}
        self.assertFault(A, "tint", "names no entry")

    def test_an_into_that_names_an_entry_with_no_block(self):
        A = atlas()
        A["verbs"].append(item("wash", "btn:wash", fold={"into": "tint", "args": {}}))
        self.assertFault(A, "wash", "has no tool block")

    def test_an_into_that_names_a_folded_item(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        A["verbs"].append(item("wash", "btn:wash", fold={"into": "tint", "args": {}}))
        self.assertFault(A, "wash", "is itself a folded item")

    # --- the arguments that choose an item -------------------------------------------------------

    def test_a_folds_args_that_are_no_object(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": ["blue"]}
        self.assertFault(A, "tint", "args must be an object")

    def test_a_folds_args_that_name_a_param_its_command_lacks(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"colour": "blue"}}
        self.assertFault(A, "tint", "'colour'")

    def test_a_folds_args_that_give_a_value_the_enum_lacks(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "green"}}
        self.assertFault(A, "tint", "'green'")

    def test_own_args_that_are_no_object(self):
        A = atlas()
        entry(A, "paint")["args"] = "red"
        self.assertFault(A, "paint", "args must be an object")

    def test_own_args_that_name_a_param_the_block_lacks(self):
        A = atlas()
        entry(A, "paint")["args"] = {"colour": "red"}
        self.assertFault(A, "paint", "'colour'")

    def test_own_args_that_give_a_value_the_enum_lacks(self):
        A = atlas()
        entry(A, "paint")["args"] = {"shade": "green"}
        self.assertFault(A, "paint", "'green'")

    def test_two_items_of_one_command_with_equal_args_and_different_handlers(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        A["verbs"].append(item("wash", "btn:wash", fold={"into": "paint", "args": {"shade": "blue"}}))
        self.assertFault(A, "wash", "the same args as tint")

    def test_an_items_args_equal_to_its_commands_own(self):
        A = atlas()
        entry(A, "paint")["args"] = {"shade": "blue"}
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        self.assertFault(A, "tint", "the args paint's own item takes")

    def test_no_args_for_an_item_of_a_command_with_a_menu_item_of_its_own(self):
        # A call with none opens the command's own item.
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {}}
        self.assertFault(A, "tint", "the args paint's own item takes")

    def test_own_args_on_an_entry_with_no_handler(self):
        A = atlas()
        A["verbs"].append({"id": "rinse", "name": "Rinse", "slot": None, "tool": block(), "args": {"shade": "red"}})
        self.assertFault(A, "rinse", "own args need a handler (action) and a tool block")

    def test_own_args_on_an_entry_with_no_block(self):
        A = atlas()
        entry(A, "tint")["args"] = {"shade": "red"}
        self.assertFault(A, "tint", "own args need a handler (action) and a tool block")

    def test_empty_own_args(self):
        A = atlas()
        entry(A, "paint")["args"] = {}
        self.assertFault(A, "paint", "own args {} are what a call with none gives")

    # --- modes -----------------------------------------------------------------------------------

    def test_items_of_one_command_in_different_modes(self):
        A = atlas()
        A["verbs"].append({"id": "rinse", "name": "Rinse", "slot": None, "tool": block()})
        entry(A, "tint")["fold"] = {"into": "rinse", "args": {"shade": "blue"}}
        A["verbs"].append(item("wash", "btn:wash", mode="sketch", fold={"into": "rinse", "args": {"shade": "red"}}))
        self.assertFault(A, "wash", "in another mode than tint")

    def test_an_item_in_another_mode_than_its_commands_own_item(self):
        A = atlas()
        entry(A, "tint")["mode"] = "sketch"
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        self.assertFault(A, "tint", "in another mode than paint's own item")

    # --- headings --------------------------------------------------------------------------------

    def test_a_command_with_no_section(self):
        A = atlas()
        del entry(A, "tint")["section"]
        self.assertFault(A, "tint", "a command needs a section")

    def test_a_section_outside_the_headings(self):
        A = atlas()
        entry(A, "paint")["section"] = "Painting"
        self.assertFault(A, "paint", "section 'Painting' is not one of")

    def test_a_section_on_a_folded_item(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        self.assertFault(A, "tint", "is no command and has no section")

    def test_a_section_on_a_greyed_item(self):
        A = atlas()
        del entry(A, "tint")["action"]
        self.assertFault(A, "tint", "is no command and has no section")

    # --- the items that act on the Feature list's row --------------------------------------------

    def test_feature_row_on_a_command_with_a_menu_item_of_its_own_passes(self):
        A = atlas()
        entry(A, "tint")["feature_row"] = True
        entry(A, "paint")["feature_row"] = True
        self.assertEqual(gen.faults(A), [])

    def test_feature_row_on_an_entry_with_no_handler(self):
        A = atlas()
        A["verbs"].append({"id": "rinse", "name": "Rinse", "section": "Solid", "slot": None, "tool": block(),
                           "feature_row": True})
        self.assertFault(A, "rinse", "feature_row needs a menu item of its own")

    def test_feature_row_on_a_folded_item(self):
        A = atlas()
        entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
        del entry(A, "tint")["section"]
        entry(A, "tint")["feature_row"] = True
        self.assertFault(A, "tint", "feature_row needs a menu item of its own")

    # --- the operations a command reaches ---------------------------------------------------------

    def test_operations_on_a_command_pass(self):
        A = atlas()
        entry(A, "paint")["operations"] = ["delete_objects"]
        entry(A, "tint")["operations"] = ["rename_object", "select_plate"]
        self.assertEqual(gen.faults(A), [])

    def test_operations_on_a_folded_item(self):
        for ops in (["delete_objects"], None):
            with self.subTest(ops=ops):
                A = atlas()
                entry(A, "tint")["fold"] = {"into": "paint", "args": {"shade": "blue"}}
                del entry(A, "tint")["section"]
                entry(A, "tint")["operations"] = ops
                self.assertFault(A, "tint", "a folded or greyed item is no command")

    def test_operations_that_are_no_list_of_names(self):
        for ops in ("delete_objects", [], [""], ["delete_objects,rename_object"], [3], None):
            with self.subTest(ops=ops):
                A = atlas()
                entry(A, "paint")["operations"] = ops
                self.assertFault(A, "paint", "operations is a list of the names")

    # --- entries and blocks ----------------------------------------------------------------------

    def test_an_entry_with_no_slot_no_handler_and_no_block(self):
        A = atlas()
        A["verbs"].append({"id": "nothing", "name": "Nothing", "slot": None})
        self.assertFault(A, "nothing", "neither a menu item, a handler (action) nor a tool block")


if __name__ == "__main__":
    unittest.main()
