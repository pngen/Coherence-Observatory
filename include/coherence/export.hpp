// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Visibility / linkage decoration for the public API.

#ifndef COHERENCE_EXPORT_HPP
#define COHERENCE_EXPORT_HPP

#if defined(_WIN32) && defined(COHERENCE_OBSERVATORY_SHARED)
#if defined(COHERENCE_OBSERVATORY_BUILDING)
#define COHERENCE_API __declspec(dllexport)
#else
#define COHERENCE_API __declspec(dllimport)
#endif
#else
#define COHERENCE_API
#endif

#if defined(__GNUC__) || defined(__clang__)
#define COHERENCE_HIDDEN __attribute__((visibility("hidden")))
#else
#define COHERENCE_HIDDEN
#endif

#endif  // COHERENCE_EXPORT_HPP
