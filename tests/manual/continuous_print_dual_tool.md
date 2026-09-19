# Continuous print: unused tools must not reject the print

Use a dual-nozzle printer project with one object instance, one assigned material,
By layer sequence, prime tower disabled, and continuous print enabled. Keep both
nozzles and both material slots in the printer/project configuration.

Run each case from a separate copy of the project; export G-code and inspect the
continuous-print report. A layer-specific geometric fallback is distinct from a
whole-print structural rejection.

| Case | Expected result |
| --- | --- |
| One assigned material, two configured nozzles | No structural rejection; eligible layers use continuous printing |
| Assign the entire object to the second material | Same as above; material index need not be zero |
| Assign top surface to material 2 and walls to material 1 | Rejected with `exactly one used material is required (found 2)` |
| Enable prime tower | Rejected with `the prime tower must be disabled` |
| Change sequence to By object | Rejected with `print sequence must be By layer` |
| Add a second object instance | Rejected with `exactly one object instance is required` |
| Disable continuous print | No continuous-print report; ordinary slicing |

Include support/interface assignments and explicit layer tool-change events when
checking used materials. Unused material slots must not count as used materials.
Ensure the exported configuration still has `enable_prime_tower = 1` for the
prime-tower case: CLI material mapping may normalize it back to zero.
Compare disabled-mode output with the previous binary, ignoring the generated-at
timestamp and object-label IDs. Audit enabled G-code with `tools/continuous_print_check.py`; individual
layer fallbacks may still be caused by geometry or support and must be reported.
