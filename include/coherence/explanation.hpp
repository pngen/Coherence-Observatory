// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Structured explanations.
//
// An explanation is data, not prose.  It is rendered to text or JSON by
// deterministic functions; identical inputs always produce byte-identical
// output, and every high-level result carries one.

#ifndef COHERENCE_EXPLANATION_HPP
#define COHERENCE_EXPLANATION_HPP

#include <string>
#include <vector>

#include "coherence/attribution.hpp"
#include "coherence/cost.hpp"
#include "coherence/export.hpp"
#include "coherence/findings.hpp"
#include "coherence/publisher.hpp"

namespace sol::coherence {

/// One key/value line inside a section.
struct COHERENCE_API ExplanationLine {
  std::string key;
  std::string value;
};

/// A named section; sections render in insertion order, lines in key order.
struct COHERENCE_API ExplanationSection {
  std::string title;
  std::vector<ExplanationLine> lines;

  void add(std::string key, std::string value);
  void add(std::string key, std::uint64_t value);
  void add(std::string key, double value);
  void add(std::string key, bool value);
};

/// A complete explanation of one result.
struct COHERENCE_API Explanation {
  /// Result class, e.g. "finding.ping_pong" or "attribution".
  std::string kind;
  /// Result subject identity text.
  std::string subject;
  std::vector<ExplanationSection> sections;

  ExplanationSection& section(std::string title);
};

/// Deterministic human-readable rendering.
COHERENCE_API std::string render_text(const Explanation& explanation);

/// Deterministic JSON rendering (stable key order, no floating-point noise).
COHERENCE_API std::string render_json(const Explanation& explanation);

// ---- Explainers --------------------------------------------------------
COHERENCE_API Explanation explain(const Finding& finding);
COHERENCE_API Explanation explain(const AttributionResult& attribution);
COHERENCE_API Explanation explain(const CostEstimate& cost);
COHERENCE_API Explanation explain_loss(const LossReport& loss);

}  // namespace sol::coherence

#endif  // COHERENCE_EXPLANATION_HPP
