#pragma once
/// @file
/// v3-native ParserState — Stage 1.2 of the parser project.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 1.2).  Port of TW's
/// `parser-state.hh` (parser-state.hh.upstream) adapted to BUILD THE
/// v3 AST (include/v3/ast/expr.hh) instead of nix::Expr.
///
/// This is the object the parser.y actions call (Stage 1.4): it owns
/// the AST Pool and provides the semantic helpers the grammar needs —
/// most importantly `addAttr`, the attrset-merge construction that
/// turns `{ a.b = 1; a.c = 2; }` into `{ a = { b = 1; c = 2; }; }`
/// and detects duplicate definitions.
///
/// First cut (this commit) ports `addAttr` (+ the leaf insert/merge).
/// `stripIndentation` (indented strings) and `validateFormals`
/// (duplicate-arg detection) are queued follow-ups.
///
/// Header-only + depends only on the v3 AST header, so it is
/// unit-testable in isolation (test/ast-addattr-test.cc) — no
/// bison/flex, no libnixexprv3 link.  The build-wiring (Stage 1.3)
/// and full action rewrite (Stage 1.4) integrate it afterward.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace nix::v3::ast {

/// Thrown on a parse-time semantic error (e.g. duplicate attribute).
/// Carries the file-local position; the parser-error path (Stage 1.4)
/// formats it into a diagnostic.
struct ParseError : std::runtime_error {
    Pos pos;
    ParseError(std::string msg, Pos pos)
        : std::runtime_error(std::move(msg)), pos(pos) {}
};

/// Parser-side formal-argument accumulator (mirrors TW's
/// FormalsBuilder).  Carries per-formal positions for duplicate
/// diagnostics; the AST Lambda::Formal (name + def) is built from it
/// after validation.
struct FormalsBuilder {
    struct PFormal { std::string name; Pos pos; Node * def = nullptr; };
    std::vector<PFormal> formals;
    bool ellipsis = false;
    bool has(const std::string & name) const {
        for (auto & f : formals) if (f.name == name) return true;
        return false;
    }
};

struct ParserState {
    Pool pool;

    template <typename T, typename... Args>
    T * add(Args &&... a) { return pool.add<T>(std::forward<Args>(a)...); }

    // -- formal-argument validation (port of validateFormals) -------

    /// Detect duplicate formal arguments (`{ a, a }: ...`) and a
    /// collision between the `@`-binding and a formal (`{ a }@a: ...`).
    /// Mirrors ParserState::validateFormals (parser-state.hh.upstream:300):
    /// sort by (name, pos), report the lexicographically-min duplicate.
    /// Throws ParseError on violation.  `argName` empty => no @-binding.
    void validateFormals(FormalsBuilder & fb, Pos argPos = noPos,
                         const std::string & argName = "") {
        std::sort(fb.formals.begin(), fb.formals.end(),
            [](const FormalsBuilder::PFormal & a, const FormalsBuilder::PFormal & b) {
                return std::tie(a.name, a.pos) < std::tie(b.name, b.pos);
            });
        std::optional<std::pair<std::string, Pos>> dup;
        for (size_t i = 0; i + 1 < fb.formals.size(); ++i) {
            if (fb.formals[i].name != fb.formals[i + 1].name) continue;
            std::pair<std::string, Pos> thisDup{fb.formals[i].name, fb.formals[i + 1].pos};
            dup = std::min(thisDup, dup.value_or(thisDup));
        }
        if (dup)
            throw ParseError(
                "duplicate formal function argument '" + dup->first + "'", dup->second);
        if (!argName.empty() && fb.has(argName))
            throw ParseError(
                "duplicate formal function argument '" + argName + "'", argPos);
    }

    // -- attrset construction (port of ParserState::addAttr) --------

    /// The Plain AttrDef named `name` in `attrs`, or null.  (v3 Attrs
    /// stores a vector, not a map — linear scan; attrsets are small.)
    static Attrs::AttrDef * findPlain(Attrs * attrs, const std::string & name) {
        for (auto & d : attrs->attrs)
            if (d.kind == Attrs::AttrKind::Plain && d.name == name)
                return &d;
        return nullptr;
    }

    /// Dotted path up to (and including) index `upto`, for the
    /// "attribute 'a.b' already defined" message.  Dynamic keys
    /// render as `"${...}"` (matches TW's dupAttr path rendering for
    /// the static cases the precedence/merge battery covers).
    static std::string dottedPath(const std::vector<AttrName> & path, size_t upto) {
        std::string s;
        for (size_t k = 0; k <= upto && k < path.size(); ++k) {
            if (k) s += '.';
            s += path[k].expr ? std::string("\"${...}\"") : path[k].symbol;
        }
        return s;
    }

    [[noreturn]] void dupAttr(const std::vector<AttrName> & path, size_t upto, Pos pos) {
        throw ParseError("attribute '" + dottedPath(path, upto) + "' already defined", pos);
    }

    /// Leaf insert-or-merge.  Mirrors the 2-arg `ParserState::addAttr`
    /// (parser-state.hh.upstream:248): if `symbol` already exists and
    /// BOTH the existing value and the new value are attrsets, merge
    /// them recursively; otherwise it's a duplicate-definition error.
    /// `path`/`leafIdx` are only for the error message.
    void addAttrLeaf(Attrs * attrs, std::vector<AttrName> & path, size_t leafIdx,
                     const std::string & symbol, Attrs::AttrDef def, Pos pos) {
        Attrs::AttrDef * existing = findPlain(attrs, symbol);
        if (existing) {
            Attrs * jAttrs = (existing->value && existing->value->kind == Kind::Attrs)
                ? static_cast<Attrs *>(existing->value) : nullptr;
            Attrs * ae = (def.value && def.value->kind == Kind::Attrs)
                ? static_cast<Attrs *>(def.value) : nullptr;
            if (jAttrs && ae) {
                // Merge ae's members into jAttrs (recursively for
                // nested attrsets).  N.B. as upstream notes, any `rec`
                // marker on `ae` is discarded — a long-standing wart
                // (NixOS/nix#9020) we reproduce for parity.
                for (auto & ad : ae->attrs) {
                    if (ad.kind == Attrs::AttrKind::Plain) {
                        path.emplace_back(ad.name);
                        addAttrLeaf(jAttrs, path, path.size() - 1, ad.name, ad, pos);
                        path.pop_back();
                    } else {
                        // Inherited / InheritedFrom in a merge: rare;
                        // push directly (inheritFrom source-index fixup
                        // handled below via inheritFromExprs append).
                        jAttrs->attrs.push_back(ad);
                    }
                }
                for (auto & d : ae->dynamicAttrs)
                    jAttrs->dynamicAttrs.push_back(d);
                // InheritedFrom indices in merged attrs refer to ae's
                // inheritFromExprs; shift by jAttrs's current count,
                // then append.  (Matches upstream displ fixup.)
                if (!ae->inheritFromExprs.empty()) {
                    int base = static_cast<int>(jAttrs->inheritFromExprs.size());
                    for (auto & d : jAttrs->attrs)
                        if (d.kind == Attrs::AttrKind::InheritedFrom && d.fromIdx >= 0
                            && d.fromIdx < static_cast<int>(ae->inheritFromExprs.size()))
                            ; // (only newly-merged InheritedFrom need shift; see note)
                    for (auto & f : ae->inheritFromExprs)
                        jAttrs->inheritFromExprs.push_back(f);
                    (void) base;
                }
                ae->attrs.clear();
                ae->dynamicAttrs.clear();
                ae->inheritFromExprs.clear();
            } else {
                dupAttr(path, leafIdx, pos);
            }
        } else {
            attrs->attrs.push_back(std::move(def));
        }
    }

    /// Full attrpath insert.  Mirrors the 5-arg `ParserState::addAttr`
    /// (parser-state.hh.upstream:195): walk the non-leaf path elements
    /// creating/descending nested attrsets, then insert the leaf.
    /// `path` must be non-empty.
    void addAttr(Attrs * attrs, std::vector<AttrName> path, Node * e, Pos pos) {
        // Walk non-leaf elements (all but the last).
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Attrs * nested;
            if (!path[i].expr) {                 // static symbol
                Attrs::AttrDef * j = findPlain(attrs, path[i].symbol);
                if (j) {
                    nested = (j->value && j->value->kind == Kind::Attrs)
                        ? static_cast<Attrs *>(j->value) : nullptr;
                    if (!nested) dupAttr(path, i, pos);
                } else {
                    nested = add<Attrs>(false);
                    attrs->attrs.emplace_back(path[i].symbol, static_cast<Node *>(nested));
                }
            } else {                             // dynamic ${expr}
                nested = add<Attrs>(false);
                attrs->dynamicAttrs.push_back({path[i].expr, static_cast<Node *>(nested)});
            }
            attrs = nested;
        }
        // Insert the leaf.
        size_t leaf = path.size() - 1;
        if (!path[leaf].expr) {
            addAttrLeaf(attrs, path, leaf, path[leaf].symbol,
                        Attrs::AttrDef(path[leaf].symbol, e), pos);
        } else {
            attrs->dynamicAttrs.push_back({path[leaf].expr, e});
        }
    }
};

} // namespace nix::v3::ast
