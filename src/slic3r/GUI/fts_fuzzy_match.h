// LICENSE
//
//   This software is dual-licensed to the public domain and under the following
//   license: you are granted a perpetual, irrevocable license to copy, modify,
//   publish, and distribute this file as you see fit.
//
// VERSION 
//   0.2.0  (2017-02-18)  Scored matches perform exhaustive search for best score
//   0.1.0  (2016-03-28)  Initial release
//
// AUTHOR
//   Forrest Smith
//
// NOTES
//   Compiling
//     You MUST add '#define FTS_FUZZY_MATCH_IMPLEMENTATION' before including this header in ONE source file to create implementation.
//
//   fuzzy_match_simple(...)
//     Returns true if each character in pattern is found sequentially within str
//
//   fuzzy_match(...)
//     Returns true if pattern is found AND calculates a score.
//     Performs exhaustive search via recursion to find all possible matches and match with highest score.
//     Scores values have no intrinsic meaning. Possible score range is not normalized and varies with pattern.
//     Recursion is limited internally (default=10) to prevent degenerate cases (pattern="aaaaaa" str="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
//     Uses uint8_t for match indices. Therefore patterns are limited to max_matches characters.
//     Score system should be tuned for YOUR use case. Words, sentences, file names, or method names all prefer different tuning.


#ifndef FTS_FUZZY_MATCH_H
#define FTS_FUZZY_MATCH_H


#include <cstdint> // uint8_t
#include <ctype.h> // ::tolower, ::toupper
#include <cwctype> // ::towlower
#include <cstring> // memcpy

#include <cstdio>

#include "../Utils/ASCIIFolding.hpp"

// Public interface
namespace fts {
	using 						char_type 	= wchar_t;
	using 						pos_type  	= uint16_t;
	static constexpr pos_type 	stopper 	= pos_type(-1);
	static constexpr int 		max_matches = 255;

    static bool fuzzy_match(char_type const * pattern, char_type const * str, int & outScore);
    static bool fuzzy_match(char_type const * pattern, char_type const * str, int & outScore, pos_type * matches);
}

#ifdef FTS_FUZZY_MATCH_IMPLEMENTATION
namespace fts {

    // Forward declarations for "private" implementation
    namespace fuzzy_internal {
        static bool fuzzy_match_recursive(const char_type * pattern, const char_type * str, int & outScore, const char_type * const strBegin,          
            pos_type const * srcMatches,  pos_type * newMatches, int nextMatch, 
            int recursionCount, const int recursionLimit);
        static void copy_matches(pos_type * dst, pos_type const* src);
    }

    // Public interface
    [[maybe_unused]] static bool fuzzy_match(char_type const * pattern, char_type const * str, int & outScore) {
        pos_type matches[max_matches + 1]; // with the room for the stopper
        matches[0] = stopper;
        return fuzzy_match(pattern, str, outScore, matches);
    }

    [[maybe_unused]] static bool fuzzy_match(char_type const * pattern, char_type const * str, int & outScore, pos_type * matches) {
        int recursionCount = 0;
        static constexpr int recursionLimit = 10;
        return fuzzy_internal::fuzzy_match_recursive(pattern, str, outScore, str, nullptr, matches, 0, recursionCount, recursionLimit);
    }

    // Private implementation
    static bool fuzzy_internal::fuzzy_match_recursive(
    	// Pattern to match over str.
    	const char_type * 		pattern, 
    	// Text to match the pattern over.
    	const char_type * 		str, 
    	// Score of the pattern matching str. Output variable.
    	int & 					outScore, 
    	// The very start of str, for calculating indices of matches and for calculating matches from the start of the input string.
        const char_type * const	strBegin, 
        // Matches when entering this function.
        pos_type const * 		srcMatches,
        // Output matches.
        pos_type * 				matches,
        // Number of matched characters stored in srcMatches when entering this function, also tracking the successive matches.
        int 					nextMatch,
        // Recursion count is input / output to track the maximum depth reached.
        // Was given by reference &recursionCount, see discussion in https://github.com/forrestthewoods/lib_fts/issues/21
//        int & 					recursionCount, 
        int				        recursionCount, 
        const int				recursionLimit)
    {
        // Count recursions
        if (++ recursionCount >= recursionLimit)
            return false;

        // Detect end of strings
        if (*pattern == '\0' || *str == '\0')
            return false;

        // Recursion params
        bool recursiveMatch = false;
        pos_type bestRecursiveMatches[max_matches + 1]; // with the room for the stopper
        int bestRecursiveScore = 0;

        // Loop through pattern and str looking for a match
        bool first_match = true;
        while (*pattern != '\0' && *str != '\0') {

        	int  num_matched  = std::towlower(*pattern) == std::towlower(*str) ? 1 : 0;
            // bool folded_match = false;

            if (! num_matched) {
                wchar_t tmp[4];
                wchar_t *end = Slic3r::fold_to_ascii(*str, tmp);
                wchar_t *c = tmp;
                for (const wchar_t* d = pattern; c != end && *d != 0 && std::towlower(*c) == std::towlower(*d); ++c, ++d);
                if (c == end) {
                    // folded_match = true;
        			num_matched = end - tmp;
        		}
	        }
            
            // Found match
            if (num_matched) {

                // Supplied matches buffer was too short
                if (nextMatch + num_matched > max_matches)
                    return false;

                // "Copy-on-Write" srcMatches into matches
                if (first_match && srcMatches) {
                    memcpy(matches, srcMatches, sizeof(pos_type) * (nextMatch + 1)); // including the stopper
                    first_match = false;
                }

                // Recursive call that "skips" this match
                pos_type recursiveMatches[max_matches + 1]; // with the room for the stopper
                int recursiveScore;
                if (fuzzy_match_recursive(pattern, str + 1, recursiveScore, strBegin, matches, recursiveMatches, nextMatch, recursionCount, recursionLimit)) {
                    
                    // Pick best recursive score
                    if (!recursiveMatch || recursiveScore > bestRecursiveScore) {
                    	copy_matches(bestRecursiveMatches, recursiveMatches);
                		bestRecursiveScore = recursiveScore;
                    }
                    recursiveMatch = true;
                }

                // Advance
                matches[nextMatch++] = (pos_type)(str - strBegin);
                // Write a stopper sign.
                matches[nextMatch] = stopper;
                // Advance pattern by the number of matched characters (could be more if ASCII folding triggers in).
                pattern += num_matched;
            } 
            ++str;
        }

        // Determine if full pattern was matched
        bool matched = *pattern == '\0';

        // Calculate score
        if (matched) {
            static constexpr int sequential_bonus = 75;            // bonus for adjacent matches
            static constexpr int separator_bonus = 10;             // bonus if match occurs after a separator
            // static constexpr int camel_bonus = 30;              // bonus if match is uppercase and prev is lower
            static constexpr int first_letter_bonus = -30;         // bonus if the first letter is matched

            static constexpr int leading_letter_penalty = -1;      // penalty applied for every letter in str before the first match
            static constexpr int max_leading_letter_penalty = -15; // maximum penalty for leading letters
            static constexpr int unmatched_letter_penalty = -1;    // penalty for every letter that doesn't matter

            // Iterate str to end
            while (*str != '\0')
                ++str;

            // Initialize score
            outScore = 100;

            // Start of the first group that contains matches[0].
            const char_type *group_start = strBegin + matches[0];
            for (const char_type *c = group_start; c >= strBegin && *c != ':'; -- c)
            	if (*c != ' ' && *c != '\t')
            		group_start = c;

            // Apply leading letter penalty or bonus.
            outScore += matches[0] == int(group_start - strBegin) ?
            	first_letter_bonus :
            	std::max((matches[0] - int(group_start - strBegin)) * leading_letter_penalty, max_leading_letter_penalty);

            // Apply unmatched letters after the end penalty
//            outScore += (int(str - group_start) - matches[nextMatch-1] + 1) * unmatched_letter_penalty;
            // Apply unmatched penalty
            outScore += (int(str - group_start) - nextMatch) * unmatched_letter_penalty;

            // Apply ordering bonuses
            int sequential_state = sequential_bonus;
            for (int i = 0; i < nextMatch; ++i) {
                pos_type currIdx = matches[i];

                // Check for bonuses based on neighbor character value
                if (currIdx > 0) {
                    if (i > 0 && currIdx == matches[i - 1] + 1) {
	                    // Sequential
                        outScore += sequential_state;
                        // Exponential grow of the sequential bonus.
                    	sequential_state = std::min(5 * sequential_bonus, sequential_state + sequential_state / 3);
                    } else {
                    	// Reset the sequential bonus exponential grow.
                    	sequential_state = sequential_bonus;
                    }
					char_type prev = strBegin[currIdx - 1];
/*
                    // Camel case
                    if (std::islower(prev) && std::isupper(strBegin[currIdx]))
                        outScore += camel_bonus;
*/
                    // Separator
                    if (prev == '_' || prev == ' ')
                        outScore += separator_bonus;
                }
            }
        }

        // Return best result
        if (recursiveMatch && (!matched || bestRecursiveScore > outScore)) {
            // Recursive score is better than "this"
            copy_matches(matches, bestRecursiveMatches);
            outScore = bestRecursiveScore;
            return true;
        }
        else if (matched) {
            // "this" score is better than recursive
            return true;
        }
        else {
            // no match
            return false;
        }
    }

    // Copy matches up to a stopper.
    static void fuzzy_internal::copy_matches(pos_type * dst, pos_type const* src)
    {
        while (*src != stopper)
            *dst++ = *src++;
        *dst = stopper;
    }

} // namespace fts

#endif // FTS_FUZZY_MATCH_IMPLEMENTATION

#endif // FTS_FUZZY_MATCH_H
