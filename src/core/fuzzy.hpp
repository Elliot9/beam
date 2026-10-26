// Ranking of a typed query against a candidate name.
//
// Scores (higher is better, kNoMatch when the candidate does not match):
//   1000      exact match
//   850..900  candidate starts with the query
//   700..800  query starts a word inside the candidate ("code" -> "Visual Studio Code")
//   650..700  initials ("vsc" -> "Visual Studio Code")
//   500..600  substring inside a word
//   180..450  scattered subsequence, rewarded for consecutive / word-start hits
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace beam {

constexpr int kNoMatch = -1;

int FuzzyScore(std::wstring_view query, std::wstring_view candidate);

// Best score over a display title and its aliases (e.g. "記事本" + "notepad").
// Alias hits are slightly penalised so a match on the visible title wins ties.
int BestScore(std::wstring_view query, std::wstring_view title,
              const std::vector<std::wstring>& aliases, int aliasPenalty = 40);

}  // namespace beam
