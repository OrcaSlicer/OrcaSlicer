.PHONY: test test-ooze-shield

# Bounty / CI verification entrypoint for OozeShield fff_print tests.
# Requires a Release build of fff_print_tests (see scripts/verify_ooze_shield_tests.sh).
test: test-ooze-shield

test-ooze-shield:
	./scripts/verify_ooze_shield_tests.sh
