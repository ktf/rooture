/// @file grammar.js — tree-sitter grammar for the rooture Lisp dialect.
///
/// Regenerate the parser after editing:
///   tree-sitter generate
/// This produces src/parser.c which is compiled by the tree-sitter-rooture
/// alibuild recipe.

module.exports = grammar({
  name: "rooture",

  // Whitespace and comments are ignored between every node.
  extras: ($) => [/\s+/, $.comment],

  rules: {
    source_file: ($) => repeat($._expr),

    _expr: ($) =>
      choice(
        $.sexpr,
        $.qexpr,
        $.float,        // must come before number (more specific)
        $.number,
        $.string,
        $.dot_method,   // must come before symbol (.Method)
        $.named_ref,    // must come before symbol (@name)
        $.symbol,
      ),

    // S-expression  (evaluated)
    sexpr: ($) => seq("(", repeat($._expr), ")"),

    // Q-expression  (quoted / unevaluated)
    // Each item is either a normal expression or '|' followed by an inline string
    // that runs to end-of-line or '}'.  Multiple '|' strings can appear in one
    // Q-expression on separate lines.
    qexpr: ($) => seq("{", repeat($.qexpr_item), "}"),

    qexpr_item: ($) => choice(
      seq("|", $.inlinestr),
      $._expr,
    ),

    // Raw text after '|': stops at newline or '}'.
    inlinestr: ($) => token(/[^}\n]*/),

    // Literals
    // Each float alternative is written WITHOUT optional groups so that
    // tree-sitter's DFA does not stop at an early accepting state.
    // "with exponent" alternatives come first so maximal munch always wins.
    // Explicit priorities: float(2) > number(1) > symbol(0).
    float: ($) => token(prec(2, choice(
      /-?[0-9]+\.[0-9]*[eE][+-]?[0-9]+/,  // 1.5e-3  2.99792458e-4
      /-?[0-9]+\.[0-9]*/,                   // 1.5     2.0
      /-?\.[0-9]+[eE][+-]?[0-9]+/,         // .5e2
      /-?\.[0-9]+/,                          // .5
      /-?[0-9]+[eE][+-]?[0-9]+/,           // 1e-9    2e4
    ))),
    number: ($) => token(prec(1, /-?[0-9]+/)),
    string: ($) => token(/"([^"\\]|\\.)*"/),

    // .Method — shorthand for (. Method ...)
    dot_method: ($) => token(/\.[a-zA-Z_][a-zA-Z0-9_]*/),

    // @name — reference to a ROOT TNamed object by name
    named_ref: ($) => token(/@[a-zA-Z0-9_]+/),

    // Everything else is a symbol (operators, ROOT class paths, …)
    symbol: ($) => token(/[a-zA-Z0-9_+\-*\/\\=<>!&.:$~?]+/),

    // Line comment — not listed in extras so it appears in the AST
    comment: ($) => token(/;[^\r\n]*/),
  },
});
